// Stage B of the cutaway: on a gated FLAT call the engine's domost never runs; this paints the sector's whole footprint
// through polymost_drawpoly (rules: cutpaint.h; hooks: cutaway.cpp). It claims nothing (vsp[] is shared sequential
// state), the depth buffer sorts. DLL only: decisions are pure functions in cutpaint.h or projmath.h.

#include <windows.h>
#include <string.h>
#include <stdio.h>

#include "cutpaint.h"
#include "footprint.h"
#include "log.h"
#include "config.h"
#include "project.h"
#include "camera.h"
#include "sectorstate.h"
#include "cutaway.h"
#include "camerascan.h"
#include "ui/wallclock.h"
#include "flatflip.h"
#include "slopez.h"
#include "nocull.h"

// Render clock only: frame_begin (nocull.cpp's seed seam) and cutpaint_flat (the domost hook) run in one
// renderDrawRoomsQ16 call, so state is bare. Both gate on nocull_should_scan, so a frame reaching cutpaint_flat reached
// frame_begin; a YAX per-level pass (polymost.cpp:7073) reads camera_render_armed() false.

// polymost_drawpoly(vec2f_t const* dpxy, int32_t n, int32_t method), polymost.cpp:3179. vec2f_t is two floats, so the
// array is INTERLEAVED, not Megaton's two double arrays. The third argument in r8d is what cutaway.cpp's
// domostpolymethod locate established.
struct EkCutVec2f { float x, y; };
typedef void (*EkDrawPolyFn)(const EkCutVec2f*, int, int);

static EkDrawPolyFn s_drawpoly = NULL;
static bool s_drawpolySettled = false;
static bool s_dead = false;

// A sector's floor is ONE polygon set but every bunch of that sector asks for it (dollhouse.cpp:288's rule).
static unsigned char s_floorDone[EK_MAXSECTORS / 8];
static unsigned char s_ceilDone[EK_MAXSECTORS / 8];

// This frame's projection, taken once at the seam.
static EkProjection s_proj;
static bool s_projOk = false;

// Cumulative counts, never sampled: running totals settle what rate-limited lines cannot. The throttle only decides how
// often they print. Wall refusals are counted by REASON (a forwarded wall still claims, so a stage forwarding most
// would look inert); clamps and refusals are separate facts (painted smaller vs handed back).
static unsigned s_frames = 0;
static unsigned s_planes = 0;
static unsigned s_repeats = 0;
static unsigned s_tris = 0;
static unsigned s_dropNear = 0;
static unsigned s_dropTilt = 0;
static unsigned s_dropWind = 0;
static unsigned s_noFootprint = 0;
static unsigned s_noSector = 0;
static unsigned s_noProj = 0;
static unsigned s_sloped = 0;

static unsigned s_walls = 0;
static unsigned s_wallInert = 0;
static unsigned s_wallNoQuad = 0;
static unsigned s_wallDropTilt = 0;
static unsigned s_wallDropWind = 0;

static unsigned s_wallClamped = 0;
static unsigned s_wallOutside = 0;
static unsigned s_wallClampEmpty = 0;
static unsigned s_wallNoBounds = 0;

// Measurement only: the method histogram (every gated wall call, whether or not painted; DAMETH_CLAMPED is polymost.h's
// 4) and the tall-wall refusal counters. The maximum is per print window while counts are cumulative (a total can be
// differenced, a high-water mark cannot).
static unsigned s_methodClamped = 0;
static unsigned s_methodUnclamped = 0;
static const int CP_METHOD_HIST_CAP = 6;
static int s_methodHistValue[CP_METHOD_HIST_CAP];
static unsigned s_methodHistCount[CP_METHOD_HIST_CAP];
static int s_methodHistN = 0;
static unsigned s_methodHistOther = 0;

static int s_methodWorstAsBuilt = 0;
static bool s_methodWorstAsBuiltSeen = false;
static int s_methodWorstAfterView = 0;
static bool s_methodWorstAfterViewSeen = false;

static unsigned s_wallUnclamped = 0;

static unsigned s_wallTallRefused = 0;
static double s_wallTallMaxAllowed = 0.0;
static bool s_wallTallMaxAllowedSeen = false;

static void cp_method_tally(int method) {
  if ((method & EK_DAMETH_CLAMPED) != 0) s_methodClamped++;
  else s_methodUnclamped++;
  for (int i = 0; i < s_methodHistN; i++) {
    if (s_methodHistValue[i] == method) { s_methodHistCount[i]++; return; }
  }
  if (s_methodHistN < CP_METHOD_HIST_CAP) {
    s_methodHistValue[s_methodHistN] = method;
    s_methodHistCount[s_methodHistN] = 1;
    s_methodHistN++;
  } else {
    s_methodHistOther++;
  }
}

static unsigned s_wallViewCut = 0;
static unsigned s_wallViewDrop = 0;
static unsigned s_wallViewKept = 0;
static unsigned s_wallNoTangs = 0;

// The extent measurement, nothing decides from it: wall-quad height against the viewport as built, after the x-clamp
// and after the viewport clip. The last is in final-row (py) space, not pre-tilt sy, which can run to infinity while py
// stays finite (cutpaint.h). The flat path's reading is the same expression as-built and after the clip.
static double s_extMaxRatio = 0.0;
static double s_extMaxPx = 0.0;
static double s_extScreenPx = 0.0;
static unsigned s_extOver1 = 0;
static unsigned s_extOver4 = 0;
static unsigned s_extOver16 = 0;
static unsigned s_extOver64 = 0;
static unsigned s_extNoMeasure = 0;
static bool s_extSeen = false;
static double s_extX0 = 0.0, s_extY0Top = 0.0, s_extY0Bot = 0.0;
static double s_extX1 = 0.0, s_extY1Top = 0.0, s_extY1Bot = 0.0;

static double s_extAfterMaxRatio = 0.0;
static double s_extAfterMaxPx = 0.0;
static unsigned s_extAfterNoMeasure = 0;
static bool s_extAfterSeen = false;

static double s_extViewMaxRatio = 0.0;
static double s_extViewMaxPx = 0.0;
static unsigned s_extViewNoMeasure = 0;
static bool s_extViewSeen = false;

static double s_extFlatMaxRatio = 0.0;
static double s_extFlatMaxPx = 0.0;
static double s_extFlatScreenPx = 0.0;
static unsigned s_extFlatOver1 = 0;
static unsigned s_extFlatOver4 = 0;
static unsigned s_extFlatOver16 = 0;
static unsigned s_extFlatOver64 = 0;
static unsigned s_extFlatNoMeasure = 0;
static bool s_extFlatSeen = false;
static int s_extFlatWorstN = 0;
static double s_extFlatWorstSx[EK_CUT_POLY_MAX];
static double s_extFlatWorstSy[EK_CUT_POLY_MAX];
static int s_extFlatWorstSector = -1;
static bool s_extFlatWorstIsFloor = false;

static double s_extFlatAfterMaxRatio = 0.0;
static double s_extFlatAfterMaxPx = 0.0;
static unsigned s_extFlatAfterNoMeasure = 0;
static bool s_extFlatAfterSeen = false;

static bool s_extFlatXSeen = false;
static double s_extFlatXMaxPx = 0.0;
static double s_extFlatXMaxRatio = 0.0;
static bool s_extFlatXHaveRatio = false;
static unsigned s_extFlatXNoMeasure = 0;

static bool s_extFlatXAfterSeen = false;
static double s_extFlatXAfterMaxPx = 0.0;
static double s_extFlatXAfterMaxRatio = 0.0;
static bool s_extFlatXAfterHaveRatio = false;
static unsigned s_extFlatXAfterNoMeasure = 0;

static int s_extXAfterWorstN = 0;
static double s_extXAfterWorstSx[EK_CUT_POLY_MAX];
static double s_extXAfterWorstSy[EK_CUT_POLY_MAX];
static int s_extXAfterWorstSector = -1;
static bool s_extXAfterWorstIsFloor = false;
static bool s_extXAfterWorstHaveVerdict = false;
static EkCutViewOutcome s_extXAfterWorstOutcome = EK_CUT_VIEW_REFUSED;
static EkCutViewOutcome s_extXAfterWorstCols = EK_CUT_VIEW_REFUSED;
static EkCutViewOutcome s_extXAfterWorstRows = EK_CUT_VIEW_REFUSED;
static bool s_extXAfterWorstHaveXBounds = false;
static double s_extXAfterWorstXLo = 0.0;
static double s_extXAfterWorstXHi = 0.0;

static CpColSpanTracker s_colSpanTracker = { 0, 0, 0.0, 0.0 };

// Flat flip: every pass paints the same COUNT of flats while one plane's picture collapses on alternate frames, so each
// flat's box and per-stage vertex counts are kept for two consecutive MAIN passes and the flipped one named. Behind
// g_cfg.debuglog.
static const int CP_FLIP_STAGES = 5;
static const char* const kCpFlipStage[CP_FLIP_STAGES] = {
    "tri", "near", "tilt", "view", "drawn"};
