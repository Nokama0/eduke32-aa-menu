// The aim arithmetic, and nothing else. Header-only and engine-free on purpose: the test host links it with no DLL
// and no game, which is the only reason these rules can be checked before a frame is ever drawn.

#pragma once
#include <math.h>
#include "projmath.h"

const int EKM_YAW_UNITS   = 2048;
const int EKM_HORIZ_MIN   = -99;
const int EKM_HORIZ_MAX   = 299;
const int EKM_HORIZ_LEVEL = 100;

// The run under horiz's rise. P_UpdateAngles forms the view pitch as atan2f(q16horiz - F16(100), F16(128))
// (player.cpp:3084), so horiz is F16(100) + F16(128) * tan(pitch) and 128 is the denominator of that tangent.
const int EKM_HORIZ_PER_SLOPE = 128;
const int EKM_UNITS_PER_METRE = 512;

// The engine's tic, in milliseconds: actors move 30 times a second. Every rate in this file is per tic, not per
// frame; a frame means nothing to the simulation (two bugs in this port came from treating them as one).
constexpr double EKM_MS_PER_TIC = 1000.0 / 30.0;

constexpr int EKM_LEAD_TICS_MAX = 4;

// Acquisition runs once per tic, not once per rendered frame (aim_frame is called from the SwapBuffers hook, so the
// candidate loop ran two to four times a tic asking the same questions about actors that had not moved).
constexpr unsigned EKM_ACQUIRE_GAP_MS = (unsigned)EKM_MS_PER_TIC;

// `everAcquired` false is the first frame of the session, which must acquire whatever the clock says. Unsigned, so a
// wrapped or future stamp reads as enormous and acquires: this fails toward doing the work.
inline bool ekm_acquire_due(bool everAcquired, unsigned msSinceAcquire) {
  if (!everAcquired) return true;
  return msSinceAcquire >= EKM_ACQUIRE_GAP_MS;
}

const unsigned EKM_VEL_STALE_MS = 83;

// The longest gap a position delta may span and still be motion: EKM_MS_PER_TIC * EKM_LEAD_TICS_MAX. Past it the
// delta is discarded rather than divided (ekm_lead_divisor). Derived from the tic, not the other way round.
constexpr unsigned EKM_LEAD_GAP_MAX_MS =
    (unsigned)(EKM_MS_PER_TIC * EKM_LEAD_TICS_MAX + 0.5);

inline int ekm_f16(int whole) { return whole * 65536; }

// Build's angle, matching getangle, which reads a table built from atanf; this computes the same directly (at most
// one unit of 2048 apart, under a fifth of a degree).
inline int ekm_aim_yaw(int dx, int dy) {
  if (dx == 0 && dy == 0) return 0;
  const double a = atan2((double)dy, (double)dx) * (EKM_YAW_UNITS / 2) / 3.14159265358979323846;
  int v = (int)(a + (a >= 0 ? 0.5 : -0.5));
  v &= (EKM_YAW_UNITS - 1);
  return v;
}

inline int ekm_yaw_delta(int from, int to) {
  int d = (to - from) & (EKM_YAW_UNITS - 1);
  if (d > EKM_YAW_UNITS / 2) d -= EKM_YAW_UNITS;
  return d;
}

// The short way round in 16.16 over the full circle. The delta cannot be taken in whole units and widened: that threw
// away the fraction the player's mouse left in q16ang and then added it straight back to the written angle.
const int EKM_YAW_UNITS_Q16 = EKM_YAW_UNITS * 65536;

inline int ekm_yaw_delta_q16(int fromQ16, int toQ16) {
  const unsigned d = ((unsigned)toQ16 - (unsigned)fromQ16) &
                     ((unsigned)EKM_YAW_UNITS_Q16 - 1u);
  return (int)d > EKM_YAW_UNITS_Q16 / 2 ? (int)d - EKM_YAW_UNITS_Q16 : (int)d;
}

