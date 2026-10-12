// Finds wall[] without an offset: a pointer in a writable section to a static array in the same image, validated by
// the map's own geometry (consistent walls, reciprocal nextwalls; zeroed memory fails at index 0). Scan thread only,
// never the render thread; reuses gamestate.cpp's region table.

#include <windows.h>
#include "log.h"
#include "gamestate.h"
#include "wallstate.h"

static const unsigned char* volatile s_walls = NULL;
static volatile LONG s_rva = 0;

struct WallScore { int checked, plausible, pairs; };

static void score_walls(const unsigned char* arr, WallScore* out) {
  out->checked = out->plausible = out->pairs = 0;
  for (int i = 0; i < EK_WALL_SCAN_DEPTH && i < EK_WALL_SLOTS; i++) {
    const unsigned char* const r = arr + (size_t)i * EK_WALL_STRIDE;
    const int point2     = *(const short*)(r + EK_WALL_POINT2);
    const int nextwall   = *(const short*)(r + EK_WALL_NEXTWALL);
    const int nextsector = *(const short*)(r + EK_WALL_NEXTSECTOR);
    const int picnum     = *(const short*)(r + EK_WALL_PICNUM);
    const int overpicnum = *(const short*)(r + EK_WALL_OVERPICNUM);
    out->checked++;
    if (!ekm_wall_record_plausible(i, point2, nextwall, nextsector, picnum, overpicnum))
      continue;
    out->plausible++;
    if (nextwall < 0) continue;

    const unsigned char* const p2  = arr + (size_t)point2 * EK_WALL_STRIDE;
    const unsigned char* const k   = arr + (size_t)nextwall * EK_WALL_STRIDE;
    const int kp2 = *(const short*)(k + EK_WALL_POINT2);
    if (kp2 < 0 || kp2 >= EK_WALL_SLOTS) continue;
    const unsigned char* const kp2r = arr + (size_t)kp2 * EK_WALL_STRIDE;

    EkWallPair pair;
    pair.index      = i;
    pair.nextwall   = nextwall;
    pair.nextOfNext = *(const short*)(k + EK_WALL_NEXTWALL);
    pair.x1   = *(const int*)(r + EK_WALL_X);
    pair.y1   = *(const int*)(r + EK_WALL_Y);
    pair.x2   = *(const int*)(p2 + EK_WALL_X);
    pair.y2   = *(const int*)(p2 + EK_WALL_Y);
    pair.kx   = *(const int*)(k + EK_WALL_X);
    pair.ky   = *(const int*)(k + EK_WALL_Y);
    pair.kp2x = *(const int*)(kp2r + EK_WALL_X);
    pair.kp2y = *(const int*)(kp2r + EK_WALL_Y);
    if (ekm_wall_pair_consistent(pair)) out->pairs++;
  }
}

static bool scan_now(void) {
  const unsigned char* const base = gamestate_module_base();
  const size_t imageSize = gamestate_module_size();
  if (!base || !imageSize) return false;

  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = nt->FileHeader.NumberOfSections;

  const unsigned char* best = NULL;
  unsigned bestRva = 0;
  int bestPairs = 0, candidates = 0, accepted = 0;

  // NEAR MISSES, so a failure says WHICH test refused: "found nothing" cannot tell a region table that never reached
  // the array from a validator that said no.
  WallScore bestSeen = { 0, 0, 0 };
  unsigned bestSeenRva = 0;

  for (int i = 0; i < nsec; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
    const unsigned char* start = base + sec[i].VirtualAddress;
    const DWORD secSpan = sec[i].Misc.VirtualSize;

    for (DWORD off = 0; off + 8 <= secSpan; off += 8) {
      const unsigned char* slot = start + off;
      if (!gamestate_region_contains(slot, 8)) continue;   // a list walk, not a syscall

      const unsigned char* cand = *(const unsigned char* const*)slot;
      if (cand < base || cand >= base + imageSize) continue;
      if (((UINT_PTR)cand & 3u) != 0) continue;
      if (!gamestate_region_contains(cand, EK_WALL_BYTES)) continue;

      candidates++;
      WallScore s;
      score_walls(cand, &s);
      if (s.plausible > bestSeen.plausible ||
          (s.plausible == bestSeen.plausible && s.pairs > bestSeen.pairs)) {
        bestSeen = s;
        bestSeenRva = (unsigned)(sec[i].VirtualAddress + off);
      }
      if (!ekm_wall_scan_accept(s.checked, s.plausible, s.pairs)) continue;

      accepted++;
      if (s.pairs > bestPairs) {
        bestPairs = s.pairs;
        best = cand;
        bestRva = (unsigned)(sec[i].VirtualAddress + off);
      }
    }
  }

  if (!best) {
    ek_log("wallstate: %d candidate(s) examined, none accepted. closest was "
           "rva 0x%07X with checked %d, plausible %d, consistent pair(s) %d "
           "(%d needed)",
           candidates, bestSeenRva, bestSeen.checked, bestSeen.plausible,
           bestSeen.pairs, EK_WALL_MIN_PAIRS);
    return false;
  }

  InterlockedExchange(&s_rva, (LONG)bestRva);
  s_walls = best;
  ek_log("wallstate: wall pointer at rva 0x%07X -> array rva 0x%07X",
         bestRva, (unsigned)(best - base));
  ek_log("wallstate:   %d candidate(s) examined, %d accepted; %d record(s) "
         "checked, %d plausible, %d consistent two-sided pair(s)",
         candidates, accepted, EK_WALL_SCAN_DEPTH, bestSeen.plausible, bestPairs);
  return true;
}

