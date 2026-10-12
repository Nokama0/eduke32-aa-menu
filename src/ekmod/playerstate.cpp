// DukePlayer_t is HEAP-allocated (game.cpp:6433), so no section search finds it. Anchor: player.cpp:5807 copies the
// position into the player sprite every tic. Scan private memory for the sprite index with x/y near it, confirm x/y
// EQUAL the sprite re-read together (z is not matched). Then find the image qword holding it, g_player[0].ps.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "gamestate.h"
#include "playerstate.h"
#include "aimsolve.h"
#include "config.h"
#include "tiles.h"
#include "freecam.h"
#include "mapextent.h"

// s_slot is the one reads prefer; s_ps is the fallback when no slot was found.
static const unsigned char* volatile s_ps = NULL;
static const unsigned char* const volatile* volatile s_slot = NULL;
static volatile LONG s_slotRva = 0;

// Eight full scans, then stop: a failing scan with a live player sprite keeps failing. Counted inside a loaded level
// only (the menu has a placeholder player sprite).
static const int EK_PS_MAX_SCANS = 8;
static int s_heavyScans = 0;
static int s_lastThinLive = -1;

// A local copy of tiles.cpp's collect_live_picnums, count only (that one is file-static).
static int count_live_picnums(void) {
  static const int kCap = 256;
  int seen[kCap];
  int n = 0;
  for (int i = 0; i < EK_MAXSPRITES && n < kCap; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;
    const short st = *(const short*)(r + EK_SPR_STATNUM);
    if (st < 0 || st >= EK_MAXSTATUS) continue;
    if (r[EK_SPR_XREPEAT] == 0 || r[EK_SPR_YREPEAT] == 0) continue;
    const short pic = *(const short*)(r + EK_SPR_PICNUM);
    if (pic <= 0 || pic >= EK_MAXTILES_ARR) continue;
    bool known = false;
    for (int k = 0; k < n; k++) if (seen[k] == pic) { known = true; break; }
    if (!known) seen[n++] = pic;
  }
  return n;
}

struct Region { const unsigned char* base; size_t size; };
static Region s_regions[8192];
static int s_regionCount = 0;
static int s_regionOverflow = 0;

// Committed, private, plainly read-write regions of the whole process; MEM_PRIVATE does most of the filtering (art, GRP
// and DLLs are MAPPED or IMAGE).
static void enumerate_regions(void) {
  s_regionCount = 0;
  s_regionOverflow = 0;

  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const unsigned char* p = (const unsigned char*)si.lpMinimumApplicationAddress;
  const unsigned char* const end = (const unsigned char*)si.lpMaximumApplicationAddress;
  const int cap = (int)(sizeof(s_regions) / sizeof(s_regions[0]));

  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
    if (mbi.RegionSize == 0) break;
    const unsigned char* rbase = (const unsigned char*)mbi.BaseAddress;

    if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
        mbi.Protect == PAGE_READWRITE) {
      Region* last = s_regionCount ? &s_regions[s_regionCount - 1] : NULL;
      if (last && last->base + last->size == rbase) last->size += mbi.RegionSize;
      else if (s_regionCount < cap) {
        s_regions[s_regionCount].base = rbase;
        s_regions[s_regionCount].size = mbi.RegionSize;
        s_regionCount++;
      } else s_regionOverflow++;
    }

    const unsigned char* next = rbase + mbi.RegionSize;
    if (next <= p) break;
    p = next;
  }
}

// Found by picnum and statnum together: game.cpp:2786-2794 sends multiplayer-start APLAYER sprites to STAT_MISC in
// single player.
static int find_player_sprite(int* xOut, int* yOut) {
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;
    if (!ekm_is_player_sprite(*(const short*)(r + EK_SPR_PICNUM),
                              *(const short*)(r + EK_SPR_STATNUM))) continue;
    if (xOut) *xOut = *(const int*)(r + EK_SPR_X);
    if (yOut) *yOut = *(const int*)(r + EK_SPR_Y);
    return i;
  }
  return -1;
}

