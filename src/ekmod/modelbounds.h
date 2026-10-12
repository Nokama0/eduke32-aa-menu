#pragma once

// Model bounds, the pure rules (reads: modelbounds.cpp).
#include <math.h>

#define EK_MDL_IDP3_MAGIC 0x33504449u

struct EkModelBox {
  double minx, miny, minz, maxx, maxy, maxz;
  int ok;
};

inline int ekm_mdl_modelid_plausible(int modelid, int nextmodelid) {
  if (modelid == -1) return 1;
  if (nextmodelid <= 0) return 0;
  return modelid >= 0 && modelid < nextmodelid;
}

inline int ekm_mdl_frame_plausible(double minx, double miny, double minz,
                                    double maxx, double maxy, double maxz) {
  if (!isfinite(minx) || !isfinite(maxx)) return 0;
  if (!isfinite(miny) || !isfinite(maxy)) return 0;
  if (!isfinite(minz) || !isfinite(maxz)) return 0;
  if (minx >= maxx) return 0;
  if (miny >= maxy) return 0;
  if (minz >= maxz) return 0;
  return 1;
}

// One MD3 frame's box in BUILD units, repeats excluded: x,y = raw*s*0.25, z = -raw*s*4 - (zadd+yoffset)*s*256 (z is
// down-positive, so min and max swap); docs/re/modelbox.md.
inline EkModelBox ekm_mdl_scale_bbox(double minx, double miny, double minz,
                                      double maxx, double maxy, double maxz,
                                      double scale, double bscale,
                                      double zadd, double yoffset) {
  EkModelBox b;
  const double s = scale * bscale;
  const double kxy = s * 0.25;
  const double kz = s * 4.0;
  const double off = (zadd + yoffset) * s * 256.0;
  b.minx = minx * kxy;
  b.maxx = maxx * kxy;
  b.miny = miny * kxy;
  b.maxy = maxy * kxy;
  b.minz = -(maxz * kz) - off;
  b.maxz = -(minz * kz) - off;
  const int scalesOk = (isfinite(scale) && scale > 0.0 &&
                        isfinite(bscale) && bscale > 0.0) ? 1 : 0;
  b.ok = (ekm_mdl_frame_plausible(minx, miny, minz, maxx, maxy, maxz) &&
          scalesOk) ? 1 : 0;
  return b;
}


const int EK_MDL_UNREADY = -1;
const int EK_MDL_NONE    = 0;
const int EK_MDL_OK      = 1;

bool modelbounds_scan(void);

bool modelbounds_ready(void);

// True only after the scan gave up; !ready is also the startup wait.
bool modelbounds_refused(void);

int modelbounds_for(int picnum, int pal, EkModelBox* out);
