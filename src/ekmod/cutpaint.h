#pragma once

// The cutaway's flat painter: paint the flats ourselves through the engine's own polymost_drawpoly and CLAIM NOTHING.
// Build's renderer is WALL-DRIVEN: a sector with no front-facing wall is never SUBMITTED, so painting its WHOLE floor
// plan when a flat call arrives is what shows it. The sector is the 9th argument of polymost_internal_nonparallaxed.

#include "projmath.h"

// WHAT MAY BE HANDED TO THE ENGINE'S PAINTER IS THE ENGINE'S NUMBER: MAX_DRAWPOLY_VERTS is 8 and polymost_drawpoly
// fills `float px[8]...` straight from n, so a ninth point writes past five stack arrays; its Bassert is
// `(void)(x)` under NDEBUG. No call of ours may exceed this.
const int EK_CUT_DRAWPOLY_MAX = 8;

// THE WORKING BUFFER IS DELIBERATELY LARGER (12): the flat chain is 3 -> near 4 -> tilt 5 -> colL 6 -> colR 7 -> rowT
// 8 -> rowB 9, and a cap of 8 would refuse exactly the polygons the clip exists to bound. A result longer than
// EK_CUT_DRAWPOLY_MAX is never painted with its extra points: the last stage that fits is returned.
const int EK_CUT_POLY_MAX = 12;

const double EKM_CUT_TILT_NEAR = 1.0;

struct EkCutVert { double wx, wy, vx, depth, sx, sy; };
struct EkCutPoly { int n; EkCutVert v[EK_CUT_POLY_MAX]; };

// WINDING, WHICH IS SILENT WHEN IT IS WRONG: polymost_drawpoly ACCEPTS ONE WINDING AND RETURNS ON THE OTHER with no
// log (polymost.cpp:3195-3206). Screen y grows DOWNWARD, so what survives is the screen-clockwise order domost
// builds.
inline int ekm_cut_poly_accepted(const double* px, const double* py, int n) {
  if (!px || !py || n < 3) return 0;
  if (n == 3)
    return (px[2] - px[1]) * (py[0] - py[1]) > (px[0] - px[1]) * (py[2] - py[1]) ? 1 : 0;
  double f = 0.0;
  for (int i = n - 2, j = n - 1, k = 0; k < n; i = j, j = k, k++)
    f += (px[i] - px[k]) * py[j];
  return f > 0.0 ? 1 : 0;
}

inline int ekm_cut_poly_fix_winding(double* px, double* py, int n) {
  if (!px || !py || n < 3 || n > EK_CUT_POLY_MAX) return 0;
  if (ekm_cut_poly_accepted(px, py, n)) return 1;
  for (int i = 0, j = n - 1; i < j; i++, j--) {
    const double tx = px[i]; px[i] = px[j]; px[j] = tx;
    const double ty = py[i]; py[i] = py[j]; py[j] = ty;
  }
  return ekm_cut_poly_accepted(px, py, n);
}

// THE WALL QUAD, WHICH THIS ENGINE HANDS OVER FOR FREE: polymost_domost's last four arguments are a CLIP BOX filled
// at FOUR call sites only (the upper and lower step, the YAX island wall, the solid wall); every other call takes the
// defaults. So "not a flat call" and "a wall call" are the same fact, and the box is the wall's quad in screen space.

inline int ekm_cut_flat_call(double y0top, double y0bot,
                             double y1top, double y1bot) {
  return (y0top == 0.0 && y0bot == -1.0 && y1top == 0.0 && y1bot == -1.0) ? 1 : 0;
}

inline int ekm_cut_quad_empty(double y0top, double y0bot,
                              double y1top, double y1bot) {
  return (!(y0bot > y0top) || !(y1bot > y1top)) ? 1 : 0;
}