inline int ekm_angular_offset(int a, int b) {
  const int d = ekm_yaw_delta(a, b);
  return d < 0 ? -d : d;
}

// q16horiz for a target dz below the eye at dist2d. P_UpdateAngles treats horiz as a slope: the view pitch is
// atan2f(q16horiz - F16(100), F16(128)), so horiz is F16(100) + F16(128) * tan(pitch), and tan(pitch) is rise over
// run.
inline int ekm_q16horiz_for(int dz, int dist2d) {
  if (dist2d <= 0) return ekm_f16(EKM_HORIZ_LEVEL);
  const double slope = (-(double)dz / (double)EKM_Z_PER_XY) / (double)dist2d;
  const double v = (double)ekm_f16(EKM_HORIZ_LEVEL)
                 + (double)EKM_HORIZ_PER_SLOPE * 65536.0 * slope;
  if (v > 2147483000.0) return 2147483000;
  if (v < -2147483000.0) return -2147483000;
  return (int)v;
}

// The view's whole answer about one target, from one snapshot: the triggerbot compares the player's own yaw with the
// direction to the target, which is only honest if both describe the same instant.
struct EkAimViewPair {
  int myYaw;
  int yaw;
  int q16horiz;
  int dist2d;
};

inline EkAimViewPair ekm_aim_view_pair(int meAngQ16, int meHorizoff,
                                       int eyeX, int eyeY, int eyeZ,
                                       int tx, int ty, int tz,
                                       int lastYaw, int lastDist) {
  EkAimViewPair p;
  p.myYaw = (meAngQ16 >> 16) & (EKM_YAW_UNITS - 1);
  p.yaw = lastYaw;
  p.dist2d = lastDist;
  const int dx = tx - eyeX, dy = ty - eyeY;
  if (dx != 0 || dy != 0) {
    p.dist2d = (int)sqrt((double)dx * dx + (double)dy * dy);
    p.yaw = ekm_aim_yaw(dx, dy);
  }
  // horizoff is subtracted because the engine adds it back (effective pitch is q16horiz plus q16horizoff).
  p.q16horiz = ekm_q16horiz_for(tz - eyeZ, p.dist2d) - meHorizoff;
  return p;
}

// How far one whole horiz unit moves the impact at a given 2D range. The shot's pitch is not the view's:
// P_PreFireHitscan builds it as fix16_to_int(F16(100) - q16horiz - q16horizoff) << 5, and fix16_to_int rounds to a
// whole horiz unit (the chaingun and shotgun path uses the same expression).
inline double ekm_pitch_quantum_z(int range2d) {
  if (range2d <= 0) return 0.0;
  return (double)range2d * (double)EKM_Z_PER_XY / (double)EKM_HORIZ_PER_SLOPE;
}

// Which of two candidates acquisition prefers. A rule because two accumulators run it: the winner and, beside it, the
// best candidate the view can be pointed at (aim.cpp).
inline bool ekm_cand_better(bool have, bool crosshair, int tier, int off,
                            int dist, int bestTier, int bestOff, int bestDist) {
  if (!have || tier < bestTier) return true;
  if (tier != bestTier) return false;
  return crosshair ? (off < bestOff || (off == bestOff && dist < bestDist))
                   : (dist < bestDist);
}

// Reach is the first key: a candidate a shot can reach beats one it cannot at any tier, offset or range; equal reach
// is the order above, so with none or all reachable the winner is unchanged. `reach` is nonzero for reachable or
// unknown.
inline bool ekm_cand_better(bool have, bool crosshair, int reach, int tier,
                            int off, int dist, int bestReach, int bestTier,
                            int bestOff, int bestDist) {
  if (!have) return true;
  if ((reach != 0) != (bestReach != 0)) return reach != 0;
  return ekm_cand_better(true, crosshair, tier, off, dist, bestTier, bestOff,
                         bestDist);
}