struct CpFlatRec {
  unsigned stamp;
  int verts[CP_FLIP_STAGES];
  int flatZ;
  bool sloped;
  double area;
  double camZ;
  unsigned countStamp;
  unsigned np;
  unsigned dmPainted, dmRepeat, dmFwd;
};
// Keyed sector * 2 + (floor ? 1 : 0). A stamp that differs from its table's pass id is an older entry, so nothing is
// cleared. About 0.8 MB, render clock.
struct CpFlatTable {
  bool valid;
  unsigned pass;
  unsigned frame;
  CpFlatRec rec[EK_MAXSECTORS * 2];
  // The engine's gotsector copied ONCE per pass by cutpaint_flip_pass_end() at the true end of the pass:
  // polymost_drawalls calls polymost_scansector for unmarked neighbours throughout (polymost.cpp:6547, :6616/:6657).
  // haveScanned false means the read failed.
  unsigned char scanned[EK_MAXSECTORS / 8];
  bool haveScanned;
  // The camera pose this pass drew with, so the side test can be recomputed for a plane that was NOT painted and has no
  // record.
  bool camValid;
  int camX, camY, camZ;
};
static CpFlatTable s_flipTab[2];
static int s_flipCur = 0;
static unsigned s_flipPassSeq = 0;
static bool s_flipMainPass = false;
static unsigned s_flips = 0;
static unsigned s_flipPairs = 0;
static unsigned s_flipsAtSeam = 0;
// Cause tally exact over every flip's not-painted side, not only the throttled lines (the count is the answer).
static unsigned s_flipCauseNotScanned = 0;
static unsigned s_flipCauseNoPlaneCall = 0;
static unsigned s_flipCauseNoDomost = 0;
static unsigned s_flipCauseNotPainted = 0;
static unsigned s_sideTestFlipped = 0;
static unsigned s_sideTestAgreed = 0;
static unsigned s_sideTestUnknown = 0;
static const unsigned CP_FLIP_LINES_PER_SEC = 4u;
static unsigned s_flipWinMs = 0;
static unsigned s_flipWinLines = 0;
static bool s_flipWinFresh = true;

// Stage B's viewport clip counted by outcome: the regression guard. An ordinary map's floor is about 1x the screen and
// wholly inside the view, so KEPT should carry nearly all of the traffic. OFFERED is counted apart so the log prints a
// total beside its enumeration.
static unsigned s_flatViewOffered = 0;
static unsigned s_flatViewKept = 0;
static unsigned s_flatViewCut = 0;
static unsigned s_flatViewDrop = 0;
static unsigned s_flatViewRefused = 0;
static unsigned s_flatViewSkipped = 0;

static unsigned s_flatColKept = 0;
static unsigned s_flatColCut = 0;
static unsigned s_flatColDrop = 0;
static unsigned s_flatColNone = 0;
static unsigned s_flatRowKept = 0;
static unsigned s_flatRowCut = 0;
static unsigned s_flatRowDrop = 0;
static unsigned s_flatRowNone = 0;

// A chain clipped by all four bounds can reach nine points and polymost_drawpoly takes eight (EK_CUT_DRAWPOLY_MAX): the
// rule hands back the last stage that fits. Expected zero; if it moves, split into a fan.
static unsigned s_flatViewFitted = 0;
// The guard on the call itself, which must never fire: a longer polygon writes past five of the ENGINE'S float[8] stack
// arrays.
static unsigned s_flatOverPaintCap = 0;
// Same guard on stage C. A wall's chain (quad 4, tilt 5, two rows 7) stays under 8 only by an argument; counted, not
// asserted.
static unsigned s_wallOverPaintCap = 0;

static double s_extAfterInX0 = 0.0, s_extAfterInY0Top = 0.0, s_extAfterInY0Bot = 0.0;
static double s_extAfterInX1 = 0.0, s_extAfterInY1Top = 0.0, s_extAfterInY1Bot = 0.0;
static double s_extAfterOutX0 = 0.0, s_extAfterOutY0Top = 0.0, s_extAfterOutY0Bot = 0.0;
static double s_extAfterOutX1 = 0.0, s_extAfterOutY1Top = 0.0, s_extAfterOutY1Bot = 0.0;
static double s_extAfterBoundLo = 0.0, s_extAfterBoundHi = 0.0;
static bool s_extAfterHasBounds = false;
static bool s_extAfterClamped = false;

static unsigned s_framesAtSeam = 0;
static unsigned s_planesAtSeam = 0;
static unsigned s_wallsAtSeam = 0;

// Every bound is a positive comparison, so a NaN or infinity comes back not measurable: latching one as a maximum would
// freeze the worst-case reading for the session.
struct CpExtent { int ok; double ratio; double px; };

static CpExtent cp_extent(const EkCutPoly* q, double screenPx) {
  CpExtent e;
  e.ok = 0; e.ratio = 0.0; e.px = 0.0;
  if (q == NULL || q->n < 3 || q->n > EK_CUT_POLY_MAX) return e;
  double lo = q->v[0].sy, hi = q->v[0].sy;
  for (int k = 1; k < q->n; k++) {
    if (q->v[k].sy < lo) lo = q->v[k].sy;
    if (q->v[k].sy > hi) hi = q->v[k].sy;
  }
  const double extPx = hi - lo;
  const double ratio = extPx / screenPx;
  if (!(screenPx > 0.0) || !(screenPx < 1e30) || !(ratio >= 0.0) ||
      !(ratio < 1e30))
    return e;
  e.ok = 1;
  e.ratio = ratio;
  e.px = extPx;
  return e;
}

static CpExtent cp_extent_rows(const EkCutPoly* q, const EkBuildProj& g,
                               double ctang, double stang, double screenPx) {
  CpExtent e;
  e.ok = 0; e.ratio = 0.0; e.px = 0.0;
  if (q == NULL || q->n < 3 || q->n > EK_CUT_POLY_MAX) return e;
  bool first = true;
  double lo = 0.0, hi = 0.0;
  for (int k = 0; k < q->n; k++) {
    const EkCutVert& vtx = q->v[k];
    const double d = ekm_build_tilt_depth(vtx.sy, g.halfx, g.horiz, g.chang,
                                          g.shang, g.gvr);
    if (!(d > 0.0)) continue;
    const double py = g.horiz + ekm_cut_py_num(g, vtx.sx, vtx.sy, ctang, stang) / d;
    if (first) { lo = hi = py; first = false; }
    else { if (py < lo) lo = py; if (py > hi) hi = py; }
  }
  if (first) return e;
  const double extPx = hi - lo;
  const double ratio = extPx / screenPx;
  if (!(screenPx > 0.0) || !(screenPx < 1e30) || !(ratio >= 0.0) ||
      !(ratio < 1e30))
    return e;
  e.ok = 1;
  e.ratio = ratio;
  e.px = extPx;
  return e;
}

static const char* cp_outcome_str(EkCutViewOutcome o) {
  switch (o) {
    case EK_CUT_VIEW_KEPT: return "KEPT";
    case EK_CUT_VIEW_TRIMMED: return "TRIMMED";
    case EK_CUT_VIEW_REMOVED: return "REMOVED";
    case EK_CUT_VIEW_REFUSED: return "REFUSED";
    case EK_CUT_VIEW_SKIPPED: return "SKIPPED";
    case EK_CUT_VIEW_FITTED: return "FITTED";
    default: return "UNKNOWN";
  }
}

static bool s_saidFirst = false;
static bool s_saidFirstWall = false;
static bool s_saidNoDrawpoly = false;
static bool s_saidNoProj = false;
static unsigned s_lastMs = 0;
static bool s_firstTick = true;

// cvars.cpp's readable(): an address inside the image does not make its page committed. Asked once for the drawpoly
// entry.
static bool cp_readable(const void* p, size_t bytes) {
  if (p == NULL || bytes == 0) return false;
  MEMORY_BASIC_INFORMATION mbi;
  SIZE_T got = VirtualQuery(p, &mbi, sizeof(mbi));
  if (got != sizeof(mbi)) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & PAGE_NOACCESS) return false;
  if (mbi.Protect & PAGE_GUARD) return false;
  const unsigned char* const start = (const unsigned char*)mbi.BaseAddress;
  const unsigned char* const end = start + mbi.RegionSize;
  const unsigned char* const want = (const unsigned char*)p + bytes;
  return want > start && want <= end;
}

static EkDrawPolyFn cp_drawpoly(void) {
  if (s_drawpolySettled) return s_drawpoly;

  const unsigned char* const base = s_base;
  if (base == NULL) return NULL;
  const unsigned rva = cutaway_drawpoly_rva();
  if (rva == 0) {
    // Not settled yet, not refused: the locate runs on the sampler thread and an armed frame can reach here first; an
    // answer of nothing settles it so the interlocked read stays off the hot path.
    if (!cutaway_located()) return NULL;
    s_drawpolySettled = true;
    s_dead = true;
    if (!s_saidNoDrawpoly) {
      s_saidNoDrawpoly = true;
      ek_log("cutpaint: INERT -- the locate published no polymost_drawpoly, so "
             "every gated flat call forwards to the engine exactly as before "
             "and nothing of ours is painted");
    }
    return NULL;
  }

  s_drawpolySettled = true;
  if (!cp_readable((const void*)(base + rva), 1)) {
    s_dead = true;
    ek_log("cutpaint: polymost_drawpoly at rva 0x%07X is not readable from the "
           "render thread, so nothing is painted for the rest of this session "
           "and everything else keeps running", rva);
    return NULL;
  }
  s_drawpoly = (EkDrawPolyFn)(const void*)(base + rva);
  return s_drawpoly;
}

// About one line a second, nocull.cpp's interval: unthrottled this runs every armed frame.
static bool cp_due(void) {
  const unsigned now = wallclock_ms();
  if (!s_firstTick && (now - s_lastMs) < 1000u) return false;
  s_firstTick = false;
  s_lastMs = now;
  return true;
}