// THE QUAD, IN THE ORDER THE CALL ARRIVES IN: (y0top, y0bot) belongs to the FIRST x argument at all four call sites.
// A ceiling call arrives with x0 > x1, which reverses the winding; ekm_cut_poly_fix_winding repairs it after the tilt
// clip. Refusing returns 0 with n = 0 and the caller must forward to the engine.
inline int ekm_cut_wall_quad(double x0, double y0top, double y0bot,
                             double x1, double y1top, double y1bot,
                             EkCutPoly* out) {
  if (!out) return 0;
  out->n = 0;
  if (!(x0 < x1) && !(x0 > x1)) return 0;
  if (ekm_cut_quad_empty(y0top, y0bot, y1top, y1bot)) return 0;
  for (int i = 0; i < 4; i++) {
    out->v[i].wx = 0.0; out->v[i].wy = 0.0;
    out->v[i].vx = 0.0; out->v[i].depth = 0.0;
  }
  out->v[0].sx = x0; out->v[0].sy = y0top;
  out->v[1].sx = x1; out->v[1].sy = y1top;
  out->v[2].sx = x1; out->v[2].sy = y1bot;
  out->v[3].sx = x0; out->v[3].sy = y0bot;
  out->n = 4;
  return 1;
}

// THE ENGINE'S OWN x-CLAMP, which the quad above does not get: polymost_domost drops a span wholly outside [xbl, xbr]
// and clamps each end, lerping y (polymost.cpp:3953-3975). A wall endpoint near the near plane projects enormous (one
// quad was 1163x the screen height). CLAMPING x IS WHAT BOUNDS y.
struct EkCutXBound {
  int ok;
  int clamped;
  double x0, y0top, y0bot, x1, y1top, y1bot;
};

// THE SIX NUMBERS COME BACK IN THE SLOTS THEY ARRIVED IN: a ceiling-side call passes the wall's RIGHT end first, so
// this orders the ends itself, clamps, and writes each answer back where it came from. `lo` and `hi` are the engine's
// two horizontal view bounds AS min AND max (see cutaway.h's locate).
inline EkCutXBound ekm_cut_x_bound(double x0, double y0top, double y0bot,
                                   double x1, double y1top, double y1bot,
                                   double lo, double hi) {
  EkCutXBound r;
  r.ok = 0; r.clamped = 0;
  r.x0 = x0; r.y0top = y0top; r.y0bot = y0bot;
  r.x1 = x1; r.y1top = y1top; r.y1bot = y1bot;
  if (!(lo < hi)) return r;
  if (!(x0 < x1) && !(x0 > x1)) return r;

  const int swapped = (x0 > x1) ? 1 : 0;
  double xa = swapped ? x1 : x0;
  double xb = swapped ? x0 : x1;
  double at = swapped ? y1top : y0top, ab = swapped ? y1bot : y0bot;
  double bt = swapped ? y0top : y1top, bb = swapped ? y0bot : y1bot;

  if (!(xb >= lo) || !(xa <= hi)) return r;

  const double dx = xb - xa;
  const double slopTop = (bt - at) / dx;
  const double slopBot = (bb - ab) / dx;

  if (xa < lo) {
    at += slopTop * (lo - xa);
    ab += slopBot * (lo - xa);
    xa = lo;
    r.clamped = 1;
  }
  if (xb > hi) {
    bt += slopTop * (hi - xb);
    bb += slopBot * (hi - xb);
    xb = hi;
    r.clamped = 1;
  }

  if (swapped) {
    r.x0 = xb; r.y0top = bt; r.y0bot = bb;
    r.x1 = xa; r.y1top = at; r.y1bot = ab;
  } else {
    r.x0 = xa; r.y0top = at; r.y0bot = ab;
    r.x1 = xb; r.y1top = bt; r.y1bot = bb;
  }
  r.ok = 1;
  return r;
}

// THE TALL-WALL REFUSAL, NOT ANOTHER CLIP: polymost_drawalls clamps a near-behind-camera endpoint to SCISDIST, so the
// projected line is enormous and the texture tiles across it. Clipping never changes the UV gradient; only refusing
// the wall removes it. `ratioOk` false REFUSES TOO; `multiple <= 0` is OFF; exactly at the threshold is accepted.
inline int ekm_cut_tall_wall_refuse(int ratioOk, double ratio, int multiple) {
  if (multiple <= 0) return 0;
  if (!ratioOk) return 1;
  return ratio > (double)multiple ? 1 : 0;
}

