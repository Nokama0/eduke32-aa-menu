// See tracer.h. Queue every round the engine fires, cast our own prediction at the next swap, pair it with the impact
// sprite the engine spawned. tracer_note_shot runs in the tic (P_PreFireHitscan detour): it stores six ints and a
// timestamp and calls nothing, since ek_hitscan, ek_cansee and ek_updatesector are callable only from their own hooks.

#include <windows.h>
#include <math.h>
#include "tracer.h"
#include "config.h"
#include "gamestate.h"
#include "playerstate.h"
#include "hitscan.h"
#include "shotray.h"
#include "project.h"
#include "projmath.h"
#include "ui/canvas.h"
#include "esp_layout.h"
#include "log.h"
#include "wallclock.h"

// A shotgun blast is seven rounds and more than one tic can land between presented frames; past 32 a round is counted
// and dropped rather than overwriting an uncast one.
const int EKM_TRACER_PENDING_MAX = 32;

struct EkPendingShot {
  int x, y, z;
  int shootAng;
  int zvel;
  unsigned ms;
};

static EkPendingShot s_pending[EKM_TRACER_PENDING_MAX];
static int s_pendingCount = 0;
static unsigned s_droppedPending = 0;
static unsigned s_droppedNoAnswer = 0;
static bool s_saidDropped = false;

static EkTracerRing s_ring;
static bool s_ringReady = false;
static long s_levelEpoch = 0;

bool tracer_impacts_wanted(void) { return g_cfg.tracers != 0; }

void tracer_note_shot(int x, int y, int z, int shootAng, int zvel) {
  if (g_cfg.tracers == 0) return;
  if (s_pendingCount >= EKM_TRACER_PENDING_MAX) { s_droppedPending++; return; }
  EkPendingShot& p = s_pending[s_pendingCount++];
  p.x = x; p.y = y; p.z = z;
  p.shootAng = shootAng;
  p.zvel = zvel;
  p.ms = wallclock_ms();
}

void tracer_frame(void) {
  // A level entry invalidates every world coordinate the ring and queue hold. Keyed on the epoch, not gm 0
  // (finish-into-next never reads gm 0). Above the row's gate so switching the row back on cannot show an old-map line.
  const long epoch = level_epoch();
  const bool newLevel = ekm_level_epoch_changed(s_levelEpoch, epoch);
  s_levelEpoch = epoch;
  if (newLevel) s_pendingCount = 0;

  if (g_cfg.tracers == 0) {
    // The queue is dropped with the row: a round fired before the switch-off would be cast against an older world.
    s_pendingCount = 0;
    return;
  }

  if (!s_ringReady || newLevel) { ekm_tracer_reset(&s_ring); s_ringReady = true; }

  // No cast into a map being built. Two statements: argument evaluation order is unspecified, so `gm` is filled before
  // it is read.
  int gm = 0;
  const bool gmOk = player_read_gm(&gm);
  if (ekm_level_loading(gm, gmOk)) {
    s_pendingCount = 0;
    return;
  }

  const int pending = s_pendingCount;
  s_pendingCount = 0;
  if (pending <= 0) return;
  if (!hitscan_ready() || !gamestate_ready()) { s_droppedNoAnswer += pending; return; }

  PlayerSnap me;
  if (!player_snap(&me)) { s_droppedNoAnswer += pending; return; }

  for (int i = 0; i < pending; i++) {
    const EkPendingShot& p = s_pending[i];

    // The cast is the engine's own shot (player.cpp:556): sintable[(ang+512)&2047] across, sintable[ang&2047] along,
    // zvel<<6 down, from values after auto-aim, spread and `z -= ZOFFSET6` (:531). zvel passes A_GetShootZvel (:465),
    // the identity unless CON sets g_overrideShootZvel; then the line is drawn at the aimed pitch.
    const int vx = ekm_sintable(p.shootAng + 512);
    const int vy = ekm_sintable(p.shootAng);
    const int vz = p.zvel << 6;

    EkHitData hit;
    if (!ek_hitscan(p.x, p.y, p.z, me.sect, vx, vy, vz, &hit)) {
      // No answer is not a prediction: a tracer without its exact line is not drawn. The round's impact may then pair
      // with a neighbouring shot, the mis-pairing the second colour exists to admit.
      s_droppedNoAnswer++;
      continue;
    }

    EkTracer t;
    t.ox = p.x; t.oy = p.y; t.oz = p.z;
    t.px = hit.x; t.py = hit.y; t.pz = hit.z;
    t.ax = 0; t.ay = 0; t.az = 0;
    t.haveActual = 0;
    t.ms = p.ms;
    ekm_tracer_add(&s_ring, t);
  }

  if (!s_saidDropped && (s_droppedPending || s_droppedNoAnswer)) {
    s_saidDropped = true;
    ek_log("tracer: %u round(s) dropped at the queue and %u for want of a "
           "hitscan answer; those shots have no predicted line and are not drawn",
           s_droppedPending, s_droppedNoAnswer);
  }
}

