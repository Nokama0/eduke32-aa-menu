// Finding the engine's sprite array without an offset (sprite is a POINTER, build.h:619-621). A range-only validator
// accepts zeroed .bss, so array_looks_real demands the free-slot marker; VirtualQuery per slot hung the game. This scan
// only witnesses camera.cpp's decode (docs/re/gamestate.md).

#include <windows.h>
#include <string.h>
#include "log.h"
#include "gamestate.h"
#include "tiles.h"
#include "badguy.h"
#include "tilenames.h"
#include "gamevars.h"
#include "gamearrays.h"
#include "weapondata.h"
#include "playerstate.h"
#include "actorstate.h"
#include "wallstate.h"
#include "sectorstate.h"
#include "mapextent.h"
#include "maskscan.h"
#include "modelbounds.h"
#include "secretvol.h"
#include "secrettrig.h"
#include "secretpath.h"
#include "nocull.h"
#include "cutaway.h"
#include "storeyflush.h"

static const unsigned char* s_base = NULL;
static size_t s_size = 0;

// Written by the worker, read by the render thread; an aligned pointer store is atomic on x64, so the reader sees NULL
// or a fully validated array.
static const unsigned char* volatile s_sprites = NULL;
static volatile LONG s_spritePtrRva = 0;
static volatile LONG s_workerState = 0;

// Published once by camera.cpp; see gamestate.h for why a decoded instruction outranks a content match.
static volatile LONG s_authRva = 0;
static volatile LONG s_authState = 0;

void gamestate_sprite_authority(unsigned spritePtrRva) {
  InterlockedExchange(&s_authRva, (LONG)spritePtrRva);
  InterlockedExchange(&s_authState,
                      spritePtrRva ? EK_SPR_AUTH_IMAGE : EK_SPR_AUTH_REFUSED);
}

// Committed region cache: the scan makes NO system calls (fine a hundred times, fatal a million).
struct Region { const unsigned char* base; size_t size; };
static Region s_regions[1024];
static int s_regionCount = 0;
static size_t s_regionBytes = 0;

// Adjacent regions are merged: an unmerged table filled its 128 slots and hid everything past them, and in_region()
// needs the whole 720KB array in ONE entry although .bss comes back in pieces with differing protections.
static void enumerate_regions(const unsigned char* start, size_t len) {
  s_regionCount = 0;
  s_regionBytes = 0;
  const unsigned char* p = start;
  const unsigned char* const end = start + len;
  const int cap = (int)(sizeof(s_regions) / sizeof(s_regions[0]));
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
    if (mbi.RegionSize == 0) break;
    const unsigned char* rbase = (const unsigned char*)mbi.BaseAddress;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      Region* last = s_regionCount ? &s_regions[s_regionCount - 1] : NULL;
      if (last && last->base + last->size == rbase) {
        last->size += mbi.RegionSize;
      } else if (s_regionCount < cap) {
        s_regions[s_regionCount].base = rbase;
        s_regions[s_regionCount].size = mbi.RegionSize;
        s_regionCount++;
      }
      s_regionBytes += mbi.RegionSize;
    }
    p = rbase + mbi.RegionSize;
  }
}

void gamestate_regions_refresh(void) {
  if (s_base && s_size) enumerate_regions(s_base, s_size);
}
int gamestate_region_count(void) { return s_regionCount; }
bool gamestate_region_at(int i, const unsigned char** base, size_t* bytes) {
  if (i < 0 || i >= s_regionCount || !base || !bytes) return false;
  *base = s_regions[i].base;
  *bytes = s_regions[i].size;
  return true;
}
const unsigned char* gamestate_module_base(void) { return s_base; }
size_t gamestate_module_size(void) { return s_size; }

// Shared with wallstate.cpp; same thread and sequence (see the header).
bool gamestate_region_contains(const void* p, size_t bytes) {
  const unsigned char* b = (const unsigned char*)p;
  for (int i = 0; i < s_regionCount; i++)
    if (b >= s_regions[i].base && b + bytes <= s_regions[i].base + s_regions[i].size)
      return true;
  return false;
}

