// The frame's perf zones: every rule that can be decided without the engine. perf.cpp owns the clock, state and draw.
#pragma once

// Zones in report order; the four per-call zones are last so the detail gate is one comparison, and a zone inserted
// in the middle fails the suite instead of becoming always-on.
enum {
  // The substituted orig call (normal pass only; on a TROR map also the armed base-level draw). Not the whole engine
  // frame: engine.extra and engine.plain are siblings of it, and G_DrawRooms' own work is in none of the three
  // (engine.frame and its partition cover it).
  EK_PZ_ENGINE = 0,
  EK_PZ_ENGINE_EXTRA,
  // Every orig call that hands the frame back unsubstituted, whatever the reason: deliberately not a list (a fifth
  // condition once went unbracketed).
  EK_PZ_ENGINE_PLAIN,
  EK_PZ_SWEEP_SCAN,
  EK_PZ_SWEEP_FRUSTUM,
  EK_PZ_SWEEP_LOOP,
  EK_PZ_SWEEP_SPRITES,
  EK_PZ_OVERLAY_BUILD,
  EK_PZ_OVERLAY_ESP,
  EK_PZ_OVERLAY_QUADS,
  EK_PZ_OVERLAY_PANEL,
  // The readout's own draw. Not circular: the table shows this ring's previous frames. It exists because the
  // instrument-cost line excluded the readout (~0.7 ms of table against 0.14 ms of zones).
  EK_PZ_OVERLAY_PERF,
  // The ESP split: three always-on zones (not detail 2), because the question is which phase owns overlay.esp's cost.
  EK_PZ_ESP_WALK,
  EK_PZ_ESP_ACTORS,
  EK_PZ_ESP_ITEMS,
  // Fullbright's arm and release around G_DrawRooms (camera.cpp); items are the shade slots lowered.
  EK_PZ_FULLBRIGHT,
  // One G_DrawRooms partitioned from stamps in the two existing hooks; the gap (frame minus the other three) is the
  // time between Q16 calls (yax_drawrooms' per-bunch sprites and masks).
  EK_PZ_ENGINE_HEAD,
  EK_PZ_ENGINE_DRAWS,
  EK_PZ_ENGINE_TAIL,
  EK_PZ_ENGINE_FRAME,
  // Which of our sweep, the engine's drawalls/domost or our domost detour owns the whole-map cost; nested inside the
  // Q16 draw, detail 1.
  EK_PZ_SWEEP_POLY,
  EK_PZ_CUT_DRAWALLS,
  // cut.drawalls split by the burst line's pass kind (nocullburst.h), contiguous in EK_NOCULL_KIND_* order. Each is
  // banked on every call, so the per-kind count is `items`.
  EK_PZ_CUT_DRAWALLS_PREPARE,
  EK_PZ_CUT_DRAWALLS_MAIN,
  EK_PZ_CUT_DRAWALLS_EXTRA,
  EK_PZ_CUT_DRAWALLS_OTHER,
  EK_PZ_CUT_DRAWALLS_THUNK,
  // Every EK_DomostThunk span: inside cut.drawalls (flats via cut.nonpar.fwd) except polymost_drawrooms' YAX clip
  // calls.
  EK_PZ_CUT_DOMOST,
  EK_PZ_CUT_DOMOST_FWD,
  EK_PZ_CUT_NONPAR,
  EK_PZ_CUT_NONPAR_FWD,
  EK_PZ_SWEEP_CULL,
  EK_PZ_SWEEP_US,
  EK_PZ_SWEEP_DS,
  EK_PZ_SWEEP_UW,
  EK_PZ_SWEEP_DW,
  EK_PERF_ZONE_COUNT
};

#define EK_PERF_DETAIL2_FIRST EK_PZ_SWEEP_CULL
#define EK_PERF_RING 256

// The bisect mask's bits. Ini-only (perfskip): they draw the map wrong on purpose to learn what a phase costs, which
// a menu row must not be able to do by accident.
#define EK_PERF_SKIP_SPRITES 1
#define EK_PERF_SKIP_WALLS   2
#define EK_PERF_SKIP_UPDATES 4
#define EK_PERF_SKIP_NOCULL  8
#define EK_PERF_SKIP_ALL     15

