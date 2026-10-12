// Locates hitscan by content, not bytes: clip.cpp logs "hitscan: tempshortnum >= MAXCLIPSECTORS..." inside its wall
// loop, so the anchor is that string and the function is the one referencing it (entry from the PE exception
// directory). cansee.cpp's pattern is compiler-dependent. Rvas and prologue proof: docs/re/hitscan.md.

// Runs inside the SwapBuffers hook on the game thread (hitscan.h): driven from overlay.cpp's per-frame path, not
// gamestate.cpp's worker.

#include <windows.h>
#include "log.h"
#include "hitscan.h"
#include "imagescan.h"
#include "gamestate.h"
#include "playerstate.h"
#include "config.h"

typedef int (*EkHitscanFn)(const void* sv, short sectnum, int vx, int vy, int vz,
                           void* hit, unsigned cliptype);

struct EkVec3 { int x, y, z; };

static const char EK_HITSCAN_ANCHOR[] =
    "hitscan: tempshortnum >= MAXCLIPSECTORS at (%d,%d,%d)!";

static EkHitscanFn volatile s_fn = NULL;
static volatile LONG s_rva = 0;

static bool s_refused = false;

static const int EK_HITSCAN_MAX_TESTS = 4;
static int s_tests = 0;
static unsigned s_entry = 0;
static DWORD s_lastTry = 0;
static const DWORD EK_HITSCAN_RETRY_MS = 2000;

// Self-test ray: due east, level, at the engine's magnitudes (sintable[512] = 16384); vz 0 is ordinary and both
// branches guard their divisor.
static const int EK_HITSCAN_TEST_VX = 16384;
static const int EK_HITSCAN_TEST_VY = 0;
static const int EK_HITSCAN_TEST_VZ = 0;

// Sentinel, so "three -1s" proves the ENGINE wrote them.
static const short EK_HITSCAN_SENTINEL = 0x7777;

// Bad-sector half: a negative sectnum sets sect, wall, sprite to -1 and returns -1, so a call with -1 over a sentinel
// proves argument order and hitdata_t offsets without touching state. Live half: a cast from an engine-given sector
// returns 0.
struct SelfTest {
  int badRet; EkHitData bad;
  int liveRet; EkHitData live;
  int reached;
};

