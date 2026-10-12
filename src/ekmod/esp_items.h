#pragma once

// Which picnum is a pickup, and its name. Every number is this game's own (names.h, GAME.CON addweapon/addammo actor
// blocks, log refusals); World Tour's FLAMETHROWERSPRITE 5134 / AMMO 5135 do not exist here. Unlisted returns NULL, not
// a number: tile names are runtime variables (g_dynTileList[]). AA's: esp_items_aa.h.
#include <string.h>
#include "esp_items_aa.h"
#include "esp_items_curated_aa.h"
#include "ui/canvas.h"

const int EKM_PIC_ACCESSCARD = 60;
const int EKM_PIC_HEAVYHBOMB = 26;

// gameexec.cpp:1035-1037 sets got_access from exactly these three pal values.
const int EKM_PAL_KEY_BLUE   = 0;
const int EKM_PAL_KEY_RED    = 21;
const int EKM_PAL_KEY_YELLOW = 23;

inline const char* ekm_item_name_stock(int picnum) {
  switch (picnum) {
    case 21:   return "Pistol";
    case 22:   return "Chaingun cannon";
    case 23:   return "RPG";
    case 24:   return "Freezethrower";
    case 25:   return "Shrinker";
    // One thrown bomb; 47 is the box. World Tour drops it, believing a pickup looks like a live bomb; not on this
    // engine.
    case 26:   return "Pipebomb";
    case 27:   return "Laser tripbomb";
    case 28:   return "Shotgun";
    case 29:   return "Devastator";
    // Expander was missing here (its ammo, 45, was not): the engine's precache lists GROWSPRITEICON (premap.cpp:181).
    case 32:   return "Expander";
    case 37:   return "Freezethrower ammo";
    case 40:   return "Pistol ammo";
    case 41:   return "Chaingun ammo";
    case 42:   return "Devastator ammo";
    case 44:   return "RPG ammo";
    case 45:   return "Expander ammo";
    case 46:   return "Shrinker ammo";
    case 47:   return "Pipebombs";
    case 48:   return "Pistol ammo";
    case 49:   return "Shotgun ammo";
    case 51:   return "Small medkit";
    case 52:   return "Large medkit";
    case 53:   return "Portable medkit";
    case 54:   return "Armour";
    case 55:   return "Steroids";
    case 56:   return "Scuba gear";
    case 57:   return "Jetpack";
    case 59:   return "Night vision goggles";
    case 60:   return "Keycard";
    case 61:   return "Boots";
    case 100:  return "Atomic health";
    case 1348: return "Holoduke";
    default:   return 0;
  }
}

// Order: stock, curated AA, generated AA, category, null; asking AA first would rename the pistol "FIRSTGUNSPRITE" on
// every game. esp_items_runtime_aa.h wins over all (a live read is never shadowed). The category fallback is a guard,
// not the fix: an unnamed AA pickup gets a category, never a CON symbol.
inline const char* ekm_item_name(int picnum) {
  const char* const stock = ekm_item_name_stock(picnum);
  if (stock) return stock;
  const char* const curated_aa = ekm_item_name_curated_aa(picnum);
  if (curated_aa) return curated_aa;
  const char* const generated = ekm_item_name_aa(picnum);
  if (generated && ekm_item_aa_symbol_kept(picnum))
    return ekm_item_category_aa(picnum);
  return generated;
}

inline const char* ekm_item_name_pal(int picnum, int pal) {
  const char* nm = ekm_item_name(picnum);
  if (nm == 0 || picnum != EKM_PIC_ACCESSCARD) return nm;
  if (pal == EKM_PAL_KEY_BLUE)   return "Blue keycard";
  if (pal == EKM_PAL_KEY_RED)    return "Red keycard";
  if (pal == EKM_PAL_KEY_YELLOW) return "Yellow keycard";
  return nm;
}

// Pickup is there to take: a respawning item is hidden by cstat bit 15, not deleted. Mask, never compare equal
// (sign-extended short).
inline bool ekm_item_present(int cstat, int xrepeat) {
  return (cstat & 0x8000) == 0 && xrepeat > 0;
}

// Whether a thrown pipebomb can be picked up: actors.cpp:5435 `HEAVYHBOMB && playerDist < 788 && pData[0] > 7 && xvel
// == 0` (playerDist omitted: the ESP marks what is takeable). An armed bomb stays marked: t_data[6] gates nothing.
inline bool ekm_pipebomb_takeable(int tdata0, int xvel) {
  return tdata0 > 7 && xvel == 0;
}

// A keycard's colour, or 0. Deliberately not World Tour's blue-for-all (its "shared with the SW_ACCESS lock" mark,
// esp_layout.h:338, has only the lock as a call site); mapping is esp_switch.h:154, yellow is C_HAZARD.
inline unsigned ekm_keycard_colour(int pal) {
  if (pal == EKM_PAL_KEY_BLUE)   return C_BLUE;
  if (pal == EKM_PAL_KEY_RED)    return C_RED;
  if (pal == EKM_PAL_KEY_YELLOW) return C_HAZARD;
  return 0;
}