// Best effort. The oldest unpaired shot inside the window wins (the engine spawns sparks in firing order); a shot pairs
// at most once, or one spark would claim every shot in a burst.
void tracer_note_impact(int x, int y, int z, int ours) {
  if (g_cfg.tracers == 0 || !s_ringReady || ours == 0) return;

  const unsigned now = wallclock_ms();
  for (int i = s_ring.count - 1; i >= 0; i--) {
    const int k = ekm_tracer_index(&s_ring, i);
    if (k < 0) continue;
    EkTracer& t = s_ring.slot[k];
    if (t.haveActual) continue;
    if (!ekm_tracer_pair_ok(t.ms, now, EKM_TRACER_PAIR_MS)) continue;
    t.ax = x; t.ay = y; t.az = z;
    t.haveActual = 1;
    return;
  }
}

// A square marks each endpoint (EKM_TRACER_MARK): a shot into a wall a metre away projects to a few invisible pixels.
static void tracer_mark(float sx, float sy, unsigned colour, const EspStrokes& st) {
  const int m = st.tracerMark;
  const EspRect r = { (int)sx - m, (int)sy - m, m * 2 + 1, m * 2 + 1 };
  esp_keyed_rects(&r, 1, colour, st.keyline);
}

// Refusal instrumentation: these counters measured the flat clip's cut end failing oz2 once the view pitched; after the
// tilt clip an oz2 refusal should be ~0, so they stay as its regression alarm. Cumulative, logged only under debuglog.
struct EkWorldlineStats {
  unsigned drawn;
  unsigned refusedBothBehindFlat;
  unsigned refusedBehindTilt;
  unsigned offscreen;
  // An endpoint's own projection failing after the clip, by reason and whether the clip moved that end.
  unsigned refuseGdepthClipped, refuseGdepthOriginal;
  unsigned refuseOz2Clipped, refuseOz2Original;
  unsigned refuseOtherClipped, refuseOtherOriginal;
  bool haveWorstOz2;
  double worstOz2;
  double worstOz2Shang, worstOz2Chang;
  double worstOz2Gdepth;
};

static EkWorldlineStats s_wlStats[2];
static unsigned s_wlLastLogMs = 0;

static void worldline_count_refusal(EkWorldlineStats* st, EkBuildProjectReason reason,
                                    bool wasClipped, double oz2, double gdepth,
                                    const EkBuildProj& g) {
  switch (reason) {
    case EKM_BUILD_PROJECT_REFUSE_GDEPTH:
      if (wasClipped) st->refuseGdepthClipped++; else st->refuseGdepthOriginal++;
      break;
    case EKM_BUILD_PROJECT_REFUSE_OZ2:
      if (wasClipped) st->refuseOz2Clipped++; else st->refuseOz2Original++;
      if (!st->haveWorstOz2 || oz2 < st->worstOz2) {
        st->haveWorstOz2 = true;
        st->worstOz2 = oz2;
        st->worstOz2Shang = g.shang;
        st->worstOz2Chang = g.chang;
        st->worstOz2Gdepth = gdepth;
      }
      break;
    default:
      if (wasClipped) st->refuseOtherClipped++; else st->refuseOtherOriginal++;
      break;
  }
}