void cutpaint_frame_begin(void) {
  if (s_dead) return;

  // Bitmaps first and unconditionally: a span carried across frames misreads as a step (dollhouse.cpp:186), and a stale
  // claim would leave a sector unpainted for the session.
  memset(s_floorDone, 0, sizeof s_floorDone);
  memset(s_ceilDone, 0, sizeof s_ceilDone);
  s_projOk = false;
  s_frames++;

  s_flipPassSeq++;
  s_flipMainPass = g_cfg.debuglog != 0 && !camera_extra_pass_active() &&
                   cutaway_nodraw_value() <= 0;

  // One throttle for the whole seam, computed once (cp_due twice would let the first consume the second's turn) and
  // above the projection's early return so a refused frame still closes the interval's books. Taken before the debug
  // row is consulted.
  const bool tick = cp_due();
  const bool due = g_cfg.debuglog != 0 && tick;
  if (due && (s_planes != 0 || s_walls != 0)) {
    const unsigned dFrames = s_frames - s_framesAtSeam;
    const unsigned dPlanes = s_planes - s_planesAtSeam;
    const unsigned dWalls = s_walls - s_wallsAtSeam;
    s_framesAtSeam = s_frames;
    s_planesAtSeam = s_planes;
    s_wallsAtSeam = s_walls;

    const unsigned dFlips = s_flips - s_flipsAtSeam;
    s_flipsAtSeam = s_flips;
    ek_log("cutpaint: %u armed frame(s); %u plane(s) painted and %u repeat "
           "call(s) suppressed; %u triangle(s) drawn, %u dropped at the near "
           "plane, %u at the tilt clip, %u degenerate; %u sloped plane(s); "
           "refusals: %u no footprint, %u no sector, %u no projection; D11 "
           "flat flips: %u in all over %u consecutive main-pass pair(s) "
           "compared, %u since the last line. OF THE NOT-PAINTED SIDES (D11 "
           "CAUSE, exact over every flip, not only the throttled lines): %u "
           "not scanned, %u scanned but no plane call, %u plane call but no "
           "domost call, %u domost call(s) but never painted",
           s_frames, s_planes, s_repeats, s_tris, s_dropNear, s_dropTilt,
           s_dropWind, s_sloped, s_noFootprint, s_noSector, s_noProj,
           s_flips, s_flipPairs, dFlips, s_flipCauseNotScanned,
           s_flipCauseNoPlaneCall, s_flipCauseNoDomost, s_flipCauseNotPainted);
    if (s_flipCauseNoPlaneCall != 0)
      ek_log("cutpaint: D11 SIDE TEST -- of %u scanned-but-no-plane-call "
             "flip side(s), the side test (polymost.cpp:5511-5513 floor, "
             ":5930-5932 ceiling) FLIPPED between the two passes' camera "
             "poses %u time(s), AGREED on both %u time(s), and could not be "
             "computed %u time(s) (sector/wall unreadable, or no camera pose "
             "that pass)",
             s_flipCauseNoPlaneCall, s_sideTestFlipped, s_sideTestAgreed,
             s_sideTestUnknown);
    char tallAllowedBuf[24];
    if (s_wallTallMaxAllowedSeen)
      _snprintf_s(tallAllowedBuf, sizeof tallAllowedBuf, _TRUNCATE, "%.2fx",
                 s_wallTallMaxAllowed);
    else
      _snprintf_s(tallAllowedBuf, sizeof tallAllowedBuf, _TRUNCATE, "none yet");
    ek_log("cutpaint: STAGE C -- %u wall quad(s) painted in all; forwarded %u "
           "with no painter or projection, %u whose clip box was no quad, %u "
           "dropped whole at the tilt clip and %u degenerate after it. THE "
           "VIEWPORT CLIP: %u quad(s) trimmed to the view's own rows, %u "
           "removed whole as wholly outside them, %u passed untouched, and %u "
           "painted with no gctang/gstang located (which is this path exactly "
           "as it behaved before). THE TALL-WALL REFUSAL (brief-refuse-tall.md): "
           "%u quad(s) refused in all, largest AS-BUILT ratio still ALLOWED "
           "through SINCE THE LAST LINE %s. %u quad(s) forwarded rather than "
           "painted because the chain outgrew what polymost_drawpoly accepts "
           "(MUST BE 0: a wall is quad 4, tilt 5, two rows 7). OVER THE LAST "
           "%u ARMED FRAME(S): %u plane(s) and %u wall(s) "
           "painted",
           s_walls, s_wallInert, s_wallNoQuad, s_wallDropTilt, s_wallDropWind,
           s_wallViewCut, s_wallViewDrop, s_wallViewKept, s_wallNoTangs,
           s_wallTallRefused, tallAllowedBuf, s_wallOverPaintCap,
           dFrames, dPlanes, dWalls);
    // The high-water mark is cleared here: a maximum cannot be differenced, and left standing it reported 994.82x while
    // the row was OFF.
    s_wallTallMaxAllowed = 0.0;
    s_wallTallMaxAllowedSeen = false;
    ek_log("cutpaint: STAGE C x-CLAMP -- %u quad(s) clamped into the engine's "
           "own view bounds, %u forwarded as wholly outside them, %u forwarded "
           "as zero-width after the clamp, and %u painted with no bounds located "
           "at all (which is this path exactly as it behaved before)",
           s_wallClamped, s_wallOutside, s_wallClampEmpty, s_wallNoBounds);
  }

  // The extent line is not behind g_cfg.debuglog: it is the measurement the next decision rests on, so it belongs in
  // the log of the run that produced the picture. Same for the clamp-method, flat and x lines.
  if (tick && (s_extSeen || s_extNoMeasure != 0)) {
    if (s_extSeen)
      ek_log("cutpaint: WALL QUAD EXTENT -- worst %.2fx the screen height "
             "(%.1f px against a %.1f px screen) AS BUILT, worst %.2fx "
             "(%.1f px) AFTER THE x-CLAMP%s, and worst %.2fx (%.1f px) AFTER "
             "THE VIEWPORT CLIP%s (this third one in FINAL-ROW py space, not "
             "pre-tilt sy, which is the space the clip actually bounds) on the "
             "polygon actually handed to polymost_drawpoly; %u quad(s) "
             "over 1x, %u over 4x, %u over 16x, %u over 64x; %u not "
             "measurable, %u after the clamp, %u after the viewport clip. That "
             "worst incoming quad arrived as x0=%.3f y0top=%.3f y0bot=%.3f "
             "x1=%.3f y1top=%.3f y1bot=%.3f",
             s_extMaxRatio, s_extMaxPx, s_extScreenPx, s_extAfterMaxRatio,
             s_extAfterMaxPx, s_extAfterSeen ? "" : " (none measured yet)",
             s_extViewMaxRatio, s_extViewMaxPx,
             s_extViewSeen ? "" : " (none measured yet)",
             s_extOver1, s_extOver4, s_extOver16, s_extOver64, s_extNoMeasure,
             s_extAfterNoMeasure, s_extViewNoMeasure, s_extX0, s_extY0Top,
             s_extY0Bot, s_extX1, s_extY1Top, s_extY1Bot);
    else
      ek_log("cutpaint: WALL QUAD EXTENT -- no ratio is printed: all %u quad(s) "
             "so far gave no finite ratio, so either the projection's horiz or "
             "the quad itself was unusable", s_extNoMeasure);
  }

  if (tick && (s_methodClamped != 0 || s_methodUnclamped != 0)) {
    char histbuf[192];
    histbuf[0] = '\0';
    int hoff = 0;
    for (int i = 0; i < s_methodHistN; i++) {
      const int room = (int)sizeof(histbuf) - hoff;
      if (room <= 16) break;
      // _snprintf_s with _TRUNCATE, never sprintf_s (it calls the invalid-parameter handler, which aborts); %.1f of
      // runaway coordinates is unbounded, so the 16-byte reserve is no guarantee.
      const int wrote = _snprintf_s(histbuf + hoff, (size_t)room, _TRUNCATE,
                                    "%s%d:%u", i ? "," : "",
                                    s_methodHistValue[i], s_methodHistCount[i]);
      if (wrote < 0) break;
      hoff += wrote;
    }
    char asBuiltBuf[24], afterViewBuf[24];
    if (s_methodWorstAsBuiltSeen)
      _snprintf_s(asBuiltBuf, sizeof asBuiltBuf, _TRUNCATE, "%d",
                 s_methodWorstAsBuilt);
    else
      _snprintf_s(asBuiltBuf, sizeof asBuiltBuf, _TRUNCATE, "none yet");
    if (s_methodWorstAfterViewSeen)
      _snprintf_s(afterViewBuf, sizeof afterViewBuf, _TRUNCATE, "%d",
                 s_methodWorstAfterView);
    else
      _snprintf_s(afterViewBuf, sizeof afterViewBuf, _TRUNCATE, "none yet");
    ek_log("cutpaint: STAGE C METHOD HISTOGRAM (candidate A) -- %u gated wall "
           "call(s) carried DAMETH_CLAMPED (4), %u did not; distinct method "
           "value(s) seen: %s%s; the worst-AS-BUILT quad's method was %s; the "
           "worst-AFTER-VIEWPORT-CLIP quad's method was %s; %u of our wall "
           "paint(s) had the bit masked off by the diagnostic row below",
           s_methodClamped, s_methodUnclamped, histbuf,
           s_methodHistOther != 0 ? " (+other)" : "", asBuiltBuf, afterViewBuf,
           s_wallUnclamped);
  }

  if (tick && (s_extFlatSeen || s_extFlatNoMeasure != 0)) {
    if (s_extFlatSeen) {
      char vtxbuf[256];
      vtxbuf[0] = '\0';
      int off = 0;
      for (int k = 0; k < s_extFlatWorstN && k < EK_CUT_POLY_MAX; k++) {
        const int room = (int)sizeof(vtxbuf) - off;
        if (room <= 16) break;
        const int wrote = _snprintf_s(vtxbuf + off, (size_t)room, _TRUNCATE,
                                      "%s(%.1f,%.1f)", k ? "," : "",
                                      s_extFlatWorstSx[k],
                                      s_extFlatWorstSy[k]);
        if (wrote < 0) break;
        off += wrote;
      }
      ek_log("cutpaint: FLAT QUAD EXTENT -- worst %.2fx the screen height "
             "(%.1f px against a %.1f px screen) AS BUILT, and worst %.2fx "
             "(%.1f px) AFTER THE VIEWPORT CLIP%s on the polygon actually "
             "handed to polymost_drawpoly (both in final-row py space, the "
             "same expression stage C's third figure uses); %u quad(s) "
             "over 1x, %u over 4x, %u over 16x, %u over 64x AS BUILT; %u not "
             "measurable, %u after the clip. Worst AS-BUILT "
             "polygon: sector %d %s, %d vertex/vertices %s",
             s_extFlatMaxRatio, s_extFlatMaxPx, s_extFlatScreenPx,
             s_extFlatAfterMaxRatio, s_extFlatAfterMaxPx,
             s_extFlatAfterSeen ? "" : " (none measured yet)",
             s_extFlatOver1, s_extFlatOver4, s_extFlatOver16, s_extFlatOver64,
             s_extFlatNoMeasure, s_extFlatAfterNoMeasure,
             s_extFlatWorstSector,
             s_extFlatWorstIsFloor ? "floor" : "ceiling", s_extFlatWorstN,
             vtxbuf);
    } else {
      ek_log("cutpaint: FLAT QUAD EXTENT -- no ratio is printed: all %u "
             "polygon(s) so far gave no finite ratio, so either the "
             "projection's horiz or the polygon itself was unusable",
             s_extFlatNoMeasure);
    }
  }

  if (tick && (s_extFlatXSeen || s_extFlatXNoMeasure != 0)) {
    if (s_extFlatXSeen && s_extFlatXHaveRatio) {
      ek_log("cutpaint: FLAT X EXTENT -- worst %.2fx the view's own width "
             "(%.1f px against the view bounds cutaway_xbounds publishes, the "
             "same width stage C's x-clamp clamps into) AS BUILT, worst %.1f "
             "px AFTER THE VIEWPORT CLIP%s; %u not measurable AS BUILT, %u "
             "after the clip",
             s_extFlatXMaxRatio, s_extFlatXMaxPx, s_extFlatXAfterMaxPx,
             !s_extFlatXAfterSeen ? " (none measured yet)"
                 : (s_extFlatXAfterHaveRatio ? "" : " (no ratio: width lost "
                    "between readings)"),
             s_extFlatXNoMeasure, s_extFlatXAfterNoMeasure);
    } else if (s_extFlatXSeen) {
      ek_log("cutpaint: FLAT X EXTENT -- worst %.1f RAW px AS BUILT, worst "
             "%.1f RAW px AFTER THE VIEWPORT CLIP; NO RATIO COULD BE FORMED "
             "FOR THAT SAMPLE, because the view's own width was unavailable "
             "when it was recorded (cutaway_xbounds INERT, unreadable, or "
             "still racing its locate) -- this says nothing about the rest of "
             "the flight, and a raw span beats a ratio against the screen "
             "height, which is the wrong denominator for x; %u not measurable "
             "AS BUILT, %u after the clip",
             s_extFlatXMaxPx, s_extFlatXAfterMaxPx,
             s_extFlatXNoMeasure, s_extFlatXAfterNoMeasure);
    } else {
      ek_log("cutpaint: FLAT X EXTENT -- no span is printed: all %u "
             "polygon(s) so far gave no finite x span",
             s_extFlatXNoMeasure);
    }
  }

  if (tick && (s_extFlatXAfterSeen || s_colSpanTracker.count != 0 ||
               s_colSpanTracker.ignored != 0)) {
    if (s_extFlatXAfterSeen) {
      char vtxbuf[256];
      vtxbuf[0] = '\0';
      int off = 0;
      for (int k = 0; k < s_extXAfterWorstN && k < EK_CUT_POLY_MAX; k++) {
        const int room = (int)sizeof(vtxbuf) - off;
        if (room <= 16) break;
        const int wrote = _snprintf_s(vtxbuf + off, (size_t)room, _TRUNCATE,
                                      "%s(%.1f,%.1f)", k ? "," : "",
                                      s_extXAfterWorstSx[k],
                                      s_extXAfterWorstSy[k]);
        if (wrote < 0) break;
        off += wrote;
      }
      char boundsBuf[96];
      if (s_extXAfterWorstHaveXBounds)
        _snprintf_s(boundsBuf, sizeof boundsBuf, _TRUNCATE,
                   "lo=%.3f hi=%.3f (span %.3f px)", s_extXAfterWorstXLo,
                   s_extXAfterWorstXHi,
                   s_extXAfterWorstXHi - s_extXAfterWorstXLo);
      else
        _snprintf_s(boundsBuf, sizeof boundsBuf, _TRUNCATE,
                   "no bound was readable for this polygon");
      ek_log("cutpaint: D11 WORST AFTER-CLIP FLAT -- sector %d %s, %d "
             "vertex/vertices %s; verdict %s (columns %s, rows %s)%s; column "
             "bound in force: %s. Column bound span this session: %.3f px "
             "narrowest, %.3f px widest, over %d accepted read(s), %d "
             "ignored (NaN, zero or negative)",
             s_extXAfterWorstSector,
             s_extXAfterWorstIsFloor ? "floor" : "ceiling", s_extXAfterWorstN,
             vtxbuf, cp_outcome_str(s_extXAfterWorstOutcome),
             cp_outcome_str(s_extXAfterWorstCols),
             cp_outcome_str(s_extXAfterWorstRows),
             s_extXAfterWorstHaveVerdict ? "" : " (NO VERDICT WAS RECORDED "
                 "for this polygon -- the measurement was reached without the "
                 "clip's outcome, which the brief asks to be named rather "
                 "than shown as a stale verdict)",
             boundsBuf,
             s_colSpanTracker.count != 0 ? s_colSpanTracker.minSpan : 0.0,
             s_colSpanTracker.count != 0 ? s_colSpanTracker.maxSpan : 0.0,
             s_colSpanTracker.count, s_colSpanTracker.ignored);
    } else {
      ek_log("cutpaint: D11 WORST AFTER-CLIP FLAT -- no after-clip polygon "
             "measured yet this session; column bound span so far: %d "
             "accepted read(s), %d ignored (NaN, zero or negative)",
             s_colSpanTracker.count, s_colSpanTracker.ignored);
    }
  }

  if (tick && s_flatViewOffered != 0) {
    ek_log("cutpaint: STAGE B VIEWPORT CLIP (D11) -- %u polygon(s) offered: "
           "%u passed untouched, %u trimmed to the view, %u removed whole as "
           "wholly outside it, %u refused as too many points for "
           "EK_CUT_POLY_MAX and painted as before, %u FITTED (clipped by every "
           "bound the engine's own 8-point painter could hold, the last one "
           "dropped to fit), and %u painted with NEITHER bound located (which "
           "is this path exactly as it behaved before). Those six sum to %u "
           "against %u offered. %u polygon(s) were withheld from "
           "polymost_drawpoly as longer than EK_CUT_DRAWPOLY_MAX, which must "
           "be zero",
           s_flatViewOffered, s_flatViewKept, s_flatViewCut, s_flatViewDrop,
           s_flatViewRefused, s_flatViewFitted, s_flatViewSkipped,
           s_flatViewKept + s_flatViewCut + s_flatViewDrop +
               s_flatViewRefused + s_flatViewFitted + s_flatViewSkipped,
           s_flatViewOffered, s_flatOverPaintCap);
    ek_log("cutpaint: STAGE B VIEWPORT CLIP BY AXIS (D11) -- THE COLUMNS "
           "(cutaway_xbounds, pre-tilt sx, the space polymost_domost's own "
           "head clamps in): %u untouched, %u trimmed, %u removed whole, %u "
           "not applied (no bounds located, or the walk refused), summing to "
           "%u. THE ROWS (gctang/gstang, final py): %u untouched, %u trimmed, "
           "%u removed whole, %u not applied, summing to %u. Against %u "
           "offered",
           s_flatColKept, s_flatColCut, s_flatColDrop, s_flatColNone,
           s_flatColKept + s_flatColCut + s_flatColDrop + s_flatColNone,
           s_flatRowKept, s_flatRowCut, s_flatRowDrop, s_flatRowNone,
           s_flatRowKept + s_flatRowCut + s_flatRowDrop + s_flatRowNone,
           s_flatViewOffered);
  }

  if (tick && s_extAfterSeen) {
    if (s_extAfterHasBounds)
      ek_log("cutpaint: WALL QUAD EXTENT (AFTER) -- the worst-AFTER quad "
             "arrived as x0=%.3f y0top=%.3f y0bot=%.3f x1=%.3f y1top=%.3f "
             "y1bot=%.3f and came out as x0=%.3f y0top=%.3f y0bot=%.3f "
             "x1=%.3f y1top=%.3f y1bot=%.3f, against bounds lo=%.3f hi=%.3f, "
             "clamped=%s",
             s_extAfterInX0, s_extAfterInY0Top, s_extAfterInY0Bot,
             s_extAfterInX1, s_extAfterInY1Top, s_extAfterInY1Bot,
             s_extAfterOutX0, s_extAfterOutY0Top, s_extAfterOutY0Bot,
             s_extAfterOutX1, s_extAfterOutY1Top, s_extAfterOutY1Bot,
             s_extAfterBoundLo, s_extAfterBoundHi,
             s_extAfterClamped ? "yes" : "no");
    else
      ek_log("cutpaint: WALL QUAD EXTENT (AFTER) -- the worst-AFTER quad "
             "arrived as x0=%.3f y0top=%.3f y0bot=%.3f x1=%.3f y1top=%.3f "
             "y1bot=%.3f and came out the same, because no bounds were "
             "located for it",
             s_extAfterInX0, s_extAfterInY0Top, s_extAfterInY0Bot,
             s_extAfterInX1, s_extAfterInY1Top, s_extAfterInY1Bot);
  }

  // One long frame when a map is first seen is the accepted cost of building on the clock that reads (footprint.h).
  footprint_frame_begin();

  cp_drawpoly();

  // The ESP's projection taken here, not at SwapBuffers: after polymost_drawrooms' resizeglcheck GL_VIEWPORT is the 3D
  // view's, and ud.camera* is already substituted. Holds only while g_cfg.freecamud and freecamsubst are at ALL
  // (config.cpp): a diverted row passes the engine's own value to the draw while s_proj.cam reads ours.
  const bool ok = project_begin(&s_proj, EK_PC_CUTPAINT);
  s_projOk = ok && s_proj.g.valid;
  if (!s_projOk) {
    s_noProj++;
    if (!s_saidNoProj) {
      s_saidNoProj = true;
      ek_log("cutpaint: no projection this frame -- &ud would not read, or the "
             "viewport is degenerate. Every flat call forwards to the engine "
             "while that lasts; this is a refusal, not a failure, and it is "
             "counted rather than latched");
    }
    return;
  }
}

