#pragma once

#include "playerstate.h"
#include "con_proj_aa.h"

// The slot's SHOOTS tile decides the lead, not the stock slot: AA rewrites WEAPONn_SHOOTS per character and upgrade
// (the Penetrator sits on the Devastator slot and shoots hitscan BARRETPROJ). WORKSLIKE is useless (AA sets it to the
// slot index). A failed read or a tile AA does not declare gives the stock table; a non-positive vel answers 0.
inline int ekm_weapon_lead_speed(int weapon, bool gotShoots, int shoots) {
  if (gotShoots) {
    const EkConProj p = ekm_con_proj_aa(shoots);
    if (p.found) return (p.hitscan || p.vel <= 0) ? 0 : p.vel;
  }
  return ekm_weapon_proj_speed(weapon);
}

// The muzzle's zoffset6 uses the same fallback in three arms so lead and muzzle agree: SHOOTS read failed -> stock slot
// table; AA declares the picnum -> its hitscan bit; otherwise hitscan only for the stock tiles P_PreFireHitscan names.
const int EKM_TILE_SHOTSPARK1 = 2595;
const int EKM_TILE_SHOTGUN    = 2613;
const int EKM_TILE_CHAINGUN   = 2536;
const int EKM_TILE_GROWSPARK  = 2448;

inline bool ekm_shot_fires_hitscan(int weapon, bool gotShoots, int shoots) {
  if (!gotShoots) return ekm_weapon_is_hitscan(weapon);
  const EkConProj p = ekm_con_proj_aa(shoots);
  if (p.found) return p.hitscan != 0;
  return shoots == EKM_TILE_SHOTSPARK1 || shoots == EKM_TILE_SHOTGUN ||
         shoots == EKM_TILE_CHAINGUN  || shoots == EKM_TILE_GROWSPARK;
}
