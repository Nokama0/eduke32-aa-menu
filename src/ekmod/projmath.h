#pragma once
#include <math.h>

// The projection arithmetic with nothing Win32 or OpenGL in it, so the suite can pin it; project.cpp holds the
// engine-facing parts.

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const int EKM_ANGLE_UNITS = 2048;

const float EKM_Z_PER_XY = 16.0f;

struct EkmVec3 { float x, y, z; };

// Masked, not shifted: the engine lets the raw value run outside 0..2047 and wraps on use.
inline int ekm_fix16_to_buildang(int angFix) {
  return (angFix >> 16) & (EKM_ANGLE_UNITS - 1);
}

inline float ekm_buildang_to_degrees(int angFix) {
  return ((float)angFix / 65536.0f) * 360.0f / (float)EKM_ANGLE_UNITS;
}

// Derived, never read post-frame: G_DrawRooms restores viewingrange when the frame ends (game.cpp:1250), so after it
// the global reads 65536. Expression: game.cpp:886-895; under Polymer only the tangent term survives.
inline float ekm_viewingrange(int fovDegrees, int playerYrepeat) {
  const float shrink = 64.0f / (float)(playerYrepeat + 28);
  return 65536.0f * tanf((float)fovDegrees * (float)M_PI / 360.0f) * shrink;
}

// Polymost's focal length is ghalfx/gvrcorrection (polymost.cpp:6870, divided again by the zenith-glitch term at
// r_projectionhack 2), so it MULTIPLIES the focal length; Polymer has none. Under Polymost yxaspect is 81920 and the
// viewingrange global differs; they cancel to 0.75 * viewingRange / 65536 (docs/re/polymer-projection.md).
inline float ekm_zenglitch_divisor(float viewingRange, int vpW, int vpH,
                                   int xdim, int ydim) {
  if (viewingRange <= 0.0f || vpW <= 0 || vpH <= 0 || xdim <= 0 || ydim <= 0)
    return 1.0f;
  const float vfovtan = 0.75f * (viewingRange / 65536.0f) *
                        (((float)xdim * (float)vpH) / ((float)vpW * (float)ydim));
  const float vfov = atanf(vfovtan) * (2.0f / (float)M_PI);
  const float zen = vfov + 0.6361136f - 0.95f;
  return zen > 0.0f ? zen * 2.5f + 1.0f : 1.0f;
}

// The Build projection, ported from duke3d-modmenu's project_core (calibrated in game; read it first). Lateral is
// unscaled and depth carries viewingrange; pitch rotates AFTER the divide, then a second divide by oz2. EDuke32's
// change: gshang uses ghalfx/gvrcorrection (polymost.cpp:6912), so the zenith divisor belongs in the pitch.
struct EkBuildProj {
  double cosang, sinang;
  double cosang2, sinang2;
  double halfx;
  double horiz;
  double shang, chang;
  double yxscale;
  // gvrcorrection scales the pitch rotation asymmetrically; Megaton has no equivalent: the one deliberate departure.
  double gvr;
  double horizonRow;
  bool   valid;
};

struct EkScreenPtF { float x, y; bool visible; };

// Build's yxaspect under Polymost: 1.25 stretch, (65536*5)/4 (engine.cpp:13714).
const float EKM_POLYMOST_YXASPECT = 81920.0f;

// Build's sine table is 16384-scaled and gcosang divides by 262144: 1/16; shared with the lateral offset in
// ekm_project_sprite_box.
const double EKM_BUILD_ANG_SCALE = 1.0 / 16.0;

inline double ekm_build_scale(double a, double b, double c) {
  return c != 0.0 ? (a * b) / c : 0.0;
}