// THE FORWARDING CONTRACT: wraps ekm_cut_tall_wall_refuse with `projOk` and `haveQuad`. Neither absence is a reason
// to refuse, only to forward, matching cutpaint_wall's own early-outs; a degenerate quad once produced `ratioOk`
// false and was refused by the caller (43% of gated wall calls).
inline int ekm_cut_engine_tall_wall_refuse(int projOk, int haveQuad,
                                           int ratioOk, double ratio,
                                           int multiple) {
  if (!projOk) return 0;
  if (!haveQuad) return 0;
  return ekm_cut_tall_wall_refuse(ratioOk, ratio, multiple);
}

// THE DEGENERATE-QUAD REFUSAL: withholding every degenerate wall call removed the repeating columns at every
// threshold, so it is a RULE of its own. TWO SHAPES, kept apart so counters can tell them: EK_CUT_DEGENERATE_X_EQUAL
// (x0 == x1) and EK_CUT_DEGENERATE_QUAD (top and bottom coincide or cross).
enum EkCutDegenerateKind {
  EK_CUT_DEGENERATE_NONE = 0,
  EK_CUT_DEGENERATE_X_EQUAL = 1,
  EK_CUT_DEGENERATE_QUAD = 2,
};

inline EkCutDegenerateKind ekm_cut_wall_degenerate_kind(
    double x0, double y0top, double y0bot,
    double x1, double y1top, double y1bot) {
  if (!(x0 < x1) && !(x0 > x1)) return EK_CUT_DEGENERATE_X_EQUAL;
  if (ekm_cut_quad_empty(y0top, y0bot, y1top, y1bot)) return EK_CUT_DEGENERATE_QUAD;
  return EK_CUT_DEGENERATE_NONE;
}

inline int ekm_cut_wall_raw_ratio(double y0top, double y0bot, double y1top,
                                  double y1bot, double screenPx,
                                  double* ratioOut) {
  double hi = y0top, lo = y0top;
  if (y0bot > hi) hi = y0bot;
  if (y0bot < lo) lo = y0bot;
  if (y1top > hi) hi = y1top;
  if (y1top < lo) lo = y1top;
  if (y1bot > hi) hi = y1bot;
  if (y1bot < lo) lo = y1bot;
  const double extPx = hi - lo;
  const double ratio = extPx / screenPx;
  if (!(screenPx > 0.0) || !(screenPx < 1e30) || !(ratio >= 0.0) ||
      !(ratio < 1e30))
    return 0;
  *ratioOut = ratio;
  return 1;
}

// THE INVERSION-ONLY PREDICATE: an also-enormous term was MEASURED DEAD (all 64,070 inverted spans at 8x sat under
// it). Any degenerate kind refuses, at the cost of a black triangle at some angles (57% of wall calls invert, most
// correctly). ekm_cut_wall_raw_ratio is the measurement that proved it; do not retry extent.
inline int ekm_cut_wall_degenerate_refuse(EkCutDegenerateKind kind) {
  return kind != EK_CUT_DEGENERATE_NONE ? 1 : 0;
}


inline EkCutVert ekm_cut_vert_lerp(const EkCutVert* a, const EkCutVert* b, double t) {
  EkCutVert r;
  r.wx    = a->wx    + (b->wx    - a->wx)    * t;
  r.wy    = a->wy    + (b->wy    - a->wy)    * t;
  r.vx    = a->vx    + (b->vx    - a->vx)    * t;
  r.depth = a->depth + (b->depth - a->depth) * t;
  r.sx    = a->sx    + (b->sx    - a->sx)    * t;
  r.sy    = a->sy    + (b->sy    - a->sy)    * t;
  return r;
}