static void self_test(EkHitscanFn fn, const PlayerSnap* p, SelfTest* r) {
  r->reached = 0;
  r->badRet = 0; r->liveRet = 0;
  __try {
    EkVec3 sv = { p->x, p->y, p->z };
    r->bad.x = r->bad.y = r->bad.z = 0;
    r->bad.sprite = r->bad.wall = r->bad.sect = EK_HITSCAN_SENTINEL;
    r->badRet = fn(&sv, (short)-1, EK_HITSCAN_TEST_VX, EK_HITSCAN_TEST_VY,
                   EK_HITSCAN_TEST_VZ, &r->bad, (unsigned)EK_CLIPMASK1);
    r->reached = 1;

    r->live.x = r->live.y = r->live.z = 0;
    r->live.sprite = r->live.wall = r->live.sect = EK_HITSCAN_SENTINEL;
    r->liveRet = fn(&sv, (short)p->sect, EK_HITSCAN_TEST_VX, EK_HITSCAN_TEST_VY,
                    EK_HITSCAN_TEST_VZ, &r->live, (unsigned)EK_CLIPMASK1);
    r->reached = 2;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static unsigned locate_once(const unsigned char* base) {
  EkStrHit s;
  ek_image_find_string(base, EK_HITSCAN_ANCHOR,
                       sizeof(EK_HITSCAN_ANCHOR) - 1, &s);
  if (s.faulted) {
    s_refused = true;
    ek_log("hitscan: a data section faulted mid-scan, so the anchor cannot be "
           "claimed unique (%d occurrence(s) before the fault); refusing", s.count);
    return 0;
  }
  if (s.count == 0) {
    s_refused = true;
    ek_log("hitscan: the anchor string is not in this build -- the shot ray "
           "stays unavailable and every caller falls back to cansee");
    return 0;
  }
  if (s.count != 1) {
    s_refused = true;
    ek_log("hitscan: the anchor string occurs %d times, first at rva 0x%07X; "
           "refusing to guess", s.count, s.rva);
    return 0;
  }

  EkRefHit h;
  ek_image_find_lea_refs(base, s.rva, &h);
  if (h.faulted) {
    s_refused = true;
    ek_log("hitscan: a code section faulted mid-scan, so uniqueness cannot be "
           "claimed (%d reference(s) before the fault); refusing", h.count);
    return 0;
  }
  if (h.count == 0) {
    s_refused = true;
    ek_log("hitscan: the anchor at rva 0x%07X is referenced by no rip-relative "
           "lea (%d raw displacement match(es)); refusing", s.rva, h.raw);
    return 0;
  }
  if (h.count != 1) {
    s_refused = true;
    ek_log("hitscan: the anchor at rva 0x%07X has %d references, first at rva "
           "0x%07X; refusing to guess", s.rva, h.count, h.rva);
    return 0;
  }

  const unsigned entry = ek_image_entry_containing(base, h.rva);
  if (entry == 0 || entry > h.rva) {
    s_refused = true;
    ek_log("hitscan: the reference at rva 0x%07X is not inside any function in "
           "the exception directory; refusing", h.rva);
    return 0;
  }

  ek_log("hitscan: anchor once at rva 0x%07X, referenced once at rva 0x%07X "
         "(%d raw displacement match(es)), inside the function at rva 0x%07X "
         "(%+d); self-testing",
         s.rva, h.rva, h.raw, entry, (int)(h.rva - entry));
  return entry;
}

bool hitscan_scan(void) {
  if (s_fn) return true;
  if (s_refused) return false;

  if (!player_ready()) return false;
  PlayerSnap snap;
  if (!player_snap(&snap)) return false;
  if (snap.sect < 0 || snap.sect >= EK_MAXSECTORS) return false;

  if (s_tests >= EK_HITSCAN_MAX_TESTS) return false;
  const DWORD now = GetTickCount();
  if (s_tests > 0 && (DWORD)(now - s_lastTry) < EK_HITSCAN_RETRY_MS) return false;
  s_lastTry = now;
  s_tests++;

  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  if (!base) { s_refused = true; ek_log("hitscan: no host module"); return false; }

  if (s_entry == 0) {
    s_entry = locate_once(base);
    if (s_entry == 0) return false;
  }

  EkHitscanFn const fn = (EkHitscanFn)(void*)(base + s_entry);
  SelfTest r;
  self_test(fn, &snap, &r);

  const bool badOk = r.reached >= 1 && r.badRet == -1 &&
                     r.bad.sprite == -1 && r.bad.wall == -1 && r.bad.sect == -1;
  const bool liveOk = r.reached >= 2 && r.liveRet == 0;
  if (!badOk || !liveOk) {
    const char* which =
        r.reached == 0 ? "the bad-sector call faulted"
      : r.reached == 1 ? "the live call faulted"
      : !badOk         ? "sector -1 did not come back as -1,-1,-1 / -1"
                       : "a live sector did not return 0";
    ek_log("hitscan: self-test FAILED at rva 0x%07X -- %s [reached=%d "
           "bad ret=%d sprite=%d wall=%d sect=%d; live ret=%d]; the shot ray "
           "stays unavailable",
           s_entry, which, r.reached, r.badRet, (int)r.bad.sprite,
           (int)r.bad.wall, (int)r.bad.sect, r.liveRet);
    if (s_tests >= EK_HITSCAN_MAX_TESTS)
      ek_log("hitscan: giving up; the triggerbot and pass 2 keep using cansee");
    return false;
  }

  InterlockedExchange(&s_rva, (LONG)s_entry);
  InterlockedExchangePointer((void* volatile*)&s_fn, (void*)fn);

  ek_log("hitscan: armed at rva 0x%07X -- self-test passed from (%d,%d,%d) "
         "sector %d: sector -1 gave ret %d and sprite/wall/sect -1/-1/-1, a "
         "live sector gave ret %d",
         s_entry, snap.x, snap.y, snap.z, snap.sect, r.badRet, r.liveRet);
  ek_log("hitscan:   probe (not gating): east ray hit sprite %d wall %d sect %d "
         "at (%d,%d,%d)",
         (int)r.live.sprite, (int)r.live.wall, (int)r.live.sect,
         r.live.x, r.live.y, r.live.z);
  return true;
}

// Origin-sector machinery (hitscan.h has the bug). updatesector writes engine lists, so hitscan's thread rule
// applies. Two witnesses must agree: the SHAPE (updatesectorz with three args writes through r9; constants 256 and
// 1536) and the CALL SITE before the engine's `call hitscan`.

typedef void (*EkUpdateSectorFn)(int x, int y, short* sectnum);

static EkUpdateSectorFn volatile s_usFn = NULL;
static volatile LONG s_usRva = 0;
static bool s_usRefused = false;
static bool s_usSaidLost = false;
static unsigned s_usEntry = 0;
static int s_usTests = 0;
static DWORD s_usLastTry = 0;
static const int EK_US_MAX_TESTS = 4;

// Window back from a `call hitscan`: shipped distance is 80, bounded because each byte risks a non-opcode 0xE8.
static const int EK_US_BACK_WINDOW = 96;

// Witness 1: functions whose prologue has updatesector's argument shape; more than one means the shape no longer
// identifies and the scan refuses.
struct ShapeHit { unsigned rva; int count; int scanned; int faulted; };

static void find_us_shape(const unsigned char* base, ShapeHit* out) {
  out->rva = 0; out->count = 0; out->scanned = 0; out->faulted = 0;
  const IMAGE_RUNTIME_FUNCTION_ENTRY* fn = NULL;
  DWORD n = 0;
  if (!ek_image_pdata(base, &fn, &n)) { out->faulted = 1; return; }
  __try {
    for (DWORD i = 0; i < n; i++) {
      const DWORD b = fn[i].BeginAddress, e = fn[i].EndAddress;
      if (e <= b) continue;
      out->scanned++;
      DWORD win = e - b;
      if (win > (DWORD)EKM_US_SHAPE_WINDOW) win = (DWORD)EKM_US_SHAPE_WINDOW;
      if (!ekm_us_shape_match(base + b, (int)win)) continue;
      if (out->count == 0) out->rva = (unsigned)b;
      out->count++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

static bool is_function_entry(const IMAGE_RUNTIME_FUNCTION_ENTRY* fn, DWORD n,
                              unsigned rva) {
  for (DWORD i = 0; i < n; i++)
    if (fn[i].BeginAddress == rva && fn[i].EndAddress > fn[i].BeginAddress)
      return true;
  return false;
}

// Witness 2: each `call hitscan` and the nearest preceding `call rel32` in the same function that resolves to a
// function entry (rejects an 0xE8 inside an operand).
struct CallWitness {
  int sites;
  int withPrev;
  int agree;
  int rival;
  unsigned rivalRva;
  int faulted;
};

static void find_us_callsites(const unsigned char* base, unsigned hitscanEntry,
                              unsigned shapeRva, CallWitness* out) {
  out->sites = 0; out->withPrev = 0; out->agree = 0; out->rival = 0;
  out->rivalRva = 0; out->faulted = 0;
  const IMAGE_RUNTIME_FUNCTION_ENTRY* fn = NULL;
  DWORD n = 0;
  if (!ek_image_pdata(base, &fn, &n)) { out->faulted = 1; return; }

  EkSpan spans[16];
  int nspan = 0;
  if (!ek_image_sections(base, true, spans, 16, &nspan)) { out->faulted = 1; return; }

  for (int i = 0; i < nspan; i++) {
    if (spans[i].size < 5) continue;
    __try {
      const DWORD last = spans[i].size - 5;
      for (DWORD off = 0; off <= last; off++) {
        if (spans[i].base[off] != 0xE8) continue;
        const unsigned site = ek_image_rva(base, spans[i].base + off);
        const int disp = *(const int*)(spans[i].base + off + 1);
        if (ekm_us_rel32_target(site, disp) != hitscanEntry) continue;
        out->sites++;

        const unsigned own = ek_image_entry_containing(base, site);
        if (own == 0 || own > site || site < own + 5) continue;
        unsigned lo = site > (unsigned)EK_US_BACK_WINDOW
                          ? site - (unsigned)EK_US_BACK_WINDOW : 0;
        if (lo < own) lo = own;

        unsigned prev = 0;
        for (unsigned q = site - 5; q >= lo; q--) {
          const unsigned char* pb = base + q;
          if (*pb == 0xE8) {
            const int d = *(const int*)(pb + 1);
            const unsigned t = ekm_us_rel32_target(q, d);
            if (is_function_entry(fn, n, t)) { prev = t; break; }
          }
          if (q == lo) break;
        }
        if (prev == 0) continue;
        out->withPrev++;
        if (prev == shapeRva) out->agree++;
        else { if (out->rival == 0) out->rivalRva = prev; out->rival++; }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      out->faulted = 1;
    }
  }
}

static unsigned locate_us_once(const unsigned char* base, unsigned hitscanEntry) {
  ShapeHit sh;
  find_us_shape(base, &sh);
  if (sh.faulted) {
    s_usRefused = true;
    ek_log("updatesector: the function table faulted mid-scan, so the argument "
           "shape cannot be claimed unique (%d match(es) in %d function(s) "
           "before the fault); refusing", sh.count, sh.scanned);
    return 0;
  }
  if (sh.count == 0) {
    s_usRefused = true;
    ek_log("updatesector: none of %d functions passes 256 into the 4th argument "
           "and 1536 into the 5th; refusing", sh.scanned);
    return 0;
  }
  if (sh.count != 1) {
    s_usRefused = true;
    ek_log("updatesector: the argument shape matches %d of %d functions, first "
           "at rva 0x%07X; refusing to guess", sh.count, sh.scanned, sh.rva);
    return 0;
  }

  CallWitness cw;
  find_us_callsites(base, hitscanEntry, sh.rva, &cw);
  if (cw.faulted) {
    s_usRefused = true;
    ek_log("updatesector: a code section faulted while checking call sites "
           "(%d site(s), %d agreeing, before the fault); refusing",
           cw.sites, cw.agree);
    return 0;
  }
  if (cw.agree == 0) {
    s_usRefused = true;
    ek_log("updatesector: rva 0x%07X has the argument shape but precedes NO "
           "`call hitscan` (%d site(s), %d with a preceding call, %d of those "
           "elsewhere, first at rva 0x%07X); the two witnesses disagree, "
           "refusing", sh.rva, cw.sites, cw.withPrev, cw.rival, cw.rivalRva);
    return 0;
  }

  ek_log("updatesector: argument shape unique at rva 0x%07X (1 of %d functions), "
         "and it is the call before %d of the %d `call hitscan` site(s) "
         "(%d had a preceding call, %d of those went elsewhere, first rva "
         "0x%07X); self-testing",
         sh.rva, sh.scanned, cw.agree, cw.sites, cw.withPrev, cw.rival,
         cw.rivalRva);
  return sh.rva;
}

// Two calls: inside the player's own sector the value comes back unchanged (a no-op also does); from -1 the engine
// scans every sector and must find the player's (a no-op fails). Which sector is logged, not asserted (inside_p is
// 2D; TROR).
struct UsTest { int kept; int found; int reached; };

static void us_self_test(EkUpdateSectorFn fn, const PlayerSnap* p, UsTest* r) {
  r->reached = 0; r->kept = -32768; r->found = -32768;
  __try {
    short s = (short)p->sect;
    fn(p->x, p->y, &s);
    r->kept = (int)s;
    r->reached = 1;

    short t = -1;
    fn(p->x, p->y, &t);
    r->found = (int)t;
    r->reached = 2;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

bool updatesector_scan(void) {
  if (s_usFn) return true;
  if (s_usRefused) return false;

  if (!s_fn) return false;

  if (!player_ready()) return false;
  PlayerSnap snap;
  if (!player_snap(&snap)) return false;
  if (snap.sect < 0 || snap.sect >= EK_MAXSECTORS) return false;

  if (s_usTests >= EK_US_MAX_TESTS) return false;
  const DWORD now = GetTickCount();
  if (s_usTests > 0 && (DWORD)(now - s_usLastTry) < EK_HITSCAN_RETRY_MS) return false;
  s_usLastTry = now;
  s_usTests++;

  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  if (!base) { s_usRefused = true; ek_log("updatesector: no host module"); return false; }

  if (s_usEntry == 0) {
    s_usEntry = locate_us_once(base, s_entry);
    if (s_usEntry == 0) {
      if (!s_usSaidLost) {
        s_usSaidLost = true;
        ek_log("updatesector: the shot ray keeps the caller's own sector, which "
               "is what it did before -- precision is lost, nothing breaks");
      }
      return false;
    }
  }

  EkUpdateSectorFn const fn = (EkUpdateSectorFn)(void*)(base + s_usEntry);
  UsTest r;
  us_self_test(fn, &snap, &r);

  const bool keptOk = r.reached >= 1 && r.kept == snap.sect;
  const bool foundOk = r.reached >= 2 && r.found >= 0 && r.found < EK_MAXSECTORS;
  if (!keptOk || !foundOk) {
    const char* which =
        r.reached == 0 ? "the first call faulted"
      : r.reached == 1 ? "the call from -1 faulted"
      : !keptOk        ? "the player's own sector did not come back unchanged"
                       : "the call from -1 found no sector, so nothing was written";
    ek_log("updatesector: self-test FAILED at rva 0x%07X -- %s [reached=%d "
           "kept=%d (wanted %d) found=%d] at (%d,%d)",
           s_usEntry, which, r.reached, r.kept, snap.sect, r.found,
           snap.x, snap.y);
    if (s_usTests >= EK_US_MAX_TESTS && !s_usSaidLost) {
      s_usSaidLost = true;
      ek_log("updatesector: giving up; the shot ray keeps the caller's own "
             "sector, which is what it did before -- precision is lost, "
             "nothing breaks");
    }
    return false;
  }

  InterlockedExchange(&s_usRva, (LONG)s_usEntry);
  InterlockedExchangePointer((void* volatile*)&s_usFn, (void*)fn);

  ek_log("updatesector: armed at rva 0x%07X -- self-test passed from (%d,%d) "
         "sector %d: the sector came back unchanged, and from -1 it found "
         "sector %d%s",
         s_usEntry, snap.x, snap.y, snap.sect, r.found,
         r.found == snap.sect ? " (the same one)"
                              : " (a different one -- 2D inside_p with TROR)");
  return true;
}

bool updatesector_ready(void) { return s_usFn != NULL; }

unsigned updatesector_rva(void) { return (unsigned)s_usRva; }

// Counts every ray whose muzzle sector differs from the caller's; always counted, the line rate-limited since the
// aimbot probe casts nine rays a frame.
static const DWORD EK_US_LOG_MIN_MS = 500;
static DWORD s_usLogMs = 0;
static int s_usLogHave = 0;
static long s_usCorrections = 0;

static void us_note_correction(int passedIn, int corrected, int x, int y, int z) {
  s_usCorrections++;
  if (!g_cfg.debuglog) return;
  const DWORD now = GetTickCount();
  if (s_usLogHave && (DWORD)(now - s_usLogMs) < EK_US_LOG_MIN_MS) return;
  s_usLogHave = 1;
  s_usLogMs = now;
  ek_log("hitscan: origin sector corrected %d -> %d at the muzzle (%d,%d,%d) "
         "-- %ld ray(s) corrected so far",
         passedIn, corrected, x, y, z, s_usCorrections);
}

// A fault costs the correction, not the cast: the caller's sector goes through.
static bool us_correct(int x, int y, int sect, int* corrected) {
  EkUpdateSectorFn const us = s_usFn;
  if (!us) return false;
  bool ok = false;
  __try {
    short s = (short)sect;
    us(x, y, &s);
    *corrected = (int)s;
    ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  return ok;
}

// For callers with a position and no sector (the free camera's pose); bounded as ek_hitscan is so a sector the engine
// never produced never indexes sector[].
bool ek_updatesector(int x, int y, int from, int* out) {
  if (!out) return false;
  if (from < -1 || from >= EK_MAXSECTORS) return false;

  int resolved = from;
  if (!us_correct(x, y, from, &resolved)) return false;
  if (resolved < 0 || resolved >= EK_MAXSECTORS) return false;
  *out = resolved;
  return true;
}

bool hitscan_ready(void) { return s_fn != NULL; }

unsigned hitscan_rva(void) { return (unsigned)s_rva; }

// One line per run each, not gated on the debug log: they concern the mod's correctness and cannot fill a file.
static bool s_saidUncleared = false;
static void hitscan_note_uncleared(void) {
  if (s_saidUncleared) return;
  s_saidUncleared = true;
  ek_log("hitscan: the player's own sprite is not confirmed, so the shooter's "
         "blocking bits are NOT being cleared around the cast -- a ray that "
         "starts inside the player can still come back as blocked");
}

// A faulted restore may leave the player's sprite non-blocking and nothing retries; a level load recovers it. Said,
// not worked around.
static bool s_saidUnrestored = false;
static void hitscan_note_unrestored(void) {
  if (s_saidUnrestored) return;
  s_saidUnrestored = true;
  ek_log("hitscan: RESTORING the player's own cstat FAULTED -- the player "
         "sprite may be left non-blocking until the next level load");
}

static bool hitscan_cast(EkHitscanFn fn, int x, int y, int z, int sect,
                         int vx, int vy, int vz, EkHitData* out) {
  // Origin sector recomputed at the muzzle as the engine does (player.cpp:555), here so every caller gets it. Not
  // armed passes `sect` through (ekm_us_origin_sector).
  int corrected = sect;
  const bool haveCorrection = us_correct(x, y, sect, &corrected);
  if (haveCorrection && corrected != sect)
    us_note_correction(sect, corrected, x, y, z);
  const int origin = ekm_us_origin_sector(haveCorrection, corrected, sect,
                                          EK_MAXSECTORS);
  if (origin < 0) return false;

  bool ok = false;
  __try {
    EkVec3 sv = { x, y, z };
    EkHitData h;
    h.x = h.y = h.z = 0;
    h.sprite = h.wall = h.sect = -1;
    const int ret = fn(&sv, (short)origin, vx, vy, vz, &h, (unsigned)EK_CLIPMASK1);
    if (ret == 0) { *out = h; ok = true; }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  return ok;
}

bool ek_hitscan(int x, int y, int z, int sect, int vx, int vy, int vz,
                EkHitData* out) {
  EkHitscanFn const fn = s_fn;
  if (!fn || !out) return false;

  // Bounded here because hitscan does not bound itself: it checks only sectnum < 0, then bitmap_set(clipsectormap,
  // sectnum), so a sector >= MAXSECTORS is an out-of-bounds engine-global write no __except can undo.
  if (sect < 0 || sect >= EK_MAXSECTORS) return false;

  if (vx == 0 && vy == 0 && vz == 0) return false;

  // The shooter's blocking bits are cleared around the cast, before the origin correction, in the engine's order
  // (clear, updatesector, hitscan, restore; hitscan.h). An unconfirmed player sprite casts without clearing rather
  // than write to an unidentified sprite.
  EkShooterCstat shooter = { NULL, 0, 0 };
  if (!player_clear_shooter_cstat(EK_SHOOTER_CSTATMASK, &shooter))
    hitscan_note_uncleared();

  bool ok = false;
  // __finally, not a restore per return path: it runs on early return and on an exception unwinding through this
  // frame.
  __try {
    ok = hitscan_cast(fn, x, y, z, sect, vx, vy, vz, out);
  } __finally {
    if (!player_restore_shooter_cstat(&shooter)) hitscan_note_unrestored();
  }
  return ok;
}
