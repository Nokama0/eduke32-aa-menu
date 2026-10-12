#pragma once

// Would AA's own pickup code refuse this item right now? Pure. FAIL OPEN: an item is refused only when every input
// its row reads was read and the row says no; anything unknown is shown.

const int EKM_TAKE_MAX_WEAPONS = 13;
const int EKM_TAKE_GET_MAX     = 11;

const int EKM_GET_STEROIDS = 0, EKM_GET_SHIELD = 1, EKM_GET_SCUBA = 2,
          EKM_GET_HOLODUKE = 3, EKM_GET_JETPACK = 4, EKM_GET_HEATS = 7,
          EKM_GET_FIRSTAID = 9, EKM_GET_BOOTS = 10;

// The AA gamevars the ammo rows read, each with its own got flag. All PERPLAYER except CHARSEL, STARTGUNS, GAMETYPE
// and STARTMODE (AADEFS.CON:3280, :3227, :3836, :3269), which are global.
enum {
  EKM_TV_LASERMODE = 0, EKM_TV_PISTOLAMMO, EKM_TV_WRISTAMMO, EKM_TV_CHARSEL,
  EKM_TV_NUKESELECT, EKM_TV_WEAP4AMMO, EKM_TV_STARTGUNS, EKM_TV_GAMETYPE,
  EKM_TV_GOTPLASMA, EKM_TV_PLASMAMMO, EKM_TV_GOTRAILGUN, EKM_TV_RAILGUNAMMO,
  EKM_TV_GOTBOW, EKM_TV_ARROWS, EKM_TV_SAWAMMO, EKM_TV_DBAMMO, EKM_TV_GOTDB,
  EKM_TV_STARTMODE,
  EKM_TV_STAKES, EKM_TV_NUKEAMOUNT, EKM_TV_LASERAMMO, EKM_TV_COMMAMMO,
  EKM_TV_GOTSAW, EKM_TV_GOTSABER, EKM_TV_ATTMODE,
  EKM_TV_UP_DUKE_PISTOL, EKM_TV_UP_SHELLY_PISTOL, EKM_TV_UP_WES_PISTOL,
  EKM_TV_UP_DUKE_SHOTGUN, EKM_TV_UP_SHELLY_SHOTGUN, EKM_TV_UP_WES_SHOTGUN,
  EKM_TV_ES_TYPE, EKM_TV_ES_BATTERY, EKM_TV_ES_BATTERYMAX,
  EKM_TV_COUNT
};
static_assert(EKM_TV_COUNT <= 64, "ekm_take_var_mask is one unsigned long long");

// One `got` flag per input group. gotPs is the player-struct span: maxhp, max_ammo, ammo, inv, the UNMASKED
// got_access (AA uses bits 3-9 too) and gotweapon.
struct EkTakeState {
  bool gotBody;  int hp;
  bool gotPs;    int maxhp;
  int maxAmmo[EKM_TAKE_MAX_WEAPONS];
  int ammo[EKM_TAKE_MAX_WEAPONS];
  int inv[EKM_TAKE_GET_MAX];
  int gotAccess;
  int gotWeapon;
  bool gotPchar; int pchar;
  bool gotStack; int steroidStack;
  bool gotUpg;   int invenUpgrades;
  bool gotVar[EKM_TV_COUNT]; int var[EKM_TV_COUNT];
};