// The viewingrange global's default as one rule, as two places need it: videoSetCorrectedAspect sets divscale16(xd*3,
// yd*4) from the WINDOW (engine.cpp:13758), G_DrawRooms multiplies by it (game.cpp:895). ekm_build_proj multiplies,
// aazoom.h divides. 0 means no answer.
inline double ekm_default_viewingrange_term(int xdim, int ydim) {
  if (xdim <= 0 || ydim <= 0) return 0.0;
  return (3.0 * (double)xdim) / (4.0 * (double)ydim);
}

// buildAng in 2048 units; horiz is the SLOPE (100 level); viewingRange is our fov-derived value, not the draw-time
// global. xdimen/ydimen are the 3D VIEWPORT, xdim/ydim the WINDOW: the viewing range derives from the window
// (engine.cpp:13715), xdimenscale, halfx, horiz and pitch from the viewport.
inline EkBuildProj ekm_build_proj(double buildAng, float horiz, float viewingRange,
                                  int xdimen, int ydimen, int xdim, int ydim) {
  EkBuildProj g;
  g.cosang = g.sinang = g.cosang2 = g.sinang2 = 0.0;
  g.halfx = g.horiz = g.shang = g.yxscale = 0.0;
  g.chang = 1.0;
  g.gvr = 1.0;
  g.horizonRow = 0.0;
  g.valid = false;
  if (xdimen <= 0 || ydimen <= 0 || viewingRange <= 0.0f) return g;
  if (xdim <= 0) xdim = xdimen;
  if (ydim <= 0) ydim = ydimen;

  // The draw-time viewing range: game.cpp:895 multiplies the fov tangent by the viewingrange global, which
  // videoSetCorrectedAspect set to divscale16(xd*3, yd*4) (engine.cpp:13758). Ours is the tangent alone, so this term
  // is what is missing.
  const double vrDraw =
      (double)viewingRange * ekm_default_viewingrange_term(xdim, ydim);
  const double xdimenscale =
      ekm_build_scale((double)xdimen, (double)EKM_POLYMOST_YXASPECT, 320.0);

  // Build's cosine is not unit: polymost.cpp:6887 gcosang = fcosglobalang/262144 with amplitude 16384 = 1/16 (:508). It
  // cancels in ox, so a unit cosine leaves the horizontal right and divides every height by sixteen.
  const double rad = buildAng * (6.283185307179586 / 2048.0);
  g.cosang = cos(rad) * EKM_BUILD_ANG_SCALE;
  g.sinang = sin(rad) * EKM_BUILD_ANG_SCALE;
  g.cosang2 = g.cosang * (vrDraw / 65536.0);
  g.sinang2 = g.sinang * (vrDraw / 65536.0);

  g.halfx = (double)(xdimen >> 1);
  g.yxscale = xdimenscale / 131072.0;

  const double slope = vrDraw != 0.0 ? xdimenscale / vrDraw : 0.0;
  const double ghorizPre = ((double)horiz - 100.0) * slope + (double)(ydimen >> 1);
  g.horizonRow = ghorizPre;
  const double dh = (double)(ydimen >> 1) - ghorizPre;

  const double D = (double)ekm_zenglitch_divisor(viewingRange, xdimen, ydimen,
                                                 xdimen, ydimen);
  const double gvr = (vrDraw / 65536.0) / (D > 0.0 ? D : 1.0);
  g.gvr = gvr > 0.0 ? gvr : 1.0;
  const double fx = gvr != 0.0 ? g.halfx / gvr : g.halfx;

  const double denom = sqrt(dh * dh + fx * fx);
  g.shang = denom != 0.0 ? dh / denom : 0.0;
  const double c2 = 1.0 - g.shang * g.shang;
  g.chang = c2 > 0.0 ? sqrt(c2) : 0.0;

  g.horiz = (double)(ydimen >> 1);
  g.valid = true;
  return g;
}

