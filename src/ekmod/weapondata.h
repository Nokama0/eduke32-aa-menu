#pragma once

#include <stdio.h>
#include "weaponspeed.h"

// AA's live weapon timings beside our stops. The rows feed firestops.h, so a field here is not cosmetic. AA rewrites
// weaponspeed.h's stock numbers from CON (WEAPON2_FIREDELAY is 3, 4, 10 and 16 in AAPLAYER.CON), and a large multiplier
// then steps over the firing frame: "animation plays, nothing fires".
enum { EK_WD_FIREDELAY = 0, EK_WD_TOTALTIME = 1, EK_WD_RELOAD = 2,
       EK_WD_HOLDDELAY = 3, EK_WD_FLAGS = 4, EK_WD_SHOTSPERBURST = 5,
       EK_WD_WORKSLIKE = 6, EK_WD_SPAWNTIME = 7, EK_WD_SOUND2TIME = 8,
       EK_WD_SHOOTS = 9,
       EK_WD_FIELD_COUNT = 10 };

// dukeweapon_t's MAX_WEAPONS (inv.h:74) is THIRTEEN: weaponspeed.h has an entry for weapon 12.
const int EK_WD_MAX_WEAPONS = 13;

// The name exactly as ADDWEAPONVAR registers it (gamevars.cpp:1633); out of range is empty, never wrong.
inline const char* ekm_weapon_field_name(int field) {
  switch (field) {
    case EK_WD_FIREDELAY:     return "FIREDELAY";
    case EK_WD_TOTALTIME:     return "TOTALTIME";
    case EK_WD_RELOAD:        return "RELOAD";
    case EK_WD_HOLDDELAY:     return "HOLDDELAY";
    case EK_WD_FLAGS:         return "FLAGS";
    case EK_WD_SHOTSPERBURST: return "SHOTSPERBURST";
    case EK_WD_WORKSLIKE:     return "WORKSLIKE";
    case EK_WD_SPAWNTIME:     return "SPAWNTIME";
    case EK_WD_SOUND2TIME:    return "SOUND2TIME";
    case EK_WD_SHOOTS:        return "SHOOTS";
    default:                  return "";
  }
}

// Which weapon field a label names, or -1; `*outWeapon` is written only on a match. A parse, not 91 label walks: the
// one walk gamevars.cpp runs parses every per-player label. Local copy only, never engine memory.
inline int ekm_weapon_var_parse(const char* label, int* outWeapon) {
  if (!label || !outWeapon) return -1;

  static const char kPrefix[] = "WEAPON";
  int i = 0;
  for (; kPrefix[i]; i++)
    if (label[i] != kPrefix[i]) return -1;
  const char* p = label + i;

  // No leading zero: Bsprintf("%d") cannot emit one, so "WEAPON02_FLAGS" is a CON author's variable.
  if (*p < '0' || *p > '9') return -1;
  if (*p == '0' && p[1] >= '0' && p[1] <= '9') return -1;
  int weapon = 0, digits = 0;
  while (*p >= '0' && *p <= '9') {
    weapon = weapon * 10 + (*p - '0');
    p++;
    if (++digits > 3) return -1;
  }
  if (weapon >= EK_WD_MAX_WEAPONS) return -1;
  if (*p != '_') return -1;
  p++;

  // The tail must end there ("WEAPON2_FIREDELAYX" is a CON's).
  for (int f = 0; f < EK_WD_FIELD_COUNT; f++) {
    const char* const want = ekm_weapon_field_name(f);
    int k = 0;
    while (want[k] && p[k] == want[k]) k++;
    if (want[k] == 0 && p[k] == 0) { *outWeapon = weapon; return f; }
  }
  return -1;
}

// The stops weaponspeed.h would use, or "no entry"; returns the count. Weapons 5, 8 and 10 have no row by design.
inline int ekm_firerate_stops_text(int weapon, char* out, int cap) {
  if (!out || cap <= 0) return 0;
  out[0] = 0;
  const FireRateWeapon* const t = firerate_weapon(weapon);
  if (!t) {
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "no entry");
    return 0;
  }
  return firerate_stops_text(*t, out, cap);
}

// What a row assumes: three numbers, not FireDelay alone. AAPLAYER.CON:1586 keeps FIREDELAY 4 for the shotgun but sets
// TOTALTIME 24/15/28/26/11, so stops {3,12,14,23,29} sat past the engine's reset. Values are weapondefaults[]'s
// (gamevars.cpp:1496-1580); Reload 0 is an assumption, not an absence.
struct EkFireRateAssumed { int weapon, fireDelay, totalTime, reload; };