// Which EKM_TV_* vars a picnum's row may read, so the caller reads them only when the walk meets that picnum.
inline unsigned long long ekm_take_var_mask(int picnum) {
  switch (picnum) {
    case 40:    return (1u << EKM_TV_LASERMODE) | (1u << EKM_TV_PISTOLAMMO) |
                       (1u << EKM_TV_WRISTAMMO) | (1u << EKM_TV_CHARSEL);
    case 44:    return (1u << EKM_TV_NUKESELECT) | (1u << EKM_TV_WEAP4AMMO);
    case 49:    return (1u << EKM_TV_STARTGUNS) | (1u << EKM_TV_GAMETYPE);
    case 37:
    case 10424: return (1u << EKM_TV_GOTPLASMA) | (1u << EKM_TV_PLASMAMMO);
    case 42:
    case 5331:  return (1u << EKM_TV_GOTRAILGUN) | (1u << EKM_TV_RAILGUNAMMO);
    case 8946:  return (1u << EKM_TV_GOTBOW) | (1u << EKM_TV_ARROWS);
    case 8867:  return (1u << EKM_TV_SAWAMMO);
    case 18670:
    case 18669: return (1u << EKM_TV_STARTMODE) | (1u << EKM_TV_GOTDB) |
                       (1u << EKM_TV_DBAMMO);
    case 21:    return (1u << EKM_TV_LASERMODE) | (1u << EKM_TV_PISTOLAMMO) |
                       (1u << EKM_TV_CHARSEL) | (1u << EKM_TV_UP_DUKE_PISTOL) |
                       (1u << EKM_TV_UP_SHELLY_PISTOL) | (1u << EKM_TV_UP_WES_PISTOL);
    case 22:    return (1u << EKM_TV_CHARSEL);
    case 8176:  return (1ull << EKM_TV_LASERMODE) | (1ull << EKM_TV_PISTOLAMMO) |
                       (1ull << EKM_TV_UP_DUKE_PISTOL) | (1ull << EKM_TV_UP_SHELLY_PISTOL);
    case 23:    return (1u << EKM_TV_NUKESELECT) | (1u << EKM_TV_WEAP4AMMO);
    case 24:
    case 10423: return (1u << EKM_TV_GOTPLASMA) | (1u << EKM_TV_PLASMAMMO);
    case 28:    return (1u << EKM_TV_STAKES) | (1u << EKM_TV_ATTMODE);
    case 29:
    case 5329:  return (1u << EKM_TV_GOTRAILGUN) | (1u << EKM_TV_RAILGUNAMMO);
    case 36:    return (1u << EKM_TV_GOTSAW) | (1u << EKM_TV_SAWAMMO) |
                       (1u << EKM_TV_GOTSABER);
    case 5860:  return (1u << EKM_TV_LASERAMMO);
    case 17494: return (1u << EKM_TV_COMMAMMO);
    case 10357: return (1u << EKM_TV_NUKEAMOUNT);
    case 10934: return (1u << EKM_TV_GOTSABER);
    case 8909:  return (1u << EKM_TV_UP_DUKE_SHOTGUN);
    case 8938:  return (1u << EKM_TV_GOTBOW) | (1u << EKM_TV_ARROWS) |
                       (1u << EKM_TV_UP_SHELLY_SHOTGUN) | (1u << EKM_TV_UP_WES_SHOTGUN);
    case 5885:  return (1ull << EKM_TV_ES_TYPE) | (1ull << EKM_TV_ES_BATTERY) |
                       (1ull << EKM_TV_ES_BATTERYMAX);
    default:    return 0;
  }
}

// A var the row needs; false (the caller shows the item) when it was not read.
inline bool ekm_take_var(const EkTakeState& st, int k, int* out) {
  if (k < 0 || k >= EKM_TV_COUNT || !st.gotVar[k]) return false;
  *out = st.var[k];
  return true;
}

// A struct failing any of these is not the player's, or not read at a sane moment, so no row may use it.
inline bool ekm_takestate_plausible(int maxhp, const int* maxAmmo, int n,
                                    bool gotHp, int hp) {
  if (maxhp <= 0 || maxhp > 1000 || !maxAmmo) return false;
  for (int i = 0; i < n; i++)
    if (maxAmmo[i] < 0 || maxAmmo[i] > 32767) return false;
  if (gotHp && hp > 4 * maxhp) return false;
  return true;
}

inline int ekm_keycard_bit(int pal) {
  switch (pal) {
    case 0:  return 1;    case 21: return 2;    case 23: return 4;
    case 11: return 8;    case 18: return 16;   case 37: return 32;
    case 45: return 64;   case 40: return 128;  case 39: return 256;
    case 38: return 512;
    default: return 0;
  }
}

const int EKM_TAKE_CAT_NONE = 0, EKM_TAKE_CAT_HEALTH = 1, EKM_TAKE_CAT_AMMO = 2,
          EKM_TAKE_CAT_INVENTORY = 3, EKM_TAKE_CAT_KEYCARD = 4,
          EKM_TAKE_CAT_WEAPON = 5, EKM_TAKE_CAT_COUNT = 6;