// Near clip on the g-space depth, Megaton's PROJ_NEAR_CLIP.
const double EKM_BUILD_NEAR_CLIP = 1.0;
// And on oz2, which the pitch can drive to zero. EDuke32 r10669 has no such bail (SOFTROTMAT is 0, polymost.cpp:82), so
// this guards our own divide; consumers were measured against 1e-3.
const double EKM_BUILD_NEAR_OZ2 = 1.0e-3;

// Same projection stopped one step early: polymost_drawpoly works in PRE-TILT screen space (the up/down rotation is a
// GL matrix, polymost.cpp:3070) relative to ghalfx/ghalfy (:3623); ekm_project_build's first three steps, named.

inline double ekm_build_glat(const EkBuildProj& g, double dx, double dy) {
  return dy * g.cosang - dx * g.sinang;
}
inline double ekm_build_gdepth_xy(const EkBuildProj& g, double dx, double dy) {
  return dx * g.cosang2 + dy * g.sinang2;
}

inline double ekm_build_pre_sx(const EkBuildProj& g, double glat, double depth) {
  return glat * g.halfx / depth + g.halfx;
}
inline double ekm_build_pre_sy(const EkBuildProj& g, double dz, double depth) {
  return dz * g.yxscale / depth + g.horiz;
}

inline double ekm_build_tilt_depth(double sy, double halfx, double horiz,
                                   double chang, double shang, double gvr) {
  const double f = (gvr > 0.0) ? halfx / gvr : halfx;
  return (sy - horiz) * shang + f * chang;
}

enum EkBuildProjectReason {
  EKM_BUILD_PROJECT_OK = 0,
  EKM_BUILD_PROJECT_REFUSE_INVALID,
  EKM_BUILD_PROJECT_REFUSE_GDEPTH,
  EKM_BUILD_PROJECT_REFUSE_OZ2,
};

// World delta to screen, dx/dy/dz the point MINUS the camera in Build units (dz NOT divided by 16); vpx/vpy the
// viewport's top-left (windowx1/y1). The diagnostic twin of the plain wrapper; reason/outGdepth/outOz2 may be NULL.
inline bool ekm_project_build_diag(const EkBuildProj& g, double dx, double dy, double dz,
                                   int vpx, int vpy, EkScreenPtF* out,
                                   EkBuildProjectReason* reason,
                                   double* outGdepth, double* outOz2) {
  if (out) { out->x = 0.0f; out->y = 0.0f; out->visible = false; }
  if (reason) *reason = EKM_BUILD_PROJECT_OK;
  if (outGdepth) *outGdepth = 0.0;
  if (outOz2) *outOz2 = 0.0;
  if (!g.valid) { if (reason) *reason = EKM_BUILD_PROJECT_REFUSE_INVALID; return false; }

  const double glat   = dy * g.cosang  - dx * g.sinang;
  const double gdepth = dx * g.cosang2 + dy * g.sinang2;
  if (outGdepth) *outGdepth = gdepth;
  // `!(x >= k)` so a NaN takes the rejection.
  if (!(gdepth >= EKM_BUILD_NEAR_CLIP)) {
    if (reason) *reason = EKM_BUILD_PROJECT_REFUSE_GDEPTH;
    return false;
  }

  const double ox  = glat * g.halfx / gdepth;
  const double oy  = dz * g.yxscale / gdepth;
  const double oz  = g.halfx;
  // The rotation is asymmetric: polymost.cpp:3064's float matrix[16] is column-major, so the literal rows 3072-3077 are
  // columns: y' = chang*y - shang*z/gvr, z' = chang*z + shang*y*gvr. Level, shang is 0 and every wrong version looks
  // right; the horizon settles it (912.43 at horiz 128; transposed 1150.69).
  const double oy2 = oy * g.chang - oz * g.shang / g.gvr;
  const double oz2 = oy * g.shang * g.gvr + oz * g.chang;
  if (outOz2) *outOz2 = oz2;
  if (!(oz2 >= EKM_BUILD_NEAR_OZ2)) {
    if (reason) *reason = EKM_BUILD_PROJECT_REFUSE_OZ2;
    return false;
  }

  const double f = g.halfx / oz2;
  if (out) {
    out->x = (float)((double)vpx + g.halfx + ox * f);
    out->y = (float)((double)vpy + g.horiz + oy2 * f);
    out->visible = true;
  }
  return true;
}