inline void ekm_bit_set(unsigned char* map, int bits, int i) {
  if (map && i >= 0 && i < bits) map[i >> 3] |= (unsigned char)(1u << (i & 7));
}
inline bool ekm_bit_test(const unsigned char* map, int bits, int i) {
  return map && i >= 0 && i < bits && ((map[i >> 3] >> (i & 7)) & 1u) != 0;
}

// What the view can hold: P_GetInput clamps q16horiz to EKM_HORIZ_MIN..MAX once per tic, so the aimbot stays gated on
// it (steering to a clamped pitch would park the view at the limit instead of turning).
inline bool ekm_horiz_reachable(int q16horiz) {
  return q16horiz >= ekm_f16(EKM_HORIZ_MIN) && q16horiz <= ekm_f16(EKM_HORIZ_MAX);
}

const int EKM_SHOT_ZVEL_WHOLE_MAX = 4096;

const int EKM_SHOT_HORIZ_MIN = EKM_HORIZ_LEVEL - EKM_SHOT_ZVEL_WHOLE_MAX;
const int EKM_SHOT_HORIZ_MAX = EKM_HORIZ_LEVEL + EKM_SHOT_ZVEL_WHOLE_MAX;

// What the shot can reach. Acquisition uses this one unconditionally: turning silent aim on must not change which
// enemy the aimbot picks. Whether the shot or the view may then act on the pitch is decided afterwards, per feature.
inline bool ekm_shot_horiz_reachable(int q16horiz) {
  return q16horiz >= ekm_f16(EKM_SHOT_HORIZ_MIN) &&
         q16horiz <= ekm_f16(EKM_SHOT_HORIZ_MAX);
}

// The shot's angle, solved from the muzzle it will actually leave: an angle is only valid for the position it was
// solved from, and solving on the draw left it stale the instant the player moved.
struct EkShotSolve {
  int yaw;
  int q16horiz;
  int ok;
  int usedLed;
};

inline EkShotSolve ekm_shot_solve(int muzzleX, int muzzleY, int muzzleZ,
                                  int ledX, int ledY, int ledZ,
                                  int rawX, int rawY, int rawZ,
                                  int q16horizoff) {
  const int rdx = rawX - muzzleX, rdy = rawY - muzzleY;
  int rawYaw = 0, rawDist = 0;
  if (rdx != 0 || rdy != 0) {
    rawDist = (int)sqrt((double)rdx * rdx + (double)rdy * rdy);
    rawYaw = ekm_aim_yaw(rdx, rdy);
  }

  const int sdx = ledX - muzzleX, sdy = ledY - muzzleY;
  int yaw = rawYaw, dist = rawDist;
  if (sdx != 0 || sdy != 0) {
    dist = (int)sqrt((double)sdx * sdx + (double)sdy * sdy);
    yaw = ekm_aim_yaw(sdx, sdy);
  }

  EkShotSolve out;
  out.usedLed = 1;
  out.q16horiz = ekm_q16horiz_for(ledZ - muzzleZ, dist) - q16horizoff;
  if (!ekm_shot_horiz_reachable(out.q16horiz)) {
    out.q16horiz = ekm_q16horiz_for(rawZ - muzzleZ, rawDist) - q16horizoff;
    yaw = rawYaw;
    out.usedLed = 0;
  }
  out.yaw = yaw;
  out.ok = ekm_shot_horiz_reachable(out.q16horiz) ? 1 : 0;
  return out;
}

inline int ekm_smooth_step(int delta, int percent) {
  if (delta == 0) return 0;
  if (percent <= 0) return delta;
  if (percent >= 100) return 0;
  int step = (int)((long long)delta * (100 - percent) / 100);
  if (step == 0) step = delta > 0 ? 1 : -1;
  return step;
}

