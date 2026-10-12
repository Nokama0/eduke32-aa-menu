#pragma once

// The combination-lock tint's pure half: the verdict, the colour, the model's origin in build z, and the box cut into
// six drawable faces. Nothing here reads engine memory or calls GL (esp.cpp cannot link into test_host, so a rule
// there could not be pinned).
#include <string.h>
#include "slopez.h"
#include "shotray.h"
#include "esp_switches.h"
#include "modelbounds.h"
#include "worldquad.h"
#include "ui/common.h"

// Only a combination lock gets a verdict: "Needs blue card" is a fact about the player's keys, not an instruction.
enum { EKM_TINT_NONE = 0, EKM_TINT_FLIP = 1, EKM_TINT_LEAVE = 2 };

// Compares the classifier's own line (esp_switches.h) rather than re-deriving the hitag rule, so tint and marker
// label cannot disagree about the same switch.
inline int ekm_tint_verdict(int family, const char* line) {
  if (family != EK_SW_COMBINATION || !line) return EKM_TINT_NONE;
  if (strcmp(line, "Flip") == 0)  return EKM_TINT_FLIP;
  if (strcmp(line, "Leave") == 0) return EKM_TINT_LEAVE;
  return EKM_TINT_NONE;
}

// 0 for EKM_TINT_NONE is not a colour (the caller refuses the tint first), stated as a value so the suite can pin it.
// Colours live only in ui/canvas.h, so they come in as parameters.
inline unsigned ekm_tint_colour(int verdict, unsigned flip, unsigned leave) {
  if (verdict == EKM_TINT_FLIP)  return flip;
  if (verdict == EKM_TINT_LEAVE) return leave;
  return 0u;
}

// The wall-sprite branch's vertical extent (engine.cpp): `centred` is cstat bit 7; when set, half the height (plus
// one more yrepeat when tilesizy is odd) is added back. z grows downward, so `top` is the smaller number.
inline void ekm_tint_wall_sprite_zrange(int z, int yrepeat, int tilesizy, int yoff,
                                         int centred, int* top, int* bottom) {
  if (!top || !bottom) return;
  long long z2 = (long long)z - (((long long)yoff * (long long)yrepeat) << 2);
  if (centred) {
    z2 += ((long long)tilesizy * (long long)yrepeat) << 1;
    if (tilesizy & 1) z2 += (long long)yrepeat << 1;
  }
  const long long z1 = z2 - (((long long)tilesizy * (long long)yrepeat) << 2);
  *top    = clamp_ll_to_int(z1);
  *bottom = clamp_ll_to_int(z2);
}

// The sector's own clamp: the flat ceilingz/floorz, not the slope height under the sprite, because that is what the
// engine compares. Each side is refused on its own parallax bit (a sky is not a surface); the ceiling only raises
// top, the floor only lowers bottom.
inline void ekm_tint_clamp_to_sector(int ceilingz, int ceilingParallax,
                                      int floorz, int floorParallax,
                                      int* top, int* bottom) {
  if (!top || !bottom) return;
  if (!ceilingParallax && *top < ceilingz) *top = ceilingz;
  if (!floorParallax && *bottom > floorz) *bottom = floorz;
}

// Where the model's origin sits in build z: not tspr->z when the sprite is y-centred, since polymer applies one extra
// glTranslatef(0, 0, -(tilesiz.y * yrepeat) / 8) when cstat&128 is set and the sprite is not floor-aligned.
inline int ekm_tint_model_origin_z(int z, int yrepeat, int tilesizy, int centred) {
  if (!centred) return z;
  return clamp_ll_to_int((long long)z +
                         (((long long)tilesizy * (long long)yrepeat) << 1));
}

// The box as six drawable faces: a volume, because no depth bias beats volume.
inline int ekm_tint_box_faces(const EkModelBox& box, int x, int y, int z,
                              int ang, int xrepeat, int yrepeat,
                              EkWorldQuad* out6, unsigned argb) {
  if (!out6 || !box.ok) return 0;
  if (xrepeat <= 0 || yrepeat <= 0) return 0;
  // Finite and ordered per axis, via modelbounds.h's own rule rather than three comparisons repeated here.
  if (!ekm_mdl_frame_plausible(box.minx, box.miny, box.minz,
                               box.maxx, box.maxy, box.maxz)) return 0;

  const double cosA = (double)ekm_sintable(ang + 512) / 16384.0;
  const double sinA = (double)ekm_sintable(ang) / 16384.0;
  const double nlo = box.minx * (double)xrepeat;
  const double nhi = box.maxx * (double)xrepeat;
  const double slo = box.miny * (double)xrepeat;
  const double shi = box.maxy * (double)xrepeat;
  const double vlo = box.minz * (double)yrepeat;
  const double vhi = box.maxz * (double)yrepeat;

  // The eight corners, indexed by bit: 1 the normal axis, 2 the span, 4 the vertical; each face is a walk flipping
  // one bit.
  double cx[8], cy[8];
  int cz[8];
  for (int i = 0; i < 8; i++) {
    const double n = (i & 1) ? nhi : nlo;
    const double s = (i & 2) ? shi : slo;
    const double v = (i & 4) ? vhi : vlo;
    cx[i] = (double)x + n * cosA + s * sinA;
    cy[i] = (double)y + n * sinA - s * cosA;
    cz[i] = clamp_double_to_int((double)z + v);
  }

  // Each face walks around its rectangle: corners given top/top/bottom/bottom make a bowtie (an hourglass on screen).
  static const int kFace[6][4] = {
      {0, 2, 6, 4},
      {1, 5, 7, 3},
      {0, 4, 5, 1},
      {2, 3, 7, 6},
      {0, 1, 3, 2},
      {4, 6, 7, 5},
  };
  for (int fi = 0; fi < 6; fi++) {
    for (int k = 0; k < 4; k++) {
      const int c = kFace[fi][k];
      out6[fi].x[k] = cx[c];
      out6[fi].y[k] = cy[c];
      out6[fi].z[k] = cz[c];
    }
    out6[fi].argb = argb;
  }
  return 6;
}