// The plane's z at one point. Flat is the rule: getzsofslopeptr returns the flat pair before the heinum term unless a
// stat carries the slope bit (engine.cpp:14292), so asking per vertex would be three calls a triangle for nothing.
static int cp_plane_z(int sect, bool isFloor, const EkSectorInfo& si,
                      double wx, double wy) {
  if (!si.sloped) return isFloor ? si.florz : si.ceilz;
  FreecamBand b;
  EkSectorInfo at;
  const bool got = cam_band_fill(sect, (int)wx, (int)wy, &b, &at);
  if (!got) return isFloor ? si.florz : si.ceilz;
  return isFloor ? b.florz : b.ceilz;
}

// What polymost_drawalls' gate (polymost.cpp:5511 floor, :5930 ceiling) would answer for one plane at one camera pose,
// computed from the located sector and wall records, never by calling the host. False: a guarded read failed; log
// "unknown", not a stale z.
static bool cp_slope_side(int sect, bool isFloor, int camx, int camy,
                          int camz, int32_t* outZ, bool* outDrawn) {
  EkSectorInfo si;
  if (!sectorstate_info(sect, &si)) return false;
  EkWallCorner w1;
  if (!wallstate_corner(si.wallptr, &w1)) return false;
  int p2 = -1;
  if (!wallstate_point2(si.wallptr, &p2)) return false;
  EkWallCorner w2;
  if (!wallstate_corner(p2, &w2)) return false;
  const int planez = isFloor ? si.florz : si.ceilz;
  const int stat = isFloor ? si.florStat : si.ceilStat;
  const int heinum = isFloor ? si.florHeinum : si.ceilHeinum;
  const int32_t z = ekm_slope_z(planez, stat, heinum, w1.x, w1.y, w2.x, w2.y,
                                camx, camy);
  *outZ = z;
  *outDrawn = ekm_slope_side_drawn(isFloor, camz, z);
  return true;
}