// A point on the target's body. Rung 0 is the row's own choice; the rest are a fixed ladder tried in order when it is
// not visible, which is why enemies behind cover are still shootable.
const float EKM_HEAD_FRAC = 0.75f;
const int EKM_AIM_RUNGS_MAX = 6;

inline float ekm_aim_rung_frac(int rung, int aimPoint,
                               float headFrac = EKM_HEAD_FRAC, float bandFloor = 0.0f) {
  if (rung < 0) return -1.0f;
  if (!(aimPoint && bandFloor > 0.0f)) {
    // Four rungs must be four heights: a fixed h*0.5 was rung 0 whenever the row is on Centre (the default), so the
    // default asked the same question twice and wasted a quarter of the probe budget.
    switch (rung) {
      case 0:  return aimPoint ? headFrac : 0.5f;
      case 1:  return 0.875f;
      case 2:  return aimPoint ? 0.5f : headFrac;
      case 3:  return 0.25f;
      default: return -1.0f;
    }
  }
  const float all[EKM_AIM_RUNGS_MAX] = { headFrac, bandFloor, 0.875f, EKM_HEAD_FRAC,
                                         0.5f, 0.25f };
  int n = 0;
  for (int i = 0; i < EKM_AIM_RUNGS_MAX; i++) {
    bool dup = false;
    for (int j = 0; j < i; j++) if (all[j] == all[i]) dup = true;
    if (dup) continue;
    if (n == rung) return all[i];
    n++;
  }
  return -1.0f;
}

inline int ekm_aim_rung_count(int aimPoint, float headFrac = EKM_HEAD_FRAC,
                              float bandFloor = 0.0f) {
  int n = 0;
  while (n < EKM_AIM_RUNGS_MAX && ekm_aim_rung_frac(n, aimPoint, headFrac, bandFloor) >= 0.0f)
    n++;
  return n;
}

inline int ekm_aim_z(int feetZ, float heightZ, int rung, int aimPoint,
                     float headFrac = EKM_HEAD_FRAC, float bandFloor = 0.0f) {
  const float h = heightZ < 0.0f ? 0.0f : heightZ;
  float f = ekm_aim_rung_frac(rung, aimPoint, headFrac, bandFloor);
  if (f < 0.0f) f = 0.25f;
  return feetZ - (int)(h * f);
}

// Span refinement: the midpoint of the longest unbroken visible span. The ladder above returns the first rung that
// answers, which is the worst point on the body.

// Nine samples over 0.125..0.875, not the whole body: the feet at 0.0 sit in the floor plane and the top pixel at 1.0
// is the silhouette edge above the head, points a probe answers for and a bullet misses past.
const int EKM_SPAN_SAMPLES = 9;

// The span refinement has its own budget, not the selection budget (it was once gated on the candidate loop's
// 64-probe budget).
const int EKM_SPAN_BUDGET = EKM_SPAN_SAMPLES;

// Whether one span sample is a point the bullet reaches: the ray that would be fired at it is cast, and it counts
// only when that ray lands on the target sprite itself.
inline bool ekm_span_sample_hit(bool castOk, int hitSprite, int targetSprite) {
  return castOk && targetSprite >= 0 && hitSprite == targetSprite;
}
const float EKM_SPAN_LO = 0.125f;
const float EKM_SPAN_HI = 0.875f;
const float EKM_SPAN_STEP =
    (EKM_SPAN_HI - EKM_SPAN_LO) / (float)(EKM_SPAN_SAMPLES - 1);

// The sample that is the Centre/Head row's own height (ekm_aim_z's rung 0). It is what the refinement keeps whenever
// the engine can see it; the midpoint rule is a fallback from it, not a tie-break between runs.
inline int ekm_span_prefer(int aimPoint, float headFrac = EKM_HEAD_FRAC) {
  const float want = aimPoint ? headFrac : 0.5f;
  int idx = (int)((want - EKM_SPAN_LO) / EKM_SPAN_STEP + 0.5f);
  if (idx < 0) idx = 0;
  if (idx >= EKM_SPAN_SAMPLES) idx = EKM_SPAN_SAMPLES - 1;
  return idx;
}