static bool in_region(const void* p, size_t bytes) {
  const unsigned char* b = (const unsigned char*)p;
  for (int i = 0; i < s_regionCount; i++)
    if (b >= s_regions[i].base && b + bytes <= s_regions[i].base + s_regions[i].size)
      return true;
  return false;
}

// MAXSECTORS is a legal stored value: initspritelists and deletesprite park free slots at sectnum = MAXSECTORS
// (engine.cpp:9380, :8838), so an exclusive bound rejected the real array while zeros passed. Inclusive at both ends of
// what the engine stores.
static bool record_plausible(const unsigned char* r) {
  const short sectnum = *(const short*)(r + EK_SPR_SECTNUM);
  const short statnum = *(const short*)(r + EK_SPR_STATNUM);
  const short picnum  = *(const short*)(r + EK_SPR_PICNUM);
  if (sectnum < -1 || sectnum > EK_MAXSECTORS) return false;
  if (statnum < 0 || statnum > EK_MAXSTATUS) return false;
  if (picnum < 0 || picnum >= EK_MAXTILES) return false;
  return true;
}

struct ArrayScore { int sampled, plausible, freeSlots, nonZero; };

// Sampled across the whole array: allocation comes from the head of the free list, so a front sample sees almost no
// free slots.
static void score_array(const unsigned char* arr, ArrayScore* out) {
  out->sampled = out->plausible = out->freeSlots = out->nonZero = 0;
  const int step = EK_MAXSPRITES / EK_SPRITE_SCAN_DEPTH;
  for (int i = 0; i < EK_MAXSPRITES; i += step) {
    const unsigned char* r = arr + (size_t)i * EK_SPR_STRIDE;
    out->sampled++;
    if (!record_plausible(r)) continue;
    out->plausible++;
    if (*(const short*)(r + EK_SPR_STATNUM) == EK_MAXSTATUS) out->freeSlots++;
    for (int b = 0; b < EK_SPR_STRIDE; b++)
      if (r[b]) { out->nonZero++; break; }
  }
}

// Asks for evidence, not absence of contradiction: engine.cpp:8851 sets statnum = MAXSTATUS on every freed sprite, so a
// real array carries thousands at 1024; zeroed memory carries none.
static bool array_looks_real(const ArrayScore& s) {
  if (s.plausible < (s.sampled * 15) / 16) return false;
  if (s.freeSlots < s.sampled / 8) return false;
  if (s.nonZero  < s.sampled / 8) return false;
  return true;
}

// The slot camera.cpp named holds the array. It gets every test a candidate does: an array that does not read as
// sprites means no level is loaded.
static const unsigned char* image_walk_array(unsigned slotRva, ArrayScore* out) {
  out->sampled = out->plausible = out->freeSlots = out->nonZero = 0;
  if (!s_base || !slotRva) return NULL;
  if ((size_t)slotRva + sizeof(void*) > s_size) return NULL;
  const unsigned char* const slot = s_base + slotRva;
  if (!in_region(slot, sizeof(void*))) return NULL;
  const unsigned char* const cand = *(const unsigned char* const*)slot;
  if (cand < s_base || cand >= s_base + s_size) return NULL;
  if (((UINT_PTR)cand & 3u) != 0) return NULL;
  if (!in_region(cand, (size_t)EK_MAXSPRITES * EK_SPR_STRIDE)) return NULL;
  score_array(cand, out);
  return array_looks_real(*out) ? cand : NULL;
}

struct ScanResult {
  const unsigned char* best;
  unsigned bestRva;
  int candidates;
  ArrayScore score;
  ArrayScore bestSeen;
  unsigned bestSeenRva;
};

