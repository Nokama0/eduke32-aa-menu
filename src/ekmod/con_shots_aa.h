#pragma once

#include "gamearrays.h"

// AA's in-cycle CON shots: each row is a `shoot` in AAPLAYER.CON fired from an equality test on kickback_pic, plus the
// gate deciding it runs. The fire-rate advance steps over such a frame unless it is a stop, and a slot-wide stop made
// the pistol inert, so a row applies only while its gate holds.

// A gate term that is absent.
const int EK_SHOT_NONE = -1;
const int EKM_CON_SHOT_CITES = 6;

struct EkConShotCite { int line; const char* text; };

struct EkConShot {
  int weapon;
  int frameIdle;    // the frame the CON shoots at while fastfire == 0
  int frameFast;
  int pchar;        // AADEFS.CON:3267: 0 duke, 1 bombshell, 2 wes, 3 mando
  int upArray;      // EK_GA_UP_*, or EK_SHOT_NONE; the element is [weapon]
  int upSet;
  int upClear;
  int gotdbClear;   // `gotDB` bits that must be clear; 0 is no term
  int shoots;
  int fdPlus1Write; // AAPLAYER.CON:12545 writes FIREDELAY+1 on this slot
  int anyWrite;     // a computed write can land on ANY frame
  const char* file;
  EkConShotCite cite[EKM_CON_SHOT_CITES];   // cite[0] is the frame's own line
};

const int EKM_CON_SHOT_COUNT = 6;

inline const EkConShot* ekm_con_shots(void) {
  static const EkConShot kShots[EKM_CON_SHOT_COUNT] = {
    { 1, 6, 5, 1, EK_GA_UP_SHELLY, 4, 8, 0, EK_SHOT_NONE, 1, 0, "AAPLAYER.CON",
      { { 12713, "ifg fastfire 0 set temp 5 else set temp 6" },
        { 12715, "ife player[].kickback_pic temp" },
        { 12671, "ife pchar 1" },
        { 12711, "ifvarand shellyupgrades[PISTOL_WEAPON] 4" },
        { 12701, "ifvarand shellyupgrades[PISTOL_WEAPON] 8" },
        { 12717, "shoot DEAGLEBULLET" } } },
    // Duke's dual pistols. Bit 8 only picks the projectile, so the gate is bit 4 alone.
    { 1, 5, 5, 0, EK_GA_UP_DUKE, 4, 0, 0, EK_SHOT_NONE, 1, 0, "AAPLAYER.CON",
      { { 12611, "ife player[].kickback_pic 5" },
        { 12604, "ife pchar 0" },
        { 12608, "ifvarand dukeupgrades[PISTOL_WEAPON] 4 // dual pistols" },
        { 12616, "shoot SHOTSPARK1" },
        { 12619, "shoot SHOTGUN" },
        { 0, 0 } } },
    // Wes's dual revolvers. anyWrite: AAGAME.CON:18343 writes kickback_pic-2 under `ife pchar 2`.
    { 1, 7, 7, 2, EK_GA_UP_WES, 4, 0, 0, EK_SHOT_NONE, 1, 1, "AAPLAYER.CON",
      { { 12828, "ife player[].kickback_pic 7 // 6" },
        { 12773, "ife pchar 2" },
        { 12825, "ifvarand wesupgrades[PISTOL_WEAPON] 4" },
        { 12835, "shoot WESBULLET" },
        { 0, 0 }, { 0, 0 } } },
    { 2, 6, 6, 3, EK_SHOT_NONE, 0, 0, 2, EK_SHOT_NONE, 0, 0, "AAPLAYER.CON",
      { { 13338, "ife player[].kickback_pic 6 shoot SWITCHBULLET2" },
        { 13298, "ife pchar 3" },
        { 13300, "ifvarand gotDB 2" },
        { 13337, "else" },
        { 0, 0 }, { 0, 0 } } },
    { 7, 3, 2, 1, EK_GA_UP_SHELLY, 2, 0, 0, EK_SHOT_NONE, 0, 0, "AAPLAYER.CON",
      { { 14208, "ifg fastfire 0 set temp 2 else set temp 3" },
        { 14209, "ife player[].kickback_pic temp" },
        { 14181, "ife pchar 1" },
        { 14206, "ifvarand shellyupgrades[DEVISTATOR_WEAPON] 2" },
        { 14213, "eshoot STICKYPROJ" },
        { 14223, "eshoot GRENADEPROJ" } } },
    // FLAMEPROJ2 is tile 18588 (AADEFS.CON:2391). Its frame-12 shot has NO stop and is seen only because 11 HOLDS:
    // never make 11 skip-safe without adding a stop for 12.
    { 9, 11, 11, 3, EK_SHOT_NONE, 0, 0, 0, 18588, 0, 0, "AAPLAYER.CON",
      { { 14384, "ife player[].kickback_pic 11" },
        { 14369, "ife pchar 3" },
        { 14372, "ife WEAPON9_SHOOTS FLAMEPROJ2" },
        { 14386, "shoot FLAMEPROJ2" },
        { 0, 0 }, { 0, 0 } } },
  };
  return kShots;
}

// Every literal `setp[].kickback_pic N` (N > 0) the generator attributes to a slot, for ekm_con_frame_only_safe.
// Hand-kept; `--check-shots` and the AAPLAYER.CON size check guard drift. 99 is given to every slot (:10326).
struct EkConWrites { int n; const int* v; };
const int EKM_CON_WRITES_MAX = 16;

inline EkConWrites ekm_con_writes_aa(int weapon) {
  static const int kW01[6]  = { 1, 5, 7, 9, 10, 99 };
  static const int kW02[10] = { 1, 5, 8, 9, 11, 14, 16, 17, 23, 99 };
  static const int kW03[5]  = { 1, 2, 5, 9, 99 };
  static const int kW07[4]  = { 1, 5, 26, 99 };
  static const int kW09[6]  = { 1, 5, 8, 10, 11, 99 };
  EkConWrites r;
  r.n = 0;
  r.v = kW01;
  switch (weapon) {
    case 1: r.n = 6;  r.v = kW01; break;
    case 2: r.n = 10; r.v = kW02; break;
    case 3: r.n = 5;  r.v = kW03; break;
    case 7: r.n = 4;  r.v = kW07; break;
    case 9: r.n = 6;  r.v = kW09; break;
    default: break;
  }
  return r;
}

inline bool ekm_con_shot_weapon(int weapon) {
  const EkConShot* const s = ekm_con_shots();
  for (int i = 0; i < EKM_CON_SHOT_COUNT; i++) if (s[i].weapon == weapon) return true;
  return false;
}
