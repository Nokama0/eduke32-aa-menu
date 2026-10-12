#pragma once

#include <string.h>

// Enemy names for the ESP label, built from EDuke32's own names.h (not World Tour's: the tile sets differ). Tile names
// are runtime variables (a CON `define COMMANDER n` writes through g_dynTileList, dnames.cpp:1595), so a mod can move
// an enemy; an unlisted picnum prints its NUMBER, since a wrong name is worse.

// pal 21 marks the Assault Captain and shares trooper picnums with the ordinary Assault Trooper (World Tour,
// esp_pics.cpp:179-182); keying on picnum alone labels every Captain a Trooper.
const int EKM_PAL_CAPTAIN = 21;

inline bool ekm_picnum_is_trooper(int picnum) {
  switch (picnum) {
    case 1680: case 1681: case 1682: case 1715:
    case 1725: case 1741: case 1742: case 1744:
      return true;
    default:
      return false;
  }
}

// The name for a picnum, or null: the caller must print the number rather than guess.
inline const char* ekm_enemy_name(int picnum) {
  if (picnum >= 2370 && picnum <= 2377) return "Protozoid Slimer";
  if (ekm_picnum_is_trooper(picnum)) return "Assault Trooper";
  switch (picnum) {
    case 675:  return "Egg";
    case 1267: return "Rat";
    case 1550: return "Shark";
    case 1810: return "Cannon";
    case 1820: case 1821: return "Octabrain";
    case 1880: return "Sentry Drone";
    case 1920: case 1921: return "Assault Commander";
    case 1960: return "Recon Patrol";
    case 1975: return "Pig Cop Tank";
    case 2000: case 2001: case 2045: return "Pig Cop";
    case 2120: case 2121: case 2150:
    case 2160: case 2165: return "Enforcer";
    case 2360: return "Turret";
    case 2420: return "Organic turret";
    case 2630: case 2631: return "Battlelord";
    case 2710: case 2711: return "Overlord";
    case 2760: case 2761: return "Cycloid Emperor";
    case 4610: case 4611: case 4670:
    case 4671: case 4690: return "Protector Drone";
    case 4740: case 4741: return "Alien Queen";
    case 5180: return "Firefly Trooper";
    case 5310: case 5311: return "Alien Overlord";
    default:   return 0;
  }
}

// BOSS1 with a non-zero pal is a different enemy, the Battlelord Sentry: GAME.CON:6466-6470 gives it strength 1000
// against 4500 and game.cpp:2073-2081 halves its size. Any non-zero pal, unlike the Captain's explicit `ifspritepal
// 21`. Neither World Tour nor Megaton names it.
const int EKM_PIC_BOSS1 = 2630, EKM_PIC_BOSS1STAYPUT = 2631;
inline bool ekm_picnum_is_boss1(int picnum) {
  return picnum == EKM_PIC_BOSS1 || picnum == EKM_PIC_BOSS1STAYPUT;
}

// The name with the palette taken into account, which the ESP calls: a Captain and a Sentry have no picnum of their
// own.
inline const char* ekm_enemy_name_pal(int picnum, int pal) {
  if (pal == EKM_PAL_CAPTAIN && ekm_picnum_is_trooper(picnum)) return "Assault Captain";
  if (pal != 0 && ekm_picnum_is_boss1(picnum)) return "Battlelord Sentry";
  return ekm_enemy_name(picnum);
}

// Health label maximum from THIS build's CON (`actor <PIC> <STRENGTH>` lines resolved through `define ...STRENGTH`),
// never World Tour's table. 0 means UNKNOWN, not no enemy: RAT, RECON, GREENSLIME, LIZMANFEEDING and some picnums the
// CON never defines have no actor line; esp_health_number turns 0 into a bare "%d".
inline int ekm_enemy_max_strength(int picnum) {
  switch (picnum) {
    case 675:  return 20;
    case 1550: return 35;
    case 1680: case 1681: case 1682: case 1715:
    case 1725: case 1741: case 1742: case 1744:
      return 30;
    case 1810: return 400;
    case 1820: case 1821: return 175;
    case 1880: return 150;
    case 1920: case 1921: return 350;
    case 1975: return 500;
    case 2000: case 2001: case 2045: return 100;
    // LIZMAN/STAYPUT/SPITTING/JUMP: LIZSTRENGTH=100; LIZMANFEEDING (2160) has no actor line.
    case 2120: case 2121: case 2150: case 2165: return 100;
    case 2360: return 40;
    case 2420: return 30;
    case 2630: case 2631: return 4500;
    case 2710: return 4500;
    case 2760: return 4500;
    // NEWBEAST family: NEWBEASTSTRENGTH=300; NEWBEASTHANGDEAD (4671) is its own actor line at TOUGH=20.
    case 4610: case 4611: case 4670: case 4690: return 300;
    case 4671: return 20;
    case 4740: case 4741: return 6000;
    // RAT, RECON, GREENSLIME: spawned by the engine, no actor line. BOSS2STAYPUT/BOSS3STAYPUT/FIREFLY/BOSS5(STAYPUT)
    // are not defined in this build's CON (Atomic Edition content).
    default: return 0;
  }
}