// Sutherland-Hodgman against the VIEW-SPACE near plane, depth >= 1. Run BEFORE the projection divide: a vertex behind
// the eye projects to a MIRRORED place and streaks the polygon across the screen.
inline EkCutPoly ekm_cut_near_clip(const EkCutPoly* in) {
  EkCutPoly out;
  out.n = 0;
  if (!in || in->n < 3 || in->n > EK_CUT_POLY_MAX) return out;
  for (int i = 0; i < in->n; i++) {
    const EkCutVert* a = &in->v[i];
    const EkCutVert* b = &in->v[(i + 1) % in->n];
    const double da = a->depth - EKM_BUILD_NEAR_CLIP;
    const double db = b->depth - EKM_BUILD_NEAR_CLIP;
    if (da >= 0.0) {
      if (out.n >= EK_CUT_POLY_MAX) { out.n = 0; return out; }
      out.v[out.n++] = *a;
    }
    if ((da < 0.0) != (db < 0.0)) {
      if (out.n >= EK_CUT_POLY_MAX) { out.n = 0; return out; }
      out.v[out.n++] = ekm_cut_vert_lerp(a, b, da / (da - db));
    }
  }
  if (out.n < 3) out.n = 0;
  return out;
}

// THE SECOND CLIP, IN SCREEN SPACE WHERE THE EDGES ARE STRAIGHT. polymost_drawpoly does not clip against the TILTED
// near plane (SOFTROTMAT 0); ekm_build_tilt_depth is what it divides by, and a vertex below EKM_CUT_TILT_NEAR is cut
// here. A WHOLE FOOTPRINT REACHES ROWS NO WALL EVER DOES, which is why this matters here and not to the engine.
inline EkCutPoly ekm_cut_tilt_clip(const EkCutPoly* in, const EkBuildProj& g) {
  EkCutPoly out;
  out.n = 0;
  if (!in || in->n < 3 || in->n > EK_CUT_POLY_MAX) return out;
  for (int i = 0; i < in->n; i++) {
    const EkCutVert* a = &in->v[i];
    const EkCutVert* b = &in->v[(i + 1) % in->n];
    const double da =
        ekm_build_tilt_depth(a->sy, g.halfx, g.horiz, g.chang, g.shang, g.gvr) -
        EKM_CUT_TILT_NEAR;
    const double db =
        ekm_build_tilt_depth(b->sy, g.halfx, g.horiz, g.chang, g.shang, g.gvr) -
        EKM_CUT_TILT_NEAR;
    if (da >= 0.0) {
      if (out.n >= EK_CUT_POLY_MAX) { out.n = 0; return out; }
      out.v[out.n++] = *a;
    }
    if ((da < 0.0) != (db < 0.0)) {
      if (out.n >= EK_CUT_POLY_MAX) { out.n = 0; return out; }
      out.v[out.n++] = ekm_cut_vert_lerp(a, b, da / (da - db));
    }
  }
  if (out.n < 3) out.n = 0;
  return out;
}

// THE THIRD CLIP: THE VIEWPORT. The engine never clips a polygon against the screen (the bound is implicit in vsp[],
// which stage C bypasses). EXACT, NOT A BAND ON sy: py = ghoriz + N/D with N and D affine in (sx, sy), so given D > 0
// each bound is a straight half-plane (slanted under roll). ekm_cut_tilt_clip establishes D > 0: run this AFTER it.

inline double ekm_cut_py_num(const EkBuildProj& g, double sx, double sy,
                             double ctang, double stang) {
  const double f = (g.gvr > 0.0) ? g.halfx / g.gvr : g.halfx;
  const double ox = sx - g.halfx;
  const double oy = (sy - g.horiz) * g.chang - f * g.shang;
  return f * (ox * stang + oy * ctang);
}

inline double ekm_cut_row_dist(const EkBuildProj& g, const EkCutVert* v,
                               double ctang, double stang,
                               double row, double side) {
  const double d =
      ekm_build_tilt_depth(v->sy, g.halfx, g.horiz, g.chang, g.shang, g.gvr);
  return side * (ekm_cut_py_num(g, v->sx, v->sy, ctang, stang) -
                 (row - g.horiz) * d);
}

