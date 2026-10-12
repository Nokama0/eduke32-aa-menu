#pragma once

// Visibility test for the whole-map sweep: the engine's own side planes from the GL matrices current in our detour
// (its functions are static). Never rejects near an edge, a sector containing the camera, sloped sectors or an
// unusable frustum. Details: docs/re/frustum-cull.md.

#include <math.h>

const float EK_CULL_MARGIN = 1024.0f;

const float EK_CULL_APEX_TOL = 256.0f;

const float EK_CULL_MIN_LEN = 0.5f;

struct EkGlPoint { float x, y, z; };

inline EkGlPoint ekm_build_to_gl(int bx, int by, int bz) {
  EkGlPoint p;
  p.x = (float)by;
  p.y = -(float)bz * (1.0f / 16.0f);
  p.z = -(float)bx;
  return p;
}

// Doubles: a wall sprite's span ends on fractions and rounding tilts the quad's plane (invisible in position, enormous
// in depth). Pinned against the integer version.
struct EkGlPointD { double x, y, z; };

inline EkGlPointD ekm_build_to_gld(double bx, double by, double bz) {
  EkGlPointD p;
  p.x = by;
  p.y = -bz * (1.0 / 16.0);
  p.z = -bx;
  return p;
}

inline void ekm_mat4_mul(const float* a, const float* b, float* out) {
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 4; r++) {
      float s = 0.0f;
      for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = s;
    }
  }
}

struct EkSidePlane { float a, b, c, d; };

inline EkSidePlane ekm_frustum_left(const float* m) {
  EkSidePlane p;
  p.a = m[3] + m[0];
  p.b = m[7] + m[4];
  p.c = m[11] + m[8];
  p.d = m[15] + m[12];
  return p;
}

inline EkSidePlane ekm_frustum_right(const float* m) {
  EkSidePlane p;
  p.a = m[3] - m[0];
  p.b = m[7] - m[4];
  p.c = m[11] - m[8];
  p.d = m[15] - m[12];
  return p;
}

inline EkSidePlane ekm_plane_normalise(EkSidePlane p) {
  EkSidePlane q = { 0.0f, 0.0f, 0.0f, 0.0f };
  const float len = sqrtf(p.a * p.a + p.b * p.b + p.c * p.c);
  if (!(len > 0.0f) || !(len < 1.0e30f)) return q;
  const float inv = 1.0f / len;
  q.a = p.a * inv;
  q.b = p.b * inv;
  q.c = p.c * inv;
  q.d = p.d * inv;
  return q;
}

inline int ekm_plane_usable(EkSidePlane p) {
  return (p.a * p.a + p.b * p.b + p.c * p.c > EK_CULL_MIN_LEN) ? 1 : 0;
}

inline float ekm_plane_eval(EkSidePlane p, EkGlPoint v) {
  return p.a * v.x + p.b * v.y + p.c * v.z + p.d;
}

inline int ekm_frustum_apex_ok(float fLeft, float fRight, float tol) {
  return (fabsf(fLeft) <= tol && fabsf(fRight) <= tol) ? 1 : 0;
}

// Vertical slab: once pitched the side planes are not vertical, so f depends on y. The band is the union over every
// accepted sector, since walls borrow the neighbour's vertices. Sloped sectors are refused: ceilingz/floorz are the
// z at the FIRST wall only.
struct EkCullSlab { int lo, hi, count; };

inline EkCullSlab ekm_slab_init(void) {
  EkCullSlab s = { 0, 0, 0 };
  return s;
}

inline EkCullSlab ekm_slab_add(EkCullSlab s, int ceilz, int florz) {
  const int lo = (ceilz < florz) ? ceilz : florz;
  const int hi = (ceilz < florz) ? florz : ceilz;
  if (s.count == 0) {
    s.lo = lo;
    s.hi = hi;
  } else {
    if (lo < s.lo) s.lo = lo;
    if (hi > s.hi) s.hi = hi;
  }
  s.count++;
  return s;
}

inline int ekm_slab_usable(EkCullSlab s) {
  return (s.count > 0 && s.hi >= s.lo) ? 1 : 0;
}

inline float ekm_slab_ylo(EkCullSlab s) { return -(float)s.hi * (1.0f / 16.0f); }
inline float ekm_slab_yhi(EkCullSlab s) { return -(float)s.lo * (1.0f / 16.0f); }

inline float ekm_cull_offset(EkSidePlane p, float yLo, float yHi, float margin) {
  const float u = p.b * yLo;
  const float v = p.b * yHi;
  return p.d + ((u > v) ? u : v) + margin;
}

inline int ekm_cull_column_out(EkSidePlane p, float off, float x, float z) {
  return ((p.a * x + p.c * z + off) < 0.0f) ? 1 : 0;
}

inline int ekm_cull_neighbour_blocks(int nextsector, int sweptEnd,
                                     int neighbourSloped) {
  if (nextsector < 0) return 0;
  if (nextsector >= sweptEnd) return 1;
  return neighbourSloped ? 1 : 0;
}

// Every corner outside the SAME plane: corners split across planes mean a view looking between them.
inline int ekm_cull_reject(int corners, int outLeft, int outRight, int blocked,
                           int ready) {
  if (!ready || blocked) return 0;
  if (corners < 3) return 0;
  return (outLeft == corners || outRight == corners) ? 1 : 0;
}
