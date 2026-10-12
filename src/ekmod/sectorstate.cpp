// Finds sector[] without an offset and reads one sector's floor/ceiling band for the free camera's sky
// (sectorstate.h).

#include <windows.h>
#include "log.h"
#include "gamestate.h"
#include "wallstate.h"
#include "sectorstate.h"

static const unsigned char* volatile s_sectors = NULL;
static volatile LONG s_rva = 0;

// Does this sector's first wall loop close inside its own wall span? Inner loops (a pillar) close in fewer than
// wallnum steps, so the test is "within wallnum steps".
static bool loop_closes(int wallptr, int wallnum) {
  int w = wallptr;
  for (int k = 0; k < wallnum; k++) {
    int p2 = -1;
    if (!wallstate_point2(w, &p2)) return false;
    if (p2 < wallptr || p2 >= wallptr + wallnum) return false;
    if (p2 == wallptr) return true;
    w = p2;
  }
  return false;
}

struct SectorScore { int chain, closed; };

// Reads stay inside a region the caller already contained the whole array in; wall indices taken out of the records
// are bounds-checked by wallstate_point2.
static void score_sectors(const unsigned char* arr, SectorScore* out) {
  out->chain = out->closed = 0;
  int prevPtr = 0, prevNum = 0;
  for (int i = 0; i < EK_SECTOR_SCAN_DEPTH && i < EK_SECTOR_SLOTS; i++) {
    const unsigned char* const r = arr + (size_t)i * EK_SECTOR_STRIDE;
    const int wallptr = *(const short*)(r + EK_SECT_WALLPTR);
    const int wallnum = *(const short*)(r + EK_SECT_WALLNUM);
    // The chain ends at numsectors rather than failing there: records past the live map are the last map's leftovers.
    if (!ekm_sector_locate_step(wallptr, wallnum, prevPtr, prevNum)) break;
    out->chain++;
    if (loop_closes(wallptr, wallnum)) out->closed++;
    prevPtr = wallptr;
    prevNum = wallnum;
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
  int bestChain = 0, bestClosed = 0, candidates = 0, accepted = 0;

  SectorScore bestSeen = { 0, 0 };
  unsigned bestSeenRva = 0;

  for (int i = 0; i < nsec; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
    const unsigned char* start = base + sec[i].VirtualAddress;
    const DWORD secSpan = sec[i].Misc.VirtualSize;

    for (DWORD off = 0; off + 8 <= secSpan; off += 8) {
      const unsigned char* slot = start + off;
      if (!gamestate_region_contains(slot, 8)) continue;

      const unsigned char* cand = *(const unsigned char* const*)slot;
      if (cand < base || cand >= base + imageSize) continue;
      if (((UINT_PTR)cand & 3u) != 0) continue;
      if (!gamestate_region_contains(cand, EK_SECTOR_BYTES)) continue;

      candidates++;
      SectorScore s;
      score_sectors(cand, &s);
      if (s.chain > bestSeen.chain ||
          (s.chain == bestSeen.chain && s.closed > bestSeen.closed)) {
        bestSeen = s;
        bestSeenRva = (unsigned)(sec[i].VirtualAddress + off);
      }
      if (s.chain < EK_SECTOR_MIN_CHAIN || s.closed < EK_SECTOR_MIN_CHAIN) continue;

      accepted++;
      // Among arrays that all pass, the longest tiling chain is the live map's; a tie keeps the first.
      if (s.chain > bestChain) {
        bestChain = s.chain;
        bestClosed = s.closed;
        best = cand;
        bestRva = (unsigned)(sec[i].VirtualAddress + off);
      }
    }
  }

  if (!best) {
    ek_log("sectorstate: %d candidate(s) examined, none accepted. closest was "
           "rva 0x%07X with a chain of %d sector(s), %d of them closing their "
           "own wall loop (%d needed of each)",
           candidates, bestSeenRva, bestSeen.chain, bestSeen.closed,
           EK_SECTOR_MIN_CHAIN);
    return false;
  }

  InterlockedExchange(&s_rva, (LONG)bestRva);
  s_sectors = best;
  ek_log("sectorstate: sector pointer at rva 0x%07X -> array rva 0x%07X",
         bestRva, (unsigned)(best - base));
  ek_log("sectorstate:   %d candidate(s) examined, %d accepted; a chain of %d "
         "tiling sector(s), %d of them closing their own wall loop",
         candidates, accepted, bestChain, bestClosed);
  return true;
}

bool sectorstate_scan(void) {
  if (s_sectors) return true;
  // Refused until wall[] is up: the only evidence this array is the engine's own is that its wall spans agree with an
  // array found by a different anchor. The worker retries.
  if (!wallstate_ready()) return false;
  gamestate_regions_refresh();
  return scan_now();
}

bool sectorstate_ready(void) { return s_sectors != NULL; }
unsigned sectorstate_rva(void) { return (unsigned)InterlockedCompareExchange(&s_rva, 0, 0); }

const void* sectorstate_record(int sectnum) {
  const unsigned char* const arr = s_sectors;
  if (!arr) return NULL;
  if (sectnum < 0 || sectnum >= EK_SECTOR_SLOTS) return NULL;
  return arr + (size_t)sectnum * EK_SECTOR_STRIDE;
}

bool sectorstate_info(int sectnum, EkSectorInfo* out) {
  const unsigned char* const arr = s_sectors;
  if (!arr || !out) return false;
  // Against the allocation, not numsectors (not located): the memory guard. Callers bound the answer against the
  // map's extent separately.
  if (sectnum < 0 || sectnum >= EK_SECTOR_SLOTS) return false;
  const unsigned char* const r = arr + (size_t)sectnum * EK_SECTOR_STRIDE;
  __try {
    out->ceilz   = *(const int*)(r + EK_SECT_CEILINGZ);
    out->florz   = *(const int*)(r + EK_SECT_FLOORZ);
    out->wallptr = *(const short*)(r + EK_SECT_WALLPTR);
    out->wallnum = *(const short*)(r + EK_SECT_WALLNUM);
    out->lotag   = *(const short*)(r + EK_SECT_LOTAG);
    const int cstat = *(const unsigned short*)(r + EK_SECT_CEILINGSTAT);
    const int fstat = *(const unsigned short*)(r + EK_SECT_FLOORSTAT);
    out->sloped = ((cstat | fstat) & EK_SECT_STAT_SLOPED) ? 1 : 0;
    // Per surface, not ORed like `sloped`: the engine's wall-sprite clamp tests each side's bit alone
    // (sectorstate.h).
    out->ceilparallax = (cstat & EK_SECT_STAT_PARALLAX) ? 1 : 0;
    out->florparallax = (fstat & EK_SECT_STAT_PARALLAX) ? 1 : 0;
    out->ceilStat = cstat;
    out->florStat = fstat;
    out->ceilHeinum = *(const short*)(r + EK_SECT_CEILINGHEINUM);
    out->florHeinum = *(const short*)(r + EK_SECT_FLOORHEINUM);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
