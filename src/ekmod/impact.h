#pragma once

// Instrumentation only: our ray matched the engine's bullet yet rounds missed. Impact sprites per round: SHOTSPARK1 at
// hitData (player.cpp:428-439), SMALLSMOKE once, BULLETHOLE on walls only.

#include "gamestate.h"

// Stock tile values only: CON define rewrites tile names (dnames.cpp:1595).
const int EKM_PIC_BULLETHOLE = 952;
const int EKM_PIC_SMALLSMOKE = 2329;
const int EKM_PIC_SHOTSPARK1 = 2595;

const int EKM_IMPACT_NONE       = 0;
const int EKM_IMPACT_BULLETHOLE = 1;
const int EKM_IMPACT_SMALLSMOKE = 2;
const int EKM_IMPACT_SHOTSPARK1 = 3;
const int EKM_IMPACT_CLASSES    = 4;

inline int ekm_impact_class(int picnum) {
  if (picnum == EKM_PIC_BULLETHOLE) return EKM_IMPACT_BULLETHOLE;
  if (picnum == EKM_PIC_SMALLSMOKE) return EKM_IMPACT_SMALLSMOKE;
  if (picnum == EKM_PIC_SHOTSPARK1) return EKM_IMPACT_SHOTSPARK1;
  return EKM_IMPACT_NONE;
}

// deletesprite (engine.cpp:8825-8839) never touches picnum, so a dead hole still reads as a hole.
inline int ekm_impact_live_class(int picnum, int statnum) {
  if (statnum < 0 || statnum >= EK_MAXSTATUS) return EKM_IMPACT_NONE;
  return ekm_impact_class(picnum);
}

inline bool ekm_impact_is_new(int prevClass, int curClass) {
  return curClass != EKM_IMPACT_NONE && curClass != prevClass;
}

// 256 is ekm_sprite_halfwidth for a 64 px tile at xrepeat 40; NEAR is 4x ON.
const int EKM_IMPACT_ON_XY   = 256;
const int EKM_IMPACT_ON_Z    = 4096;
const int EKM_IMPACT_NEAR_XY = 1024;
const int EKM_IMPACT_NEAR_Z  = 16384;

const int EKM_BAND_ON   = 0;
const int EKM_BAND_NEAR = 1;
const int EKM_BAND_FAR  = 2;
const int EKM_BAND_COUNT = 3;

inline int ekm_impact_band(int dist2d, int dzAbs) {
  if (dist2d <= EKM_IMPACT_ON_XY && dzAbs <= EKM_IMPACT_ON_Z) return EKM_BAND_ON;
  if (dist2d <= EKM_IMPACT_NEAR_XY && dzAbs <= EKM_IMPACT_NEAR_Z) return EKM_BAND_NEAR;
  return EKM_BAND_FAR;
}

inline const char* ekm_impact_name(int cls) {
  switch (cls) {
    case EKM_IMPACT_BULLETHOLE: return "BULLETHOLE";
    case EKM_IMPACT_SMALLSMOKE: return "SMALLSMOKE";
    case EKM_IMPACT_SHOTSPARK1: return "SHOTSPARK1";
    default:                    return "not an impact";
  }
}

inline const char* ekm_impact_band_name(int band) {
  switch (band) {
    case EKM_BAND_ON:   return "ON THE TARGET";
    case EKM_BAND_NEAR: return "a near miss";
    case EKM_BAND_FAR:  return "FAR from the target";
    default:            return "unbanded";
  }
}
