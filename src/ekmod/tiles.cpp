// Finding tilesiz[] by tying it to something proven. A range test is not enough (a zeroed .bss passes any range test on
// small numbers), so MOST live sprite picnums must have a non-zero size: sprite[] is confirmed against level data.
// Most, not all: the engine holds a 0x0 tile and skips it (artReadManifest, :415).

#include <windows.h>
#include <stdio.h>
#include "log.h"
#include "config.h"
#include "gamestate.h"
#include "tiles.h"

static const unsigned char* volatile s_tiles = NULL;
static volatile LONG s_rva = 0;
static volatile LONG s_nonzero = 0;
static int s_attempts = 0;
static bool s_budgetSpent = false;
static int s_lastThinNpics = -1;

static const unsigned char* s_base = NULL;
static size_t s_size = 0;

struct Region { const unsigned char* base; size_t size; };
static Region s_regions[1024];
static int s_regionCount = 0;

static void enumerate_regions(void) {
  s_regionCount = 0;
  const unsigned char* p = s_base;
  const unsigned char* const end = s_base + s_size;
  const int cap = (int)(sizeof(s_regions) / sizeof(s_regions[0]));
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
    if (mbi.RegionSize == 0) break;
    const unsigned char* rbase = (const unsigned char*)mbi.BaseAddress;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      Region* last = s_regionCount ? &s_regions[s_regionCount - 1] : NULL;
      if (last && last->base + last->size == rbase) last->size += mbi.RegionSize;
      else if (s_regionCount < cap) {
        s_regions[s_regionCount].base = rbase;
        s_regions[s_regionCount].size = mbi.RegionSize;
        s_regionCount++;
      }
    }
    p = rbase + mbi.RegionSize;
  }
}

static bool in_region(const void* p, size_t bytes) {
  const unsigned char* b = (const unsigned char*)p;
  for (int i = 0; i < s_regionCount; i++)
    if (b >= s_regions[i].base && b + bytes <= s_regions[i].base + s_regions[i].size)
      return true;
  return false;
}

// The picnums the engine is drawing, from the verified sprite[].
static const int kMaxWanted = 256;
static int collect_live_picnums(int* out) {
  int n = 0;
  for (int i = 0; i < EK_MAXSPRITES && n < kMaxWanted; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;
    const short st = *(const short*)(r + EK_SPR_STATNUM);
    if (st < 0 || st >= EK_MAXSTATUS) continue;
    if (r[EK_SPR_XREPEAT] == 0 || r[EK_SPR_YREPEAT] == 0) continue;
    const short pic = *(const short*)(r + EK_SPR_PICNUM);
    if (pic <= 0 || pic >= EK_MAXTILES_ARR) continue;
    bool known = false;
    for (int k = 0; k < n; k++) if (out[k] == pic) { known = true; break; }
    if (!known) out[n++] = pic;
  }
  return n;
}