// Where sample `i` sits as a fraction of body height, clamped. The row's own sample is the row's own height exactly.
inline float ekm_span_frac(int sample, int aimPoint = 0, float headFrac = EKM_HEAD_FRAC,
                           float bandFloor = 0.0f) {
  const int i = sample < 0 ? 0 : (sample >= EKM_SPAN_SAMPLES ? EKM_SPAN_SAMPLES - 1 : sample);
  if (i == ekm_span_prefer(aimPoint, headFrac)) return aimPoint ? headFrac : 0.5f;
  const float grid = EKM_SPAN_LO + (float)i * EKM_SPAN_STEP;
  if (aimPoint && bandFloor > 0.0f && i == ekm_span_prefer(1) && grid < bandFloor)
    return EKM_HEAD_FRAC;
  return grid;
}

inline int ekm_span_z(int feetZ, float heightZ, int sample, int aimPoint = 0,
                      float headFrac = EKM_HEAD_FRAC, float bandFloor = 0.0f) {
  const float h = heightZ < 0.0f ? 0.0f : heightZ;
  return feetZ - (int)(h * ekm_span_frac(sample, aimPoint, headFrac, bandFloor));
}

// The z between a sample and the nearer edge of its run: how much room a shot has before it leaves what the engine
// can see. Fractions come from ekm_span_frac so the row's snapped sample is measured where it is.
inline double ekm_span_margin_z(int sample, int lo, int hi, double heightZ,
                                int aimPoint = 0, float headFrac = EKM_HEAD_FRAC,
                                float bandFloor = 0.0f) {
  const double h = heightZ > 0.0 ? heightZ : 0.0;
  const double f = (double)ekm_span_frac(sample, aimPoint, headFrac, bandFloor);
  const double half = (double)EKM_SPAN_STEP * 0.5;
  const double dLo = f - (double)ekm_span_frac(lo, aimPoint, headFrac, bandFloor) + half;
  const bool toTop = aimPoint && bandFloor > 0.0f && hi >= EKM_SPAN_SAMPLES - 1;
  const double dHi = toTop ? 1.0 - f
                           : (double)ekm_span_frac(hi, aimPoint, headFrac, bandFloor) - f + half;
  double d = dLo < dHi ? dLo : dHi;
  if (d < 0.0) d = 0.0;
  return d * h;
}

// The sample to aim at; -1 when none could be seen, which the caller reads as keep the answer you have (the first
// test already proved the enemy visible, and a refinement that can lose a lock is a regression).
inline int ekm_span_mid(const unsigned char* vis, int n, int preferIdx,
                        int* first = 0, int* last = 0,
                        float heightZ = 0.0f, int range2d = 0,
                        int aimPoint = 0, int* tight = 0,
                        float headFrac = EKM_HEAD_FRAC, float bandFloor = 0.0f) {
  const double quantum = ekm_pitch_quantum_z(range2d);
  const double h = heightZ > 0.0f ? (double)heightZ : 0.0;
  int bestMid = -1, bestLen = 0, bestDist = 0, bestLo = 0, bestHi = 0;
  if (vis) {
    for (int i = 0; i < n; ) {
      if (!vis[i]) { i++; continue; }
      const int lo = i;
      while (i < n && vis[i]) i++;
      const int hi = i - 1;
      const int len = hi - lo + 1;
      // The row's own point when this run holds it AND has a whole horiz unit of room around it, the run's midpoint
      // otherwise; asked per run so the tie-break compares the answers the runs would give.
      const bool holds = preferIdx >= lo && preferIdx <= hi;
      const bool room =
          holds && ekm_span_margin_z(preferIdx, lo, hi, h, aimPoint, headFrac,
                                     bandFloor) >= quantum;
      const int mid = room ? preferIdx : (lo + hi + 1) / 2;
      const int d = mid > preferIdx ? mid - preferIdx : preferIdx - mid;
      if (bestMid < 0 || len > bestLen || (len == bestLen && d < bestDist)) {
        bestMid = mid; bestLen = len; bestDist = d; bestLo = lo; bestHi = hi;
      }
    }
  }
  if (first) *first = bestMid < 0 ? -1 : bestLo;
  if (last)  *last  = bestMid < 0 ? -1 : bestHi;
  // The best point available even with no margin; the caller logs that rather than silently aim at something the
  // engine cannot be pointed at. An int, not a bool, like every flag here.
  if (tight)
    *tight = (bestMid >= 0 && quantum > 0.0 &&
              ekm_span_margin_z(bestMid, bestLo, bestHi, h, aimPoint, headFrac,
                                bandFloor) < quantum)
                 ? 1 : 0;
  return bestMid;
}