inline const char* ekm_perf_zone_name(int zone) {
  static const char* const names[EK_PERF_ZONE_COUNT] = {
    "engine", "engine.extra", "engine.plain", "sweep.scan", "sweep.frustum", "sweep.loop",
    "sweep.sprites",
    "overlay.build", "overlay.esp", "overlay.quads", "overlay.panel",
    "overlay.perf",
    "esp.walk", "esp.actors", "esp.items", "fullbright",
    "engine.head", "engine.draws", "engine.tail", "engine.frame",
    "sweep.poly", "cut.drawalls", "cut.drawalls.prepare", "cut.drawalls.main",
    "cut.drawalls.extra", "cut.drawalls.other", "cut.drawalls.thunk",
    "cut.domost", "cut.domost.fwd",
    "cut.nonpar", "cut.nonpar.fwd",
    "sweep.cull", "sweep.us", "sweep.ds", "sweep.uw", "sweep.dw"
  };
  return (zone >= 0 && zone < EK_PERF_ZONE_COUNT) ? names[zone] : "?";
}

inline int ekm_perf_zone_admits(int zone, int detail2) {
  if (zone < 0 || zone >= EK_PERF_ZONE_COUNT) return 0;
  return (zone < EK_PERF_DETAIL2_FIRST || detail2 != 0) ? 1 : 0;
}

// One zone's live state. `open` is an int: MSVC 19.50 at /O2 loses a bool member of a struct an inline function
// returns.
typedef struct EkPerfZone {
  long long ticks;
  long long start;
  int calls;
  int items;
  int open;
} EkPerfZone;

inline EkPerfZone ekm_perf_zone_zero(void) {
  EkPerfZone z = { 0, 0, 0, 0, 0 };
  return z;
}

inline void ekm_perf_zone_begin(EkPerfZone* z, long long now) {
  if (!z || z->open) return;
  z->start = now;
  z->open = 1;
}

// An end with no begin adds nothing: the sweep runs under __try and an unwind skips the end, so a lost pair costs one
// sample and never corrupts the total. A backwards span adds no time but counts the call (QPC can step back across
// cores).
inline void ekm_perf_zone_end(EkPerfZone* z, long long now, int items) {
  if (!z || !z->open) return;
  z->open = 0;
  const long long d = now - z->start;
  if (d > 0) z->ticks += d;
  z->calls++;
  z->items += items;
}

inline void ekm_perf_zone_add(EkPerfZone* z, long long ticks, int items) {
  if (!z) return;
  if (ticks > 0) z->ticks += ticks;
  z->calls++;
  z->items += items;
}

// One G_DrawRooms from four QPC stamps and the summed Q16 spans. calls == 0 means no Q16 call ran, so the whole span
// is head. An order no real frame has is refused rather than partitioned into nonsense.
typedef struct EkPerfPartition {
  long long head, draws, tail, whole;
  int ok;
} EkPerfPartition;

inline EkPerfPartition ekm_perf_partition(long long entry, long long firstIn,
                                          long long lastOut, long long exit,
                                          long long draws, int calls) {
  EkPerfPartition p = { 0, 0, 0, 0, 0 };
  if (entry <= 0 || exit < entry) return p;
  p.whole = exit - entry;
  if (calls <= 0) {
    p.head = p.whole;
    p.ok = 1;
    return p;
  }
  if (firstIn < entry || lastOut > exit || firstIn > lastOut) {
    p.whole = 0;
    return p;
  }
  p.head = firstIn - entry;
  p.tail = exit - lastOut;
  p.draws = draws > 0 ? draws : 0;
  p.ok = 1;
  return p;
}

inline long long ekm_perf_partition_gap(long long whole, long long head,
                                        long long draws, long long tail) {
  return whole - (head + draws + tail);
}

inline long long ekm_perf_span(long long in, long long out) {
  return (in > 0 && out > 0) ? out - in : 0;
}