// Discriminating test first, load-bearing: sweeping all 30720 entries per candidate is ~150 billion reads. The picnum
// test kills nearly every candidate in a few dozen. Returns the first rejecting gate so the log says why every
// candidate died (under AA all did).
static EkTileReject candidate_reason(const unsigned char* arr, const int* pics, int npics,
                                     int* nonzeroOut, int* hitsOut) {
  // A majority, not all: the engine holds a 0x0 tile and skips it (artReadManifest, :415; six draw paths guard
  // `tilesiz[pic].x <= 0`), and one such sprite must not reject the real array. A zeroed region still scores zero of
  // hundreds.
  int hits = 0;
  bool liveRangeOk = true;
  for (int k = 0; k < npics; k++) {
    const unsigned char* e = arr + (size_t)pics[k] * EK_TILESIZ_STRIDE;
    const short w = *(const short*)e;
    const short h = *(const short*)(e + 2);
    if (w < 0 || h < 0 || w > EK_TILE_MAX_DIM || h > EK_TILE_MAX_DIM) { liveRangeOk = false; break; }
    if (w > 0 && h > 0) hits++;
  }
  // hitsOut is NOT written on the live-range path: the loop broke early, and a partial count let a candidate that died
  // on picnum 112 of 113 outrank a real ratio near-miss and report the wrong gate.
  if (!liveRangeOk) return EK_TILE_LIVE_RANGE;
  if (hitsOut) *hitsOut = hits;
  if (hits * 100 < npics * EK_TILE_RATIO_PCT) return EK_TILE_RATIO;

  int nonzero = 0;
  for (int i = 0; i < EK_MAXTILES_ARR; i++) {
    const short w = *(const short*)(arr + (size_t)i * EK_TILESIZ_STRIDE);
    const short h = *(const short*)(arr + (size_t)i * EK_TILESIZ_STRIDE + 2);
    if (w < 0 || h < 0 || w > EK_TILE_MAX_DIM || h > EK_TILE_MAX_DIM) return EK_TILE_SWEEP_RANGE;
    if (w || h) nonzero++;
  }
  // Measured: stock DUKE3D.GRP's art manifest has 3546 non-zero tiles over slots 0..5119. The floor is well under that
  // so add-on GRPs pass, far above a stray run of small integers. The HRP feeds h_xsize/h_ysize, not tilesiz.
  if (nonzeroOut) *nonzeroOut = nonzero;
  return tiles_reject_reason(true, hits, npics, true, nonzero);
}

// Measurement: 18 of 20 item picnums read 0x0 and took the 64x64 fallback; every zero is <= 1348, every good read >=
// 1405. Either we located the WRONG array or it IS tilesiz and holds zeros at read time. Report only; short lines
// (ek_log's buffer is 1024), each with its own total.

static int tiles_dump_put(char* line, int cap, int used, const char* fmt,
                          int a, int b, int c) {
  if (used < 0 || used >= cap - 1) return cap - 1;
  const int n = _snprintf_s(line + used, (size_t)(cap - used), _TRUNCATE, fmt, a, b, c);
  if (n < 0) return cap - 1;
  return used + n;
}

// The bound lives here, not at call sites: callers' `arr` was checked for the array's full length, so a picnum in [0,
// EK_MAXTILES_ARR) cannot overrun and one outside can. A refused read answers 0, which ekm_tile_probe_hit treats as a
// miss.
static int tiles_dump_w(const unsigned char* arr, int picnum) {
  if (picnum < 0 || picnum >= EK_MAXTILES_ARR) return 0;
  return *(const short*)(arr + (size_t)picnum * EK_TILESIZ_STRIDE);
}
static int tiles_dump_h(const unsigned char* arr, int picnum) {
  if (picnum < 0 || picnum >= EK_MAXTILES_ARR) return 0;
  return *(const short*)(arr + (size_t)picnum * EK_TILESIZ_STRIDE + 2);
}

// Do the zeros stop at a boundary or taper? Entries with w > 0 && h > 0 per 512-picnum block; the total also reports
// the scan's own w || h predicate so the instrument is checked against different code.
static void tiles_dump_blocks(const unsigned char* arr) {
  const int kBlock = 512, kPerLine = 10, kCap = 900;
  char line[kCap];
  int used = 0, n = 0, first = 0, sized = 0, nonzero = 0;
  for (int start = 0; start < EK_MAXTILES_ARR; start += kBlock) {
    int blockSized = 0;
    for (int i = start; i < start + kBlock && i < EK_MAXTILES_ARR; i++) {
      const int w = tiles_dump_w(arr, i), h = tiles_dump_h(arr, i);
      if (w > 0 && h > 0) blockSized++;
      if (w || h) nonzero++;
    }
    sized += blockSized;
    if (n == 0) { line[0] = 0; used = 0; first = start; }
    used = tiles_dump_put(line, kCap, used, " %d", blockSized, 0, 0);
    n++;
    if (n == kPerLine || start + kBlock >= EK_MAXTILES_ARR) {
      ek_log("tiles: d9 sized-per-512 picnum %5d..%5d:%s", first,
             start + kBlock - 1, line);
      n = 0;
    }
  }
  ek_log("tiles: d9 sized-per-512 TOTAL %d of %d sized; %d non-zero by the "
         "scan's own w||h test (it reported %d)",
         sized, EK_MAXTILES_ARR, nonzero, tiles_nonzero());
}

