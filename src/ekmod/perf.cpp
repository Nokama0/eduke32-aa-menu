// The live instrument: clock, zone array, frame boundary and calibration. The rules are in perf.h; this file touches
// the process, so it is read rather than run.
#include <windows.h>
#include <cstdio>
#include "perf.h"
#include "config.h"
#include "log.h"
#include "ui/canvas.h"
#include "ui/font.h"

// One thread, hence plain ints: Group A runs inside EK_DrawRoomsDetour and Group B inside the SwapBuffers hook, both
// the game thread. The scan worker must never touch a zone.
static EkPerfZone s_zone[EK_PERF_ZONE_COUNT];
static EkPerfRing s_ring[EK_PERF_ZONE_COUNT];
static long long  s_freq = 0;
static int        s_ready = 0;
static int        s_tried = 0;
static long long  s_pairCostTenths = 0;
static int        s_calibrated = 0;
static int        s_frameCalls = 0;
static int        s_frameStamps = 0;
static unsigned   s_lastDumpMs = 0;

// The q16 residual over one five-second window, not a ring: the buckets are sums.
static EkPerfQ16Bucket s_q16[EK_PERF_Q16_BUCKETS];
static EkPerfQ16SplitBucket s_q16Split[EK_PERF_Q16_BUCKETS];
static int s_q16Frames = 0;
static int s_q16ThisFrame = 0;
static int s_q16Have = 0;

static long long perf_now(void) {
  LARGE_INTEGER n;
  if (!QueryPerformanceCounter(&n)) return 0;
  return (long long)n.QuadPart;
}

static void perf_init_once(void) {
  if (s_tried) return;
  s_tried = 1;
  LARGE_INTEGER f;
  if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) {
    ek_log("perf: QueryPerformanceFrequency failed, frame timings are off");
    return;
  }
  s_freq = (long long)f.QuadPart;
  for (int i = 0; i < EK_PERF_ZONE_COUNT; i++) {
    s_zone[i] = ekm_perf_zone_zero();
    s_ring[i] = ekm_perf_ring_init(EK_PERF_RING);
  }
  s_ready = 1;
}

int  perf_ready(void) { return s_ready; }
long long perf_freq(void) { return s_freq; }
// Gated on perfzones too: the sub-row's switch only dims when the parent is off, it does not clear, so without this
// the ~4600 QPC pairs a frame of detail-2 zones kept running after the parent went off.
int  perf_detail2(void) { return (g_cfg.perfzones && g_cfg.perfdetail2) ? 1 : 0; }
int  perf_skip_mask(void) { return ekm_perf_skip_sane(g_cfg.perfskip); }

void perf_zone_begin(int zone) {
  if (!s_ready || !ekm_perf_zone_admits(zone, perf_detail2())) return;
  ekm_perf_zone_begin(&s_zone[zone], perf_now());
}

void perf_zone_end(int zone, int items) {
  if (!s_ready || !ekm_perf_zone_admits(zone, perf_detail2())) return;
  ekm_perf_zone_end(&s_zone[zone], perf_now(), items);
  s_frameCalls++;
}

// Two stamps cost what one pair does: counted and folded into the instrument line at half weight.
long long perf_stamp(void) {
  if (!s_ready) return 0;
  s_frameStamps++;
  return perf_now();
}

void perf_zone_add(int zone, long long ticks, int items) {
  if (!s_ready || !ekm_perf_zone_admits(zone, perf_detail2())) return;
  ekm_perf_zone_add(&s_zone[zone], ticks, items);
}

void perf_q16_bank(int bunches, long long residualTicks) {
  if (!s_ready || !g_cfg.perfzones) return;
  ekm_perf_q16_bank(s_q16, bunches, residualTicks);
  s_q16ThisFrame = 1;
  s_q16Have = 1;
}

void perf_q16_bank_split(int bunches, const EkPerfQ16Split* split, long long rawTicks) {
  if (!s_ready || !g_cfg.perfzones) return;
  ekm_perf_q16_split_bank(s_q16Split, bunches, split, rawTicks);
  s_q16Have = 1;
}