static void content_scan(ScanResult* r) {
  r->best = NULL; r->bestRva = 0; r->candidates = 0; r->bestSeenRva = 0;
  r->score.sampled = r->score.plausible = 0;
  r->score.freeSlots = r->score.nonZero = 0;
  r->bestSeen = r->score;
  const size_t arrayBytes = (size_t)EK_MAXSPRITES * EK_SPR_STRIDE;
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)s_base;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(s_base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = nt->FileHeader.NumberOfSections;

  // The ranking is a moving number (the game creates and deletes sprites during the walk), which is why this search no
  // longer decides.
  const unsigned char* best = NULL;
  unsigned bestRva = 0;
  int bestScore = 0, candidates = 0;

  // Near misses, so a failure says WHICH test refused (region table never reached the array, or the validator said no).
  ArrayScore bestSeen = {0, 0, 0, 0};
  unsigned bestSeenRva = 0;

  for (int i = 0; i < nsec; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
    const unsigned char* start = s_base + sec[i].VirtualAddress;
    const DWORD secSpan = sec[i].Misc.VirtualSize;

    for (DWORD off = 0; off + 8 <= secSpan; off += 8) {
      const unsigned char* slot = start + off;
      if (!in_region(slot, 8)) continue;

      const unsigned char* cand = *(const unsigned char* const*)slot;
      if (cand < s_base || cand >= s_base + s_size) continue;
      if (((UINT_PTR)cand & 3u) != 0) continue;
      if (!in_region(cand, arrayBytes)) continue;

      candidates++;
      ArrayScore s;
      score_array(cand, &s);
      if (s.plausible > bestSeen.plausible) {
        bestSeen = s;
        bestSeenRva = (unsigned)(sec[i].VirtualAddress + off);
      }
      if (!array_looks_real(s)) continue;

      // Among real-looking candidates, the one with the most free-list markers.
      if (s.freeSlots > bestScore) {
        bestScore = s.freeSlots;
        best = cand;
        bestRva = (unsigned)(sec[i].VirtualAddress + off);
      }
    }
  }

  r->best = best;
  r->bestRva = bestRva;
  r->candidates = candidates;
  r->bestSeen = bestSeen;
  r->bestSeenRva = bestSeenRva;
  if (best) score_array(best, &r->score);
}

