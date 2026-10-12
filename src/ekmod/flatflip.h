// On train, mode 2, one base-level plane flips between whole and a thin edge line while the flat COUNT is equal; this
// header picks that surface out (cutpaint.cpp records areas and logs).
#pragma once

#include <float.h>

// A painted bounding box at least 4x the other pass's: a whole plane never legitimately changes 4x between main passes.
const double EK_FLAT_FLIP_RATIO = 4.0;

// Appear or vanish counts only from 64 px^2 (8x8): motion moves slivers across zero every frame.
const double EK_FLAT_FLIP_MIN_AREA = 64.0;

// Areas in px^2, 0 = nothing painted. No division (hi * 0.25 >= lo), so 0 and 1.7e308 are safe; NaN or negative never
// flips.
inline bool ekm_flat_flipped(double prevArea, double curArea) {
  if (!(prevArea >= 0.0) || !(prevArea <= DBL_MAX)) return false;
  if (!(curArea >= 0.0) || !(curArea <= DBL_MAX)) return false;
  const double lo = prevArea < curArea ? prevArea : curArea;
  const double hi = prevArea < curArea ? curArea : prevArea;
  if (lo == 0.0) return hi >= EK_FLAT_FLIP_MIN_AREA;
  return hi * (1.0 / EK_FLAT_FLIP_RATIO) >= lo;
}

enum EkFlatFlipCause {
  EK_FLIP_CAUSE_NOT_SCANNED = 0,
  EK_FLIP_CAUSE_SCANNED_NO_PLANE_CALL = 1,
  EK_FLIP_CAUSE_PLANE_CALL_NO_DOMOST = 2,
  EK_FLIP_CAUSE_DOMOST_NOT_PAINTED = 3,
  EK_FLIP_CAUSE_PAINTED = 4,
};

inline EkFlatFlipCause ekm_flat_flip_cause(int scanned, unsigned np,
                                           unsigned dmPainted,
                                           unsigned dmRepeat,
                                           unsigned dmFwd) {
  if (!scanned) return EK_FLIP_CAUSE_NOT_SCANNED;
  if (np == 0) return EK_FLIP_CAUSE_SCANNED_NO_PLANE_CALL;
  if (dmPainted == 0 && dmRepeat == 0 && dmFwd == 0)
    return EK_FLIP_CAUSE_PLANE_CALL_NO_DOMOST;
  if (dmPainted == 0 && dmRepeat == 0) return EK_FLIP_CAUSE_DOMOST_NOT_PAINTED;
  return EK_FLIP_CAUSE_PAINTED;
}