static void perf_q16_clear(void) {
  for (int b = 0; b < EK_PERF_Q16_BUCKETS; b++) {
    s_q16[b] = ekm_perf_q16_bucket_zero();
    s_q16Split[b] = ekm_perf_q16_split_bucket_zero();
  }
  s_q16Frames = 0;
  s_q16ThisFrame = 0;
  s_q16Have = 0;
}

static void perf_q16_ratio(char* buf, size_t len, long long ticks, long long denom) {
  const double v = ekm_perf_ratio_ns(ticks, denom, s_freq);
  if (v < 0.0) snprintf(buf, len, "-");
  else snprintf(buf, len, "%.1f", v);
}

static double perf_signed_ms(long long t) {
  return t < 0 ? -ekm_perf_ms(-t, s_freq) : ekm_perf_ms(t, s_freq);
}

// One line per non-empty bucket, then the total against the zone means it should roughly match.
static void perf_q16_dump(void) {
  long long totalR = 0;
  for (int b = 0; b < EK_PERF_Q16_BUCKETS; b++) {
    const EkPerfQ16Bucket* const k = &s_q16[b];
    totalR += k->sumR;
    if (k->calls <= 0) continue;
    char hi[16], y[32], z[32];
    const int top = ekm_perf_q16_bucket_hi(b);
    if (top < 0) snprintf(hi, sizeof(hi), "inf");
    else snprintf(hi, sizeof(hi), "%d", top);
    perf_q16_ratio(y, sizeof(y), k->sumR, k->sumN2);
    perf_q16_ratio(z, sizeof(z), k->sumR, k->sumN);
    ek_log("perf: q16 residual n in [%d,%s): passes %lld, mean n %lld, mean R %.3f ms, "
           "R/n^2 %s ns, R/n %s ns",
           ekm_perf_q16_bucket_lo(b), hi, k->calls, k->sumN / k->calls,
           ekm_perf_ms(k->sumR / k->calls, s_freq), y, z);
  }
  for (int b = 0; b < EK_PERF_Q16_BUCKETS; b++) {
    const EkPerfQ16SplitBucket* const k = &s_q16Split[b];
    if (k->calls <= 0) continue;
    char hi[16], y[32];
    const int top = ekm_perf_q16_bucket_hi(b);
    if (top < 0) snprintf(hi, sizeof(hi), "inf");
    else snprintf(hi, sizeof(hi), "%d", top);
    const double perN = ekm_perf_ratio_ns(k->sumBetween, k->sumN, s_freq);
    if (perN < 0.0 || k->sumBetween < 0) snprintf(y, sizeof(y), "-");
    else snprintf(y, sizeof(y), "%.2f", perN / 1000.0);
    const long long c = k->calls;
    ek_log("perf: q16 split n in [%d,%s): pre %.3f ms, between %.3f ms, post %.3f ms "
           "(sum %.3f vs R %.3f), max gap %.3f ms (%s), between/n %s us",
           ekm_perf_q16_bucket_lo(b), hi, perf_signed_ms(k->sumPre / c),
           perf_signed_ms(k->sumBetween / c), perf_signed_ms(k->sumPost / c),
           perf_signed_ms((k->sumPre + k->sumBetween + k->sumPost) / c),
           perf_signed_ms(k->sumRaw / c), perf_signed_ms(k->sumMaxGap / c),
           ekm_perf_gap_name(ekm_perf_q16_split_mode(k)), y);
  }
  const long long perFrame = s_q16Frames > 0 ? totalR / s_q16Frames : 0;
  const long long d = perf_zone_mean(EK_PZ_ENGINE_DRAWS);
  const long long a = perf_zone_mean(EK_PZ_CUT_DRAWALLS);
  const long long s = perf_zone_mean(EK_PZ_SWEEP_POLY);
  const long long t = d - a - s;
  ek_log("perf: q16 residual total R %.2f ms vs engine.draws - cut.drawalls - "
         "sweep.poly = %.2f ms (per frame, over %d frame(s))",
         ekm_perf_ms(perFrame, s_freq),
         t < 0 ? -ekm_perf_ms(-t, s_freq) : ekm_perf_ms(t, s_freq), s_q16Frames);
}

long long perf_zone_ticks(int zone) {
  return (s_ready && zone >= 0 && zone < EK_PERF_ZONE_COUNT) ? s_zone[zone].ticks : 0;
}