inline int ekm_perf_kind_zone(int kind) {
  return (kind >= 0 && kind < 4) ? EK_PZ_CUT_DRAWALLS_PREPARE + kind
                                 : EK_PZ_CUT_DRAWALLS_OTHER;
}

inline long long ekm_perf_split_sum(long long p, long long m, long long e,
                                    long long o) {
  return p + m + e + o;
}

// A hook's own time, floored at 0: forwarded above total can only be a clock step.
inline long long ekm_perf_own(long long total, long long fwd) {
  if (total <= 0) return 0;
  if (fwd <= 0) return total;
  return fwd >= total ? 0 : total - fwd;
}

// The Q16 residual by bunch count. One sample per engine pass (its span less the drawalls and sweep.poly ticks inside
// it), filed by bunch count n. R/n^2 flat while R/n rises is the O(n^2) bunch-selection shape.
#define EK_PERF_Q16_BUCKETS 5

inline int ekm_perf_q16_bucket(int n) {
  if (n < 64) return 0;
  if (n < 256) return 1;
  if (n < 1024) return 2;
  if (n < 4096) return 3;
  return 4;
}
inline int ekm_perf_q16_bucket_lo(int b) {
  static const int lo[EK_PERF_Q16_BUCKETS] = { 0, 64, 256, 1024, 4096 };
  return (b >= 0 && b < EK_PERF_Q16_BUCKETS) ? lo[b] : 0;
}
inline int ekm_perf_q16_bucket_hi(int b) {
  return (b >= 0 && b < EK_PERF_Q16_BUCKETS - 1) ? ekm_perf_q16_bucket_lo(b + 1) : -1;
}

// numbunches <= numscans always (each bunch heads a scan), so the smaller of nocull's two undecided counter words is
// numbunches.
inline int ekm_perf_bunches(int a, int b) {
  const int m = a < b ? a : b;
  return m > 0 ? m : 0;
}

inline long long ekm_perf_q16_residual(long long span, long long dAlls,
                                       long long dSweep) {
  const long long r = span - dAlls - dSweep;
  return r > 0 ? r : 0;
}

typedef struct EkPerfQ16Bucket {
  long long calls, sumN, sumN2, sumR;
} EkPerfQ16Bucket;

inline EkPerfQ16Bucket ekm_perf_q16_bucket_zero(void) {
  EkPerfQ16Bucket b = { 0, 0, 0, 0 };
  return b;
}

inline void ekm_perf_q16_bank(EkPerfQ16Bucket* b, int n, long long r) {
  if (!b) return;
  if (n < 0) n = 0;
  EkPerfQ16Bucket* const k = &b[ekm_perf_q16_bucket(n)];
  k->calls++;
  k->sumN += n;
  k->sumN2 += (long long)n * (long long)n;
  if (r > 0) k->sumR += r;
}

// Where in a pass the residual sits. R was linear in n (~8.5 us a bunch); splitting at the drawalls stamps tells many
// small gaps (per-bunch CPU work) from one big gap at either end (a stall at a pass boundary).
#define EK_PERF_GAP_NONE    0
#define EK_PERF_GAP_PRE     1
#define EK_PERF_GAP_BETWEEN 2
#define EK_PERF_GAP_POST    3

inline const char* ekm_perf_gap_name(int kind) {
  static const char* const names[4] = { "none", "pre", "between", "post" };
  return (kind >= 0 && kind < 4) ? names[kind] : "?";
}

// R before the floor: the split's sum is checked against this, and a floor would hide the disagreement.
inline long long ekm_perf_q16_residual_raw(long long span, long long dAlls,
                                           long long dSweep) {
  return span - dAlls - dSweep;
}

typedef struct EkPerfQ16Split {
  long long pre, between, post;
  long long maxGap;
  long long lastRet;
  int maxKind;
  int drawn;
} EkPerfQ16Split;

inline EkPerfQ16Split ekm_perf_q16_split_zero(void) {
  EkPerfQ16Split s = { 0, 0, 0, 0, 0, EK_PERF_GAP_NONE, 0 };
  return s;
}