inline bool ekm_project_build(const EkBuildProj& g, double dx, double dy, double dz,
                              int vpx, int vpy, EkScreenPtF* out) {
  return ekm_project_build_diag(g, dx, dy, dz, vpx, vpy, out, NULL, NULL, NULL);
}

struct EkScreenBoxF { float x, y, w, h; bool valid; };

// A face sprite's box as four projected corners (width from projected height fails: Build's screen axes differ in
// scale). The side corners share the anchor's depth (the lateral offset along (-sinang, cosang) adds zero to gdepth),
// so it applies straight to glat. dzTop/dzBottom are relative to the camera z.
inline bool ekm_project_sprite_box(const EkBuildProj& g, double dx, double dy,
                                   double halfWidth, double dzTop, double dzBottom,
                                   int vpx, int vpy, EkScreenBoxF* out) {
  if (out) { out->x = out->y = out->w = out->h = 0.0f; out->valid = false; }
  if (!g.valid) return false;

  const double glat   = dy * g.cosang  - dx * g.sinang;
  const double gdepth = dx * g.cosang2 + dy * g.sinang2;
  if (!(gdepth >= EKM_BUILD_NEAR_CLIP)) return false;

  const double hw = (halfWidth >= 0.0 ? halfWidth : -halfWidth) * EKM_BUILD_ANG_SCALE;
  const double oxL = (glat - hw) * g.halfx / gdepth;
  const double oxR = (glat + hw) * g.halfx / gdepth;

  double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
  bool first = true;
  const double dzs[2] = { dzTop, dzBottom };
  for (int i = 0; i < 2; i++) {
    const double oy  = dzs[i] * g.yxscale / gdepth;
    const double oy2 = oy * g.chang - g.halfx * g.shang / g.gvr;
    const double oz2 = oy * g.shang * g.gvr + g.halfx * g.chang;
    if (!(oz2 >= EKM_BUILD_NEAR_OZ2)) return false;
    const double f = g.halfx / oz2;
    const double sy = (double)vpy + g.horiz + oy2 * f;
    const double sxL = (double)vpx + g.halfx + oxL * f;
    const double sxR = (double)vpx + g.halfx + oxR * f;
    if (first) {
      minX = sxL < sxR ? sxL : sxR; maxX = sxL < sxR ? sxR : sxL;
      minY = maxY = sy; first = false;
    } else {
      if (sxL < minX) minX = sxL;
      if (sxR < minX) minX = sxR;
      if (sxL > maxX) maxX = sxL;
      if (sxR > maxX) maxX = sxR;
      if (sy < minY) minY = sy;
      if (sy > maxY) maxY = sy;
    }
  }

  if (out) {
    out->x = (float)minX;
    out->y = (float)minY;
    out->w = (float)(maxX - minX);
    out->h = (float)(maxY - minY);
    out->valid = true;
  }
  return true;
}

inline EkmVec3 ekm_build_rel(int wx, int wy, int wz, int cx, int cy, int cz) {
  EkmVec3 v;
  v.x = (float)(wx - cx);
  v.y = (float)(wy - cy);
  v.z = (float)(wz - cz);
  return v;
}

inline double ekm_build_gdepth(const EkBuildProj& g, EkmVec3 v) {
  return (double)v.x * g.cosang2 + (double)v.y * g.sinang2;
}

inline bool ekm_build_in_front(const EkBuildProj& g, EkmVec3 v) {
  return ekm_build_gdepth(g, v) >= EKM_BUILD_NEAR_CLIP;
}

