#pragma once
#include "udstate.h"
#include "projmath.h"

// World-to-screen for the POLYMER renderer, from polymer.cpp (modelview 1113-1171, perspective 919-936); it skips the
// classic yxaspect path (engine.cpp:9561-9565).

struct EkViewport {
  int x, y, w, h;
  bool fromGL;
};

struct EkProjection {
  EkCamera cam;
  EkViewport vp;
  // 65536-scaled; replaced by vr / term while AA's scope zoom is in force (aazoom.h).
  float viewingRange;
  EkBuildProj g;
  bool valid;
};

// Once per frame, render thread, inside SwapBuffers with GL current.
bool project_begin(EkProjection* out, int caller);

// False if behind the camera or outside the near plane; sx/sy untouched.
bool project_point(const EkProjection& p, int wx, int wy, int wz,
                   float* sx, float* sy);

// First half of project_point: a segment must be clipped against the near plane first (an endpoint behind the eye
// mirrors and streaks), in this space (ekm_build_clip_near).
EkmVec3 project_to_build(const EkProjection& p, int wx, int wy, int wz);

bool project_build_point(const EkProjection& p, EkmVec3 rel, float* sx, float* sy);

bool project_build_point_diag(const EkProjection& p, EkmVec3 rel, float* sx, float* sy,
                              EkBuildProjectReason* reason, double* outGdepth, double* outOz2);

// Assumptions (derived, not read): r_projectionhack == 2, r_yshearing == 0 (polymost.cpp:6903-6907), r_usenewaspect ==
// 1. r_screenxy comes from the monitor (winlayer.cpp:498): logged, not compared.
enum EkProjAssumptionVerdict {
  EKM_PROJ_ASSUME_MATCH = 0,
  EKM_PROJ_ASSUME_MISMATCH,
  EKM_PROJ_ASSUME_UNREADABLE,
};

inline EkProjAssumptionVerdict ekm_proj_assumption_verdict(bool readOk, int have, int want) {
  if (!readOk) return EKM_PROJ_ASSUME_UNREADABLE;
  return have == want ? EKM_PROJ_ASSUME_MATCH : EKM_PROJ_ASSUME_MISMATCH;
}

// Train's detachment moves with the view: this tells whether the ESP's pose is the drawn one.
struct EkViewPose {
  int x, y, z, angFix, horizFix, sect;
  double range;
  int yxaspect;
  int haveRange, haveYx; // int, not bool: MSVC /O2 note in sectorstate.h.
  int vpX, vpY, vpW, vpH;
  int xdim, ydim;
  int haveVp, haveDim;
};

enum {
  EKM_VD_X = 1 << 0, EKM_VD_Y = 1 << 1, EKM_VD_Z = 1 << 2,
  EKM_VD_ANG = 1 << 3, EKM_VD_HORIZ = 1 << 4, EKM_VD_SECT = 1 << 5,
  EKM_VD_RANGE = 1 << 6, EKM_VD_YX = 1 << 7,
  EKM_VD_VPX = 1 << 8, EKM_VD_VPY = 1 << 9, EKM_VD_VPW = 1 << 10,
  EKM_VD_VPH = 1 << 11, EKM_VD_XDIM = 1 << 12, EKM_VD_YDIM = 1 << 13,
  EKM_VD_FIELD_COUNT = 14,
};

// The engine's range passes three integer roundings; 8 is 0.2 px at 3440 wide.
const double EKM_VD_RANGE_TOL = 8.0;

inline unsigned ekm_view_diff(const EkViewPose& eng, const EkViewPose& esp) {
  unsigned d = 0;
  if (eng.x != esp.x) d |= EKM_VD_X;
  if (eng.y != esp.y) d |= EKM_VD_Y;
  if (eng.z != esp.z) d |= EKM_VD_Z;
  if (eng.angFix != esp.angFix) d |= EKM_VD_ANG;
  if (eng.horizFix != esp.horizFix) d |= EKM_VD_HORIZ;
  if (eng.sect != esp.sect) d |= EKM_VD_SECT;
  if (eng.haveRange && esp.haveRange) {
    const double dr = eng.range - esp.range;
    if (dr > EKM_VD_RANGE_TOL || dr < -EKM_VD_RANGE_TOL) d |= EKM_VD_RANGE;
  }
  if (eng.haveYx && esp.haveYx && eng.yxaspect != esp.yxaspect) d |= EKM_VD_YX;
  if (eng.haveVp && esp.haveVp) {
    if (eng.vpX != esp.vpX) d |= EKM_VD_VPX;
    if (eng.vpY != esp.vpY) d |= EKM_VD_VPY;
    if (eng.vpW != esp.vpW) d |= EKM_VD_VPW;
    if (eng.vpH != esp.vpH) d |= EKM_VD_VPH;
  }
  if (eng.haveDim && esp.haveDim) {
    if (eng.xdim != esp.xdim) d |= EKM_VD_XDIM;
    if (eng.ydim != esp.ydim) d |= EKM_VD_YDIM;
  }
  return d;
}

inline const char* ekm_view_diff_name(int i) {
  static const char* const names[EKM_VD_FIELD_COUNT] = {
    "x", "y", "z", "ang", "horiz", "sect", "range", "yxaspect",
    "vpx", "vpy", "vpw", "vph", "xdim", "ydim" };
  return (i >= 0 && i < EKM_VD_FIELD_COUNT) ? names[i] : "?";
}

// project_begin runs up to six times a frame; each caller names itself.
enum EkProjCaller {
  EK_PC_ESP = 0,
  EK_PC_AIM,
  EK_PC_CAM_MARKER,
  EK_PC_TRACER,
  EK_PC_DMGNUM,
  EK_PC_CUTPAINT,
  EK_PC_NOCULL,
  EK_PC_WORLDQUAD,
  EK_PC_COUNT,
};

inline const char* ekm_proj_caller_name(int c) {
  static const char* const names[EK_PC_COUNT] = {
    "esp", "aim", "cam-marker", "tracer", "dmgnum", "cutpaint", "nocull",
    "worldquad" };
  return (c >= 0 && c < EK_PC_COUNT) ? names[c] : "?";
}

// AA's HUD moves ud.camerax/y AFTER the 3D view is drawn (weapons_hud.CON:22154-22177), so at SwapBuffers the
// player-view record is the drawn pose; otherwise ud.camera*.
inline bool ekm_pose_use_record(int have, int complete, unsigned recFrame,
                                unsigned curFrame, int sect, int maxSectors) {
  return have != 0 && complete != 0 && curFrame != 0 && recFrame == curFrame &&
         sect >= 0 && sect < maxSectors;
}

inline void ekm_pose_apply(EkCamera* cam, int x, int y, int z, int angFix,
                           int horizFix, int sect) {
  cam->x = x;
  cam->y = y;
  cam->z = z;
  cam->angFix = angFix;
  cam->horizFix = horizFix;
  cam->sect = sect;
  cam->ang = (angFix >> 16) & (EK_BUILD_ANGLE_UNITS - 1);
}

// Shared so camera.cpp samples with the same code. Render thread, GL current.
struct EkViewRead {
  EkViewport vp;
  int clientW, clientH;
  int xdim, ydim;
};
void project_read_view(EkViewRead* out);

// Once at install, after cvars_scan(); logs mismatches and r_screenxy only.
void project_check_assumptions(void);