// Both on one pass, so the cross-check uses one view of memory. True having published an array.
static bool locate_now(bool waitedOut) {
  const int authState = (int)InterlockedCompareExchange(&s_authState, 0, 0);
  const unsigned authSlot = (unsigned)InterlockedCompareExchange(&s_authRva, 0, 0);

  ArrayScore imgScore;
  imgScore.sampled = imgScore.plausible = 0;
  imgScore.freeSlots = imgScore.nonZero = 0;
  const unsigned char* const fromImage =
      (authState == EK_SPR_AUTH_IMAGE) ? image_walk_array(authSlot, &imgScore)
                                       : NULL;

  ScanResult sc;
  content_scan(&sc);

  const int src = ekm_sprite_source_pick(authState, fromImage != NULL,
                                         sc.best != NULL, waitedOut);
  if (src == EK_SPR_SRC_IMAGE) {
    InterlockedExchange(&s_spritePtrRva, (LONG)authSlot);
    s_sprites = fromImage;
    ek_log("gamestate: sprite[] at array rva 0x%07X, from the engine's own "
           "sprite load -- the `sprite` pointer at rva 0x%07X, decoded inside "
           "the inlined polymer_scansprites. sampled %d, plausible %d, free "
           "slots %d, non-zero %d",
           (unsigned)(fromImage - s_base), authSlot, imgScore.sampled,
           imgScore.plausible, imgScore.freeSlots, imgScore.nonZero);
    if (!sc.best)
      ek_log("gamestate:   the content scan has no answer to witness it with on "
             "this pass; %d candidate(s) examined", sc.candidates);
    else if (sc.best == fromImage)
      ek_log("gamestate:   and the content scan agrees on the array, reading it "
             "out of the slot at rva 0x%07X (%d candidate(s) examined). The two "
             "searches may name different slots and often do; the array is what "
             "they have to agree on",
             sc.bestRva, sc.candidates);
    else
      ek_log("gamestate:   BUT THE CONTENT SCAN DISAGREES: its slot at rva "
             "0x%07X names array rva 0x%07X, not 0x%07X. The decoded "
             "instruction wins and nothing reads the scan's answer; the ESP, "
             "the triggerbot and the aim are unaffected",
             sc.bestRva, (unsigned)(sc.best - s_base),
             (unsigned)(fromImage - s_base));
    return true;
  }

  if (src == EK_SPR_SRC_SCAN) {
    InterlockedExchange(&s_spritePtrRva, (LONG)sc.bestRva);
    s_sprites = sc.best;
    ek_log("gamestate: sprite[] at array rva 0x%07X, from the content scan "
           "alone and UNWITNESSED -- the image walk %s. Read out of the slot at "
           "rva 0x%07X; %d candidate array(s) examined; sampled %d, plausible "
           "%d, free slots %d, non-zero %d",
           (unsigned)(sc.best - s_base),
           authState == EK_SPR_AUTH_REFUSED ? "refused" : "never answered",
           sc.bestRva, sc.candidates, sc.score.sampled, sc.score.plausible,
           sc.score.freeSlots, sc.score.nonZero);
    return true;
  }

  if (authState == EK_SPR_AUTH_IMAGE)
    ek_log("gamestate: the `sprite` pointer at rva 0x%07X holds nothing that "
           "reads as sprites yet, which is what an unloaded level looks like; "
           "%d content-scan candidate(s) examined meanwhile",
           authSlot, sc.candidates);
  else
    ek_log("gamestate: %d candidate(s) examined, none accepted. closest was "
           "rva 0x%07X with sampled %d, plausible %d, free slots %d, non-zero %d",
           sc.candidates, sc.bestSeenRva, sc.bestSeen.sampled,
           sc.bestSeen.plausible, sc.bestSeen.freeSlots, sc.bestSeen.nonZero);
  return false;
}

// The five the locate phase waits for. mapextent_ready() is NOT in it: including it ran the whole 900 attempts in the
// never-found case.
static bool locate_required_ready(void) {
  return player_ready() && actorstate_ready() && actorstate_script_ready() &&
         wallstate_ready() && sectorstate_ready();
}

static void locate_pass(void) {
  // g_tile first: actorstate_script_scan refuses without it (actorstate.cpp:324) and apScript needs it.
  if (!badguy_ready())             badguy_scan();
  // Reads AA's names.h once, with its own spent-budget state.
  if (!tilenames_ready())          tilenames_scan();
  // The gamevar registry walks every writable section and fills while the CON parses. NOT in locate_required_ready:
  // stock Duke has no `team`, and must not hold the budget.
  if (!gamevars_ready())           gamevars_scan();
  if (!player_ready())             player_scan();
  if (!actorstate_ready())         actorstate_scan();
  if (!actorstate_script_ready())  actorstate_script_scan();
  // wall[] needs nothing from the others; without it the triggerbot reads every wall hit as blocked.
  if (!wallstate_ready())          wallstate_scan();
  // sector[] refuses until wall[] is up (its validator is wall[]'s geometry, sectorstate.h). The 900-attempt budget (30
  // minutes) is permanent and nothing detects a level change.
  if (!sectorstate_ready())        sectorstate_scan();
  // The two model locates need nothing here. Not in locate_required_ready, like mapextent: a machine with no HRP would
  // burn the whole budget.
  if (!modelbounds_ready())        modelbounds_scan();
  // tilesiz needs live picnums from a loaded map, so it can be refused all session; not in locate_required_ready.
  if (!tiles_ready())              tiles_scan();
  // After tiles: aGameArrays proves itself by tilesizx.pValues == &tilesiz[0].x and defers uncharged until then. Not in
  // locate_required_ready: stock Duke has none of AA's arrays.
  if (!gamearrays_ready())         gamearrays_scan();
}

