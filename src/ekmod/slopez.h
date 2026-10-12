#pragma once

// SIDE TEST: a pure port of the engine's own slope-plane evaluation, so the flip log can say what polymost_drawalls'
// side test would have answered at a chosen camera x/y without calling into the host image.

#include <stdint.h>

// engine.cpp:14265/14284's `sec->ceilingstat&2` / `sec->floorstat&2` slope bit.
const int32_t EKM_SLOPE_STAT_BIT = 2;

inline uint32_t ekm_uhypsq(int32_t dx, int32_t dy) {
  return (uint32_t)dx * (uint32_t)dx + (uint32_t)dy * (uint32_t)dy;
}

// dmulscale3 in generic-C form: both products widen to 64 bits before the add, and the >>3 narrows back to 32.
inline int32_t ekm_dmulscale3(int32_t a, int32_t d, int32_t s, int32_t D) {
  return (int32_t)((((int64_t)a * d) + ((int64_t)s * D)) >> 3);
}

// pragmas.h's `scale`: tabledivide64 is an exact signed 64-bit division, i.e. truncate-toward-zero on the widened
// product. Guarded against c == 0 although ekm_slope_z already refuses first.
inline int32_t ekm_scale(int32_t a, int32_t b, int32_t c) {
  if (c == 0) return 0;
  return (int32_t)(((int64_t)a * (int64_t)b) / (int64_t)c);
}

namespace ekm_slopez_detail {

// engine.cpp:8461-8472, verbatim.
inline uint32_t ekm_msqrtasm(uint32_t c) {
  uint32_t a = 0x40000000u, b = 0x20000000u;
  for (;;) {
    if (c >= a) {
      c -= a;
      a += b * 4;
    }
    a -= b;
    a >>= 1;
    b >>= 2;
    if (!b) break;
  }
  if (c >= a) a++;
  return a >> 1;
}

// initksqrt's tables, built once here rather than at engine startup (this header never touches the engine's globals).
struct EkmSqrtTables {
  uint16_t sqrtable[4096];
  uint16_t shlookup[4096 + 256];
  EkmSqrtTables() {
    int j = 1, k = 0;
    for (int i = 0; i < 4096; i++) {
      if (i >= j) {
        j <<= 2;
        k++;
      }
      sqrtable[i] = (uint16_t)(ekm_msqrtasm((uint32_t)((i << 18) + 131072)) << 1);
      shlookup[i] = (uint16_t)((k << 1) + ((10 - k) << 8));
      if (i < 256)
        shlookup[i + 4096] = (uint16_t)(((k + 6) << 1) + ((10 - (k + 6)) << 8));
    }
  }
};

inline const EkmSqrtTables& ekm_sqrt_tables() {
  static const EkmSqrtTables t;
  return t;
}

}

// engine_priv.h:138-145, verbatim against the tables above.
inline uint32_t ekm_nsqrtasm(uint32_t a) {
  const ekm_slopez_detail::EkmSqrtTables& t = ekm_slopez_detail::ekm_sqrt_tables();
  const uint16_t c = (a & 0xff000000u) ? t.shlookup[(a >> 24) + 4096] : t.shlookup[a >> 12];
  a >>= (c & 0xff);
  return ((a & 0xffff0000u) | t.sqrtable[a & 0xffffu]) >> ((c & 0xff00) >> 8);
}

// getceilzofslopeptr / getflorzofslopeptr as one function: both bodies differ only in which stat/heinum/z they take.
inline int32_t ekm_slope_z(int32_t planez, int32_t stat, int32_t heinum,
                           int32_t wx, int32_t wy, int32_t w2x, int32_t w2y,
                           int32_t x, int32_t y) {
  if (!(stat & EKM_SLOPE_STAT_BIT)) return planez;
  const int32_t dx = w2x - wx;
  const int32_t dy = w2y - wy;
  const int32_t i = (int32_t)(ekm_nsqrtasm(ekm_uhypsq(dx, dy)) << 5);
  if (i == 0) return planez;
  const int32_t j = ekm_dmulscale3(dx, y - wy, -dy, x - wx);
  const int32_t shift = 1;
  return planez + (int32_t)(ekm_scale(heinum, j >> shift, i) << shift);
}

// polymost.cpp:5511-5513 (floor) /:5930-5932 (ceiling): the side test polymost_drawalls runs before each plane call,
// at the camera's own x/y.
inline bool ekm_slope_side_drawn(bool isFloor, int32_t camz, int32_t z) {
  return isFloor ? camz <= z : camz >= z;
}
