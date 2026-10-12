#pragma once

// Mando's dual shotgun: alt fire fires the right barrel, so SK_FIRE alone never uses it. Gate: AAPLAYER.CON `ife pchar
// 3` (:13298), `ifvarand gotDB 2` (:13300), alt sets rightDB_weapcount (:13330-13335).

#include "playerstate.h"

const int EKM_DB_PCHAR_MANDO = 3;
const int EKM_DB_GOTDB_UP = 2;

inline bool ekm_tbot_wants_altfire(bool readsOk, int pchar, int weapon, int gotdb) {
  return readsOk && pchar == EKM_DB_PCHAR_MANDO && weapon == EK_WEAPON_SHOTGUN &&
         (gotdb & EKM_DB_GOTDB_UP) != 0;
}

inline bool ekm_dualbarrel_right_ready(bool readsOk, int rightCount, int weaponPos,
                                       int dbammo) {
  return readsOk && rightCount == 0 && weaponPos == 0 && dbammo > 0;
}

const int EKM_DB_NONE = 0;
const int EKM_DB_LEFT = 1;
const int EKM_DB_RIGHT = 2;

// Alternate, never both nor overlapped: the side not used last fires only when BOTH are ready.
inline int ekm_dualbarrel_pick(int lastSide, bool leftReady, bool rightReady) {
  if (!leftReady || !rightReady) return EKM_DB_NONE;
  return lastSide == EKM_DB_LEFT ? EKM_DB_RIGHT : EKM_DB_LEFT;
}

// recordFire/recordAlt are the bits BEFORE our write: a barrel already there IS this tic's shot.
struct EkmDbFrame {
  int fire;
  int wants;
  int recordOk;
  int recordFire;
  int recordAlt;
  int leftReady;
  int rightReady;
};

// LEFT = fire bit, RIGHT = alt bit; off the DB it is LEFT whenever fire holds.
inline int ekm_dualbarrel_step(int* lastSide, const EkmDbFrame& f) {
  if (!f.fire || !lastSide) return EKM_DB_NONE;
  if (!f.wants) return EKM_DB_LEFT;
  if (!f.recordOk || f.recordFire || f.recordAlt) return EKM_DB_NONE;
  const int side = ekm_dualbarrel_pick(*lastSide, f.leftReady != 0, f.rightReady != 0);
  if (side != EKM_DB_NONE) *lastSide = side;
  return side;
}