static void tiles_dump_edge(const unsigned char* arr) {
  const int kFrom = 1330, kTo = 1420, kPerLine = 10, kCap = 900;
  char line[kCap];
  int used = 0, n = 0, first = kFrom, sized = 0;
  for (int pic = kFrom; pic <= kTo; pic++) {
    const int w = tiles_dump_w(arr, pic), h = tiles_dump_h(arr, pic);
    if (w > 0 && h > 0) sized++;
    if (n == 0) { line[0] = 0; used = 0; first = pic; }
    used = tiles_dump_put(line, kCap, used, " %d:%dx%d", pic, w, h);
    n++;
    if (n == kPerLine || pic == kTo) {
      ek_log("tiles: d9 edge %4d..%4d:%s", first, pic, line);
      n = 0;
    }
  }
  ek_log("tiles: d9 edge TOTAL %d of %d entries in %d..%d have a size",
         sized, kTo - kFrom + 1, kFrom, kTo);
}

// The decisive part: what every offset in the scan's candidate window answers for six tiles straddling the split
// (EK_TILE_PROBES). Exactly one other all-six offset is answer (A, wrong array); none, or only ours, is (B). Bounded by
// kMaxOffsets, and each offset is re-checked with in_region for the full array length.
static void tiles_dump_window(const unsigned char* arr, unsigned bestRva,
                              unsigned firstRva, unsigned lastRva) {
  const int kMaxOffsets = 8192, kPerLine = 6, kMaxListed = 48, kCap = 900;

  int chosenHits = 0;
  for (int i = 0; i < EK_TILE_PROBE_COUNT; i++) {
    const int pic = EK_TILE_PROBES[i].picnum;
    const int w = tiles_dump_w(arr, pic), h = tiles_dump_h(arr, pic);
    const bool hit = ekm_tile_probe_hit(w, h, EK_TILE_PROBES[i].w, EK_TILE_PROBES[i].h);
    if (hit) chosenHits++;
    ek_log("tiles: d9 probe %5d art %4dx%-4d read %4dx%-4d %s",
           pic, EK_TILE_PROBES[i].w, EK_TILE_PROBES[i].h, w, h,
           hit ? "HIT" : "MISS");
  }
  ek_log("tiles: d9 probe TOTAL chosen rva 0x%07X scores %d of %d",
         bestRva, chosenHits, EK_TILE_PROBE_COUNT);

  const unsigned lo = firstRva <= lastRva ? firstRva : lastRva;
  const unsigned hi = firstRva <= lastRva ? lastRva : firstRva;
  int examined = 0, skipped = 0, allSix = 0, listed = 0, bestPartial = -1;
  unsigned bestPartialRva = 0;
  char line[kCap];
  int used = 0, n = 0;
  line[0] = 0;

  for (int k = 0; k < kMaxOffsets; k++) {
    const unsigned rva = lo + (unsigned)k * (unsigned)EK_TILESIZ_STRIDE;
    if (rva > hi) break;
    const unsigned char* const cand = s_base + rva;
    if (!in_region(cand, EK_TILESIZ_BYTES)) { skipped++; continue; }
    examined++;
    int hits = 0;
    for (int i = 0; i < EK_TILE_PROBE_COUNT; i++) {
      const int pic = EK_TILE_PROBES[i].picnum;
      if (ekm_tile_probe_hit(tiles_dump_w(cand, pic), tiles_dump_h(cand, pic),
                             EK_TILE_PROBES[i].w, EK_TILE_PROBES[i].h)) hits++;
    }
    if (hits > bestPartial) { bestPartial = hits; bestPartialRva = rva; }
    if (hits != EK_TILE_PROBE_COUNT) continue;
    allSix++;
    if (listed >= kMaxListed) continue;
    listed++;
    used = tiles_dump_put(line, kCap, used, " 0x%07X(%+dt)", (int)rva,
                          ((int)rva - (int)bestRva) / EK_TILESIZ_STRIDE, 0);
    if (++n == kPerLine) { ek_log("tiles: d9 all-six at:%s", line); n = 0; used = 0; line[0] = 0; }
  }
  if (n) ek_log("tiles: d9 all-six at:%s", line);

  ek_log("tiles: d9 window rva 0x%07X..0x%07X stride %d -- %d offset(s) read, "
         "%d skipped as not fully readable, cap %d",
         lo, hi, EK_TILESIZ_STRIDE, examined, skipped, kMaxOffsets);
  ek_log("tiles: d9 window TOTAL %d of %d offset(s) reproduce all %d probe "
         "tile(s), %d listed above; best partial %d of %d at rva 0x%07X",
         allSix, examined, EK_TILE_PROBE_COUNT, listed,
         bestPartial < 0 ? 0 : bestPartial, EK_TILE_PROBE_COUNT, bestPartialRva);
  if (allSix == 1 && bestPartial == EK_TILE_PROBE_COUNT && bestPartialRva != bestRva)
    ek_log("tiles: d9 verdict -- ONE offset reproduces all %d and it is NOT the "
           "one we chose: answer (A), the located array is wrong. CHECK ITS RVA "
           "IS 16-ALIGNED before believing the scan could have picked it -- this "
           "walk steps %d where the scan skips non-16-aligned candidates, so a "
           "winner on an alignment the scan rejects means the real table is one "
           "the ranking never sees, which is a different fault",
           EK_TILE_PROBE_COUNT, EK_TILESIZ_STRIDE);
  else if (allSix == 0)
    ek_log("tiles: d9 verdict -- NO offset in the window reproduces all %d: "
           "answer (B), or the right array is outside this window",
           EK_TILE_PROBE_COUNT);
  else if (allSix == 1 && chosenHits == EK_TILE_PROBE_COUNT)
    ek_log("tiles: d9 verdict -- only the chosen offset reproduces all %d, so "
           "the zeros are the engine's: answer (B)", EK_TILE_PROBE_COUNT);
  else
    ek_log("tiles: d9 verdict -- %d offsets reproduce all %d; neither (A) nor "
           "(B) as stated, read the list above", allSix, EK_TILE_PROBE_COUNT);
}