int perf_zone_calls(int zone) {
  return (s_ready && zone >= 0 && zone < EK_PERF_ZONE_COUNT) ? s_zone[zone].calls : 0;
}

int perf_zone_items(int zone) {
  return (s_ready && zone >= 0 && zone < EK_PERF_ZONE_COUNT) ? s_zone[zone].items : 0;
}

long long perf_zone_stat(int zone, int permille) {
  if (!s_ready || zone < 0 || zone >= EK_PERF_ZONE_COUNT) return 0;
  // Own buffer rather than a shared static: runs once per report, 2 KB of stack beats a lifetime question.
  long long scratch[EK_PERF_RING];
  return ekm_perf_ring_select(&s_ring[zone], permille, scratch, EK_PERF_RING);
}

long long perf_zone_mean(int zone) {
  return (s_ready && zone >= 0 && zone < EK_PERF_ZONE_COUNT)
             ? ekm_perf_ring_mean(&s_ring[zone]) : 0;
}

// Ring depth, not this frame's call count: a zone admitted only some frames can show a p99 over a handful of samples,
// and a reader needs the depth to judge it.
int perf_zone_samples(int zone) {
  return (s_ready && zone >= 0 && zone < EK_PERF_ZONE_COUNT)
             ? s_ring[zone].count : 0;
}

static int perf_frame_pairs(void) { return s_frameCalls + (s_frameStamps + 1) / 2; }

long long perf_overhead_ticks_now(void) {
  return ekm_perf_overhead_ticks(s_pairCostTenths, perf_frame_pairs());
}

// 4096 empty pairs, once, on the first frame the readout is on (never at DLL load: 0.1 ms is a visible hitch). Timed
// through the live entry points, not the inline pure ones: build.ps1 has no /GL, so the real path pays a cross-TU
// call plus the admits gate, and the inline helpers understated the cost by 10-20%.
static void perf_calibrate_once(void) {
  if (s_calibrated || !s_ready) return;
  s_calibrated = 1;
  const int savedCalls = s_frameCalls;
  // Saved, not zeroed: drawrooms is bracketed with EK_PZ_ENGINE and perf_frame_end runs after that bracket recorded
  // the real sample, so zeroing would feed the engine ring a false zero. Do not simplify back to a zero.
  const EkPerfZone savedZone = s_zone[EK_PZ_ENGINE];
  const long long t0 = perf_now();
  for (int i = 0; i < 4096; i++) {
    perf_zone_begin(EK_PZ_ENGINE);
    perf_zone_end(EK_PZ_ENGINE, 0);
  }
  const long long t1 = perf_now();
  // Own statement: an out-param is never read in the argument list that fills it.
  const long long span = t1 - t0;
  s_pairCostTenths = ekm_perf_pair_cost_tenths(span, 4096);
  s_zone[EK_PZ_ENGINE] = savedZone;
  s_frameCalls = savedCalls;
  ek_log("perf: one zone pair costs %lld/10 ticks (%.4f ms per 1000 pairs)",
         s_pairCostTenths, ekm_perf_ms(s_pairCostTenths * 100, s_freq));
}