static bool cp_flip_line_due(void) {
  const unsigned now = wallclock_ms();
  if (s_flipWinFresh || (now - s_flipWinMs) >= 1000u) {
    s_flipWinFresh = false;
    s_flipWinMs = now;
    s_flipWinLines = 0;
  }
  if (s_flipWinLines >= CP_FLIP_LINES_PER_SEC) return false;
  s_flipWinLines++;
  return true;
}

static bool cp_flip_scanned_bit(const CpFlatTable& t, int sect) {
  if (sect < 0 || sect >= EK_MAXSECTORS) return false;
  return (t.scanned[sect >> 3] & (unsigned char)(1 << (sect & 7))) != 0;
}

static void cp_flip_touch_counts(CpFlatRec& r, unsigned pass) {
  if (r.countStamp == pass) return;
  r.countStamp = pass;
  r.np = 0;
  r.dmPainted = 0;
  r.dmRepeat = 0;
  r.dmFwd = 0;
}

// At the first flat of a new MAIN pass the current table holds the pass just finished: it is compared with the one
// before and the tables swap. Only consecutive frames are compared. gotsector is not frozen yet (drawalls scans
// neighbours throughout), so the "scanned" snapshot is cutpaint_flip_pass_end's.
static void cp_flip_rollover(void) {
  const CpFlatTable& cur = s_flipTab[s_flipCur];
  const CpFlatTable& prev = s_flipTab[s_flipCur ^ 1];
  if (cur.valid && prev.valid && cur.frame == prev.frame + 1u) {
    s_flipPairs++;
    for (int key = 0; key < EK_MAXSECTORS * 2; key++) {
      const CpFlatRec& a = prev.rec[key];
      const CpFlatRec& b = cur.rec[key];
      const bool haveA = a.stamp == prev.pass;
      const bool haveB = b.stamp == cur.pass;
      if (!haveA && !haveB) continue;
      const double areaA = haveA ? a.area : 0.0;
      const double areaB = haveB ? b.area : 0.0;
      if (!ekm_flat_flipped(areaA, areaB)) continue;
      s_flips++;

      const int sect = key >> 1;
      const bool isFloor = (key & 1) != 0;
      const unsigned npA = a.countStamp == prev.pass ? a.np : 0;
      const unsigned npB = b.countStamp == cur.pass ? b.np : 0;
      const unsigned dmPA = a.countStamp == prev.pass ? a.dmPainted : 0;
      const unsigned dmPB = b.countStamp == cur.pass ? b.dmPainted : 0;
      const unsigned dmRA = a.countStamp == prev.pass ? a.dmRepeat : 0;
      const unsigned dmRB = b.countStamp == cur.pass ? b.dmRepeat : 0;
      const unsigned dmFA = a.countStamp == prev.pass ? a.dmFwd : 0;
      const unsigned dmFB = b.countStamp == cur.pass ? b.dmFwd : 0;

      int32_t prevTestZ = 0, curTestZ = 0;
      bool prevTestDrawn = false, curTestDrawn = false;
      const bool prevTestOk = prev.camValid &&
          cp_slope_side(sect, isFloor, prev.camX, prev.camY, prev.camZ,
                        &prevTestZ, &prevTestDrawn);
      const bool curTestOk = cur.camValid &&
          cp_slope_side(sect, isFloor, cur.camX, cur.camY, cur.camZ,
                        &curTestZ, &curTestDrawn);

      if (!haveA && prev.haveScanned) {
        switch (ekm_flat_flip_cause(cp_flip_scanned_bit(prev, sect) ? 1 : 0,
                                    npA, dmPA, dmRA, dmFA)) {
          case EK_FLIP_CAUSE_NOT_SCANNED: s_flipCauseNotScanned++; break;
          case EK_FLIP_CAUSE_SCANNED_NO_PLANE_CALL:
            s_flipCauseNoPlaneCall++;
            if (!prevTestOk || !haveB || !curTestOk) s_sideTestUnknown++;
            else if (prevTestDrawn != curTestDrawn) s_sideTestFlipped++;
            else s_sideTestAgreed++;
            break;
          case EK_FLIP_CAUSE_PLANE_CALL_NO_DOMOST: s_flipCauseNoDomost++; break;
          case EK_FLIP_CAUSE_DOMOST_NOT_PAINTED: s_flipCauseNotPainted++; break;
          default: break;
        }
      }
      if (!haveB && cur.haveScanned) {
        switch (ekm_flat_flip_cause(cp_flip_scanned_bit(cur, sect) ? 1 : 0,
                                    npB, dmPB, dmRB, dmFB)) {
          case EK_FLIP_CAUSE_NOT_SCANNED: s_flipCauseNotScanned++; break;
          case EK_FLIP_CAUSE_SCANNED_NO_PLANE_CALL:
            s_flipCauseNoPlaneCall++;
            if (!curTestOk || !haveA || !prevTestOk) s_sideTestUnknown++;
            else if (curTestDrawn != prevTestDrawn) s_sideTestFlipped++;
            else s_sideTestAgreed++;
            break;
          case EK_FLIP_CAUSE_PLANE_CALL_NO_DOMOST: s_flipCauseNoDomost++; break;
          case EK_FLIP_CAUSE_DOMOST_NOT_PAINTED: s_flipCauseNotPainted++; break;
          default: break;
        }
      }

      if (!cp_flip_line_due()) continue;

      char vbuf[160];
      vbuf[0] = '\0';
      int off = 0;
      int firstDiff = -1;
      for (int st = 0; st < CP_FLIP_STAGES; st++) {
        const int va = haveA ? a.verts[st] : 0;
        const int vb = haveB ? b.verts[st] : 0;
        if (va != vb && firstDiff < 0) firstDiff = st;
        const int room = (int)sizeof(vbuf) - off;
        if (room <= 1) break;
        const int wrote = _snprintf_s(vbuf + off, (size_t)room, _TRUNCATE,
                                      "%s%s=%d/%d", st ? " " : "",
                                      kCpFlipStage[st], va, vb);
        if (wrote < 0) break;
        off += wrote;
      }
      char scanBufA[64], scanBufB[64];
      if (prev.haveScanned)
        _snprintf_s(scanBufA, sizeof scanBufA, _TRUNCATE,
                   "scanned %d np %u dm %u (painted %u repeat %u fwd %u)",
                   cp_flip_scanned_bit(prev, sect) ? 1 : 0, npA,
                   dmPA + dmRA + dmFA, dmPA, dmRA, dmFA);
      else
        _snprintf_s(scanBufA, sizeof scanBufA, _TRUNCATE,
                   "scanned unknown (gotsector unreadable)");
      if (cur.haveScanned)
        _snprintf_s(scanBufB, sizeof scanBufB, _TRUNCATE,
                   "scanned %d np %u dm %u (painted %u repeat %u fwd %u)",
                   cp_flip_scanned_bit(cur, sect) ? 1 : 0, npB,
                   dmPB + dmRB + dmFB, dmPB, dmRB, dmFB);
      else
        _snprintf_s(scanBufB, sizeof scanBufB, _TRUNCATE,
                   "scanned unknown (gotsector unreadable)");

      char sideBufA[64], sideBufB[64];
      if (prevTestOk)
        _snprintf_s(sideBufA, sizeof sideBufA, _TRUNCATE,
                   "cam (%d,%d,%d) test z %d -> %s", prev.camX, prev.camY,
                   prev.camZ, prevTestZ, prevTestDrawn ? "drawn" : "culled");
      else
        _snprintf_s(sideBufA, sizeof sideBufA, _TRUNCATE, "test unknown");
      if (curTestOk)
        _snprintf_s(sideBufB, sizeof sideBufB, _TRUNCATE,
                   "cam (%d,%d,%d) test z %d -> %s", cur.camX, cur.camY,
                   cur.camZ, curTestZ, curTestDrawn ? "drawn" : "culled");
      else
        _snprintf_s(sideBufB, sizeof sideBufB, _TRUNCATE, "test unknown");

      ek_log("cutpaint flip: frame %u sector %d %s area %.0f -> %.0f px^2, "
             "verts %s, first differing stage %s; flat z %d -> %d%s, camera z "
             "%.0f -> %.0f (previous main pass frame %u%s%s); prev pass %s; "
             "this pass %s; prev side %s; this side %s",
             cur.frame, key >> 1, (key & 1) ? "floor" : "ceiling", areaA,
             areaB, vbuf, firstDiff >= 0 ? kCpFlipStage[firstDiff] : "none",
             haveA ? a.flatZ : 0, haveB ? b.flatZ : 0,
             (haveA && a.sloped) || (haveB && b.sloped) ? " (sloped: header z)"
                                                        : "",
             haveA ? a.camZ : 0.0, haveB ? b.camZ : 0.0, prev.frame,
             haveA ? "" : "; NOT PAINTED in the previous pass",
             haveB ? "" : "; NOT PAINTED in this pass", scanBufA, scanBufB,
             sideBufA, sideBufB);
    }
  }
  s_flipCur ^= 1;
  CpFlatTable& fresh = s_flipTab[s_flipCur];
  fresh.valid = true;
  fresh.pass = s_flipPassSeq;
  fresh.frame = camera_frame_number();
  fresh.haveScanned = false;
  memset(fresh.scanned, 0, sizeof fresh.scanned);
  fresh.camValid = s_projOk;
  fresh.camX = s_projOk ? s_proj.cam.x : 0;
  fresh.camY = s_projOk ? s_proj.cam.y : 0;
  fresh.camZ = s_projOk ? s_proj.cam.z : 0;
}

