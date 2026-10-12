#pragma once
#include "projmath.h"

// AA's scope zoom writes the viewing range directly (AAGAME.CON:16996 `setaspect vr yxaspect`; `vr` computed at
// AAPLAYER.CON:17661-17679), not the `fov` cvar. The engine's global reads back but alternates within a frame with
// 65536 (project_begin runs up to six times), so the correction uses AA's `vr`.

// AA's no-zoom sentinel, `gamevar vr -1 0` (AADEFS.CON:4189); never a viewing range.
const int EK_AA_VR_NONE = -1;

// A zoom is in force when `zoomin` and `vr` both say so: AAPLAYER.CON:13521 sets `zoomin YES` and `vr -1` on one tic,
// and :17720 parks `vr` at vrhi for a tic after the zoom ends. AA gates on `ife zoomin YES`; `!= 0` is equal because
// all 20 writes in AA's scripts are YES or NO (AADEFS.CON:3164).
inline bool ekm_aa_zoom_active(int zoomin, int vr) {
  return zoomin != 0 && vr != EK_AA_VR_NONE;
}

// AA's applied viewing range as a fraction of the unzoomed one: 1.0 on the first zoomed frame, 2/3 at zoomlevel 2.
// False leaves *outRatio untouched.
inline bool ekm_aa_zoom_ratio(int vr, int vrhi, float* outRatio) {
  if (!outRatio) return false;
  if (vr == EK_AA_VR_NONE || vr <= 0) return false;
  if (vrhi <= 0) return false;
  *outRatio = (float)vr / (float)vrhi;
  return true;
}

// mulscale16's own scale: G_DrawRooms draws with renderSetAspect(mulscale16(vr, viewingrange), yxaspect)
// (game.cpp:895). AA's `vr` is not an operand of that multiply: `setaspect vr` runs in EVENT_DISPLAYROOMS
// (game.cpp:1115, after :895, before the draw at :1142) and REPLACES the drawn global.
const int EK_AA_VR_SCALE = 65536;

// A viewing range as a factor on the unzoomed projection: instrumentation only, ekm_aa_corrected_range does not call
// it. False leaves *outFactor untouched.
inline bool ekm_aa_range_factor(int range, float* outFactor) {
  if (!outFactor) return false;
  if (range <= 0) return false;
  *outFactor = (float)range / (float)EK_AA_VR_SCALE;
  return true;
}

// Whole decision in one place; project_begin assigns only on true. Read flags are parameters since 0 is a value AA
// itself produces. Numerator is AA's `vr` alone (setaspect assigns the drawn range, engine.cpp:13799); divisor is
// ekm_default_viewingrange_term, so corrected * term == vr. Refuses a non-positive window or vr.
inline bool ekm_aa_corrected_range(bool gotZoomin, int zoomin,
                                   bool gotVr, int vr,
                                   int xdim, int ydim,
                                   float* outRange) {
  if (!outRange) return false;
  if (!gotZoomin || !gotVr) return false;
  if (!ekm_aa_zoom_active(zoomin, vr)) return false;
  if (vr <= 0) return false;
  const double term = ekm_default_viewingrange_term(xdim, ydim);
  if (term <= 0.0) return false;
  *outRange = (float)((double)vr / term);
  return true;
}