inline void ekm_perf_q16_split_gap(EkPerfQ16Split* s, long long g, int kind) {
  if (s->maxKind == EK_PERF_GAP_NONE || g > s->maxGap) {
    s->maxGap = g;
    s->maxKind = kind;
  }
}

// One drawalls call's entry and return stamps; `sweepPre` is sweep.poly's ticks since the pass opened, read at the
// first call.
inline void ekm_perf_q16_split_alls(EkPerfQ16Split* s, long long passIn,
                                    long long in, long long out, long long sweepPre) {
  if (!s) return;
  if (!s->drawn) {
    s->drawn = 1;
    s->pre = in - passIn - sweepPre;
    ekm_perf_q16_split_gap(s, s->pre, EK_PERF_GAP_PRE);
  } else {
    const long long g = in - s->lastRet;
    s->between += g;
    ekm_perf_q16_split_gap(s, g, EK_PERF_GAP_BETWEEN);
  }
  s->lastRet = out;
}

inline void ekm_perf_q16_split_close(EkPerfQ16Split* s, long long passIn,
                                     long long passOut, long long sweep) {
  if (!s) return;
  if (!s->drawn) {
    s->pre = passOut - passIn - sweep;
    ekm_perf_q16_split_gap(s, s->pre, EK_PERF_GAP_PRE);
  } else {
    s->post = passOut - s->lastRet;
    ekm_perf_q16_split_gap(s, s->post, EK_PERF_GAP_POST);
  }
}

inline long long ekm_perf_q16_split_sum(const EkPerfQ16Split* s) {
  return s ? s->pre + s->between + s->post : 0;
}

typedef struct EkPerfQ16SplitBucket {
  long long calls, sumN, sumPre, sumBetween, sumPost, sumRaw, sumMaxGap;
  long long wins[4];
} EkPerfQ16SplitBucket;

inline EkPerfQ16SplitBucket ekm_perf_q16_split_bucket_zero(void) {
  EkPerfQ16SplitBucket b = { 0, 0, 0, 0, 0, 0, 0, { 0, 0, 0, 0 } };
  return b;
}

inline void ekm_perf_q16_split_bank(EkPerfQ16SplitBucket* b, int n,
                                    const EkPerfQ16Split* s, long long raw) {
  if (!b || !s) return;
  if (n < 0) n = 0;
  EkPerfQ16SplitBucket* const k = &b[ekm_perf_q16_bucket(n)];
  k->calls++;
  k->sumN += n;
  k->sumPre += s->pre;
  k->sumBetween += s->between;
  k->sumPost += s->post;
  k->sumRaw += raw;
  k->sumMaxGap += s->maxGap;
  if (s->maxKind >= 0 && s->maxKind < 4) k->wins[s->maxKind]++;
}

inline int ekm_perf_q16_split_mode(const EkPerfQ16SplitBucket* k) {
  int best = EK_PERF_GAP_NONE;
  if (!k) return best;
  for (int i = EK_PERF_GAP_PRE; i <= EK_PERF_GAP_POST; i++)
    if (k->wins[i] > 0 && (best == EK_PERF_GAP_NONE || k->wins[i] > k->wins[best])) best = i;
  return best;
}

inline double ekm_perf_ratio_ns(long long ticks, long long denom, long long freq) {
  if (denom <= 0 || freq <= 0) return -1.0;
  return (double)ticks * 1.0e9 / (double)freq / (double)denom;
}

inline double ekm_perf_ms(long long ticks, long long freq) {
  return (freq > 0 && ticks > 0) ? (double)ticks * 1000.0 / (double)freq : 0.0;
}

typedef struct EkPerfRing {
  long long v[EK_PERF_RING];
  int cap;
  int count;
  int next;
} EkPerfRing;

inline EkPerfRing ekm_perf_ring_init(int cap) {
  EkPerfRing r;
  r.cap = (cap > 0 && cap <= EK_PERF_RING) ? cap : EK_PERF_RING;
  r.count = 0;
  r.next = 0;
  for (int i = 0; i < EK_PERF_RING; i++) r.v[i] = 0;
  return r;
}