// The gotsector snapshot, taken from camera.cpp right after a main pass's own orig() returns: any earlier undercounts,
// as drawalls' polymost_scansector calls (polymost.cpp:6547/6616/6657) interleave with the pass's domost/flat calls.
void cutpaint_flip_pass_end(void) {
  if (!s_flipMainPass) return;
  if (s_flipTab[s_flipCur].pass != s_flipPassSeq) cp_flip_rollover();
  CpFlatTable& t = s_flipTab[s_flipCur];
  t.haveScanned = false;
  memset(t.scanned, 0, sizeof t.scanned);
  const unsigned char* const base = s_base;
  const unsigned gotRva = nocull_gotsector_rva();
  const int gotBytes = nocull_gotsector_bytes();
  if (base != NULL && gotRva != 0 && gotBytes > 0 &&
      (size_t)gotBytes <= sizeof t.scanned &&
      cp_readable(base + gotRva, (size_t)gotBytes)) {
    memcpy(t.scanned, base + gotRva, (size_t)gotBytes);
    t.haveScanned = true;
  }
}

void cutpaint_flip_note_np(int sect, bool isFloor) {
  if (!s_flipMainPass || sect < 0 || sect >= EK_MAXSECTORS) return;
  if (s_flipTab[s_flipCur].pass != s_flipPassSeq) cp_flip_rollover();
  CpFlatTable& t = s_flipTab[s_flipCur];
  CpFlatRec& r = t.rec[sect * 2 + (isFloor ? 1 : 0)];
  cp_flip_touch_counts(r, t.pass);
  r.np++;
}

void cutpaint_flip_note_dm_forward(int sect, bool isFloor) {
  if (!s_flipMainPass || sect < 0 || sect >= EK_MAXSECTORS) return;
  if (s_flipTab[s_flipCur].pass != s_flipPassSeq) cp_flip_rollover();
  CpFlatTable& t = s_flipTab[s_flipCur];
  CpFlatRec& r = t.rec[sect * 2 + (isFloor ? 1 : 0)];
  cp_flip_touch_counts(r, t.pass);
  r.dmFwd++;
}

unsigned cutpaint_flat_painted(void)      { return s_planes; }
unsigned cutpaint_flat_no_footprint(void) { return s_noFootprint; }
unsigned cutpaint_flat_no_sector(void)    { return s_noSector; }
unsigned cutpaint_flat_no_proj(void)      { return s_noProj; }
unsigned cutpaint_wall_painted(void)      { return s_walls; }
unsigned cutpaint_wall_inert(void)        { return s_wallInert; }
unsigned cutpaint_wall_no_quad(void)      { return s_wallNoQuad; }
unsigned cutpaint_wall_drop_tilt(void)    { return s_wallDropTilt; }
unsigned cutpaint_wall_drop_wind(void)    { return s_wallDropWind; }
unsigned cutpaint_wall_tall_refused(void) { return s_wallTallRefused; }