inline int ekm_item_take_category(int picnum) {
  switch (picnum) {
    case 100: case 53: case 51: case 52:              return EKM_TAKE_CAT_HEALTH;
    case 48: case 41: case 46: case 19941: case 45:
    case 40: case 44: case 49: case 47: case 37: case 42: case 5331:
    case 8946: case 8867: case 18670: case 10424:     return EKM_TAKE_CAT_AMMO;
    case 54: case 61: case 59: case 56: case 57:
    case 1348: case 55: case 5885:                    return EKM_TAKE_CAT_INVENTORY;
    case 60:                                          return EKM_TAKE_CAT_KEYCARD;
    case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28:
    case 29: case 32: case 36: case 5329: case 5860: case 8176: case 8909: case 8938:
    case 10357: case 10423: case 10700: case 10934: case 17494:
    case 18669:                                       return EKM_TAKE_CAT_WEAPON;
    default:                                          return EKM_TAKE_CAT_NONE;
  }
}

// Ammo rows, AAITEMS.CON per row, each re-read over its whole actor body: a path that gives anything, or turns the
// sprite into another pickup, is a take. The MAX* limits are compiled into the script as literals, so they are here
// too.
inline bool ekm_item_refused_ammo(int picnum, const EkTakeState& st) {
  const int* const ammo = st.ammo;
  const int* const mx = st.maxAmmo;
  int a = 0, b = 0;
  switch (picnum) {
    case 40: {
      if (!st.gotPchar) return false;
      if (st.pchar == 3 && !(st.gotWeapon & (1 << 1))) return false;
      if (!ekm_take_var(st, EKM_TV_LASERMODE, &a)) return false;
      if (a > 0)
        return ekm_take_var(st, EKM_TV_PISTOLAMMO, &b) && b >= mx[1];
      if (ammo[1] < mx[1]) return false;
      if (!ekm_take_var(st, EKM_TV_CHARSEL, &a)) return false;
      if (!(a & 8)) return true;
      return ekm_take_var(st, EKM_TV_WRISTAMMO, &b) && b >= mx[1];
    }
    case 44: {
                // RPGAMMO: both arms feed weap4ammo (the nukeselect arm checks its limit first); the else arm then
                // `addammo RPG_WEAPON`.
      if (!ekm_take_var(st, EKM_TV_NUKESELECT, &a) ||
          !ekm_take_var(st, EKM_TV_WEAP4AMMO, &b)) return false;
      if (b < 50) return false;
      return a == 1 || ammo[4] >= mx[4];
    }
    case 49: {
                // SHOTGUNAMMO: startguns&2 in gametype > 0 adds arrows and dbammo and consumes the box even at full,
                // else addammo.
      if (!ekm_take_var(st, EKM_TV_STARTGUNS, &a)) return false;
      if (a & 2) {
        if (!ekm_take_var(st, EKM_TV_GAMETYPE, &b) || b > 0) return false;
      }
      return ammo[2] >= mx[2];
    }
    case 47:
                // HBOMBAMMO: `addweapon` gives the weapon when not owned; `ifgotweaponce 0` refuses more only in MP.
      return (st.gotWeapon & (1 << 5)) != 0 && ammo[5] >= mx[5];
    case 37: {
      if (!ekm_take_var(st, EKM_TV_GOTPLASMA, &a)) return false;
      if (a & 2) return ekm_take_var(st, EKM_TV_PLASMAMMO, &b) && b >= 200;
      return ammo[9] >= mx[9];
    }
    case 42:
    case 5331: {
      if (!ekm_take_var(st, EKM_TV_GOTRAILGUN, &a)) return false;
      const bool railPool = (picnum == 42) == ((a & 2) != 0);
      if (railPool) {
        if (!ekm_take_var(st, EKM_TV_RAILGUNAMMO, &b)) return false;
      } else {
        b = ammo[7];
      }
      return b >= (picnum == 42 ? 120 : 100);
    }
    case 8946:
      if (!ekm_take_var(st, EKM_TV_GOTBOW, &a) || !(a & 1)) return false;
      return ekm_take_var(st, EKM_TV_ARROWS, &b) && b >= 60;
    case 8867:
      return ekm_take_var(st, EKM_TV_SAWAMMO, &a) && a == 1000;
    case 18670:
      if (!ekm_take_var(st, EKM_TV_STARTMODE, &a) || !(a & (1 | 8 | 16))) return false;
      if (!ekm_take_var(st, EKM_TV_GOTDB, &a) || !(a & 1)) return false;
      return ekm_take_var(st, EKM_TV_DBAMMO, &b) && b >= mx[2];
    case 10424:
                // PLASMAAMMO: refused only with the gun (bit 1); bit 2 picks the pool. MAXPLASMAMOUNT 200.
      if (!ekm_take_var(st, EKM_TV_GOTPLASMA, &a) || !(a & 1)) return false;
      if (a & 2) return ammo[9] >= 200;
      return ekm_take_var(st, EKM_TV_PLASMAMMO, &b) && b >= 200;
    default:
      return false;
  }
}