static void tiles_dump_d9(const unsigned char* arr, unsigned bestRva,
                          unsigned firstRva, unsigned lastRva) {
  if (!g_cfg.debuglog) return;
  tiles_dump_blocks(arr);
  tiles_dump_edge(arr);
  tiles_dump_window(arr, bestRva, firstRva, lastRva);
}

bool tiles_scan(void) {
  if (s_tiles) return true;
  // The refused latch is required now this rides the 1 Hz sampler: without it a refusing map runs the whole
  // ~1.3M-candidate walk and an unthrottled log write every second (modelbounds.cpp:979's terminal state).
  if (s_budgetSpent) return false;
  if (!gamestate_ready()) return false;

  // The cheap test first, for the 1 Hz sampler's sake: collecting picnums is a sprite walk, enumerate_regions a
  // VirtualQuery sweep, and between levels the count is always too low.
  int pics[kMaxWanted];
  const int npics = collect_live_picnums(pics);
  if (npics < 8) {
    if (npics != s_lastThinNpics) {
      s_lastThinNpics = npics;
      ek_log("tiles: only %d live picnum(s); too few to identify tilesiz safely", npics);
    }
    return false;
  }
  s_lastThinNpics = -1;

  s_base = (const unsigned char*)GetModuleHandleA(NULL);
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)s_base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(s_base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  s_size = nt->OptionalHeader.SizeOfImage;
  enumerate_regions();

  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = nt->FileHeader.NumberOfSections;
  // 914 candidates passed once. Suspected to be shifted views of one array (Duke's non-zero tiles are largely
  // contiguous), so the span of matching addresses is recorded: all within one array length means one array seen many
  // times.
  int matches = 0, bestNonzero = 0, bestHits = 0;
  int bestScore = -1;
  unsigned firstRva = 0, lastRva = 0;
  const unsigned char* best = NULL;
  unsigned bestRva = 0;
  int rejects[EK_TILE_FLOOR + 1] = { 0 };
  int nearHits = -1;
  EkTileReject nearWhy = EK_TILE_OK;

  for (int i = 0; i < nsec; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
    const unsigned char* start = s_base + sec[i].VirtualAddress;
    const DWORD span = sec[i].Misc.VirtualSize;
    if (span < EK_TILESIZ_BYTES) continue;

    for (DWORD off = 0; off + EK_TILESIZ_BYTES <= span; off += 4) {
      const unsigned char* cand = start + off;
      if ((((UINT_PTR)cand) & 15u) != 0) continue;
      if (!in_region(cand, EK_TILESIZ_BYTES)) continue;
      int nonzero = 0, hits = 0;
      const EkTileReject why = candidate_reason(cand, pics, npics, &nonzero, &hits);
      if (why != EK_TILE_OK) {
        // The near-miss makes the failure actionable: a ratio just under the gate and a zeroed region look alike in
        // "not found".
        rejects[why]++;
        if (hits > nearHits || (hits == nearHits && why > nearWhy)) {
          nearHits = hits; nearWhy = why;
        }
        continue;
      }
      matches++;
      const unsigned thisRva = (unsigned)(sec[i].VirtualAddress + off);
      if (!firstRva) firstRva = thisRva;
      lastRva = thisRva;

      // Shape tests cannot see a window shifted 100 tiles into the real table (one passed and won on non-zero count by
      // running past the table's end). Each survivor is scored on how much of the game's own art it reproduces, first;
      // references: tiles.h.
      int score = 0;
      for (int r = 0; r < EK_TILE_REF_COUNT; r++) {
        const int pic = EK_TILE_REFS[r].picnum;
        if (pic < 0 || pic >= EK_MAXTILES_ARR) continue;
        const unsigned char* e = cand + (size_t)pic * EK_TILESIZ_STRIDE;
        if (*(const short*)e == (short)EK_TILE_REFS[r].w &&
            *(const short*)(e + 2) == (short)EK_TILE_REFS[r].h)
          score++;
      }

      if (ekm_tile_candidate_better(score, hits, nonzero,
                                    bestScore, bestHits, bestNonzero)) {
        bestScore = score; bestHits = hits; bestNonzero = nonzero; best = cand;
        bestRva = (unsigned)(sec[i].VirtualAddress + off);
      }
    }
  }

  if (!best) {
    // Charged on expensive attempts only: early-outs (answered, no sprites, too few picnums) must not eat the budget
    // during a slow level load.
    if (++s_attempts >= EK_TILE_SCAN_BUDGET) {
      s_budgetSpent = true;
      // Boxes are drawn from the sprite's repeats; tilesiz gives the right SIZE, so without it they are there and
      // wrong, which is harder to notice than absent ones.
      ek_log("tiles: giving up on tilesiz after %d attempts -- ESP boxes will "
             "draw at a wrong, fixed size, and nothing will retry for the life "
             "of this process", s_attempts);
    }
    ek_log("tiles: tilesiz not found -- %d live picnum(s); closest candidate %d/%d "
           "with a size, refused at the %s gate (ratio gate needs %d%%); refusals: "
           "live-range=%d ratio=%d sweep-range=%d floor=%d",
           npics, nearHits < 0 ? 0 : nearHits, npics, tiles_reject_name(nearWhy),
           EK_TILE_RATIO_PCT,
           rejects[EK_TILE_LIVE_RANGE], rejects[EK_TILE_RATIO],
           rejects[EK_TILE_SWEEP_RANGE], rejects[EK_TILE_FLOOR]);
    return false;
  }
  if (matches > 1) {
    const unsigned span = lastRva - firstRva;
    if (span <= (unsigned)EK_TILESIZ_BYTES)
      ek_log("tiles: %d candidates, all within 0x%X bytes (rva 0x%07X..0x%07X) -- "
             "one array seen at shifted offsets, not an ambiguity",
             matches, span, firstRva, lastRva);
    else
      ek_log("tiles: WARNING -- %d candidates spanning 0x%X bytes "
             "(rva 0x%07X..0x%07X). That is wider than one array: the densest was "
             "taken but this is a genuine ambiguity, not shifted views.",
             matches, span, firstRva, lastRva);
  }

  InterlockedExchange(&s_rva, (LONG)bestRva);
  InterlockedExchange(&s_nonzero, (LONG)bestNonzero);
  s_tiles = best;
  ek_log("tiles: tilesiz at rva 0x%07X, %d non-zero tile(s); %d of %d live "
         "picnum(s) have a size; %d of %d reference tiles matched",
         bestRva, bestNonzero, bestHits, npics, bestScore, EK_TILE_REF_COUNT);
  if (bestScore < EK_TILE_REF_COUNT) {
    // Not fatal but said plainly: a partial match means the winner is the best of a bad field, invisible on screen (a
    // wrong size is still a box).
    ek_log("tiles:   WARNING -- the chosen array does not reproduce all of the "
           "game's own art, so its dimensions are suspect; the per-tile lines "
           "below say which");
  }

  // Checked against the game's own art: a shifted window passes "all live picnums have a size", two sessions picked
  // addresses 400 bytes apart and the pick has ~6600 more non-zero tiles than the art accounts for. Dimensions come
  // from AA's ART (reproducing the stock 3546); 14590 comes from duke3d.def's `dummytile 14590 1043 400`.
  for (int i = 0; i < EK_TILE_REF_COUNT; i++) {
    int w = 0, h = 0;
    tile_size(EK_TILE_REFS[i].picnum, &w, &h);
    const bool ok = (w == EK_TILE_REFS[i].w && h == EK_TILE_REFS[i].h);
    ek_log("tiles:   check %-5d %-11s art %4dx%-4d read %4dx%-4d %s",
           EK_TILE_REFS[i].picnum, EK_TILE_REFS[i].name,
           EK_TILE_REFS[i].w, EK_TILE_REFS[i].h, w, h,
           ok ? "OK" : "<-- MISMATCH");
  }

  // Measurement only; firstRva/lastRva are this run's own matching-candidate window.
  tiles_dump_d9(best, bestRva, firstRva, lastRva);
  return true;
}

bool tiles_ready(void) { return s_tiles != NULL; }
unsigned tiles_rva(void) { return (unsigned)InterlockedCompareExchange(&s_rva, 0, 0); }
int tiles_nonzero(void) { return (int)InterlockedCompareExchange(&s_nonzero, 0, 0); }

void tile_size(int picnum, int* w, int* h) {
  if (w) *w = 0;
  if (h) *h = 0;
  const unsigned char* const t = s_tiles;
  if (!t || picnum < 0 || picnum >= EK_MAXTILES_ARR) return;
  const unsigned char* e = t + (size_t)picnum * EK_TILESIZ_STRIDE;
  if (w) *w = *(const short*)e;
  if (h) *h = *(const short*)(e + 2);
}