// The table is a floor: the shown maximum is the greater of the CON strength and the highest health seen on that
// sprite. A_Spawn doubles `extra` for every non-zero pal (game.cpp:2060-2062), so the table alone labelled a 60 HP
// Captain 30/30. 0 out means unknown.
inline int ekm_enemy_shown_max(int strength, int observed) {
  const int a = strength > 0 ? strength : 0;
  const int b = observed > 0 ? observed : 0;
  return a > b ? a : b;
}

// First-tic maxima. Captain: `checktrooppalette` (GAME.CON:4709-4718, `ifai 0`) adds TROOPSTRENGTH once, so twice the
// spawn strength. Pal'd bosses: `ifspritepal 0 ... else` SETS strength per boss: BOSS1 1000 (GAME.CON:6464), BOSS2/3 a
// literal 1 (:6796, :7071). STAYPUT variants are not covered: 2631 becomes 2630 on its first tic.
inline int ekm_boss_pal_strength(int picnum) {
  switch (picnum) {
    case 2630: return 1000;
    case 2710: return 1;
    case 2760: return 1;
    default:   return 0;
  }
}

inline int ekm_enemy_ruled_max(int picnum, int pal) {
  if (pal == 0) return 0;
  const int bossPal = ekm_boss_pal_strength(picnum);
  if (bossPal > 0) return bossPal;
  if (pal != EKM_PAL_CAPTAIN || !ekm_picnum_is_trooper(picnum)) return 0;
  const int spawn = ekm_enemy_max_strength(picnum);
  return spawn > 0 ? spawn * 2 : 0;
}

// The maximum to measure bar and label against when the CON states one, else 0. Never below the shown health (World
// Tour's guard, esp_pics.cpp:235-240): a bar past full reads as broken; flooring puts the disagreement in the number.
inline int ekm_enemy_bar_max(int ruledMax, int current) {
  if (ruledMax <= 0) return 0;
  return ruledMax > current ? ruledMax : current;
}

// Current health as shown, before the pending-damage discount. A dormant Captain carries spawn 30 with a real maximum
// 60; `hasRun` is the first tic having happened. Narrower than Megaton's rule on purpose: requiring hp exactly on the
// spawn strength limits a misread `hasRun` to one wrong value. From World Tour esp_pics.h:44-55.
inline int ekm_display_hp(int hp, int spawnStrength, int ruledMax, bool hasRun) {
  if (hasRun) return hp;
  if (ruledMax <= 0 || ruledMax == spawnStrength) return hp;
  return hp == spawnStrength ? ruledMax : hp;
}

// One sprite slot's learned maximum (esp.cpp holds one per sprite index); pure so the rule can be pinned.
struct EkmHealthLatch {
  short max;
  short picnum;
  short pal;
};

// Two occupants of a slot are the same enemy when the panel would call them the same thing, not when picnums match: one
// actor changes tile as it acts (LIZTROOPONTOILET `cactor LIZTROOP`, GAME.CON:4749-4777). Accepted divergence: Duke
// rewrites a live actor's pal (actors.cpp:4905, freeze), so it relearns its ceiling; ruled maxima override the latch.
inline bool ekm_same_enemy(int picnumA, int palA, int picnumB, int palB) {
  const char* const a = ekm_enemy_name_pal(picnumA, palA);
  const char* const b = ekm_enemy_name_pal(picnumB, palB);
  if (!a || !b) return picnumA == picnumB && palA == palB;
  return a == b || strcmp(a, b) == 0;
}

// The ceiling after this frame's observation, or 0 while nothing is known.
inline int ekm_health_latch(EkmHealthLatch* slot, int picnum, int pal, int hp) {
  if (!slot) return 0;
  if (!ekm_same_enemy(picnum, pal, (int)slot->picnum, (int)slot->pal)) {
    slot->max = 0;
    slot->picnum = (short)picnum;
    slot->pal = (short)pal;
  }
  if (hp > (int)slot->max) slot->max = (short)hp;
  return (int)slot->max;
}