inline bool ekm_take_owned_full(const EkTakeState& st, int w) {
  return (st.gotWeapon & (1 << w)) != 0 && st.ammo[w] >= st.maxAmmo[w];
}

// Weapon rows, each read over its whole actor body including what runs before the range test: refused only when
// touching gives nothing and the sprite does not first turn into another pickup. Upgrade pickups are not here: they
// depend on the character, and a wrong hide is worse than a mark too many.
inline bool ekm_item_refused_weapon(int picnum, const EkTakeState& st) {
  const int* const ammo = st.ammo;
  int a = 0, b = 0;
  switch (picnum) {
    case 21: {
      if (!st.gotPchar) return false;
      if (st.pchar == 0 || st.pchar == 1) {   // FIRSTGUNSPRITE: bit 8 turns it into GLOCKSPRITE.
        if (!ekm_take_var(st, st.pchar ? EKM_TV_UP_SHELLY_PISTOL : EKM_TV_UP_DUKE_PISTOL, &a) ||
            (a & 8))
          return false;
      }
      if (!ekm_take_var(st, EKM_TV_LASERMODE, &a)) return false;
      if (a > 0) {
        if (st.pchar == 2 && (!ekm_take_var(st, EKM_TV_UP_WES_PISTOL, &b) || !(b & 4)))
          return false;
        return (st.gotWeapon & (1 << 1)) != 0 &&
               ekm_take_var(st, EKM_TV_PISTOLAMMO, &b) && b >= st.maxAmmo[1];
      }
      // FIRSTGUNSPRITE: Wes gets dual pistols first, then the pistolammo gate; wesupgrades and weap_owned are written
      // under charsel&4.
      if (!ekm_take_var(st, EKM_TV_CHARSEL, &a) || (a & 4)) return false;
      return ekm_take_owned_full(st, 1);
    }
    case 8176: {
                  // GLOCKSPRITE: only pchar 0/1 with upgrade bit 8 stay one; not owned is a take. pickupglock's clip
                  // refill runs before its refusal, so touching a refused Glock still refills the clip (a known side
                  // effect; treated as refused).
      if (!st.gotPchar || (st.pchar != 0 && st.pchar != 1)) return false;
      if (!ekm_take_var(st, st.pchar ? EKM_TV_UP_SHELLY_PISTOL : EKM_TV_UP_DUKE_PISTOL, &a) ||
          !(a & 8)) return false;
      if (!(st.gotWeapon & (1 << 1))) return false;
      if (!ekm_take_var(st, EKM_TV_LASERMODE, &a)) return false;
      if (a > 0)
        return ekm_take_var(st, EKM_TV_PISTOLAMMO, &b) && b >= st.maxAmmo[1];
      return ammo[1] >= st.maxAmmo[1];
    }
    case 22:
      if (!st.gotPchar || st.pchar == 2) return false;
      if (!ekm_take_var(st, EKM_TV_CHARSEL, &a) || (a & 4)) return false;
      return ekm_take_owned_full(st, 3);
    case 23: {
                // RPGSPRITE: both arms fill weap4ammo first (MAXRPGAMMO 50); the nukeselect arm refuses on it alone.
      if (!ekm_take_var(st, EKM_TV_NUKESELECT, &a) ||
          !ekm_take_var(st, EKM_TV_WEAP4AMMO, &b)) return false;
      if (b < 50) return false;
      return a == 1 || ekm_take_owned_full(st, 4);
    }
    case 24:
                // FREEZESPRITE: gotplasma&2 feeds plasmammo (MAXFREEZEAMMO 200) whether owned or not, else addweapon.
      if (!ekm_take_var(st, EKM_TV_GOTPLASMA, &a)) return false;
      if (a & 2) return ekm_take_var(st, EKM_TV_PLASMAMMO, &b) && b >= 200;
      return ekm_take_owned_full(st, 9);
    case 25:  return ekm_take_owned_full(st, 6);
    case 26:  return ammo[5] >= st.maxAmmo[5];
    case 27:
    case 10700: // TRIPBOMBSPRITE: an upgraded one becomes SPIDERMINESPRITE, whose addweapon is the same.
      return ekm_take_owned_full(st, 8);
    case 28:
                // SHOTGUNSPRITE: attrition may replace it; killit or addweapon refuse when owned and full, and Wes's
                // stakes arm also needs 99 stakes.
      if (!st.gotPchar || !ekm_take_var(st, EKM_TV_ATTMODE, &a) || a == 1) return false;
      if (!ekm_take_owned_full(st, 2)) return false;
      if (st.pchar != 2) return true;
      return ekm_take_var(st, EKM_TV_STAKES, &b) && b >= 99;
    case 29:
      if (!(st.gotWeapon & (1 << 7))) return false;
      if (!ekm_take_var(st, EKM_TV_GOTRAILGUN, &a)) return false;
      if (a & 2) {
        if (!ekm_take_var(st, EKM_TV_RAILGUNAMMO, &b)) return false;
      } else {
        b = ammo[7];
      }
      return b >= 120;
    case 32:  return ekm_take_owned_full(st, 11);
    case 36:
                // SAWSPRITE: Wes/Dea convert it, a saber is swapped out, then `ife gotsaw YES ife sawammo MAXSAWAMMO
                // break` (1000).
      if (!st.gotPchar || st.pchar == 2 || st.pchar == 4) return false;
      if (!ekm_take_var(st, EKM_TV_GOTSABER, &a) || a > 0) return false;
      return ekm_take_var(st, EKM_TV_GOTSAW, &a) && a == 1 &&
             ekm_take_var(st, EKM_TV_SAWAMMO, &b) && b == 1000;
    case 5329:
      if (!ekm_take_var(st, EKM_TV_GOTRAILGUN, &a) || !(a & 1)) return false;
      if (a & 2) return ammo[7] >= 100;
      return ekm_take_var(st, EKM_TV_RAILGUNAMMO, &b) && b >= 100;
    case 10423:
      if (!ekm_take_var(st, EKM_TV_GOTPLASMA, &a) || !(a & 1)) return false;
      if (a & 2) return ammo[9] >= 200;
      return ekm_take_var(st, EKM_TV_PLASMAMMO, &b) && b >= 200;
    case 5860:  return ekm_take_var(st, EKM_TV_LASERAMMO, &a) && a >= 100;
    case 17494: return ekm_take_var(st, EKM_TV_COMMAMMO, &a) && a >= 50;
    case 10357: return ekm_take_var(st, EKM_TV_NUKEAMOUNT, &a) && a > 0;
    case 10934:
      if (!st.gotPchar) return false;
      if (st.pchar == 4) return true;
      return ekm_take_var(st, EKM_TV_GOTSABER, &a) && a >= 99;
    case 8909:
                // ATOMICSHOTTY: Bombshell breaks; without dukeupgrades[2]&4 it grants it, with it addweapon.
      if (!st.gotPchar) return false;
      if (st.pchar == 1) return true;
      if (!ekm_take_var(st, EKM_TV_UP_DUKE_SHOTGUN, &a) || !(a & 4)) return false;
      return ekm_take_owned_full(st, 2);
    case 8938:
                // BOWSPRITE: only pchar 1/2 reach pickupbow, which gives the shotgun first, then `ifvarand gotbow 1
                // ifge arrows MAXSHOTGUNAMMO break` (60).
      if (!st.gotPchar) return false;
      if (st.pchar != 1 && st.pchar != 2) return true;
      if (!ekm_take_var(st, EKM_TV_UP_SHELLY_SHOTGUN, &a) || (a & 36) != 36) return false;
      if (!ekm_take_var(st, EKM_TV_UP_WES_SHOTGUN, &a) || (a & 36) != 36) return false;
      if (!(st.gotWeapon & (1 << 2))) return false;
      if (!ekm_take_var(st, EKM_TV_GOTBOW, &a) || !(a & 1)) return false;
      return ekm_take_var(st, EKM_TV_ARROWS, &b) && b >= 60;
    case 18669:
                // DBSPRITE: becomes BOWSPRITE without startmode 1/8/16; grants the DB without gotDB&1, the shotgun,
                // then `ifge dbammo max_ammo[2] break`.
      if (!ekm_take_var(st, EKM_TV_STARTMODE, &a) || !(a & (1 | 8 | 16))) return false;
      if (!ekm_take_var(st, EKM_TV_GOTDB, &a) || !(a & 1)) return false;
      if (!(st.gotWeapon & (1 << 2))) return false;
      return ekm_take_var(st, EKM_TV_DBAMMO, &b) && b >= st.maxAmmo[2];
    default:
      return false;
  }
}