// Off the render thread: a scan's cost must not land on the frame. It retries because at the main menu there is
// correctly nothing to find.
static DWORD WINAPI scan_thread(LPVOID) {
  for (int attempt = 1; attempt <= 900 && !s_sprites; attempt++) {
    enumerate_regions(s_base, s_size);
    if (locate_now(attempt > EK_SPRITE_AUTHORITY_WAIT)) break;
    if (attempt == 1)
      ek_log("gamestate: no sprite array yet -- %d merged region(s) covering "
             "%llu bytes; retrying every 2s while you play",
             s_regionCount, (unsigned long long)s_regionBytes);
    Sleep(2000);
  }
  if (!s_sprites) {
    ek_log("gamestate: gave up looking for the sprite array");
    InterlockedExchange(&s_workerState, 2);
    return 0;
  }

  // tilesiz and badguy used to sit here in their own loops, and one refusing scan (tilesiz on an AA map) disabled ten
  // subsystems for the session. Both now join the shared tick and sampler; neither is in locate_required_ready, since
  // neither is guaranteed to succeed.

  int locateTick = 0, locateAttempts = 0;
  bool locating = true;

  // A 1 Hz sampler for the Debug log. mapextent_scan runs unconditionally (guarding it would freeze the first map's
  // numbers). secretvol_scan and secrettrig_scan ride it because their answers change on map load and when a secret is
  // found; secretpath_scan is the heaviest (map walks plus a BFS over 8192 edges) and runs last.
  for (;;) {
    // The locate pass rides this tick, first, and nothing waits behind it: a separate loop kept weapondata_tick (the
    // fire rate's only producer) from running when wall[] refused.
    if (locating) {
      if (ekm_locate_pass_due(locateTick, locateAttempts,
                              locate_required_ready())) {
        locate_pass();
        locateAttempts++;
      }
      locateTick++;
      locating = ekm_locate_phase_active(locateAttempts, locate_required_ready());
      // 2 means the bounded locate work is over; the only reader is gamestate_scan's start-once CAS.
      if (!locating) InterlockedExchange(&s_workerState, 2);
    }
    player_log_tick();
    mapextent_scan();
    maskscan_probe();
    // Needs camera_scan to have published the image base; a non-fault refusal latches at once, a fault gets a few
    // attempts. Reads only.
    nocull_locate();
    // Runs AFTER nocull_locate: two of its log lines corroborate against nocull's extents. Reads the image, hooks
    // nothing, writes no engine memory.
    cutaway_locate();
    // After cutaway_locate, whose g_nodraw store it reads beside; waits for that and camera's yax locate.
    storeyflush_locate();
    // The footprint cache is built at the seed scansector seam: an array that grows by realloc cannot cross threads
    // (footprint.h).
    if (!locating) {
      modelbounds_scan();
      // Both answer once. The budget latch inside each scan (EK_TILE_SCAN_BUDGET), not this guard, bounds the cost on a
      // refusing map.
      if (!tiles_ready())  tiles_scan();
      if (!badguy_ready()) badguy_scan();
      // One-shot file read, bounded like the two above.
      if (!tilenames_ready()) tilenames_scan();
      // Bounded by its own latch: aGameVars is one table for the process.
      if (!gamevars_ready()) gamevars_scan();
      // Bounded by its own latch; its deferral keeps it cheap until tilesiz answers.
      if (!gamearrays_ready()) gamearrays_scan();
    }
    // The two scans it needs may answer on different ticks; it latches after its one log.
    tilenames_maybe_log_inventory();
    // AA rewrites WEAPONx_* from CON on a character switch and an upgrade; reprints only a weapon whose fields moved.
    // Feeds firestops.h, so the Debug log row alone does not gate it.
    weapondata_tick();
    secretvol_scan();
    secrettrig_scan();
    secretpath_scan();
    // The route's cheap half runs at 8 Hz (secretpath_refresh re-solves from the staged graph); sleeps still total 1000
    // ms.
    for (int i = 0; i < EK_SPATH_SUBTICKS; i++) {
      Sleep(EK_SPATH_SUBTICK_MS);
      secretpath_refresh();
    }
    Sleep(EK_SPATH_SUBTICK_MS);
  }

}