// Span refinement with AA's headshot band: only samples at or above `bandFloor` may answer, so the longest run cannot
// drag Head down to the legs under a blocked chest.
inline int ekm_span_mid_banded(const unsigned char* vis, int n, float bandFloor,
                               int* inBand, int* first = 0, int* last = 0,
                               float heightZ = 0.0f, int range2d = 0,
                               int aimPoint = 0, int* tight = 0,
                               float headFrac = EKM_HEAD_FRAC) {
  if (inBand) *inBand = 0;
  if (vis && aimPoint && bandFloor > 0.0f && n > 0 && n <= EKM_SPAN_SAMPLES) {
    unsigned char band[EKM_SPAN_SAMPLES];
    for (int i = 0; i < n; i++)
      band[i] = (vis[i] && ekm_span_frac(i, aimPoint, headFrac, bandFloor) >= bandFloor)
                    ? 1 : 0;
    const int mid = ekm_span_mid(band, n, ekm_span_prefer(aimPoint, headFrac), first,
                                 last, heightZ, range2d, aimPoint, tight, headFrac,
                                 bandFloor);
    if (mid >= 0) {
      if (inBand) *inBand = 1;
      return mid;
    }
  }
  return ekm_span_mid(vis, n, ekm_span_prefer(aimPoint), first, last, heightZ,
                      range2d, aimPoint, tight);
}

// Did the winning run cover the whole body? Everything else is partial, including the run that does not exist
// (-1/-1), the most interesting line the log can carry.
inline bool ekm_span_is_full(int first, int last, int n) {
  return n > 0 && first == 0 && last == n - 1;
}

// How many tics a position delta spans (EK_AIM_PROJ_SPEED is per tic, so velocity must be too). Above 30 fps the
// divisor is 1, and that is the point.
inline int ekm_lead_tics(unsigned msElapsed) {
  const int t = (int)((double)msElapsed / EKM_MS_PER_TIC + 0.5);
  if (t < 1) return 1;
  if (t > EKM_LEAD_TICS_MAX) return EKM_LEAD_TICS_MAX;
  return t;
}

// Divisor for a delta over msElapsed, or 0 when the gap is too long to represent and the delta must be discarded: a
// cap that lies is worse than no sample (ekm_lead_tics caps at EKM_LEAD_TICS_MAX, right for a late frame, wrong for a
// pause).
inline int ekm_lead_divisor(unsigned msElapsed) {
  if (msElapsed > EKM_LEAD_GAP_MAX_MS) return 0;
  return ekm_lead_tics(msElapsed);
}

// A target whose position has not changed for this long has stopped and its stored velocity must go with it;
// otherwise a standing enemy is led for ever, turning an intermittent miss into a permanent one.
inline bool ekm_velocity_stale(unsigned msSinceChange) {
  return msSinceChange >= EKM_VEL_STALE_MS;
}

