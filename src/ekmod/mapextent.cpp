// The extent's derive: chain walk, cross-check against the engine's own counts, and publish. Rules: mapextent.h.
#include <windows.h>
#include "mapextent.h"
#include "sectorstate.h"
#include "gamestate.h"
#include "log.h"

// One value, not two: this map's sectors with the last map's wall total would bound a map that never existed. One
// aligned 64-bit slot, one interlocked store; zero is "no map".
static volatile LONG64 s_pair = 0;

static LONG64 pair_pack(int n, int w) {
  return (LONG64)(((unsigned long long)(unsigned int)w << 32) |
                  (unsigned long long)(unsigned int)n);
}

static int pair_sectors(LONG64 p) {
  return (int)(unsigned int)((unsigned long long)p & 0xFFFFFFFFull);
}

static int pair_walls(LONG64 p) {
  return (int)(unsigned int)(((unsigned long long)p >> 32) & 0xFFFFFFFFull);
}

static LONG64 pair_get(void) {
  return InterlockedCompareExchange64(&s_pair, 0, 0);
}

// The walk is the derive; its stop test is ekm_extent_chain_stops. EK_MAXSECTORS bounds it, so a chain that never stops
// costs 4096 checked reads, not a hang.
static int extent_walk(int* wallSum) {
  int prevPtr = 0, prevNum = 0, walked = 0, sum = 0;
  for (int i = 0; i < EK_MAXSECTORS; i++) {
    EkSectorInfo si = { 0, 0, 0, 0, 0, 0 };
    const int ok = sectorstate_info(i, &si) ? 1 : 0;
    if (ekm_extent_chain_stops(i, ok, si.wallptr, si.wallnum, prevPtr, prevNum))
      break;
    walked++;
    sum += si.wallnum;
    prevPtr = si.wallptr;
    prevNum = si.wallnum;
  }
  *wallSum = sum;
  return walked;
}

static const unsigned char* s_numsectorsAt = NULL;
static const unsigned char* s_numwallsAt = NULL;
static int s_harden = EK_EXTENT_HARDEN_PENDING;
static int s_drops = 0;
// Reset by any agreeing scan, so a load-window blip and a real over-run differ by reaching 2.
static int s_disagreeStreak = 0;

// The loop bound IS ekm_extent_window_holds, so the pinned rule and the searched bytes cannot drift.
static int window_hits(const unsigned char* anchor, int value,
                       const unsigned char** where) {
  int hits = 0;
  for (int off = -EK_EXTENT_WINDOW_BACK; ekm_extent_window_holds(off); off += 2) {
    const unsigned char* const p = anchor + off;
    if (!gamestate_region_contains(p, sizeof(short))) continue;
    if ((int)*(const short*)p != value) continue;
    hits++;
    *where = p;
  }
  return hits;
}

static void harden_locate(int n, int w) {
  const unsigned char* const base = gamestate_module_base();
  const unsigned anchorRva = sectorstate_rva();
  if (!base || !anchorRva) return;
  const unsigned char* const anchor = base + anchorRva;

  // Refreshed because a stale table would make a populated window look empty.
  gamestate_regions_refresh();

  const unsigned char* nAt = NULL;
  const unsigned char* wAt = NULL;
  const int nHits = window_hits(anchor, n, &nAt);
  const int wHits = window_hits(anchor, w, &wAt);

  if (!ekm_extent_locate_unique(nHits, wHits)) {
    s_harden = EK_EXTENT_HARDEN_NONE;
    ek_log("mapextent: UNHARDENED -- the window %d..+%d bytes around the sector "
           "pointer at rva 0x%07X holds numsectors=%d at %d position(s) and "
           "numwalls=%d at %d position(s); exactly one of each is required. The "
           "chain walk stands alone and mapextent.h's residual risk applies in "
           "full: an over-run would publish the PREVIOUS map's extent undetected",
           -EK_EXTENT_WINDOW_BACK, EK_EXTENT_WINDOW_FWD, anchorRva,
           n, nHits, w, wHits);
    return;
  }

  s_numsectorsAt = nAt;
  s_numwallsAt = wAt;
  s_harden = EK_EXTENT_HARDEN_LOCATED;
  // Logged because the offsets are not in the code (+84 and +8 on the build this was written against).
  ek_log("mapextent: hardened. numsectors at rva 0x%07X (anchor%+d), numwalls at "
         "rva 0x%07X (anchor%+d), anchor = the sector pointer at rva 0x%07X; the "
         "chain walk is cross-checked against both every second",
         (unsigned)(nAt - base), (int)(nAt - anchor),
         (unsigned)(wAt - base), (int)(wAt - anchor), anchorRva);
}