bool cutpaint_flat(int sect, bool isFloor, int method) {
  if (s_dead || !s_projOk) return false;
  const EkDrawPolyFn draw = s_drawpoly;
  if (!draw) return false;
  if (sect < 0 || sect >= EK_MAXSECTORS) return false;

  // Every whole-surface refusal is decided before the first polygon is painted, so "never paint half a surface" is
  // true: past here only the two clips can drop anything, and a clipped-away triangle is geometry not on screen.
  const double* tris = NULL;
  int count = 0;
  const bool haveTris = footprint_sector_tris(sect, &tris, &count);
  if (!haveTris) { s_noFootprint++; return false; }

  EkSectorInfo si;
  const bool haveInfo = sectorstate_info(sect, &si);
  if (!haveInfo) { s_noSector++; return false; }

  unsigned char* const done = isFloor ? s_floorDone : s_ceilDone;
  const unsigned char bit = (unsigned char)(1 << (sect & 7));
  if (done[sect >> 3] & bit) {
    // Already painted this frame, and the answer is still "do not run the engine's": forwarding would paint the plane
    // twice against the accumulated claims (the apron this feature removes) and claim the span.
    s_repeats++;
    if (s_flipMainPass) {
      if (s_flipTab[s_flipCur].pass != s_flipPassSeq) cp_flip_rollover();
      CpFlatTable& t = s_flipTab[s_flipCur];
      CpFlatRec& r = t.rec[sect * 2 + (isFloor ? 1 : 0)];
      cp_flip_touch_counts(r, t.pass);
      r.dmRepeat++;
    }
    return true;
  }
  done[sect >> 3] |= bit;

  const EkBuildProj& g = s_proj.g;
  const double camx = (double)s_proj.cam.x;
  const double camy = (double)s_proj.cam.y;
  const double camz = (double)s_proj.cam.z;
  if (si.sloped) s_sloped++;

  const bool flipRec = s_flipMainPass;
  if (flipRec && s_flipTab[s_flipCur].pass != s_flipPassSeq) cp_flip_rollover();
  int flipVerts[CP_FLIP_STAGES] = {0, 0, 0, 0, 0};
  bool flipAny = false;
  float flipMinX = 0.0f, flipMaxX = 0.0f, flipMinY = 0.0f, flipMaxY = 0.0f;

  float extTc = 1.0f, extTs = 0.0f;
  const bool haveTangs = cutaway_tangs(&extTc, &extTs);
  const double extScreenPx = 2.0 * g.horiz;

  float extXLo = 0.0f, extXHi = 0.0f;
  const bool haveXBoundsRaw = cutaway_xbounds(&extXLo, &extXHi);
  const double extWidthPx = (double)extXHi - (double)extXLo;
  const bool haveXWidth = haveXBoundsRaw && extWidthPx > 0.0 &&
                          extWidthPx < 1e30;

  if (haveXBoundsRaw) {
    ekm_cut_col_span_track(&s_colSpanTracker, extWidthPx);
  }

  for (int i = 0; i < count; i++) {
    const double* const d = tris + (size_t)i * 6;

    EkCutPoly p;
    memset(&p, 0, sizeof p);
    p.n = 3;
    for (int k = 0; k < 3; k++) {
      const double wx = d[k * 2];
      const double wy = d[k * 2 + 1];
      p.v[k].wx = wx;
      p.v[k].wy = wy;
      p.v[k].vx = ekm_build_glat(g, wx - camx, wy - camy);
      p.v[k].depth = ekm_build_gdepth_xy(g, wx - camx, wy - camy);
    }
    flipVerts[0] += p.n;

    const EkCutPoly n = ekm_cut_near_clip(&p);
    if (n.n < 3) { s_dropNear++; continue; }
    flipVerts[1] += n.n;

    EkCutPoly s = n;
    for (int k = 0; k < s.n; k++) {
      const int z = cp_plane_z(sect, isFloor, si, s.v[k].wx, s.v[k].wy);
      s.v[k].sx = ekm_build_pre_sx(g, s.v[k].vx, s.v[k].depth);
      s.v[k].sy = ekm_build_pre_sy(g, (double)z - camz, s.v[k].depth);
    }

    const EkCutPoly c = ekm_cut_tilt_clip(&s, g);
    if (c.n < 3) { s_dropTilt++; continue; }
    flipVerts[2] += c.n;

    const CpExtent flatExt = cp_extent_rows(&c, g, (double)extTc, (double)extTs,
                                            extScreenPx);
    if (!flatExt.ok) {
      s_extFlatNoMeasure++;
    } else {
      if (flatExt.ratio > 1.0) s_extFlatOver1++;
      if (flatExt.ratio > 4.0) s_extFlatOver4++;
      if (flatExt.ratio > 16.0) s_extFlatOver16++;
      if (flatExt.ratio > 64.0) s_extFlatOver64++;
      if (!s_extFlatSeen || flatExt.ratio > s_extFlatMaxRatio) {
        s_extFlatSeen = true;
        s_extFlatMaxRatio = flatExt.ratio;
        s_extFlatMaxPx = flatExt.px;
        s_extFlatScreenPx = extScreenPx;
        s_extFlatWorstN = c.n;
        for (int k = 0; k < c.n; k++) {
          s_extFlatWorstSx[k] = c.v[k].sx;
          s_extFlatWorstSy[k] = c.v[k].sy;
        }
        s_extFlatWorstSector = sect;
        s_extFlatWorstIsFloor = isFloor;
      }
    }

    const CpExtentX flatExtX = ekm_cut_extent_x(&c, haveXWidth ? 1 : 0,
                                                extWidthPx);
    if (!flatExtX.ok) {
      s_extFlatXNoMeasure++;
    } else if (!s_extFlatXSeen || flatExtX.px > s_extFlatXMaxPx) {
      s_extFlatXSeen = true;
      s_extFlatXMaxPx = flatExtX.px;
      s_extFlatXHaveRatio = flatExtX.haveRatio != 0;
      s_extFlatXMaxRatio = flatExtX.haveRatio ? flatExtX.ratio : 0.0;
    }

    // The viewport clip, AFTER the tilt clip (the py/N/D equivalence needs D > 0) and before the winding fix. Refused
    // paints `c` as before the clip. It cannot fire: `c` is convex (coplanar vertices, affine clips), so each
    // half-plane adds one point (3 -> 9 against EK_CUT_POLY_MAX 12). A per-triangle refusal paints, not forwards.
    EkCutPoly vc = c;
    s_flatViewOffered++;
    EkCutViewOutcome vcOutcome = EK_CUT_VIEW_REFUSED;
    EkCutViewOutcome vcCols = EK_CUT_VIEW_REFUSED;
    EkCutViewOutcome vcRows = EK_CUT_VIEW_REFUSED;
    bool haveVcVerdict = false;
    {
      const EkCutViewXYResult vr = ekm_cut_view_clip_xy(
          &c, g, (double)extTc, (double)extTs, haveTangs ? 1 : 0, 0.0,
          extScreenPx, haveXBoundsRaw ? 1 : 0, (double)extXLo, (double)extXHi);
      vcOutcome = vr.outcome;
      vcCols = vr.cols;
      vcRows = vr.rows;
      haveVcVerdict = true;
      if (vr.cols == EK_CUT_VIEW_KEPT) s_flatColKept++;
      else if (vr.cols == EK_CUT_VIEW_TRIMMED) s_flatColCut++;
      else if (vr.cols == EK_CUT_VIEW_REMOVED) s_flatColDrop++;
      else s_flatColNone++;
      if (vr.rows == EK_CUT_VIEW_KEPT) s_flatRowKept++;
      else if (vr.rows == EK_CUT_VIEW_TRIMMED) s_flatRowCut++;
      else if (vr.rows == EK_CUT_VIEW_REMOVED) s_flatRowDrop++;
      else s_flatRowNone++;

      if (vr.outcome == EK_CUT_VIEW_REMOVED) { s_flatViewDrop++; continue; }
      if (vr.outcome == EK_CUT_VIEW_REFUSED) s_flatViewRefused++;
      else if (vr.outcome == EK_CUT_VIEW_SKIPPED) s_flatViewSkipped++;
      else if (vr.outcome == EK_CUT_VIEW_KEPT) s_flatViewKept++;
      else if (vr.outcome == EK_CUT_VIEW_FITTED) {
        s_flatViewFitted++;
        vc = vr.poly;
      } else { s_flatViewCut++; vc = vr.poly; }
    }
    flipVerts[3] += vc.n;

    const CpExtent flatAfter = cp_extent_rows(&vc, g, (double)extTc,
                                              (double)extTs, extScreenPx);
    if (!flatAfter.ok) s_extFlatAfterNoMeasure++;
    else if (!s_extFlatAfterSeen || flatAfter.ratio > s_extFlatAfterMaxRatio) {
      s_extFlatAfterSeen = true;
      s_extFlatAfterMaxRatio = flatAfter.ratio;
      s_extFlatAfterMaxPx = flatAfter.px;
    }

    const CpExtentX flatAfterX = ekm_cut_extent_x(&vc, haveXWidth ? 1 : 0,
                                                  extWidthPx);
    if (!flatAfterX.ok) {
      s_extFlatXAfterNoMeasure++;
    } else if (!s_extFlatXAfterSeen || flatAfterX.px > s_extFlatXAfterMaxPx) {
      s_extFlatXAfterSeen = true;
      s_extFlatXAfterMaxPx = flatAfterX.px;
      s_extFlatXAfterHaveRatio = flatAfterX.haveRatio != 0;
      s_extFlatXAfterMaxRatio = flatAfterX.haveRatio ? flatAfterX.ratio : 0.0;

      s_extXAfterWorstN = vc.n;
      for (int k = 0; k < vc.n && k < EK_CUT_POLY_MAX; k++) {
        s_extXAfterWorstSx[k] = vc.v[k].sx;
        s_extXAfterWorstSy[k] = vc.v[k].sy;
      }
      s_extXAfterWorstSector = sect;
      s_extXAfterWorstIsFloor = isFloor;
      s_extXAfterWorstHaveVerdict = haveVcVerdict;
      s_extXAfterWorstOutcome = vcOutcome;
      s_extXAfterWorstCols = vcCols;
      s_extXAfterWorstRows = vcRows;
      s_extXAfterWorstHaveXBounds = haveXBoundsRaw;
      s_extXAfterWorstXLo = (double)extXLo;
      s_extXAfterWorstXHi = (double)extXHi;
    }

    // Checked, not trusted: polymost_drawpoly fills `float px[8], py[8], dd[8], uu[8], vv[8]` from n
    // (polymost.cpp:3232), so a ninth point writes past five of the engine's stack arrays silently.
    if (vc.n > EK_CUT_DRAWPOLY_MAX) { s_flatOverPaintCap++; continue; }

    double px[EK_CUT_POLY_MAX], py[EK_CUT_POLY_MAX];
    for (int k = 0; k < vc.n; k++) { px[k] = vc.v[k].sx; py[k] = vc.v[k].sy; }
    // polymost_drawpoly accepts ONE winding and returns on the other without saying so: not a tidy-up, the difference
    // between a painted surface and an invisible one.
    if (!ekm_cut_poly_fix_winding(px, py, vc.n)) { s_dropWind++; continue; }

    EkCutVec2f dp[EK_CUT_POLY_MAX];
    for (int k = 0; k < vc.n; k++) {
      dp[k].x = (float)px[k];
      dp[k].y = (float)py[k];
    }
    flipVerts[4] += vc.n;
    for (int k = 0; k < vc.n; k++) {
      if (!flipAny) {
        flipMinX = flipMaxX = dp[k].x;
        flipMinY = flipMaxY = dp[k].y;
        flipAny = true;
        continue;
      }
      if (dp[k].x < flipMinX) flipMinX = dp[k].x;
      if (dp[k].x > flipMaxX) flipMaxX = dp[k].x;
      if (dp[k].y < flipMinY) flipMinY = dp[k].y;
      if (dp[k].y > flipMaxY) flipMaxY = dp[k].y;
    }
    // The engine's painter on the engine's texture state: gdx..gvo, globalpicnum, globalshade and pow2xsplit were set
    // by polymost_drawalls just before this domost call and hold for it alone, so it happens in the hook and cannot be
    // batched.
    draw(dp, vc.n, method);
    s_tris++;
  }

  if (flipRec) {
    CpFlatTable& t = s_flipTab[s_flipCur];
    CpFlatRec& r = t.rec[sect * 2 + (isFloor ? 1 : 0)];
    cp_flip_touch_counts(r, t.pass);
    r.dmPainted++;
    r.stamp = t.pass;
    for (int st = 0; st < CP_FLIP_STAGES; st++) r.verts[st] = flipVerts[st];
    r.flatZ = isFloor ? si.florz : si.ceilz;
    r.sloped = si.sloped != 0;
    r.area = flipAny ? ((double)flipMaxX - (double)flipMinX) *
                           ((double)flipMaxY - (double)flipMinY)
                     : 0.0;
    r.camZ = camz;
  }

  s_planes++;
  if (!s_saidFirst) {
    s_saidFirst = true;
    ek_log("cutpaint: PAINTING -- sector %d's %s is the first whole footprint "
           "drawn by this mod, %d triangle(s) through polymost_drawpoly at rva "
           "0x%07X with method %d, and the engine's own domost did not run for "
           "it. Interiors visible through what used to be opaque is the "
           "PREDICTED outcome here; straight down staying largely black is too, "
           "because with no front-facing walls there are no bunches and so no "
           "calls to paint from",
           sect, isFloor ? "floor" : "ceiling", count, cutaway_drawpoly_rva(),
           method);
  }
  return true;
}