// The frame boundary: ONE call site, in ek_SwapBuffers, which has no early return. overlay_on_swap has four, two of
// them on every panel-shut frame, so a boundary there would publish some frames and not others.
void perf_frame_end(void) {
  perf_init_once();
  if (!s_ready) return;
  if (g_cfg.perfzones) perf_calibrate_once();
  const int detail2 = perf_detail2();
  if (s_q16ThisFrame) { s_q16Frames++; s_q16ThisFrame = 0; }
  if (!g_cfg.perfzones && s_q16Have) perf_q16_clear();
  // Every five seconds while the row is on, so the log can be pasted. Above the zeroing loop: below it every value
  // reads zero.
  if (g_cfg.perfzones) {
    const unsigned now = GetTickCount();
    if ((unsigned)(now - s_lastDumpMs) >= 5000u) {
      s_lastDumpMs = now;
      ek_log("perf: --- frame timings, mean/p50/p99/max ms ---");
      for (int i = 0; i < EK_PERF_ZONE_COUNT; i++) {
        if (!ekm_perf_zone_admits(i, detail2)) continue;
        // calls = begin/end pairs this frame, items = what the zone counts this frame, samples = ring depth of the
        // stats.
        ek_log("perf: %-20s %6.2f %6.2f %6.2f %6.2f  calls %d items %d samples %d",
               ekm_perf_zone_name(i),
               ekm_perf_ms(perf_zone_mean(i), s_freq),
               ekm_perf_ms(perf_zone_stat(i, 500), s_freq),
               ekm_perf_ms(perf_zone_stat(i, 990), s_freq),
               ekm_perf_ms(perf_zone_stat(i, 1000), s_freq),
               perf_zone_calls(i), perf_zone_items(i), perf_zone_samples(i));
        // Beside engine: the total next to its parts, from means (which add where p50s do not). The four zones bank
        // together on every G_DrawRooms, so their rings hold the same frames.
        if (i == EK_PZ_ENGINE) {
          const long long h = perf_zone_mean(EK_PZ_ENGINE_HEAD);
          const long long d = perf_zone_mean(EK_PZ_ENGINE_DRAWS);
          const long long t = perf_zone_mean(EK_PZ_ENGINE_TAIL);
          const long long e = perf_zone_mean(EK_PZ_ENGINE_FRAME);
          const long long gap = ekm_perf_partition_gap(e, h, d, t);
          const double gapMs = gap < 0 ? -ekm_perf_ms(-gap, s_freq)
                                       : ekm_perf_ms(gap, s_freq);
          ek_log("perf: engine partition: head %.2f + draws %.2f + tail %.2f = "
                 "%.2f vs engine.frame %.2f (diff %.2f ms: between Q16 calls, "
                 "e.g. yax_drawrooms' bunch scans + per-bunch masks, mirror passes)",
                 ekm_perf_ms(h, s_freq), ekm_perf_ms(d, s_freq),
                 ekm_perf_ms(t, s_freq), ekm_perf_ms(h + d + t, s_freq),
                 ekm_perf_ms(e, s_freq), gapMs);
        }
        if (i == EK_PZ_CUT_DRAWALLS_OTHER) {
          const long long p = perf_zone_mean(EK_PZ_CUT_DRAWALLS_PREPARE);
          const long long m = perf_zone_mean(EK_PZ_CUT_DRAWALLS_MAIN);
          const long long x = perf_zone_mean(EK_PZ_CUT_DRAWALLS_EXTRA);
          const long long o = perf_zone_mean(EK_PZ_CUT_DRAWALLS_OTHER);
          ek_log("perf: cut.drawalls split: prepare %.2f + main %.2f + extra %.2f + "
                 "other %.2f = %.2f vs %.2f ms",
                 ekm_perf_ms(p, s_freq), ekm_perf_ms(m, s_freq),
                 ekm_perf_ms(x, s_freq), ekm_perf_ms(o, s_freq),
                 ekm_perf_ms(ekm_perf_split_sum(p, m, x, o), s_freq),
                 ekm_perf_ms(perf_zone_mean(EK_PZ_CUT_DRAWALLS), s_freq));
        }
        if (i == EK_PZ_CUT_DRAWALLS_THUNK) {
          const long long t = perf_zone_mean(EK_PZ_CUT_DRAWALLS_THUNK);
          const long long o = perf_zone_mean(EK_PZ_CUT_DRAWALLS);
          ek_log("perf: cut.drawalls thunk %.2f - orig %.2f = own %.2f ms%s",
                 ekm_perf_ms(t, s_freq), ekm_perf_ms(o, s_freq),
                 ekm_perf_ms(ekm_perf_own(t, o), s_freq),
                 o > t ? " (orig above thunk: a clock step, floored at 0)" : "");
        }
        if (i == EK_PZ_CUT_DOMOST_FWD || i == EK_PZ_CUT_NONPAR_FWD) {
          const int whole = i == EK_PZ_CUT_DOMOST_FWD ? EK_PZ_CUT_DOMOST : EK_PZ_CUT_NONPAR;
          const long long t = perf_zone_mean(whole);
          const long long f = perf_zone_mean(i);
          ek_log("perf: %s own = total %.2f - forwarded %.2f = %.2f ms%s",
                 ekm_perf_zone_name(whole), ekm_perf_ms(t, s_freq),
                 ekm_perf_ms(f, s_freq), ekm_perf_ms(ekm_perf_own(t, f), s_freq),
                 f > t ? " (forwarded above total: a clock step, floored at 0)" : "");
        }
        if (i == EK_PZ_CUT_NONPAR_FWD) {
          perf_q16_dump();
          perf_q16_clear();
        }
      }
      // Named bits, not the bare mask: the log is read without the game running, where a raw set bit reads as noise.
      // Omitted at the default mask of 0.
      const int skip = perf_skip_mask();
      if (skip) {
        char skipbuf[64];
        snprintf(skipbuf, sizeof(skipbuf), "%s%s%s%s",
                 ekm_perf_skips(skip, EK_PERF_SKIP_SPRITES) ? "sprites " : "",
                 ekm_perf_skips(skip, EK_PERF_SKIP_WALLS) ? "walls " : "",
                 ekm_perf_skips(skip, EK_PERF_SKIP_UPDATES) ? "updates " : "",
                 ekm_perf_skips(skip, EK_PERF_SKIP_NOCULL) ? "nocull " : "");
        ek_log("perf: instrument %.3f ms over %d pairs, perfskip %d: %s",
               ekm_perf_ms(perf_overhead_ticks_now(), s_freq), perf_frame_pairs(),
               skip, skipbuf);
      } else {
        ek_log("perf: instrument %.3f ms over %d pairs, perfskip none",
               ekm_perf_ms(perf_overhead_ticks_now(), s_freq), perf_frame_pairs());
      }
    }
  }
  for (int i = 0; i < EK_PERF_ZONE_COUNT; i++) {
    // Push only a zone that was admitted AND ran this frame (calls > 0): admitted is not entered, and a skipped
    // bracket (early orig return, sweep bailing) would bank a zero and dilute the ring for up to 256 frames.
    if (ekm_perf_zone_admits(i, detail2) && s_zone[i].calls > 0)
      ekm_perf_ring_push(&s_ring[i], s_zone[i].ticks);
    s_zone[i] = ekm_perf_zone_zero();
  }
  s_frameCalls = 0;
  s_frameStamps = 0;
}