// Flat arms, for a lock drawn flat: one quad on the surface the player sees, only where it is flat. A quad on a
// sprite's plane is wrong wherever a model stands proud of it, so esp.cpp reaches these only when modelbounds_for
// finds no model.

struct EkTintClamp { int ceilz, ceilParallax, florz, florParallax; };

// polymost_drawsprite's wall-sprite branch in Build world coordinates: flip bits negate the offsets, the span is
// xrepeat*sintable[ang]/65536 per texel starting (tsiz.x>>1)+xoff back. Not modelled: picanm/hightile offset bytes,
// the hightile size swap, and the re-aim of a sprite within 4.5 degrees of a wall line (moves ends up to ~10 units).
inline int ekm_tint_wall_sprite_quad(int x, int y, int z, int ang,
                                     int xrepeat, int yrepeat,
                                     int tilesizx, int tilesizy,
                                     int xoffset, int yoffset, int cstat,
                                     const EkTintClamp* clamp,
                                     EkWorldQuad* out, unsigned argb) {
  if (!out) return 0;
  if (xrepeat <= 0 || yrepeat <= 0 || tilesizx <= 0 || tilesizy <= 0) return 0;
  const int xoff = (cstat & 4) ? -xoffset : xoffset;
  const int yoff = (cstat & 8) ? -yoffset : yoffset;

  const double ex = (double)xrepeat * (double)ekm_sintable(ang) / 65536.0;
  const double ey = (double)xrepeat * (double)ekm_sintable(ang + 1536) / 65536.0;
  const double f = (double)(tilesizx >> 1) + (double)xoff;
  const double ax = (double)x - ex * f, ay = (double)y - ey * f;
  const double bx = ax + ex * (double)tilesizx, by = ay + ey * (double)tilesizx;

  int top = 0, bottom = 0;
  ekm_tint_wall_sprite_zrange(z, yrepeat, tilesizy, yoff, (cstat & 128) ? 1 : 0,
                              &top, &bottom);
  if (clamp)
    ekm_tint_clamp_to_sector(clamp->ceilz, clamp->ceilParallax, clamp->florz,
                             clamp->florParallax, &top, &bottom);
  if (top >= bottom) return 0;

  // Walked round the rectangle, never crossed: the bowtie ekm_tint_box_faces already paid for.
  out->x[0] = ax; out->y[0] = ay; out->z[0] = top;
  out->x[1] = bx; out->y[1] = by; out->z[1] = top;
  out->x[2] = bx; out->y[2] = by; out->z[2] = bottom;
  out->x[3] = ax; out->y[3] = ay; out->z[3] = bottom;
  out->argb = argb;
  return 1;
}

// One surface of a sector as ekm_slope_z takes it: its own flat z, stat (bit 1 is the slope) and heinum, never the
// OR'd summary.
struct EkTintPlane { int z, stat, heinum; };

// A wall-texture lock is the whole wall face, as Megaton tints it: the wall's two vertices, ceiling to floor at each
// (what polymost_drawalls spans). ekm_slope_z is the engine's integer form, within 2 units of the float plane at a
// vertex.
inline int ekm_tint_wall_face_quad(int x0, int y0, int x1, int y1,
                                   EkTintPlane ceil, EkTintPlane flor,
                                   int fwx, int fwy, int fw2x, int fw2y,
                                   EkWorldQuad* out, unsigned argb) {
  if (!out) return 0;
  if (x0 == x1 && y0 == y1) return 0;
  const int c0 = ekm_slope_z(ceil.z, ceil.stat, ceil.heinum, fwx, fwy, fw2x, fw2y, x0, y0);
  const int c1 = ekm_slope_z(ceil.z, ceil.stat, ceil.heinum, fwx, fwy, fw2x, fw2y, x1, y1);
  const int f0 = ekm_slope_z(flor.z, flor.stat, flor.heinum, fwx, fwy, fw2x, fw2y, x0, y0);
  const int f1 = ekm_slope_z(flor.z, flor.stat, flor.heinum, fwx, fwy, fw2x, fw2y, x1, y1);
  // A ceiling through its floor at either end would walk a bowtie; no height at both ends is no face.
  if (c0 > f0 || c1 > f1) return 0;
  if (c0 == f0 && c1 == f1) return 0;

  out->x[0] = (double)x0; out->y[0] = (double)y0; out->z[0] = c0;
  out->x[1] = (double)x1; out->y[1] = (double)y1; out->z[1] = c1;
  out->x[2] = (double)x1; out->y[2] = (double)y1; out->z[2] = f1;
  out->x[3] = (double)x0; out->y[3] = (double)y0; out->z[3] = f0;
  out->argb = argb;
  return 1;
}
