#pragma once
// Free-camera viewpoint as clipboard text in Megaton's `cam x=%d y=%d z=%d ang=%d pitch=%d` format. It carries PITCH,
// our pose HORIZ: copy and paste convert opposite ways.
#include "freecam.h"
#include <stdio.h>
#include <string.h>

// BXY_MAX (build.h:232).
const int CAMPOS_MAX_XY = 524288;

const int CAMPOS_MAX_Z = CAMPOS_MAX_XY * FREECAM_Z_PER_XY;

struct CamposReadout { int x, y, z, ang, pitch; };

inline bool campos_pose_plausible(const CamposReadout& p) {
  if (p.x < -CAMPOS_MAX_XY || p.x > CAMPOS_MAX_XY) return false;
  if (p.y < -CAMPOS_MAX_XY || p.y > CAMPOS_MAX_XY) return false;
  if (p.z < -CAMPOS_MAX_Z  || p.z > CAMPOS_MAX_Z)  return false;
  if (p.ang < 0 || p.ang >= FREECAM_ANG_UNITS) return false;
  if (p.pitch < -FREECAM_PITCH_MAX || p.pitch > FREECAM_PITCH_MAX) return false;
  return true;
}

inline bool campos_parse_readout(const char* text, CamposReadout* out) {
  if (!text || !out) return false;
  const char* p = text;
  while ((p = strstr(p, "cam x=")) != 0) {
    CamposReadout r;
    if (sscanf_s(p, "cam x=%d y=%d z=%d ang=%d pitch=%d",
                 &r.x, &r.y, &r.z, &r.ang, &r.pitch) == 5) {
      *out = r;
      return true;
    }
    p += 6;
  }
  return false;
}

inline bool campos_format(const FreecamPose& p, char* out, int cap) {
  if (!out || cap <= 0) return false;
  out[0] = 0;
  const int n = _snprintf_s(out, (size_t)cap, _TRUNCATE,
                            "cam x=%d y=%d z=%d ang=%d pitch=%d",
                            p.x, p.y, p.z, p.ang,
                            freecam_horiz_to_pitch(p.horiz));
  if (n < 0) { out[0] = 0; return false; }
  return true;
}

inline bool campos_accept(const char* text, FreecamPose* out) {
  if (!text || !out) return false;
  CamposReadout r;
  if (!campos_parse_readout(text, &r)) return false;
  if (!campos_pose_plausible(r)) return false;
  FreecamPose p;
  p.x = r.x;
  p.y = r.y;
  p.z = r.z;
  p.ang = r.ang;
  p.horiz = freecam_pitch_to_horiz(r.pitch);
  *out = p;
  return true;
}