// Stage C: far simpler than the flat, as domost's own clip box already IS the wall's quad in screen space (cutpaint.h):
// only the tilt clip and the winding stand between the arguments and the painter. Every refusal precedes the one
// drawpoly call, so a wall is wholly painted by us or wholly left to the engine.
bool cutpaint_wall(double x0, double y0top, double y0bot,
                   double x1, double y1top, double y1bot, int method) {
  // Counted, not logged here: both refusals are the two facts cutpaint_frame_begin tests at the top of every armed
  // frame, and it owns their once-only lines. Correct only while frame_begin runs first (nocull's seed seam calls it
  // above the gate).
  cp_method_tally(method);

  if (s_dead || !s_projOk) { s_wallInert++; return false; }
  const EkDrawPolyFn draw = s_drawpoly;
  if (!draw) { s_wallInert++; return false; }

  EkCutPoly q;
  const bool haveQuad =
      ekm_cut_wall_quad(x0, y0top, y0bot, x1, y1top, y1bot, &q) != 0;
  if (!haveQuad) { s_wallNoQuad++; return false; }

  // The viewport's height is 2 * horiz (EkBuildProj::horiz is ydimen/2).
  const double screenPx = 2.0 * s_proj.g.horiz;
  // Taken on the quad AS BUILT: the incoming boxes are the engine's projection and the clamp below does not change
  // them.
  const CpExtent before = cp_extent(&q, screenPx);
  if (!before.ok) {
    s_extNoMeasure++;
  } else {
    if (before.ratio > 1.0) s_extOver1++;
    if (before.ratio > 4.0) s_extOver4++;
    if (before.ratio > 16.0) s_extOver16++;
    if (before.ratio > 64.0) s_extOver64++;
    if (!s_extSeen || before.ratio > s_extMaxRatio) {
      s_extSeen = true;
      s_extMaxRatio = before.ratio;
      s_extMaxPx = before.px;
      s_extScreenPx = screenPx;
      s_extX0 = x0; s_extY0Top = y0top; s_extY0Bot = y0bot;
      s_extX1 = x1; s_extY1Top = y1top; s_extY1Bot = y1bot;
      s_methodWorstAsBuiltSeen = true;
      s_methodWorstAsBuilt = method;
    }
  }

  // The tall-wall refusal uses the as-built measurement: the engine clamps a near endpoint to SCISDIST and the line is
  // enormous before any clip of ours; clips never touch the wrapped texture's UV gradient. Neither painted nor
  // forwarded: true makes the caller skip domost too.
  const int tallMultiple =
      ekm_cutaway_tall_wall_multiple(g_cfg.freecamcutawaytallwall);
  if (ekm_cut_tall_wall_refuse(before.ok, before.ratio, tallMultiple)) {
    s_wallTallRefused++;
    return true;
  }
  if (before.ok &&
      (!s_wallTallMaxAllowedSeen || before.ratio > s_wallTallMaxAllowed)) {
    s_wallTallMaxAllowedSeen = true;
    s_wallTallMaxAllowed = before.ratio;
  }

  // The engine's own x-clamp, once skipped: polymost_domost clamps span ends into [xbl, xbr] and lerps y
  // (polymost.cpp:3953-3975); without it a quad 1163 screen heights tall reached polymost_drawpoly. If cutaway_xbounds
  // refuses, the quad paints unclamped.
  double outX0 = x0, outY0Top = y0top, outY0Bot = y0bot;
  double outX1 = x1, outY1Top = y1top, outY1Bot = y1bot;
  bool clampReported = false;

  float boundLo = 0.0f, boundHi = 0.0f;
  const bool haveBounds = cutaway_xbounds(&boundLo, &boundHi);
  if (!haveBounds) {
    s_wallNoBounds++;
  } else {
    const EkCutXBound xb = ekm_cut_x_bound(x0, y0top, y0bot, x1, y1top, y1bot,
                                           (double)boundLo, (double)boundHi);
    if (!xb.ok) { s_wallOutside++; return false; }
    // Rebuilt from the clamped six so the quad's vertex order has one author, and a clamp that pinched an end to zero
    // width is refused by the same rule.
    const bool haveClamped =
        ekm_cut_wall_quad(xb.x0, xb.y0top, xb.y0bot,
                          xb.x1, xb.y1top, xb.y1bot, &q) != 0;
    if (!haveClamped) { s_wallClampEmpty++; return false; }
    if (xb.clamped) s_wallClamped++;
    outX0 = xb.x0; outY0Top = xb.y0top; outY0Bot = xb.y0bot;
    outX1 = xb.x1; outY1Top = xb.y1top; outY1Bot = xb.y1bot;
    clampReported = xb.clamped != 0;
  }

  const CpExtent after = cp_extent(&q, screenPx);
  if (!after.ok) {
    s_extAfterNoMeasure++;
  } else if (!s_extAfterSeen || after.ratio > s_extAfterMaxRatio) {
    s_extAfterSeen = true;
    s_extAfterMaxRatio = after.ratio;
    s_extAfterMaxPx = after.px;
    s_extAfterInX0 = x0; s_extAfterInY0Top = y0top; s_extAfterInY0Bot = y0bot;
    s_extAfterInX1 = x1; s_extAfterInY1Top = y1top; s_extAfterInY1Bot = y1bot;
    s_extAfterOutX0 = outX0; s_extAfterOutY0Top = outY0Top;
    s_extAfterOutY0Bot = outY0Bot; s_extAfterOutX1 = outX1;
    s_extAfterOutY1Top = outY1Top; s_extAfterOutY1Bot = outY1Bot;
    s_extAfterBoundLo = haveBounds ? (double)boundLo : 0.0;
    s_extAfterBoundHi = haveBounds ? (double)boundHi : 0.0;
    s_extAfterHasBounds = haveBounds;
    s_extAfterClamped = clampReported;
  }

  // The tilt clip, not the near clip: the points arrive projected, but polymost_drawpoly divides by a TILTED depth it
  // never clips against (polymost.cpp:3241-3246), and a wall quad is not bounded by the viewport as the engine's span
  // is.
  const EkCutPoly c = ekm_cut_tilt_clip(&q, s_proj.g);
  if (c.n < 3) { s_wallDropTilt++; return false; }

  // The viewport, which vsp used to carry: after the tilt clip (the py/N/D equivalence needs D > 0, kept by
  // EKM_CUT_TILT_NEAR) and before the winding fix (running after it would undo the winding). Without cutaway_tangs the
  // quad paints as before.
  EkCutPoly v = c;
  float tc = 1.0f, ts = 0.0f;
  const bool haveTangs = cutaway_tangs(&tc, &ts);
  if (!haveTangs) {
    s_wallNoTangs++;
  } else {
    v = ekm_cut_view_clip(&c, s_proj.g, (double)tc, (double)ts,
                          0.0, 2.0 * s_proj.g.horiz);
    if (v.n < 3) { s_wallViewDrop++; return false; }
    bool moved = v.n != c.n;
    for (int k = 0; k < v.n && !moved; k++)
      moved = v.v[k].sx != c.v[k].sx || v.v[k].sy != c.v[k].sy;
    if (moved) s_wallViewCut++; else s_wallViewKept++;
  }

  const CpExtent painted = cp_extent_rows(&v, s_proj.g, (double)tc, (double)ts,
                                          screenPx);
  if (!painted.ok) s_extViewNoMeasure++;
  else if (!s_extViewSeen || painted.ratio > s_extViewMaxRatio) {
    s_extViewSeen = true;
    s_extViewMaxRatio = painted.ratio;
    s_extViewMaxPx = painted.px;
    s_methodWorstAfterViewSeen = true;
    s_methodWorstAfterView = method;
  }

  double px[EK_CUT_POLY_MAX], py[EK_CUT_POLY_MAX];
  for (int k = 0; k < v.n; k++) { px[k] = v.v[k].sx; py[k] = v.v[k].sy; }
  // A ceiling-side call arrives with x0 > x1 (polymost.cpp:6409, :6536 pass the right end first; :3942 swaps back), so
  // its quad is wound the opposite way and polymost_drawpoly would drop it silently: this makes the two sides one path.
  if (!ekm_cut_poly_fix_winding(px, py, v.n)) { s_wallDropWind++; return false; }

  EkCutVec2f dp[EK_CUT_POLY_MAX];
  for (int k = 0; k < v.n; k++) {
    dp[k].x = (float)px[k];
    dp[k].y = (float)py[k];
  }
  // The single-variable experiment: masks DAMETH_CLAMPED off the method for OUR paint call only. Every forwarding path
  // returned earlier and cutaway.cpp's orig(...) never reads `method`; default 0 leaves it bit for bit.
  int paintedMethod = method;
  if (g_cfg.freecamcutawayunclamp != 0 && (method & EK_DAMETH_CLAMPED) != 0) {
    paintedMethod = method & ~EK_DAMETH_CLAMPED;
    s_wallUnclamped++;
  }

  // The engine's painter on its texture state. Suppressing domost leaves drawpoly_alpha and drawpoly_blend
  // (polymost.cpp:3451, :3470) already zeroed by polymost_drawalls (:5353). Over the cap, false forwards cleanly
  // (nothing of this quad was painted).
  if (v.n > EK_CUT_DRAWPOLY_MAX) { s_wallOverPaintCap++; return false; }
  draw(dp, v.n, paintedMethod);
  s_walls++;

  if (!s_saidFirstWall) {
    s_saidFirstWall = true;
    ek_log("cutpaint: STAGE C PAINTING -- the first wall quad drawn by this mod "
           "is (%.1f,%.1f)-(%.1f,%.1f)-(%.1f,%.1f)-(%.1f,%.1f), %d point(s) "
           "after the tilt and viewport clips, through polymost_drawpoly at rva "
           "0x%07X with method %d, and the engine's own domost did not run for "
           "it. With the walls no longer claiming either, NOTHING closes the "
           "screen: every bunch should now reach its flat call, so the plane "
           "count per frame is the number that says whether this stage did what "
           "it claims",
           q.v[0].sx, q.v[0].sy, q.v[1].sx, q.v[1].sy, q.v[2].sx, q.v[2].sy,
           q.v[3].sx, q.v[3].sy, v.n, cutaway_drawpoly_rva(), method);
  }
  return true;
}

// The identical AS-BUILT ratio, exposed for cutaway.cpp's domost thunk deciding whether to FORWARD a wall; touches no
// painting counter.
CpWallRatio cutpaint_wall_ratio(double x0, double y0top, double y0bot,
                                double x1, double y1top, double y1bot) {
  CpWallRatio r; r.projOk = 0; r.haveQuad = 0; r.ratioOk = 0; r.ratio = 0.0;
  if (!s_projOk) return r;
  r.projOk = 1;

  EkCutPoly q;
  const bool haveQuad =
      ekm_cut_wall_quad(x0, y0top, y0bot, x1, y1top, y1bot, &q) != 0;
  r.haveQuad = haveQuad ? 1 : 0;
  // No quad must FORWARD, not refuse: the caller checks `haveQuad` before treating `ratioOk == 0` as a refusal (the
  // defect that withheld 43% of gated wall calls).
  if (!haveQuad) return r;

  const double screenPx = 2.0 * s_proj.g.horiz;
  const CpExtent e = cp_extent(&q, screenPx);
  r.ratioOk = e.ok;
  r.ratio = e.ratio;
  return r;
}

// The raw counterpart for an inverted box, where ekm_cut_wall_quad returns nothing; no projection means forward, not
// refuse.
CpWallRawRatio cutpaint_wall_raw_ratio(double y0top, double y0bot,
                                       double y1top, double y1bot) {
  CpWallRawRatio r; r.projOk = 0; r.ok = 0; r.ratio = 0.0;
  if (!s_projOk) return r;
  r.projOk = 1;

  const double screenPx = 2.0 * s_proj.g.horiz;
  double ratio = 0.0;
  r.ok = ekm_cut_wall_raw_ratio(y0top, y0bot, y1top, y1bot, screenPx, &ratio);
  r.ratio = ratio;
  return r;
}
