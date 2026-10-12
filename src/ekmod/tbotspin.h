#pragma once

// An empty spin-up gun holds the triggerbot's fire: AA's three spin-up guns cap `spinup` at 9 while the pool is 0 (one
// below the 10 that fires), so a held trigger spins forever and P_CheckWeapon never switches. The Executioner on top
// (bit 8 plus gotshock & 2) takes another branch. CON line refs: docs/re/playerstate.md.

#include "playerstate.h"

const int EKM_SPIN_PCHAR_DUKE = 0;
const int EKM_SPIN_PCHAR_NOVA = 4;
const int EKM_SPIN_PCHAR_DEV_FIRST = 3;
const int EKM_SPIN_UP_MINIGUN = 1;
const int EKM_SPIN_UP_EXECUTIONER = 8;
const int EKM_SPIN_GOTSHOCK_TOP = 2;
const int EKM_SPIN_RAILGUN_TOP = 2;

// An unread input the gun's branch needs makes the answer false, so the triggerbot fires as before.
struct EkmSpinIn {
  bool weaponOk;   int weapon;
  bool ammoOk;     int ammo;
  bool pcharOk;    int pchar;
  bool dukeUpOk;   int dukeUp;
  bool gotshockOk; int gotshock;
  bool railOk;     int gotrailgun;
};

inline bool ekm_spin_executioner_up(const EkmSpinIn& s, bool* unread) {
  if ((s.dukeUp & EKM_SPIN_UP_EXECUTIONER) == 0) return false;
  if (!s.gotshockOk) { *unread = true; return false; }
  return (s.gotshock & EKM_SPIN_GOTSHOCK_TOP) != 0;
}

inline bool ekm_tbot_empty_spin(const EkmSpinIn& s) {
  if (!s.weaponOk || !s.ammoOk || s.ammo != 0 || !s.pcharOk) return false;
  if (s.weapon == EK_WEAPON_CHAINGUN) {
    const bool duke = s.pchar == EKM_SPIN_PCHAR_DUKE;
    if (!duke && s.pchar != EKM_SPIN_PCHAR_NOVA) return false;
    if (!s.dukeUpOk) return false;
    bool unread = false;
    const bool exec = ekm_spin_executioner_up(s, &unread);
    if (unread || exec) return false;
    return !duke || (s.dukeUp & EKM_SPIN_UP_MINIGUN) != 0;
  }
  if (s.weapon == EK_WEAPON_DEVISTATOR)
    return s.pchar >= EKM_SPIN_PCHAR_DEV_FIRST && s.railOk &&
           (s.gotrailgun & EKM_SPIN_RAILGUN_TOP) == 0;
  return false;
}