inline int ekm_build_clip_near(const EkBuildProj& g, EkmVec3* a, EkmVec3* b) {
  if (!a || !b || !g.valid) return 0;
  const double da = ekm_build_gdepth(g, *a);
  const double db = ekm_build_gdepth(g, *b);
  const bool aFront = da >= EKM_BUILD_NEAR_CLIP;
  const bool bFront = db >= EKM_BUILD_NEAR_CLIP;
  if (aFront && bFront) return 1;
  if (!aFront && !bFront) return 0;

  // A hair past the plane, not onto it: the cut is stored as float and ekm_project_build recomputes gdepth and demands
  // >= the clip, so an exact cut falls under it about half the time and the segment is dropped.
  const double target = EKM_BUILD_NEAR_CLIP * 1.001;
  const double t = (target - da) / (db - da);
  EkmVec3* cut = aFront ? b : a;
  const EkmVec3 from = aFront ? *a : *b;
  const EkmVec3 to   = aFront ? *b : *a;
  const double u = aFront ? t : (1.0 - t);
  cut->x = (float)((double)from.x + ((double)to.x - (double)from.x) * u);
  cut->y = (float)((double)from.y + ((double)to.y - (double)from.y) * u);
  cut->z = (float)((double)from.z + ((double)to.z - (double)from.z) * u);
  return 1;
}

// The second near plane, the one the pitch tilts: once the view pitches, a behind end left at gdepth 1.001 gives oz2
// hugely negative (-205751 at shang 0.231) and the tracer was dropped. For gdepth > 0, oz2 >= k is T(v) =
// dz*yxscale*shang*gvr + (halfx*chang - k)*gdepth >= 0, linear in the delta, so the crossing is one interpolation.

// The oz2 the cut is placed at: the threshold plus a hair (relative 1e-3 of the halfx*chang term, ~1.67 at 3440 wide,
// vs float error ~1e-7). Shares the flat twin's limit (gdepth cancelling past ~65536 lateral units) and fails soft:
// dropped and counted as a refusal.
inline double ekm_build_tilt_target(const EkBuildProj& g) {
  return EKM_BUILD_NEAR_OZ2 + 1.0e-3 * (EKM_BUILD_NEAR_OZ2 + g.halfx * g.chang);
}

inline double ekm_build_tilt_side(const EkBuildProj& g, EkmVec3 v, double k) {
  return (double)v.z * g.yxscale * g.shang * g.gvr +
         (g.halfx * g.chang - k) * ekm_build_gdepth(g, v);
}

inline bool ekm_build_tilt_in_front(const EkBuildProj& g, EkmVec3 v) {
  if (!g.valid || !(ekm_build_gdepth(g, v) > 0.0)) return false;
  return ekm_build_tilt_side(g, v, ekm_build_tilt_target(g)) >= 0.0;
}

// Clip on T in place, AFTER ekm_build_clip_near; an end at or behind the flat plane refuses the whole segment (the
// equivalence needs gdepth > 0).
inline int ekm_build_clip_tilt(const EkBuildProj& g, EkmVec3* a, EkmVec3* b) {
  if (!a || !b || !g.valid) return 0;
  if (!(ekm_build_gdepth(g, *a) > 0.0) || !(ekm_build_gdepth(g, *b) > 0.0)) return 0;
  const double k = ekm_build_tilt_target(g);
  const double ta = ekm_build_tilt_side(g, *a, k);
  const double tb = ekm_build_tilt_side(g, *b, k);
  if (!(ta == ta) || !(tb == tb)) return 0;
  const bool aFront = ta >= 0.0;
  const bool bFront = tb >= 0.0;
  if (aFront && bFront) return 1;
  if (!aFront && !bFront) return 0;

  const double t = ta / (ta - tb);
  EkmVec3 cut;
  cut.x = (float)((double)a->x + ((double)b->x - (double)a->x) * t);
  cut.y = (float)((double)a->y + ((double)b->y - (double)a->y) * t);
  cut.z = (float)((double)a->z + ((double)b->z - (double)a->z) * t);
  if (aFront) *b = cut; else *a = cut;
  return 1;
}