// Sutherland-Hodgman against one of them. `refused` IS THE ONE THING THE RETURN VALUE CANNOT SAY: n == 0 means either
// wholly outside (DROP it) or a buffer that could not hold it (never measured: paint as before). NULL asks nothing.
inline EkCutPoly ekm_cut_row_clip(const EkCutPoly* in, const EkBuildProj& g,
                                  double ctang, double stang,
                                  double row, double side, int* refused) {
  EkCutPoly out;
  out.n = 0;
  if (!in || in->n < 3 || in->n > EK_CUT_POLY_MAX) return out;
  for (int i = 0; i < in->n; i++) {
    const EkCutVert* a = &in->v[i];
    const EkCutVert* b = &in->v[(i + 1) % in->n];
    const double da = ekm_cut_row_dist(g, a, ctang, stang, row, side);
    const double db = ekm_cut_row_dist(g, b, ctang, stang, row, side);
    if (da >= 0.0) {
      if (out.n >= EK_CUT_POLY_MAX) {
        if (refused) *refused = 1;
        out.n = 0; return out;
      }
      out.v[out.n++] = *a;
    }
    if ((da < 0.0) != (db < 0.0)) {
      if (out.n >= EK_CUT_POLY_MAX) {
        if (refused) *refused = 1;
        out.n = 0; return out;
      }
      out.v[out.n++] = ekm_cut_vert_lerp(a, b, da / (da - db));
    }
  }
  if (out.n < 3) out.n = 0;
  return out;
}

// THE 3D VIEWPORT'S OWN ROWS: `top`/`bot` are py bounds (0 and 2 * g.horiz), exact only when r_projectionhack != 1,
// r_yshearing == 0 and polymostcenterhoriz == 100; otherwise they only widen, never tighten. The engine's xbl/xbr
// describe the EXPANDED span polygon, not what is visible, so they are not used here.
inline EkCutPoly ekm_cut_view_clip(const EkCutPoly* in, const EkBuildProj& g,
                                   double ctang, double stang,
                                   double top, double bot) {
  EkCutPoly out;
  out.n = 0;
  if (!in || in->n < 3 || in->n > EK_CUT_POLY_MAX) return out;
  if (!(top < bot)) return out;
  const EkCutPoly t = ekm_cut_row_clip(in, g, ctang, stang, top, 1.0, NULL);
  if (t.n < 3) return out;
  return ekm_cut_row_clip(&t, g, ctang, stang, bot, -1.0, NULL);
}

// THE SAME WALK IN x, which no row clip touches (the worst flat was 3.8 million px wide). The space is PRE-TILT sx
// (polymost_drawrooms runs the screen corners BACKWARDS through the tilt, polymost.cpp:6941-6985). The bound is
// conservative BY CONSTRUCTION: the expanded span box, the pair domost admits a span against (:3953).
inline double ekm_cut_col_dist(const EkCutVert* v, double x, double side) {
  return side * (v->sx - x);
}

inline EkCutPoly ekm_cut_col_clip(const EkCutPoly* in, double x, double side,
                                  int* refused) {
  EkCutPoly out;
  out.n = 0;
  if (!in || in->n < 3 || in->n > EK_CUT_POLY_MAX) return out;
  for (int i = 0; i < in->n; i++) {
    const EkCutVert* a = &in->v[i];
    const EkCutVert* b = &in->v[(i + 1) % in->n];
    const double da = ekm_cut_col_dist(a, x, side);
    const double db = ekm_cut_col_dist(b, x, side);
    if (da >= 0.0) {
      if (out.n >= EK_CUT_POLY_MAX) {
        if (refused) *refused = 1;
        out.n = 0; return out;
      }
      out.v[out.n++] = *a;
    }
    if ((da < 0.0) != (db < 0.0)) {
      if (out.n >= EK_CUT_POLY_MAX) {
        if (refused) *refused = 1;
        out.n = 0; return out;
      }
      out.v[out.n++] = ekm_cut_vert_lerp(a, b, da / (da - db));
    }
  }
  if (out.n < 3) out.n = 0;
  return out;
}