inline const EkFireRateAssumed* ekm_firerate_assumed(int weapon) {
  static const EkFireRateAssumed kAssumed[10] = {
    {  0,  7, 14,  0 },
    {  1,  2,  5, 27 },
    {  2,  4, 30, 13 },
    {  3,  3, 12,  0 },
    {  4,  4, 20,  0 },
    {  6, 10, 12,  0 },
    {  7,  3,  6,  0 },
    {  9,  3,  5,  0 },
    { 11,  3,  5,  0 },
    { 12,  2, 16,  0 },
  };
  for (int i = 0; i < 10; i++)
    if (kAssumed[i].weapon == weapon) return &kAssumed[i];
  return 0;
}

const int EK_WD_DERIVED_COUNT = 3;
inline int ekm_firerate_derived_field(int i) {
  switch (i) {
    case 0:  return EK_WD_FIREDELAY;
    case 1:  return EK_WD_TOTALTIME;
    default: return EK_WD_RELOAD;
  }
}

// A failed read is not a zero (gamevars.cpp's value_text rule).
struct EkWeaponRow { int got[EK_WD_FIELD_COUNT]; int v[EK_WD_FIELD_COUNT]; };

// Does the live game agree on FIREDELAY, TOTALTIME and RELOAD? "Agrees" narrows the hypothesis, not closes it:
// unmodelled fields (SHOTSPERBURST, WORKSLIKE) and unbought upgrades are invisible here. An unread member is
// inconclusive.
inline void ekm_firerate_verdict_text(int weapon, const EkWeaponRow& row,
                                      char* out, int cap) {
  if (!out || cap <= 0) return;
  out[0] = 0;
  const EkFireRateAssumed* const a = ekm_firerate_assumed(weapon);
  if (!a) {
    _snprintf_s(out, (size_t)cap, _TRUNCATE,
                "no table entry -- this weapon gets no speed-up by design, so "
                "the stops assume nothing about it");
    return;
  }
  const int want[EK_WD_DERIVED_COUNT] = { a->fireDelay, a->totalTime, a->reload };

  const int kDetail = 192;
  char detail[192];
  detail[0] = 0;
  int used = 0, bad = 0;

  for (int i = 0; i < EK_WD_DERIVED_COUNT; i++) {
    const int f = ekm_firerate_derived_field(i);
    if (row.got[f]) continue;
    bad++;
    if (used >= kDetail - 1) continue;
    const int n = _snprintf_s(detail + used, (size_t)(kDetail - used), _TRUNCATE,
                              "%s%s", used ? ", " : "", ekm_weapon_field_name(f));
    if (n < 0) { used = kDetail - 1; continue; }
    used += n;
  }
  if (bad) {
    _snprintf_s(out, (size_t)cap, _TRUNCATE,
                "INCONCLUSIVE -- %s would not read", detail);
    return;
  }

  for (int i = 0; i < EK_WD_DERIVED_COUNT; i++) {
    const int f = ekm_firerate_derived_field(i);
    if (row.v[f] == want[i]) continue;
    bad++;
    if (used >= kDetail - 1) continue;
    const int n = _snprintf_s(detail + used, (size_t)(kDetail - used), _TRUNCATE,
                              "%s%s live %d vs table %d", used ? "; " : "",
                              ekm_weapon_field_name(f), row.v[f], want[i]);
    if (n < 0) { used = kDetail - 1; continue; }
    used += n;
  }
  if (bad) _snprintf_s(out, (size_t)cap, _TRUNCATE, "DISAGREES: %s", detail);
  else     _snprintf_s(out, (size_t)cap, _TRUNCATE,
                       "AGREES on FIREDELAY, TOTALTIME and RELOAD -- the three "
                       "the stops are derived from, and only those");
}

// Keyed on field values so upgrades show (the multi-rocket upgrade rewrites WEAPON5_* silently); unread is its own
// state.
inline bool ekm_weapon_row_differs(const EkWeaponRow& a, const EkWeaponRow& b) {
  for (int f = 0; f < EK_WD_FIELD_COUNT; f++) {
    if ((a.got[f] != 0) != (b.got[f] != 0)) return true;
    if (a.got[f] && a.v[f] != b.v[f]) return true;
  }
  return false;
}

inline bool ekm_weapon_row_should_log(bool everLogged, const EkWeaponRow& now,
                                      const EkWeaponRow& last) {
  return !everLogged || ekm_weapon_row_differs(now, last);
}

// --- weapondata.cpp (DLL only): worker thread, off the frame path ---
void weapondata_tick(void);