// How far into the current tic we are, 0..1: the engine's smoothratio as a double. The aim point must move the way
// the sprite visibly moves.
inline double ekm_smoothratio(double msSinceTic) {
  if (!(msSinceTic > 0.0)) return 0.0;
  const double r = msSinceTic / EKM_MS_PER_TIC;
  return r >= 1.0 ? 1.0 : r;
}

// A Build coordinate between the previous tic's value and this one's. Exact at both ends and for equal endpoints, so
// a standing target interpolates to itself and adds no wander.
inline int ekm_lerp_pos(int from, int to, double ratio) {
  if (from == to) return from;
  if (!(ratio > 0.0)) return from;
  if (ratio >= 1.0) return to;
  const double v = (double)from + ((double)to - (double)from) * ratio;
  return (int)(v + (v >= 0.0 ? 0.5 : -0.5));
}

// A fraction of the remaining delta retained per tic, over `tics` of them: smoothing must mean the same at 30 and 240
// fps. The per-frame ekm_smooth_step converged in about five frames (under 20 ms) at 240 fps, the snap the player
// reported.
inline int ekm_smooth_step_tics(int delta, int percent, double tics) {
  if (delta == 0) return 0;
  if (percent <= 0) return delta;
  if (percent >= 100) return 0;
  if (tics <= 0.0) return 0;
  const double keptN = pow((double)percent, tics);
  const double fullN = pow(100.0, tics);
  int step = (int)((double)delta * (fullN - keptN) / fullN);
  if (step == 0) step = delta > 0 ? 1 : -1;
  return step;
}

// Where a target moving at v will be when a projectile at projVel arrives: two fixed iterations of t = dist / vel.
inline void ekm_lead_point(int tx, int ty, int tz, int vx, int vy, int vz,
                           int sx, int sy, int projVel, int* ox, int* oy, int* oz) {
  int px = tx, py = ty, pz = tz;
  if (projVel > 0) {
    for (int i = 0; i < 2; i++) {
      const double dx = (double)px - (double)sx;
      const double dy = (double)py - (double)sy;
      const double d = sqrt(dx * dx + dy * dy);
      if (d <= 0.0) { px = tx; py = ty; pz = tz; break; }
      const double t = d / (double)projVel;
      px = tx + (int)(vx * t);
      py = ty + (int)(vy * t);
      pz = tz + (int)(vz * t);
    }
  }
  if (ox) *ox = px;
  if (oy) *oy = py;
  if (oz) *oz = pz;
}

inline int ekm_lead_scaled(int raw, int led, int percent) {
  return raw + (int)((long long)(led - raw) * percent / 100);
}

inline int ekm_metres(int units) { return units / EKM_UNITS_PER_METRE; }

// The ESP draw-distance cull is compared against dist2d, never depth: depth under-reads off to one side, so the cull
// range would move with the field of view (World Tour's finding, carried verbatim).
inline bool ekm_within_draw_distance(int dist2d, int limit, int unlimited) {
  if (unlimited) return true;
  if (dist2d < 0) return false;
  return dist2d <= limit;
}

// Silent aim's bound: is the yaw a shot would be redirected to within maxDegrees of where the player actually looks?
inline bool ekm_redirect_within(int fromYaw, int toYaw, int maxDegrees) {
  if (maxDegrees <= 0) return false;
  const int deg = maxDegrees > 180 ? 180 : maxDegrees;
  return ekm_angular_offset(fromYaw, toYaw) <= deg * EKM_YAW_UNITS / 360;
}

// A solution older than this was not computed for the situation the shot is fired into; same ceiling as
// EKM_LEAD_GAP_MAX_MS, since past four tics a gap is a pause, a load or a teleport.
const unsigned EKM_SHOT_STALE_MS = EKM_LEAD_GAP_MAX_MS;

// Is a published aim solution still the one to shoot with? Measured in time, not frames.
inline bool ekm_shot_fresh(unsigned msSincePublish) {
  return msSincePublish <= EKM_SHOT_STALE_MS;
}