// Excluded from its own measurement: no zone brackets anything below, or the readout would report a cost that exists
// only while someone looks.
void perf_draw(void) {
  if (!g_cfg.perfzones || !g_cfg.perftable || !s_ready) return;
  const int detail2 = perf_detail2();
  int y = 8;
  char line[128];
  snprintf(line, sizeof(line),
           "zone                  mean    p50    p99    max   calls   items  samples");
  canvas_text(8, y, line, C_BONEFNT, FONT_MONO);
  y += 12;
  for (int i = 0; i < EK_PERF_ZONE_COUNT; i++) {
    if (!ekm_perf_zone_admits(i, detail2)) continue;
    // calls = begin/end pairs this frame, items = what the zone counts this frame, samples = ring depth of the stats.
    snprintf(line, sizeof(line), "%-20s %6.2f %6.2f %6.2f %6.2f %7d %7d %8d",
             ekm_perf_zone_name(i),
             ekm_perf_ms(perf_zone_mean(i), s_freq),
             ekm_perf_ms(perf_zone_stat(i, 500), s_freq),
             ekm_perf_ms(perf_zone_stat(i, 990), s_freq),
             ekm_perf_ms(perf_zone_stat(i, 1000), s_freq),
             perf_zone_calls(i), perf_zone_items(i), perf_zone_samples(i));
    canvas_text(8, y, line, C_BONEFNT, FONT_MONO);
    y += 12;
  }
  // Shows what the instrument itself cost, so the number can be checked.
  snprintf(line, sizeof(line), "instrument %.3f ms over %d pairs",
           ekm_perf_ms(perf_overhead_ticks_now(), s_freq), perf_frame_pairs());
  canvas_text(8, y, line, C_BONEDIM, FONT_MONO);
  y += 12;
  const int skip = perf_skip_mask();
  if (skip) {
    snprintf(line, sizeof(line),
             "perfskip %d: the map is drawn wrong on purpose", skip);
    canvas_text(8, y, line, C_HAZARD, FONT_MONO);
  }
}