const double EKM_SCREEN_CLIP_MARGIN = 1024.0;

// Liang-Barsky in place: a cut end can project millions of pixels out, past canvas_line's int casts. 1 when something
// is left, 0 (inputs untouched) for nothing or non-finite.
inline int ekm_clip_segment_2d(double* x0, double* y0, double* x1, double* y1,
                               double minX, double minY, double maxX, double maxY,
                               int* moved0, int* moved1) {
  if (moved0) *moved0 = 0;
  if (moved1) *moved1 = 0;
  if (!x0 || !y0 || !x1 || !y1) return 0;
  const double ax = *x0, ay = *y0, bx = *x1, by = *y1;
  if (!(fabs(ax) <= 1e300) || !(fabs(ay) <= 1e300) ||
      !(fabs(bx) <= 1e300) || !(fabs(by) <= 1e300))
    return 0;
  if (!(minX <= maxX) || !(minY <= maxY)) return 0;

  const double dx = bx - ax, dy = by - ay;
  const double pk[4] = { -dx, dx, -dy, dy };
  const double qk[4] = { ax - minX, maxX - ax, ay - minY, maxY - ay };
  double u0 = 0.0, u1 = 1.0;
  for (int i = 0; i < 4; i++) {
    if (pk[i] == 0.0) {
      if (qk[i] < 0.0) return 0;
      continue;
    }
    const double r = qk[i] / pk[i];
    if (pk[i] < 0.0) {
      if (r > u1) return 0;
      if (r > u0) u0 = r;
    } else {
      if (r < u0) return 0;
      if (r < u1) u1 = r;
    }
  }

  double nax = ax, nay = ay, nbx = bx, nby = by;
  if (u0 > 0.0) { nax = ax + dx * u0; nay = ay + dy * u0; }
  if (u1 < 1.0) { nbx = ax + dx * u1; nby = ay + dy * u1; }
  nax = nax < minX ? minX : (nax > maxX ? maxX : nax);
  nbx = nbx < minX ? minX : (nbx > maxX ? maxX : nbx);
  nay = nay < minY ? minY : (nay > maxY ? maxY : nay);
  nby = nby < minY ? minY : (nby > maxY ? maxY : nby);
  *x0 = nax; *y0 = nay; *x1 = nbx; *y1 = nby;
  if (moved0) *moved0 = u0 > 0.0 ? 1 : 0;
  if (moved1) *moved1 = u1 < 1.0 ? 1 : 0;
  return 1;
}

// polymer.cpp:3983: xratio = xrepeat*0.20, yratio = yrepeat*0.25 against tilesiz; polymer-y is -z/16.

inline float ekm_sprite_height_z(int yrepeat, int tileH) {
  return 4.0f * (float)yrepeat * (float)tileH;
}

inline float ekm_sprite_halfwidth(int xrepeat, int tileW) {
  return 0.1f * (float)xrepeat * (float)tileW;
}

inline float ekm_sprite_height_flat(int yrepeat, int tileH) {
  return ekm_sprite_height_z(yrepeat, tileH) / EKM_Z_PER_XY;
}

inline int ekm_feet_z(int spriteZ, float heightZ, unsigned cstat) {
  return (cstat & 128u) ? spriteZ + (int)(heightZ / 2.0f) : spriteZ;
}

// AA draws most pickups as voxels standing proud of the tile's plane. Constants are polymost_voxdraw's
// (voxmodel.cpp:1038-1163, bscale 1): one model unit is 0.2*scale*xrepeat horizontally and 4*scale*yrepeat vertically,
// the same 0.20 and 4 as the flat pair (derived before compared).