static void worldline_maybe_log(void) {
  if (!g_cfg.debuglog) return;
  const unsigned now = wallclock_ms();
  if (s_wlLastLogMs != 0 && (now - s_wlLastLogMs) < 1000) return;
  s_wlLastLogMs = now;
  static const char* const kCallerTag[2] = { "tracer", "other" };
  for (int c = 0; c < 2; c++) {
    const EkWorldlineStats& st = s_wlStats[c];
    if (!st.drawn && !st.refusedBothBehindFlat && !st.refusedBehindTilt &&
        !st.offscreen && !st.refuseGdepthClipped &&
        !st.refuseGdepthOriginal && !st.refuseOz2Clipped && !st.refuseOz2Original &&
        !st.refuseOtherClipped && !st.refuseOtherOriginal)
      continue;
    ek_log("tracer draw: [%s] drawn %u refused-both-behind-flat %u "
           "refused-behind-tilt %u offscreen %u "
           "refused-gdepth clipped %u original %u "
           "refused-oz2 clipped %u original %u "
           "refused-other clipped %u original %u",
           kCallerTag[c], st.drawn, st.refusedBothBehindFlat,
           st.refusedBehindTilt, st.offscreen,
           st.refuseGdepthClipped, st.refuseGdepthOriginal,
           st.refuseOz2Clipped, st.refuseOz2Original,
           st.refuseOtherClipped, st.refuseOtherOriginal);
    if (st.haveWorstOz2) {
      ek_log("tracer draw: [%s] worst oz2 %.6f at shang %.6f chang %.6f, "
             "refused end's own gdepth %.3f",
             kCallerTag[c], st.worstOz2, st.worstOz2Shang, st.worstOz2Chang,
             st.worstOz2Gdepth);
    }
  }
}

bool worldline_draw(const EkProjection& p, int x0, int y0, int z0,
                    int x1, int y1, int z1, unsigned colour, EkWorldLine* out,
                    EkWorldlineCaller caller, int lineW, int keyline) {
  if (!out) return false;
  EkWorldlineStats* st = &s_wlStats[caller == EKM_WL_CALLER_TRACER ? 0 : 1];
  EkmVec3 a = project_to_build(p, x0, y0, z0);
  EkmVec3 b = project_to_build(p, x1, y1, z1);
  const bool aWasFront = ekm_build_in_front(p.g, a);
  const bool bWasFront = ekm_build_in_front(p.g, b);
  if (!ekm_build_clip_near(p.g, &a, &b)) {
    st->refusedBothBehindFlat++;
    worldline_maybe_log();
    return false;
  }

  // The tilted near plane, after the flat one it depends on: pitching the view drove the flat clip's cut end to oz2
  // -205751 and dropped the tracer. An end it moves is no more real than one the flat clip moved.
  const bool aReal = aWasFront && ekm_build_tilt_in_front(p.g, a);
  const bool bReal = bWasFront && ekm_build_tilt_in_front(p.g, b);
  if (!ekm_build_clip_tilt(p.g, &a, &b)) {
    st->refusedBehindTilt++;
    worldline_maybe_log();
    return false;
  }

  float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
  EkBuildProjectReason reasonA = EKM_BUILD_PROJECT_OK, reasonB = EKM_BUILD_PROJECT_OK;
  double gdepthA = 0.0, oz2A = 0.0, gdepthB = 0.0, oz2B = 0.0;
  if (!project_build_point_diag(p, a, &ax, &ay, &reasonA, &gdepthA, &oz2A)) {
    worldline_count_refusal(st, reasonA, !aReal, oz2A, gdepthA, p.g);
    worldline_maybe_log();
    return false;
  }
  if (!project_build_point_diag(p, b, &bx, &by, &reasonB, &gdepthB, &oz2B)) {
    worldline_count_refusal(st, reasonB, !bReal, oz2B, gdepthB, p.g);
    worldline_maybe_log();
    return false;
  }

  // A cut end near the planes can land millions of pixels out, past an int: clip to the viewport plus a margin before
  // any cast; an end moved here is not marked.
  double sx0 = ax, sy0 = ay, sx1 = bx, sy1 = by;
  int aMoved = 0, bMoved = 0;
  const double m = EKM_SCREEN_CLIP_MARGIN;
  if (!ekm_clip_segment_2d(&sx0, &sy0, &sx1, &sy1,
                           (double)p.vp.x - m, (double)p.vp.y - m,
                           (double)p.vp.x + (double)p.vp.w + m,
                           (double)p.vp.y + (double)p.vp.h + m,
                           &aMoved, &bMoved)) {
    st->offscreen++;
    worldline_maybe_log();
    return false;
  }

  if (lineW <= 1 && keyline <= 0) {
    canvas_line((int)sx0, (int)sy0, (int)sx1, (int)sy1, colour);
  } else {
    if (keyline > 0)
      canvas_line_w((int)sx0, (int)sy0, (int)sx1, (int)sy1, esp_keyline_colour(colour),
                    lineW + 2 * keyline);
    canvas_line_w((int)sx0, (int)sy0, (int)sx1, (int)sy1, colour, lineW);
  }
  out->ax = (float)sx0; out->ay = (float)sy0;
  out->bx = (float)sx1; out->by = (float)sy1;
  out->aFront = (aReal && !aMoved) ? 1 : 0;
  out->bFront = (bReal && !bMoved) ? 1 : 0;
  st->drawn++;
  worldline_maybe_log();
  return true;
}

