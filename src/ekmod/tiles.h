#pragma once

// tilesiz[]: every tile's pixel dimensions, turning a sprite's xrepeat/yrepeat into a world size. It is a DIRECT
// array in .bss (`vec2_16_t tilesiz[MAXTILES]`), not a pointer, so we look for the array itself.
const int EK_MAXTILES_ARR = 30720;
const int EK_TILESIZ_STRIDE = 4;
const size_t EK_TILESIZ_BYTES = (size_t)EK_MAXTILES_ARR * EK_TILESIZ_STRIDE;

// An upper bound on a tile's pixel dimensions, a necessary condition only. 8192, not 1024: AA's tile 4895 is
// 168x1372, and the sweep demands EVERY one of 30720 entries be in range, so one big tile refused the real array and
// the scan settled on a look-alike. ART stores int16, so any bound is a judgement; random memory still fails fast.
const int EK_TILE_MAX_DIM = 8192;

const int EK_TILE_MIN_NONZERO = 1000;

const int EK_TILE_RATIO_PCT = 80;

// How many EXPENSIVE attempts the scan gets before it latches refused: the walk covers ~1.3M candidates on AA's
// image, so an unlatched 1 Hz retry would burn a core for the life of the process.
const int EK_TILE_SCAN_BUDGET = 900;

// Which gate refused a tilesiz candidate. ORDER IS THE RULE: the gates in the order candidate_ok applies them; the
// first that fires is reported.
enum EkTileReject {
  EK_TILE_OK = 0,
  EK_TILE_LIVE_RANGE,
  EK_TILE_RATIO,
  EK_TILE_SWEEP_RANGE,
  EK_TILE_FLOOR,
};

inline EkTileReject tiles_reject_reason(bool liveRangeOk, int hits, int npics,
                                        bool sweepRangeOk, int nonzero) {
  if (!liveRangeOk) return EK_TILE_LIVE_RANGE;
  if (hits * 100 < npics * EK_TILE_RATIO_PCT) return EK_TILE_RATIO;
  if (!sweepRangeOk) return EK_TILE_SWEEP_RANGE;
  if (nonzero < EK_TILE_MIN_NONZERO) return EK_TILE_FLOOR;
  return EK_TILE_OK;
}

inline const char* tiles_reject_name(EkTileReject r) {
  switch (r) {
    case EK_TILE_OK:          return "ok";
    case EK_TILE_LIVE_RANGE:  return "live-range";
    case EK_TILE_RATIO:       return "ratio";
    case EK_TILE_SWEEP_RANGE: return "sweep-range";
    case EK_TILE_FLOOR:       return "floor";
  }
  return "?";
}

// Picking the right array, not a plausible one: a window shifted 100 tiles (400 bytes) passes every shape gate, so the
// scan checks CONTENT against tile sizes read from AA's own ART. AA-only. A reference must be MAP-INVARIANT: per-map
// <mapname>_NN.art can override it (GUARD 10550 did, on E1L1); 14590 comes from duke3d.def's dummytile.
struct EkTileRef { int picnum; int w; int h; const char* name; };

const EkTileRef EK_TILE_REFS[] = {
  { 1405,    40,   82, "APLAYER"    },
  { 1680,    55,   78, "LIZTROOP"   },
  { 2521,   154,   67, "KNEE"       },
  { 4895,   168, 1372, "LIGHTNING2" },
  { 14590, 1043,  400, "BACKDROP"   },
};
const int EK_TILE_REF_COUNT = (int)(sizeof(EK_TILE_REFS) / sizeof(EK_TILE_REFS[0]));

// Is candidate A better than B? SCORE FIRST: agreement with the art outranks the shape tests the shifted window
// already passed. A tie keeps the first seen, so the answer does not depend on iteration order.
inline bool ekm_tile_candidate_better(int aScore, int aHits, int aNonzero,
                                      int bScore, int bHits, int bNonzero) {
  if (aScore != bScore) return aScore > bScore;
  if (aHits  != bHits)  return aHits  > bHits;
  return aNonzero > bNonzero;
}

// Diagnostic: six tiles straddling the split. EK_TILE_REFS cannot see it: all five picnums are >= 1405 while the
// items reading 0x0 are <= 1348, so an array correct above ~1400 and zero below scores 5 of 5. Nothing here feeds the
// scan's gates or pick; none of the six is touched by the 91 per-map art files.
struct EkTileProbe { int picnum; int w; int h; };

const EkTileProbe EK_TILE_PROBES[] = {
  {    21,  41,  27 },
  {   100,  27,  24 },
  {  1348,  41,  60 },
  {  1405,  40,  82 },
  {  1680,  55,  78 },
  {  5329,  79,  24 },
};
const int EK_TILE_PROBE_COUNT = (int)(sizeof(EK_TILE_PROBES) / sizeof(EK_TILE_PROBES[0]));

// Does one entry read back as AA's art says? A ZERO OR NEGATIVE ENTRY IS NEVER A HIT, even against a zero
// expectation: "no data" and "really 0x0" are what the dump separates.
inline bool ekm_tile_probe_hit(int readW, int readH, int artW, int artH) {
  if (readW <= 0 || readH <= 0) return false;
  return readW == artW && readH == artH;
}

inline bool ekm_tile_size_is_fallback(int tw, int th) {
  return tw <= 0 || th <= 0;
}

bool tiles_scan(void);
bool tiles_ready(void);

// Tile dimensions, or 0,0 if unknown: callers must treat 0 as "do not size anything from this".
void tile_size(int picnum, int* w, int* h);
unsigned tiles_rva(void);
int tiles_nonzero(void);