// `sizZ` is the .kvx zsiz and `scale` the def block's (defs.cpp:2027), multiplying all three axes.
inline float ekm_voxel_height_z(int yrepeat, float sizZ, float scale) {
  return 4.0f * (float)yrepeat * sizZ * scale;
}

// `radius` is already a half-extent (the bounding cylinder about the spin pivot, mdsprite.cpp:1869), so the factor is
// the full 0.2.
inline float ekm_voxel_halfwidth(int xrepeat, float radius, float scale) {
  return 0.2f * (float)xrepeat * radius * scale;
}

// The cylinder is the ceiling, not the answer: exact broadside, too wide by sizX/sizY edge-on (8.8x on 5329's 79x9, 24x
// on picnum 60's 48x2, 2.83x median over 51 voxel rows). The next three answer the yaw and never exceed it.

// Half-extent about the pivot from the stored radius (the table has sizX/sizY and radius, not pivots): radius ==
// hypot(ax, ay), so ax <= sqrt(r^2 - (sizY/2)^2), an upper bound that cannot clip; exact for 453 of 608 rows, at worst
// 2x on 155, capped by `ownSiz`.
inline float ekm_voxel_halfextent(float ownSiz, float otherSiz, float radius) {
  const float half = 0.5f * otherSiz;
  const float sq = radius * radius - half * half;
  float e = sq > 0.0f ? sqrtf(sq) : 0.0f;
  if (e > ownSiz) e = ownSiz;
  if (e < 0.5f * ownSiz) e = 0.5f * ownSiz;
  return e;
}

// Silhouette half-width at a yaw: md3_vox_calcmat_common (mdsprite.cpp:1870) rotates the voxel by the sprite's ang
// alone, so model X (across the facing) scales by |cos(sprang - camang)| and Y (along it) by |sin|. Only absolute
// values appear; the suite pins the 90-degree axis swap. The min keeps the cylinder a ceiling.
inline float ekm_voxel_width_model(float sizX, float sizY, float radius,
                                   int yawDelta) {
  const double rad = (double)(yawDelta & (EKM_ANGLE_UNITS - 1)) *
                     (6.283185307179586 / (double)EKM_ANGLE_UNITS);
  const float c = (float)fabs(cos(rad));
  const float s = (float)fabs(sin(rad));
  const float a = ekm_voxel_halfextent(sizX, sizY, radius);
  const float b = ekm_voxel_halfextent(sizY, sizX, radius);
  const float w = a * c + b * s;
  return w > radius ? radius : w;
}

// Through ekm_voxel_halfwidth so the 0.2 factor lives in one place.
inline float ekm_voxel_halfwidth_yaw(int xrepeat, float sizX, float sizY,
                                     float radius, float scale, int yawDelta) {
  return ekm_voxel_halfwidth(
      xrepeat, ekm_voxel_width_model(sizX, sizY, radius, yawDelta), scale);
}

// World z of the voxel's BOTTOM; not ekm_feet_z: voxmodel.cpp:1076 puts the sprite's z at model z `zoff` (sizZ/2 +
// zpivot, or zpivot alone for cstat bit 7), and the bottom sits (sizZ - zoff) below. 74 of AA's 608 voxels have no
// centred pivot.
inline int ekm_voxel_feet_z(int spriteZ, int yrepeat, float sizZ, float pivZ,
                            float scale, unsigned cstat) {
  const float zoff = (cstat & 128u) ? pivZ : (sizZ * 0.5f + pivZ);
  return spriteZ + (int)(4.0f * (float)yrepeat * scale * (sizZ - zoff));
}

// Whether the engine draws the voxel at all: polymost_voxdraw returns 0 for a floor-aligned sprite (voxmodel.cpp:1046)
// and it falls to the flat sprite (polymost.cpp:7621). It tests the floor bit alone, so cstat 48 is refused too.
inline bool ekm_voxel_drawn(unsigned cstat) {
  return (cstat & 32u) == 0u;
}