inline EkCutPoly ekm_cut_span_clip(const EkCutPoly* in, double lo, double hi) {
  EkCutPoly out;
  out.n = 0;
  if (!in || in->n < 3 || in->n > EK_CUT_POLY_MAX) return out;
  if (!(lo < hi)) return out;
  const EkCutPoly l = ekm_cut_col_clip(in, lo, 1.0, NULL);
  if (l.n < 3) return out;
  return ekm_cut_col_clip(&l, hi, -1.0, NULL);
}

// THE SAME CLIP WITH ITS OUTCOME NAMED, FOR STAGE B: n == 0 means wholly outside (DROP) or a buffer that could not
// hold it (never measured: paint as before); TRUNCATING IS NEVER AN OPTION. The labels are the regression guard: on
// an ordinary map an in-bounds floor reads KEPT and the clip is inert.
enum EkCutViewOutcome {
  EK_CUT_VIEW_KEPT = 0,
  EK_CUT_VIEW_TRIMMED = 1,
  EK_CUT_VIEW_REMOVED = 2,
  EK_CUT_VIEW_REFUSED = 3,
  EK_CUT_VIEW_SKIPPED = 4,
  EK_CUT_VIEW_FITTED = 5,
};

struct EkCutViewResult { EkCutViewOutcome outcome; EkCutPoly poly; };

inline int ekm_cut_poly_same(const EkCutPoly* a, const EkCutPoly* b) {
  if (!a || !b || a->n != b->n) return 0;
  for (int i = 0; i < a->n; i++) {
    if (a->v[i].wx != b->v[i].wx || a->v[i].wy != b->v[i].wy ||
        a->v[i].vx != b->v[i].vx || a->v[i].depth != b->v[i].depth ||
        a->v[i].sx != b->v[i].sx || a->v[i].sy != b->v[i].sy)
      return 0;
  }
  return 1;
}

// BOTH AXES, EACH WITH ITS OWN VERDICT: a flight must tell "ROWS TRIMMED IT" from "COLUMNS TRIMMED IT". They fail
// independently (rows need gctang/gstang, columns need cutaway_xbounds); a pair whose bound could not be read is
// SKIPPED, never guessed.
struct EkCutViewXYResult {
  EkCutViewOutcome outcome;
  EkCutViewOutcome cols;
  EkCutViewOutcome rows;
  EkCutPoly poly;
};