// The tracer's own segment: the shared line plus endpoint marks, drawn only for ends that were not clipped.
static bool tracer_segment(const EkProjection& p, int x0, int y0, int z0,
                           int x1, int y1, int z1, unsigned colour,
                           const EspStrokes& st) {
  EkWorldLine ln = { 0.0f, 0.0f, 0.0f, 0.0f, 0, 0 };
  const bool drew = worldline_draw(p, x0, y0, z0, x1, y1, z1, colour, &ln,
                                   EKM_WL_CALLER_TRACER, st.lineW, st.keyline);
  if (!drew) return false;
  if (ln.aFront) tracer_mark(ln.ax, ln.ay, colour, st);
  if (ln.bFront) tracer_mark(ln.bx, ln.by, colour, st);
  return true;
}

int tracer_draw(void) {
  if (g_cfg.tracers == 0 || !s_ringReady) return 0;
  if (s_ring.count <= 0) return 0;

  EkProjection p;
  if (!project_begin(&p, EK_PC_TRACER)) return 0;

  const unsigned now = wallclock_ms();
  int drawn = 0;
  const EspStrokes st = esp_strokes(canvas_frame_height(), g_cfg.hivis != 0);

  for (int i = 0; i < s_ring.count; i++) {
    const int k = ekm_tracer_index(&s_ring, i);
    if (k < 0) continue;
    const EkTracer& t = s_ring.slot[k];

    // break, not continue: the ring is in insertion order, so the first entry past ten seconds is followed only by
    // older ones. The unsigned difference is exact across the clock's wrap.
    const int alpha = ekm_tracer_alpha((unsigned)(now - t.ms));
    if (alpha <= 0) break;

    bool any = tracer_segment(p, t.ox, t.oy, t.oz, t.px, t.py, t.pz,
                              canvas_with_alpha(canvas_tracer_predicted_colour(), alpha), st);

    // The actual line only where a pairing was made; an unpaired shot keeps its predicted line alone.
    if (t.haveActual)
      any = tracer_segment(p, t.ox, t.oy, t.oz, t.ax, t.ay, t.az,
                           canvas_with_alpha(canvas_tracer_actual_colour(), alpha), st) || any;

    if (any) drawn++;
  }
  return drawn;
}
