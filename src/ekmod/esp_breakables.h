#pragma once

// Crack sprites and the nineteen panels A_DamageWall_Internal hands to G_BreakWall.
#include <string.h>
#include "ui/common.h"

inline const char* ekm_crack_name(int picnum) {
  return (picnum >= 546 && picnum <= 549) ? "Explodable" : 0;
}

// A crack detonates only with hitag > 0 (signed short); the sprite is deleted when it blows.
inline bool ekm_crack_live(int hitag) { return hitag > 0; }

// Membership is the liveness test: G_BreakWall rewrites a broken panel's picnum.
inline const char* ekm_breakable_name(int picnum) {
  switch (picnum) {
    case 293:  case 297:  case 299:  case 301:
    case 305:  case 306:  case 307:
    case 1212: case 1215:
    case 4130: case 4131: case 4132: case 4133:
    case 4134: case 4136: case 4138: case 4140:
    case 4142: case 4181:
      return "Breakable";
    default:
      return 0;
  }
}

inline int ekm_wall_mid(int a, int b) {
  return clamp_ll_to_int(((long long)a + (long long)b) / 2);
}
inline int ekm_wall_mid_z(int florz, int ceilz) {
  return clamp_ll_to_int(((long long)florz + (long long)ceilz) / 2);
}

const int EK_WALL_MARK_MAX = 256;

enum { EK_WKIND_BREAKABLE = 0, EK_WKIND_SWITCH = 1 };

inline int ekm_wall_mark_count_clamped(int n) { return clamp_int(n, 0, EK_WALL_MARK_MAX); }

const unsigned EK_WALL_SCAN_PERIOD_MS = 1000;

inline bool ekm_wall_scan_due(int scanned, int sigNow, int sigWas,
                              unsigned nowMs, unsigned lastMs) {
  if (!scanned) return true;
  if (sigNow != sigWas) return true;
  return (unsigned)(nowMs - lastMs) >= EK_WALL_SCAN_PERIOD_MS;
}