// COLUMNS FIRST, AND THE ORDER IS AN ARGUMENT: the chain can outgrow EK_CUT_DRAWPOLY_MAX and whatever ran LAST is
// dropped to fit; x is the axis to keep. The row predicate needs D > 0, which a column-invented vertex keeps (a lerp
// of two valid ones). `cols`/`rows` describe what each pair did even when the fallback dropped the last one.
inline EkCutViewXYResult ekm_cut_view_clip_xy(const EkCutPoly* in,
                                              const EkBuildProj& g,
                                              double ctang, double stang,
                                              int haveRows, double top,
                                              double bot,
                                              int haveCols, double lo,
                                              double hi) {
  EkCutViewXYResult r;
  r.outcome = EK_CUT_VIEW_REFUSED;
  r.cols = EK_CUT_VIEW_SKIPPED;
  r.rows = EK_CUT_VIEW_SKIPPED;
  r.poly.n = 0;
  if (!in || in->n < 3 || in->n > EK_CUT_POLY_MAX) return r;
  if (haveRows && !(top < bot)) return r;
  if (haveCols && !(lo < hi)) return r;

  EkCutPoly fit = *in;
  int haveFit = in->n <= EK_CUT_DRAWPOLY_MAX ? 1 : 0;
  EkCutPoly cur = *in;

  if (haveCols) {
    int refused = 0;
    const EkCutPoly a = ekm_cut_col_clip(&cur, lo, 1.0, &refused);
    if (refused) return r;
    if (a.n >= 3 && a.n <= EK_CUT_DRAWPOLY_MAX) { fit = a; haveFit = 1; }
    EkCutPoly b;
    b.n = 0;
    if (a.n >= 3) {
      b = ekm_cut_col_clip(&a, hi, -1.0, &refused);
      if (refused) return r;
    }
    if (b.n < 3) {
      r.cols = EK_CUT_VIEW_REMOVED;
      r.outcome = EK_CUT_VIEW_REMOVED;
      return r;
    }
    if (b.n <= EK_CUT_DRAWPOLY_MAX) { fit = b; haveFit = 1; }
    r.cols = ekm_cut_poly_same(&cur, &b) ? EK_CUT_VIEW_KEPT
                                         : EK_CUT_VIEW_TRIMMED;
    cur = b;
  }

  if (haveRows) {
    const EkCutPoly before = cur;
    int refused = 0;
    const EkCutPoly t = ekm_cut_row_clip(&cur, g, ctang, stang, top, 1.0,
                                         &refused);
    if (refused) return r;
    if (t.n >= 3 && t.n <= EK_CUT_DRAWPOLY_MAX) { fit = t; haveFit = 1; }
    EkCutPoly u;
    u.n = 0;
    if (t.n >= 3) {
      u = ekm_cut_row_clip(&t, g, ctang, stang, bot, -1.0, &refused);
      if (refused) return r;
    }
    if (u.n < 3) {
      r.rows = EK_CUT_VIEW_REMOVED;
      r.outcome = EK_CUT_VIEW_REMOVED;
      return r;
    }
    if (u.n <= EK_CUT_DRAWPOLY_MAX) { fit = u; haveFit = 1; }
    r.rows = ekm_cut_poly_same(&before, &u) ? EK_CUT_VIEW_KEPT
                                            : EK_CUT_VIEW_TRIMMED;
    cur = u;
  }

  int fitted = 0;
  if (cur.n > EK_CUT_DRAWPOLY_MAX) {
    if (!haveFit) return r;
    cur = fit;
    fitted = 1;
  }

  r.poly = cur;
  if (fitted) r.outcome = EK_CUT_VIEW_FITTED;
  else if (!haveCols && !haveRows) r.outcome = EK_CUT_VIEW_SKIPPED;
  else r.outcome = ekm_cut_poly_same(in, &cur) ? EK_CUT_VIEW_KEPT
                                               : EK_CUT_VIEW_TRIMMED;
  return r;
}

inline EkCutViewResult ekm_cut_view_clip_outcome(const EkCutPoly* in,
                                                 const EkBuildProj& g,
                                                 double ctang, double stang,
                                                 double top, double bot) {
  const EkCutViewXYResult x =
      ekm_cut_view_clip_xy(in, g, ctang, stang, 1, top, bot, 0, 0.0, 0.0);
  EkCutViewResult r;
  r.outcome = x.outcome;
  r.poly = x.poly;
  return r;
}

// THE X SPAN: after both row clips the row pair reads a tautology (~1.00x), and x is untouched by a row clip (the
// worst flat reached sx = -234586 on a 3440-wide view). `haveWidth` false means the view's width could not be read
// and `px` is still the raw span: never divide an x span by the screen HEIGHT. A NaN comes back not measurable.
struct CpExtentX { int ok; double px; int haveRatio; double ratio; };

inline CpExtentX ekm_cut_extent_x(const EkCutPoly* q, int haveWidth,
                                  double widthPx) {
  CpExtentX e;
  e.ok = 0; e.px = 0.0; e.haveRatio = 0; e.ratio = 0.0;
  if (!q || q->n < 3 || q->n > EK_CUT_POLY_MAX) return e;
  double lo = q->v[0].sx, hi = q->v[0].sx;
  for (int k = 0; k < q->n; k++) {
    const double sx = q->v[k].sx;
    if (!(sx == sx)) return e;
    if (sx < lo) lo = sx;
    if (sx > hi) hi = sx;
  }
  const double px = hi - lo;
  if (!(px >= 0.0) || !(px < 1e30)) return e;
  e.ok = 1;
  e.px = px;
  if (haveWidth && widthPx > 0.0 && widthPx < 1e30) {
    const double ratio = px / widthPx;
    if (ratio >= 0.0 && ratio < 1e30) { e.haveRatio = 1; e.ratio = ratio; }
  }
  return e;
}