// Bounds-checked on every read: an address that stops being readable must refuse, not fault.
static bool globals_read(int* n, int* w) {
  const unsigned char* const na = s_numsectorsAt;
  const unsigned char* const wa = s_numwallsAt;
  if (!na || !wa) return false;
  if (!gamestate_region_contains(na, sizeof(short))) return false;
  if (!gamestate_region_contains(wa, sizeof(short))) return false;
  __try {
    *n = *(const short*)na;
    *w = *(const short*)wa;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool mapextent_scan(void) {
  // Refused until sector[] is up: the walk IS the sector array.
  if (!sectorstate_ready()) return false;

  int wallSum = 0;
  const int n = extent_walk(&wallSum);
  const bool walkOk = ekm_extent_counts_plausible(n, wallSum);

  if (walkOk && s_harden == EK_EXTENT_HARDEN_PENDING &&
      ekm_extent_values_searchable(n, wallSum)) {
    harden_locate(n, wallSum);
  }

  int globN = 0, globW = 0;
  bool globPlausible = false;
  if (s_harden == EK_EXTENT_HARDEN_LOCATED) {
    const bool readOk = globals_read(&globN, &globW);
    globPlausible = readOk && ekm_extent_counts_plausible(globN, globW);
  }

  const int verdict = ekm_extent_publish_verdict(
      s_harden, walkOk ? 1 : 0, globPlausible ? 1 : 0,
      ekm_extent_agrees(n, wallSum, globN, globW) ? 1 : 0);

  // The loud line waits for a second look: loadboard writes the counts before the sector records, so a level change
  // disagrees for one scan.
  const int disagreeingNow = (verdict == EK_EXTENT_PUBLISH_REFUSE) ? 1 : 0;
  const int priorStreak = s_disagreeStreak;
  s_disagreeStreak = ekm_extent_disagree_next(priorStreak, disagreeingNow);

  if (disagreeingNow) {
    if (ekm_extent_disagree_confirmed(s_disagreeStreak)) {
      ek_log("mapextent: ***** EXTENT REFUSED ***** the chain walk says "
             "numsectors=%d numwalls=%d, the engine's own counts say "
             "numsectors=%d numwalls=%d, and they must agree -- confirmed "
             "across two consecutive scans, not a level-load window. The "
             "extent is withdrawn and no secrets are drawn until they do -- "
             "this is the over-run mapextent.h describes, caught instead of "
             "drawn",
             n, wallSum, globN, globW);
    }
    // Confirmed is true only at exactly 2: a stable over-run logs once.
  } else if (priorStreak >= 2 && verdict == EK_EXTENT_PUBLISH_WALK &&
             globPlausible) {
    ek_log("mapextent: the chain walk and the engine's own counts agree again "
           "at numsectors=%d numwalls=%d; the extent is published once more",
           n, wallSum);
  } else if (priorStreak == 1 && verdict == EK_EXTENT_PUBLISH_WALK &&
             globPlausible) {
    ek_log("mapextent: the chain walk and the engine's own counts disagreed "
           "for one scan and agree again at numsectors=%d numwalls=%d -- "
           "likely a level load crossing the 1 Hz sample; the extent was "
           "withdrawn for that one second and nothing else changed",
           n, wallSum);
  }

  // Addresses go back if they stop holding counts (a layout cannot move, so the window named the wrong bytes); bounded.
  if (verdict == EK_EXTENT_PUBLISH_RELOCATE) {
    s_numsectorsAt = NULL;
    s_numwallsAt = NULL;
    s_drops++;
    if (ekm_extent_may_relocate(s_drops)) {
      s_harden = EK_EXTENT_HARDEN_PENDING;
      ek_log("mapextent: the located addresses no longer read as a map "
             "(numsectors=%d numwalls=%d; zeros mean the read itself refused); "
             "dropping both and locating again (drop %d of %d)",
             globN, globW, s_drops, EK_EXTENT_RELOCATE_MAX);
    } else {
      s_harden = EK_EXTENT_HARDEN_NONE;
      ek_log("mapextent: UNHARDENED -- the located addresses went bad %d time(s), "
             "so the window is not naming the engine's counts on this build and "
             "the search is given up. The chain walk stands alone and "
             "mapextent.h's residual risk applies in full", s_drops);
    }
  }

  // Zero on an implausible walk rather than the last pair; a refused cross-check publishes the same zero.
  const LONG64 now = (verdict == EK_EXTENT_PUBLISH_NOTHING ||
                      verdict == EK_EXTENT_PUBLISH_REFUSE)
                         ? 0
                         : pair_pack(n, wallSum);
  const LONG64 was = pair_get();
  if (now == was) return now != 0;
  InterlockedExchange64(&s_pair, now);

  // Logged on the change, not each second.
  if (now != 0) {
    ek_log("mapextent: numsectors=%d numwalls=%d", n, wallSum);
  } else if (verdict == EK_EXTENT_PUBLISH_NOTHING) {
    ek_log("mapextent: the sector chain tiles %d sector(s) over %d wall(s), "
           "which is not a map; the extent is withdrawn", n, wallSum);
  }
  return now != 0;
}

bool mapextent_ready(void) { return pair_get() != 0; }

int mapextent_numsectors(void) { return pair_sectors(pair_get()); }

// One pair_get, hence no numwalls scalar: two scalars are two reads with a publish between.
bool mapextent_extent(int* numsectors, int* numwalls) {
  const LONG64 p = pair_get();
  const int n = pair_sectors(p);
  const int w = pair_walls(p);
  const bool ok = ekm_extent_counts_plausible(n, w);
  *numsectors = ok ? n : 0;
  *numwalls   = ok ? w : 0;
  return ok;
}