inline bool ekm_item_refused(int picnum, int pal, const EkTakeState& st) {
  if (picnum == 5885 && pal != 0) return true;
  if (!st.gotPs) return false;
  if (!ekm_takestate_plausible(st.maxhp, st.maxAmmo, EKM_TAKE_MAX_WEAPONS,
                               st.gotBody, st.hp))
    return false;
  const int* const inv = st.inv;
  switch (picnum) {
    case 100:
      return st.gotBody && st.hp >= 2 * st.maxhp;
    case 53:
      return inv[EKM_GET_FIRSTAID] >= st.maxhp;
    case 51:
    case 52:
      if (!st.gotBody || st.hp < st.maxhp) return false;
      if (st.gotUpg)
        return !((st.invenUpgrades & 1) && inv[EKM_GET_FIRSTAID] < st.maxhp);
      return inv[EKM_GET_FIRSTAID] >= st.maxhp;
    case 54:  return inv[EKM_GET_SHIELD]  >= 100;
    case 61:  return inv[EKM_GET_BOOTS]   >= 160;
    case 59:  return inv[EKM_GET_HEATS]   >= 900;
    case 56:  return inv[EKM_GET_SCUBA]   >= 5000;
    case 57:  return inv[EKM_GET_JETPACK] >= 1200;
    case 1348:
      return st.gotPchar && inv[EKM_GET_HOLODUKE] > 1800 && st.pchar != 3;
    case 55: {
      const int amt = inv[EKM_GET_STEROIDS];
      if (amt == 0) return false;
      if (st.gotStack)
        return !(amt == 400 && st.steroidStack >= 0 && st.steroidStack < 3);
      return amt > 0 && amt < 400;
    }
    case 60: {
      const int bit = ekm_keycard_bit(pal);
      return bit != 0 && (st.gotAccess & bit) != 0;
    }
    case 5885: {
                  // SHIELDBATTERY: taken only with es_type != 0 and es_battery < es_batterymax; a full battery may
                  // first turn into a SHIELD (another picnum, shown by its own row).
      int t = 0, b = 0, m = 0;
      if (ekm_take_var(st, EKM_TV_ES_TYPE, &t) && t == 0) return true;
      return ekm_take_var(st, EKM_TV_ES_BATTERY, &b) &&
             ekm_take_var(st, EKM_TV_ES_BATTERYMAX, &m) && b >= m;
    }
    case 48:  return st.ammo[1]  >= st.maxAmmo[1];
    case 41:  return st.ammo[3]  >= st.maxAmmo[3];
    case 46:
    case 19941: return st.ammo[6] >= st.maxAmmo[6];
    case 45:  return st.ammo[11] >= st.maxAmmo[11];
    default:  return ekm_item_refused_ammo(picnum, st) ||
                     ekm_item_refused_weapon(picnum, st);
  }
}