// Guarded: another thread may free a region between enumeration and read. No C++ objects live here, so the handler has
// nothing to unwind.
static void scan_regions(int px, int py, int spr, EkPsCands* cands, size_t* bytesOut) {
  size_t bytes = 0;

  for (int r = 0; r < s_regionCount; r++) {
    const unsigned char* const base = s_regions[r].base;
    const size_t span = s_regions[r].size;
    if (span < (size_t)EK_PS_SIZE) continue;
    bytes += span;

    __try {
      for (size_t off = 0; off + EK_PS_SIZE <= span; off += 4) {
        const unsigned char* const cand = base + off;
        if (ekm_ps_candidate(cand, spr, px, py)) ekm_ps_cands_add(cands, cand);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }

  if (bytesOut) *bytesOut = bytes;
}

// Phase 2: sprite x/y then the candidate, in one __try (the candidate may since be freed). A sprite no longer the
// player, or an i no longer naming it, refuses.
static bool read_confirm_pair(const unsigned char* cand, int spr,
                              int* cx, int* cy, int* sx, int* sy) {
  const unsigned char* const r = gamestate_sprite(spr);
  if (!r) return false;
  __try {
    if (!ekm_is_player_sprite(*(const short*)(r + EK_SPR_PICNUM),
                              *(const short*)(r + EK_SPR_STATNUM))) return false;
    if (*(const short*)(cand + EK_PS_I) != (short)spr) return false;
    *sx = *(const int*)(r + EK_SPR_X);
    *sy = *(const int*)(r + EK_SPR_Y);
    *cx = *(const int*)(cand + EK_PS_POS + 0);
    *cy = *(const int*)(cand + EK_PS_POS + 4);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

struct PsConfirmRead {
  const EkPsCands* cands;
  int spr;
  bool operator()(int k, int* cx, int* cy, int* sx, int* sy) {
    return read_confirm_pair((const unsigned char*)cands->at[k], spr, cx, cy, sx, sy);
  }
};
struct PsConfirmPause { void operator()() { Sleep(1); } };

// The image qword holding addr, i.e. g_player[0].ps (first member of playerdata_t, player.h:271). STRIDE 1:
// playerdata_t is packed to 133 bytes, so the slot is never 8-aligned. A slot flanked by NULLs at +/- one playerdata_t
// is inside the array; candidates are kept for the ambiguity log.
struct SlotCand {
  unsigned rva;
  char sec[9];
  unsigned char verdict;
  const void* held;
};
static const int SLOT_CAND_MAX = 8;
static SlotCand s_slotCands[SLOT_CAND_MAX];
static int s_slotCandCount = 0;

static const unsigned char* const volatile* find_image_slot(const unsigned char* addr,
                                                            unsigned* rvaOut,
                                                            int* hitsOut, int* shapedOut) {
  s_slotCandCount = 0;
  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = nt->FileHeader.NumberOfSections;

  const unsigned char* firstHit = NULL;  unsigned firstRva = 0;
  const unsigned char* shaped = NULL;    unsigned shapedRva = 0;
  int hits = 0, shapedHits = 0;

  for (int i = 0; i < nsec; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
    const unsigned char* const start = base + sec[i].VirtualAddress;
    const DWORD span = sec[i].Misc.VirtualSize;
    if (span < sizeof(void*)) continue;

    __try {
      for (DWORD off = 0; off + sizeof(void*) <= span; off++) {
        const unsigned char* const at = start + off;
        if (*(const unsigned char* const*)at != addr) continue;
        hits++;
        if (!firstHit) { firstHit = at; firstRva = (unsigned)(sec[i].VirtualAddress + off); }
        SlotCand* const cand = (s_slotCandCount < SLOT_CAND_MAX) ? &s_slotCands[s_slotCandCount++] : NULL;
        if (cand) {
          cand->rva = (unsigned)(sec[i].VirtualAddress + off);
          memcpy(cand->sec, sec[i].Name, 8);
          cand->sec[8] = 0;
          cand->verdict = 1;
          cand->held = *(const void* const*)at;
        }

        if (off < (DWORD)EK_PD_SIZE) continue;
        if ((size_t)off + (size_t)EK_PD_SIZE * (EK_MAXPLAYERS - 1) + sizeof(void*) > span) continue;

        const void* players[EK_MAXPLAYERS];
        for (int k = 0; k < EK_MAXPLAYERS; k++)
          players[k] = *(const void* const*)(at + (size_t)EK_PD_SIZE * k);
        if (!ekm_slot_run_ok(*(const void* const*)(at - EK_PD_SIZE), players,
                             EK_MAXPLAYERS)) { if (cand) cand->verdict = 2; continue; }
        if (cand) cand->verdict = 0;
        shapedHits++;
        if (!shaped) { shaped = at; shapedRva = (unsigned)(sec[i].VirtualAddress + off); }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
  }

  if (hitsOut) *hitsOut = hits;
  if (shapedOut) *shapedOut = shapedHits;

  // Exactly one slot of the right shape is the answer; several means the shape test did not discriminate. None with a
  // single raw hit is still usable: one global holding this heap address almost surely owns it.
  if (shapedHits == 1) { if (rvaOut) *rvaOut = shapedRva; return (const unsigned char* const volatile*)shaped; }
  if (shapedHits == 0 && hits == 1) { if (rvaOut) *rvaOut = firstRva; return (const unsigned char* const volatile*)firstHit; }
  return NULL;
}

bool player_scan(void) {
  if (s_ps) return true;
  if (!gamestate_ready()) return false;

  int px = 0, py = 0;
  const int spr = find_player_sprite(&px, &py);
  if (spr < 0) return false;

  if (px == 0 && py == 0) return false;

  if (s_heavyScans >= EK_PS_MAX_SCANS) return false;

  const int live = count_live_picnums();
  if (!ekm_ps_level_loaded(live)) {
    if (live != s_lastThinLive) {
      s_lastThinLive = live;
      ek_log("player: waiting for a level -- %d live picnum(s), scan budget unspent (%d/%d)",
             live, s_heavyScans, EK_PS_MAX_SCANS);
    }
    return false;
  }
  s_lastThinLive = -1;
  s_heavyScans++;

  const DWORD t0 = GetTickCount();
  enumerate_regions();
  size_t bytes = 0;
  static EkPsCands cands;
  static unsigned char states[EKM_PS_MAX_CANDS];
  cands.count = 0;
  cands.dropped = 0;
  scan_regions(px, py, spr, &cands, &bytes);

  PsConfirmRead read = { &cands, spr };
  PsConfirmPause pause;
  const int confirmed = ekm_ps_confirm_rounds(read, pause, cands.count, states);
  const unsigned char* best = NULL;
  for (int k = 0; k < cands.count && !best; k++)
    if (states[k] == EKM_PS_CONFIRMED) best = (const unsigned char*)cands.at[k];
  const int verdict = ekm_ps_verdict(confirmed, cands.dropped);
  const DWORD ms = GetTickCount() - t0;

  if (!ekm_ps_verdict_usable(verdict) || !best) {
    ek_log("player: scan %d/%d %s -- sprite %d at (%d,%d); %d candidate(s) "
           "in the window (%d more not kept), %d confirmed; "
           "%d region(s)%s covering %llu bytes, %lu ms",
           s_heavyScans, EK_PS_MAX_SCANS,
           verdict == EKM_PS_MANY ? "REFUSED, several confirmed" : "found nothing",
           spr, px, py, cands.count, cands.dropped, confirmed, s_regionCount,
           s_regionOverflow ? " (TABLE FULL)" : "", (unsigned long long)bytes, ms);
    {
      int n = 0, shown = 0;
      char buf[160];
      buf[0] = 0;
      for (int i = 0; i < EK_MAXSPRITES; i++) {
        const unsigned char* r = gamestate_sprite(i);
        if (!r) break;
        if (!ekm_is_player_sprite(*(const short*)(r + EK_SPR_PICNUM),
                                  *(const short*)(r + EK_SPR_STATNUM))) continue;
        n++;
        if (shown < 4) {
          const size_t len = strlen(buf);
          _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, " #%d (%d,%d)", i,
                      *(const int*)(r + EK_SPR_X), *(const int*)(r + EK_SPR_Y));
          shown++;
        }
      }
      ek_log("player:   anchor check -- %d sprite(s) read as the player (APLAYER, "
             "STAT_PLAYER):%s", n, buf);
    }
    if (s_heavyScans >= EK_PS_MAX_SCANS)
      ek_log("player: giving up; nothing in this port will use the player struct");
    return false;
  }

  unsigned slotRva = 0;
  int hits = 0, shapedHits = 0;
  const unsigned char* const volatile* const slot =
      find_image_slot(best, &slotRva, &hits, &shapedHits);

  // Slot published first: player_ready() gates on s_ps. The sprite index is NOT published: a stale copy once refused
  // the rest of the session after a level transition.
  InterlockedExchange(&s_slotRva, (LONG)slotRva);
  InterlockedExchangePointer((void* volatile*)&s_slot, (void*)slot);
  InterlockedExchangePointer((void* volatile*)&s_ps, (void*)best);

  ek_log("player: DukePlayer_t at %p (heap), anchored on sprite %d at (%d,%d); "
         "scan %d/%d, %d candidate(s) (%d more not kept), 1 confirmed%s; "
         "%d region(s), %llu bytes, %lu ms",
         (const void*)best, spr, px, py, s_heavyScans, EK_PS_MAX_SCANS,
         cands.count, cands.dropped,
         verdict == EKM_PS_ONE_UNPROVEN ? " (NOT PROVEN UNIQUE: candidates were dropped)" : "",
         s_regionCount, (unsigned long long)bytes, ms);
  if (slot)
    ek_log("player:   g_player[0].ps at rva 0x%07X -- reads go through it "
           "(%d raw hit(s), %d of the right shape)", slotRva, hits, shapedHits);
  else
    ek_log("player:   no usable image pointer (%d raw hit(s), %d of the right "
           "shape); reading the heap address directly", hits, shapedHits);

  if (hits > 1) {
    static const char* const kVerdict[3] = { "right shape", "run leaves its section",
                                             "shape test failed" };
    for (int k = 0; k < s_slotCandCount; k++)
      ek_log("player:   slot candidate %d/%d: rva 0x%07X in %s, %s, held %p",
             k + 1, hits, s_slotCands[k].rva, s_slotCands[k].sec,
             kVerdict[s_slotCands[k].verdict], s_slotCands[k].held);
    if (hits > s_slotCandCount)
      ek_log("player:   and %d more slot candidate(s) not listed (%d total)",
             hits - s_slotCandCount, hits);
  }

  PlayerSnap s;
  if (player_snap(&s))
    ek_log("player:   ang=%d (%d deg) horiz=%d sect=%d",
           s.ang, ekm_q16ang_degrees(s.ang), s.horiz, s.sect);
  return true;
}

// Prefer the slot: a reallocation survives, and a NULL in it is believed (G_MaybeAllocPlayer only allocates, so null
// means freed). Falling back to the cached address there would hand out a use-after-free.
static const unsigned char* player_ptr(void) {
  const unsigned char* const volatile* const slot = s_slot;
  if (!slot) return s_ps;
  const unsigned char* const p = *slot;
  return (p && ((UINT_PTR)p & 7) == 0) ? p : NULL;
}

// Every live read goes through here, guarded: a heap pointer cannot be bounds-checked like an rva, and game.cpp:5977
// frees g_player[i].ps at shutdown WITHOUT nulling it.
bool player_snap(PlayerSnap* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  __try {
    out->x     = *(const int*)(p + EK_PS_POS + 0);
    out->y     = *(const int*)(p + EK_PS_POS + 4);
    out->z     = *(const int*)(p + EK_PS_POS + 8);
    out->ang   = *(const int*)(p + EK_PS_Q16ANG);
    out->horiz = *(const int*)(p + EK_PS_Q16HORIZ);
    out->horizoff = *(const int*)(p + EK_PS_Q16HORIZOFF);
    out->sect  = *(const short*)(p + EK_PS_CURSECTNUM);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool player_read_draw_mode(int* newowner, int* overShoulderOn) {
  const unsigned char* const p = player_ptr();
  if (!p || !newowner || !overShoulderOn) return false;
  __try {
    *newowner      = (int)*(const short*)(p + EK_PS_NEWOWNER);
    *overShoulderOn = (int)*(const unsigned char*)(p + EK_PS_OVER_SHOULDER_ON);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// One byte: MODE_MENU is bit 0 (player.h:120), right under either layout and touches nothing past the struct.
bool player_read_gm(int* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  __try {
    *out = (int)*(const unsigned char*)(p + EK_PS_GM);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool player_read_par(int* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  __try {
    *out = *(const int*)(p + EK_PS_PLAYER_PAR);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// Written only here on the game thread, interlocked so any clock reads a whole value.
static volatile LONG s_levelEpoch = 0;
static EkLevelEpochState s_epochState = { 0, 0, 0, 0 };

void level_epoch_tick(void) {
  int gm = 0, par = 0;
  const bool gmOk = player_read_gm(&gm);
  const bool gotPar = player_read_par(&par);
  const int prevPar = s_epochState.prevPar;
  const int trig = ekm_level_epoch_step(&s_epochState, gmOk, gm, gotPar, par);
  if (trig == EKM_EPOCH_NONE) return;
  const LONG now = InterlockedIncrement(&s_levelEpoch);
  if (g_cfg.debuglog)
    ek_log("level epoch: %ld (trigger=%s, gm %d, par %d -> %d)", (long)now,
           trig == EKM_EPOCH_GM0 ? "gm0" : "par-drop", gm, prevPar, par);
}

long level_epoch(void) { return (long)InterlockedCompareExchange(&s_levelEpoch, 0, 0); }

bool player_read_weapon(int* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  __try {
    *out = (int)*(const signed char*)(p + EK_PS_CURR_WEAPON);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// The one write, and nothing new: P_UpdateAngles writes q16ang and q16horiz every frame (player.cpp:3080-3084). Guarded
// because the struct is heap memory freed without nulling (game.cpp:5977).
bool player_write_view(int q16ang, int q16horiz, bool alsoOld) {
  unsigned char* const p = (unsigned char*)player_ptr();
  if (!p) return false;
  __try {
    *(int*)(p + EK_PS_Q16ANG)   = q16ang;
    *(int*)(p + EK_PS_Q16HORIZ) = q16horiz;
    if (alsoOld) {
      *(int*)(p + EK_PS_OQ16ANG)   = q16ang;
      *(int*)(p + EK_PS_OQ16HORIZ) = q16horiz;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// kickback_pic is UNSIGNED (uint8_t, player.h:241), weapon_pos SIGNED (int8_t, :257): a signed kickback read would turn
// a frame past 127 negative. Read each as the engine declares it.
bool player_read_weapon_anim(int* weapon, int* kickback, int* weaponPos) {
  const unsigned char* const p = player_ptr();
  if (!p || !weapon || !kickback || !weaponPos) return false;
  __try {
    *weapon    = (int)*(const signed char*)(p + EK_PS_CURR_WEAPON);
    *kickback  = (int)*(const unsigned char*)(p + EK_PS_KICKBACK);
    *weaponPos = (int)*(const signed char*)(p + EK_PS_WEAPON_POS);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// Read only: ammo_amount[weapon], int16_t signed (player.h:220), bounds-checked against the array length before the
// offset arithmetic.
bool player_read_ammo(int weapon, int* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  if (weapon < 0 || weapon >= EK_PS_MAX_WEAPONS) return false;
  __try {
    *out = (int)*(const short*)(p + EK_PS_AMMO_AMOUNT_BASE + weapon * 2);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool player_read_reloading(int* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  __try {
    *out = (int)*(const unsigned char*)(p + EK_PS_RELOADING);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// Written back at the width the engine declares; P_ProcessWeapon steps both every tic, and weaponspeed.h decides the
// value.
bool player_write_kickback(int v) {
  unsigned char* const p = (unsigned char*)player_ptr();
  if (!p) return false;
  __try {
    *(unsigned char*)(p + EK_PS_KICKBACK) = (unsigned char)v;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool player_write_weapon_pos(int v) {
  unsigned char* const p = (unsigned char*)player_ptr();
  if (!p) return false;
  __try {
    *(signed char*)(p + EK_PS_WEAPON_POS) = (signed char)v;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool player_read_pyoff(int* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  __try {
    *out = (int)*(const short*)(p + EK_PS_PYOFF);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// auto_aim is uint8_t (player.h:240), read-only: silent aim writes it for one shot, and A_FindTargetSprite
// (player.cpp:221-222), not GetAutoAimAng, returns -1 only at 0.
bool player_read_auto_aim(int* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  __try {
    *out = (int)*(const unsigned char*)(p + EK_PS_AUTO_AIM);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// got_access is int16_t (player.h:225), masked to bits 0..2 (sector.cpp:1315-1332). Fill a local, write *out only on
// success.
bool player_read_got_access(int* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  int got = 0;
  __try {
    got = ((int)*(const short*)(p + EK_PS_GOT_ACCESS)) & 0x07;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  *out = got;
  return true;
}

bool player_read_take_span(PlayerTakeSpan* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  PlayerTakeSpan s;
  __try {
    s.maxhp = *(const int*)(p + EK_PS_MAX_PLAYER_HEALTH);
    for (int i = 0; i < EK_PS_MAX_WEAPONS; i++) {
      s.maxAmmo[i] = (int)*(const short*)(p + EK_PS_MAX_AMMO_BASE + i * 2);
      s.ammo[i]    = (int)*(const short*)(p + EK_PS_AMMO_AMOUNT_BASE + i * 2);
    }
    for (int i = 0; i < EK_PS_GET_MAX; i++)
      s.inv[i] = (int)*(const short*)(p + EK_PS_INV_AMOUNT_BASE + i * 2);
    s.gotAccess = (int)*(const short*)(p + EK_PS_GOT_ACCESS);
    s.gotWeapon = (int)*(const unsigned short*)(p + EK_PS_GOTWEAPON);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  *out = s;
  return true;
}

// opos is a vec3_t right after pos (player.h:185). All three or none: the caller must fall back to no interpolation,
// not two thirds of a position.
bool player_read_opos(int* x, int* y, int* z) {
  const unsigned char* const p = player_ptr();
  if (!p || !x || !y || !z) return false;
  __try {
    *x = *(const int*)(p + EK_PS_OPOS + 0);
    *y = *(const int*)(p + EK_PS_OPOS + 4);
    *z = *(const int*)(p + EK_PS_OPOS + 8);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// ps->i, read FRESH: it changes when the player sprite respawns at a level transition, so a cache stays wrong. The
// struct itself never moves (game.cpp:6433).
static bool player_read_sprite_index(int* out) {
  const unsigned char* const p = player_ptr();
  if (!p || !out) return false;
  __try {
    *out = (int)*(const short*)(p + EK_PS_I);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool player_sprite_index(int* out) { return player_read_sprite_index(out); }

static bool sprite_reads_as_player(int spriteNum) {
  const unsigned char* const r = gamestate_sprite(spriteNum);
  if (!r) return false;
  bool ok = false;
  __try {
    ok = ekm_is_player_sprite(*(const short*)(r + EK_SPR_PICNUM),
                              *(const short*)(r + EK_SPR_STATNUM));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  return ok;
}

bool player_sprite_matches(int spriteNum) {
  int cur = 0;
  if (!player_read_sprite_index(&cur)) return false;
  if (cur != spriteNum) return false;
  // The second answer is not belt and braces: if the struct were the wrong one, this stops a solved yaw landing in some
  // actor's sprite.
  return sprite_reads_as_player(spriteNum);
}

// The player's sprite record, writable, or NULL. The index comes from ps->i and is confirmed, never the latch: a stale
// index would put the angle into whatever actor now owns the slot. The const is dropped in exactly this one place.
static unsigned char* player_sprite_rec(void) {
  int spr = 0;
  if (!player_read_sprite_index(&spr)) return NULL;
  if (!sprite_reads_as_player(spr)) return NULL;
  return (unsigned char*)gamestate_sprite(spr);
}

// The engine keeps neither health nor "frozen" in DukePlayer_t; it tests the player's SPRITE for both. Goes through
// player_sprite_rec so it uses the fresh ps->i.
bool player_read_body(int* hp, int* pal) {
  const unsigned char* const r = player_sprite_rec();
  if (!r) return false;
  __try {
    if (hp)  *hp  = (int)*(const short*)(r + EK_SPR_EXTRA);
    if (pal) *pal = (int)r[EK_SPR_PAL];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// Silent aim substitution: every field is one the engine writes each tic, and it is put back in the same engine call
// (playerstate.h). Save first, before any write.
bool player_write_shot_view(int q16ang, int q16horiz, int autoAim, EkShotSave* save) {
  unsigned char* const p = (unsigned char*)player_ptr();
  if (!p || !save) return false;
  save->sprOk = 0;
  save->sprAng = 0;

  __try {
    save->q16ang    = *(const int*)(p + EK_PS_Q16ANG);
    save->oq16ang   = *(const int*)(p + EK_PS_OQ16ANG);
    save->q16horiz  = *(const int*)(p + EK_PS_Q16HORIZ);
    save->oq16horiz = *(const int*)(p + EK_PS_OQ16HORIZ);
    save->autoAim   = (int)*(const unsigned char*)(p + EK_PS_AUTO_AIM);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }

  unsigned char* const s = player_sprite_rec();
  if (s) {
    __try {
      save->sprAng = (int)*(const short*)(s + EK_SPR_ANG);
      save->sprOk = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      save->sprOk = 0;
    }
  }

  // A fault part-way leaves the struct half substituted, so undo here rather than leaving it to a caller told the write
  // failed.
  __try {
    *(int*)(p + EK_PS_Q16ANG)    = q16ang;
    *(int*)(p + EK_PS_OQ16ANG)   = q16ang;
    *(int*)(p + EK_PS_Q16HORIZ)  = q16horiz;
    *(int*)(p + EK_PS_OQ16HORIZ) = q16horiz;
    *(unsigned char*)(p + EK_PS_AUTO_AIM) = (unsigned char)autoAim;
    if (save->sprOk && s)
      *(short*)(s + EK_SPR_ANG) = (short)((q16ang >> 16) & (EKM_YAW_UNITS - 1));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    player_restore_shot_view(save);
    return false;
  }
  return true;
}

// Called on EVERY path out of a substituted shot, the faulting one included: a player left aiming where they never
// looked is worse than no feature.
bool player_restore_shot_view(const EkShotSave* save) {
  unsigned char* const p = (unsigned char*)player_ptr();
  if (!p || !save) return false;
  int ok = 1;

  __try {
    *(int*)(p + EK_PS_Q16ANG)    = save->q16ang;
    *(int*)(p + EK_PS_OQ16ANG)   = save->oq16ang;
    *(int*)(p + EK_PS_Q16HORIZ)  = save->q16horiz;
    *(int*)(p + EK_PS_OQ16HORIZ) = save->oq16horiz;
    *(unsigned char*)(p + EK_PS_AUTO_AIM) = (unsigned char)save->autoAim;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = 0;
  }

  if (save->sprOk) {
    unsigned char* const s = player_sprite_rec();
    if (!s) ok = 0;
    else {
      __try {
        *(short*)(s + EK_SPR_ANG) = (short)save->sprAng;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
      }
    }
  }
  return ok != 0;
}

// The shooter's own blocking bits (contract: playerstate.h; why the write is allowed: hitscan.h). The save is written
// BEFORE the store: the safe error is a restore with nothing to do, never a restore that does not know it owes a write.
bool player_clear_shooter_cstat(int mask, EkShooterCstat* save) {
  if (!save) return false;
  save->rec = NULL;
  save->saved = 0;
  save->held = 0;

  unsigned char* const r = player_sprite_rec();
  if (!r) return false;

  __try {
    const int cur = (int)*(const unsigned short*)(r + EK_SPR_CSTAT);
    const int cleared = cur & ~mask;
    if (cleared == cur) return true;
    save->rec = r;
    save->saved = cur;
    save->held = 1;
    *(unsigned short*)(r + EK_SPR_CSTAT) = (unsigned short)cleared;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // A faulted READ leaves nothing to put back; a faulted store is put back anyway.
    if (!save->held) { save->rec = NULL; save->saved = 0; }
    return false;
  }
  return true;
}

bool player_restore_shooter_cstat(const EkShooterCstat* save) {
  if (!save || !save->held || !save->rec) return true;
  int ok = 1;
  // Through the saved pointer, not a second lookup: a lookup that answered differently would leave the player
  // permanently non-blocking.
  __try {
    *(unsigned short*)(save->rec + EK_SPR_CSTAT) = (unsigned short)save->saved;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = 0;
  }
  return ok != 0;
}

// Refuses before touching anything and writes all ten fields or none: a half-written pose is worse than a refused one.
bool player_write_pos(const EkTeleportPlan& plan) {
  if (!plan.ok) return false;
  if (plan.sect < 0 || plan.sect >= EK_MAXSECTORS) return false;
  // Also against the live map: player.cpp:5038 rescues only cursectnum == -1, so an index past this map but inside 4096
  // slots is trusted by P_ClampZ and the renderer.
  const int live = mapextent_numsectors();
  if (live > 0 && plan.sect >= live) return false;

  unsigned char* const p = (unsigned char*)player_ptr();
  if (!p) return false;

  __try {
    int* const pos    = (int*)(p + EK_PS_POS);
    int* const opos   = (int*)(p + EK_PS_OPOS);
    int* const bobpos = (int*)(p + EK_PS_BOBPOS);
    pos[0]  = plan.x; pos[1]  = plan.y; pos[2]  = plan.z;
    opos[0] = plan.x; opos[1] = plan.y; opos[2] = plan.z;
    // The bob origin too: the next tic measures xvel as the distance from bobpos to pos (player.cpp:5308), so leaving
    // it reports the teleport as one tic of running and jolts view bob and weapon sway.
    bobpos[0] = plan.x; bobpos[1] = plan.y;
    *(short*)(p + EK_PS_CURSECTNUM) = (short)plan.sect;
    *(int*)(p + EK_PS_VELZ) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool player_ready(void) { return s_ps != NULL; }
unsigned player_rva(void) { return (unsigned)InterlockedCompareExchange(&s_slotRva, 0, 0); }
bool player_q16ang(int* out) {
  PlayerSnap s;
  if (!out || !player_snap(&s)) return false;
  *out = s.ang;
  return true;
}

bool player_q16horiz(int* out) {
  PlayerSnap s;
  if (!out || !player_snap(&s)) return false;
  *out = s.horiz;
  return true;
}

bool player_pos(int* x, int* y, int* z) {
  PlayerSnap s;
  if (!player_snap(&s)) return false;
  if (x) *x = s.x;
  if (y) *y = s.y;
  if (z) *z = s.z;
  return true;
}

// Live sampling for the Debug log toggle, only when something moved: the number following the player turn proves the
// struct is the right one.
void player_log_tick(void) {
  if (!g_cfg.debuglog) return;
  PlayerSnap s;
  if (!player_snap(&s)) return;

  static int lastAng = 0, lastHoriz = 0, lastSect = -2;
  if (s.ang == lastAng && s.horiz == lastHoriz && s.sect == lastSect) return;
  lastAng = s.ang; lastHoriz = s.horiz; lastSect = s.sect;

  ek_log("player: ang=%d (%d deg) horiz=%d sect=%d",
         s.ang, ekm_q16ang_degrees(s.ang), s.horiz, s.sect);
}