bool gamestate_scan(void) {
  if (s_sprites) return true;
  // Starts the worker once; later calls are cheap no-ops.
  if (InterlockedCompareExchange(&s_workerState, 1, 0) != 0) return false;

  s_base = (const unsigned char*)GetModuleHandleA(NULL);
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)s_base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    ek_log("gamestate: host is not a DOS image");
    InterlockedExchange(&s_workerState, 2); return false;
  }
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(s_base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    ek_log("gamestate: host is not a PE image");
    InterlockedExchange(&s_workerState, 2); return false;
  }
  s_size = nt->OptionalHeader.SizeOfImage;

  const HANDLE h = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
  if (!h) {
    ek_log("gamestate: CreateThread failed (%lu); the scan will not run", GetLastError());
    InterlockedExchange(&s_workerState, 2); return false;
  }
  CloseHandle(h);
  ek_log("gamestate: scanning for the sprite array on a worker thread");
  return false;
}

bool gamestate_ready(void) { return s_sprites != NULL; }

unsigned gamestate_sprite_ptr_rva(void) {
  return (unsigned)InterlockedCompareExchange(&s_spritePtrRva, 0, 0);
}

unsigned gamestate_sprite_array_rva(void) {
  const unsigned char* const arr = s_sprites;
  if (!arr || !s_base || arr <= s_base) return 0;
  return (unsigned)(arr - s_base);
}

const unsigned char* gamestate_sprite(int i) {
  const unsigned char* const arr = s_sprites;
  if (!arr || i < 0 || i >= EK_MAXSPRITES) return NULL;
  return arr + (size_t)i * EK_SPR_STRIDE;
}

// gamevars.cpp's writable() shape: committed, inside one region, with a write bit.
static bool sprite_bytes_writable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const unsigned char* const end = (const unsigned char*)p + bytes;
  if (end > (const unsigned char*)mbi.BaseAddress + mbi.RegionSize) return false;
  const DWORD w = PAGE_READWRITE | PAGE_WRITECOPY |
                  PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return (mbi.Protect & w) != 0;
}

static bool s_spriteWriteOff = false;

bool gamestate_sprite_write(int i, int off, const void* src, size_t n) {
  if (s_spriteWriteOff || !src || n == 0) return false;
  if (off < 0 || (size_t)off + n > (size_t)EK_SPR_STRIDE) return false;
  if (!ekm_sprite_write_allowed(off, n)) {
    s_spriteWriteOff = true;
    ek_log("gamestate: a sprite write of field 0x%02X, %u byte(s) is not on the "
           "allow-list; no sprite field is written again this session",
           (unsigned)off, (unsigned)n);
    return false;
  }
  const unsigned char* const rec = gamestate_sprite(i);
  if (!rec) return false;
  unsigned char* const dst = (unsigned char*)rec + off;
  bool ok = sprite_bytes_writable(dst, n);
  if (ok) {
    __try {
      memcpy(dst, src, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
    }
  }
  if (!ok) {
    s_spriteWriteOff = true;
    ek_log("gamestate: a sprite write (sprite %d, field 0x%02X, %u byte(s)) was "
           "refused; no sprite field is written again this session",
           i, (unsigned)off, (unsigned)n);
  }
  return ok;
}

int gamestate_live_sprites(void) {
  const unsigned char* const arr = s_sprites;
  if (!arr) return 0;
  int live = 0;
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const short st = *(const short*)(arr + (size_t)i * EK_SPR_STRIDE + EK_SPR_STATNUM);
    if (st >= 0 && st < EK_MAXSTATUS) live++;
  }
  return live;
}