bool wallstate_scan(void) {
  if (s_walls) return true;
  gamestate_regions_refresh();
  return scan_now();
}

bool wallstate_ready(void) { return s_walls != NULL; }
unsigned wallstate_rva(void) { return (unsigned)InterlockedCompareExchange(&s_rva, 0, 0); }

bool wallstate_face(int wallnum, EkWallFace* out) {
  const unsigned char* const arr = s_walls;   // one read; it cannot change back
  if (!arr || !out) return false;
  // Bounded by the allocation (MAXWALLS plus editor slack): the engine's hitscan cannot return an index at or past
  // numwalls, so this is the memory guard; numwalls is not located.
  if (wallnum < 0 || wallnum >= EK_WALL_SLOTS) return false;
  const unsigned char* const r = arr + (size_t)wallnum * EK_WALL_STRIDE;
  __try {
    out->picnum     = *(const short*)(r + EK_WALL_PICNUM);
    out->overpicnum = *(const short*)(r + EK_WALL_OVERPICNUM);
    out->cstat      = *(const unsigned short*)(r + EK_WALL_CSTAT);
    out->nextsector = *(const short*)(r + EK_WALL_NEXTSECTOR);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool wallstate_corner(int wallnum, EkWallCorner* out) {
  const unsigned char* const arr = s_walls;
  if (!arr || !out) return false;
  if (wallnum < 0 || wallnum >= EK_WALL_SLOTS) return false;
  const unsigned char* const r = arr + (size_t)wallnum * EK_WALL_STRIDE;
  __try {
    out->x          = *(const int*)(r + EK_WALL_X);
    out->y          = *(const int*)(r + EK_WALL_Y);
    out->nextsector = *(const short*)(r + EK_WALL_NEXTSECTOR);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// pal is the ONE UNSIGNED FIELD of the three (uint8_t); the classifier compares it against 21 and 23.
bool wallstate_tags(int wallnum, EkWallTags* out) {
  const unsigned char* const arr = s_walls;
  if (!arr || !out) return false;
  if (wallnum < 0 || wallnum >= EK_WALL_SLOTS) return false;
  const unsigned char* const r = arr + (size_t)wallnum * EK_WALL_STRIDE;
  __try {
    out->pal   = *(const unsigned char*)(r + EK_WALL_PAL);
    out->lotag = *(const short*)(r + EK_WALL_LOTAG);
    out->hitag = *(const short*)(r + EK_WALL_HITAG);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// BOTH UNSIGNED (uint8_t): a yrepeat above 127 read signed would make an ordinary tall wall look like the defect.
bool wallstate_panel(int wallnum, EkWallPanel* out) {
  const unsigned char* const arr = s_walls;
  if (!arr || !out) return false;
  if (wallnum < 0 || wallnum >= EK_WALL_SLOTS) return false;
  const unsigned char* const r = arr + (size_t)wallnum * EK_WALL_STRIDE;
  __try {
    out->yrepeat  = *(const unsigned char*)(r + EK_WALL_YREPEAT);
    out->ypanning = *(const unsigned char*)(r + EK_WALL_YPANNING);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

const void* wallstate_record(int wallnum) {
  const unsigned char* const arr = s_walls;
  if (!arr) return NULL;
  if (wallnum < 0 || wallnum >= EK_WALL_SLOTS) return NULL;
  return arr + (size_t)wallnum * EK_WALL_STRIDE;
}

bool wallstate_point2(int wallnum, int* out) {
  const unsigned char* const arr = s_walls;
  if (!arr || !out) return false;
  if (wallnum < 0 || wallnum >= EK_WALL_SLOTS) return false;
  const unsigned char* const r = arr + (size_t)wallnum * EK_WALL_STRIDE;
  __try {
    *out = *(const short*)(r + EK_WALL_POINT2);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