struct CpColSpanTracker { int count; int ignored; double minSpan; double maxSpan; };

inline void ekm_cut_col_span_track(CpColSpanTracker* t, double span) {
  if (!t) return;
  if (!(span == span) || !(span > 0.0)) { t->ignored++; return; }
  if (t->count == 0) { t->minSpan = span; t->maxSpan = span; }
  else {
    if (span < t->minSpan) t->minSpan = span;
    if (span > t->maxSpan) t->maxSpan = span;
  }
  t->count++;
}


// ONE FRAME'S RESET, from the seed polymost_scansector seam inside polymost_drawrooms (the render clock, before any
// domost call): rebuilds the footprint cache on a map change, forgets last frame's painted planes, takes this
// frame's projection. Gated by its caller.
void cutpaint_frame_begin(void);

// PAINT ONE SECTOR'S WHOLE FOOTPRINT, once per sector per side per frame. TRUE means painted: the caller must NOT run
// the engine's domost. FALSE means nothing was painted and the caller forwards. AN ALREADY-PAINTED SIDE RETURNS TRUE:
// a later call must still be suppressed, or it would claim the span and occlude what the footprint revealed.
bool cutpaint_flat(int sect, bool isFloor, int method);

void cutpaint_flip_note_np(int sect, bool isFloor);
void cutpaint_flip_note_dm_forward(int sect, bool isFloor);

void cutpaint_flip_pass_end(void);

unsigned cutpaint_flat_painted(void);
unsigned cutpaint_flat_no_footprint(void);
unsigned cutpaint_flat_no_sector(void);
unsigned cutpaint_flat_no_proj(void);
unsigned cutpaint_wall_painted(void);
unsigned cutpaint_wall_inert(void);
unsigned cutpaint_wall_no_quad(void);
unsigned cutpaint_wall_drop_tilt(void);
unsigned cutpaint_wall_drop_wind(void);
unsigned cutpaint_wall_tall_refused(void);

// PAINT ONE WALL, STEP OR ISLAND WALL, from the clip box its domost call carries. TRUE means painted: do NOT run the
// engine's domost. FALSE means NOTHING was painted and the caller forwards. The tall-wall refusal also returns TRUE
// with no draw(): forwarding would reproduce the defect. (x0, y0top, y0bot) stay one end of the wall.
bool cutpaint_wall(double x0, double y0top, double y0bot,
                   double x1, double y1top, double y1bot, int method);

// THE SAME AS-BUILT MEASUREMENT cutpaint_wall takes, EXPOSED so a caller about to FORWARD a wall call can apply
// ekm_cut_tall_wall_refuse to the identical ratio. `projOk` false (no usable projection) and `haveQuad` false (x0 ==
// x1 or a pinched box) mean the caller must NOT refuse, only forward.
struct CpWallRatio { int projOk; int haveQuad; int ratioOk; double ratio; };
CpWallRatio cutpaint_wall_ratio(double x0, double y0top, double y0bot,
                                double x1, double y1top, double y1bot);

// THE RAW COUNTERPART: skips the quad and applies ekm_cut_wall_raw_ratio to the call's four y values against screenPx
// = 2*horiz, for an INVERTED box that cutpaint_wall_ratio cannot measure. `projOk` false means forward. Kept as
// instrumentation even though no caller decides on it now.
struct CpWallRawRatio { int projOk; int ok; double ratio; };
CpWallRawRatio cutpaint_wall_raw_ratio(double y0top, double y0bot,
                                       double y1top, double y1bot);