inline void ekm_perf_ring_push(EkPerfRing* r, long long x) {
  if (!r || r->cap <= 0) return;
  r->v[r->next] = x;
  r->next = (r->next + 1) % r->cap;
  if (r->count < r->cap) r->count++;
}

// Nearest rank, p in permille so the index is exact integer arithmetic (ceil(p * n / 1000) - 1, clamped): no floating
// point, since an FP rounding tie has erased a pin before. -1 for an empty set, which is not index 0.
inline int ekm_perf_rank_index(int permille, int n) {
  if (n <= 0) return -1;
  if (permille <= 0) return 0;
  if (permille >= 1000) return n - 1;
  int idx = (permille * n + 999) / 1000 - 1;
  if (idx < 0) idx = 0;
  if (idx > n - 1) idx = n - 1;
  return idx;
}

inline long long ekm_perf_ring_select(const EkPerfRing* r, int permille,
                                      long long* scratch, int scratchLen) {
  if (!r || r->count <= 0 || !scratch || scratchLen < r->count) return 0;
  const int n = r->count;
  for (int i = 0; i < n; i++) scratch[i] = r->v[i];
  for (int i = 1; i < n; i++) {
    const long long k = scratch[i];
    int j = i - 1;
    while (j >= 0 && scratch[j] > k) { scratch[j + 1] = scratch[j]; j--; }
    scratch[j + 1] = k;
  }
  const int idx = ekm_perf_rank_index(permille, n);
  return (idx >= 0) ? scratch[idx] : 0;
}

inline long long ekm_perf_ring_max(const EkPerfRing* r) {
  if (!r || r->count <= 0) return 0;
  long long m = r->v[0];
  for (int i = 1; i < r->count; i++) if (r->v[i] > m) m = r->v[i];
  return m;
}

inline long long ekm_perf_ring_mean(const EkPerfRing* r) {
  if (!r || r->count <= 0) return 0;
  long long sum = 0;
  for (int i = 0; i < r->count; i++) sum += r->v[i];
  return sum / (long long)r->count;
}

inline long long ekm_perf_pair_cost_tenths(long long spanTicks, int pairs) {
  if (pairs <= 0 || spanTicks <= 0) return 0;
  return (spanTicks * 10) / (long long)pairs;
}

inline long long ekm_perf_overhead_ticks(long long pairCostTenths, int calls) {
  if (pairCostTenths <= 0 || calls <= 0) return 0;
  return (pairCostTenths * (long long)calls) / 10;
}

// A hand-edited ini can hold anything; keep only the four bits that mean something.
inline int ekm_perf_skip_sane(int mask) { return mask & EK_PERF_SKIP_ALL; }

// One named bit at a time: a query for anything else is refused, so a typo reads as "do not skip".
inline int ekm_perf_skips(int mask, int bit) {
  if (bit != EK_PERF_SKIP_SPRITES && bit != EK_PERF_SKIP_WALLS &&
      bit != EK_PERF_SKIP_UPDATES && bit != EK_PERF_SKIP_NOCULL) return 0;
  return (ekm_perf_skip_sane(mask) & bit) ? 1 : 0;
}

#ifndef EK_PERF_PURE_ONLY
void perf_zone_begin(int zone);
void perf_zone_end(int zone, int items);
long long perf_stamp(void);
void perf_zone_add(int zone, long long ticks, int items);
void perf_q16_bank(int bunches, long long residualTicks);
void perf_q16_bank_split(int bunches, const EkPerfQ16Split* split, long long rawTicks);
void perf_frame_end(void);
int  perf_detail2(void);
int  perf_skip_mask(void);
int  perf_ready(void);
long long perf_freq(void);
long long perf_zone_ticks(int zone);
int       perf_zone_calls(int zone);
int       perf_zone_items(int zone);
long long perf_zone_stat(int zone, int permille);
long long perf_zone_mean(int zone);
int       perf_zone_samples(int zone);
long long perf_overhead_ticks_now(void);
void      perf_draw(void);
#endif
