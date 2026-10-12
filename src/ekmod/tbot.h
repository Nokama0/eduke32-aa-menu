#pragma once

// The triggerbot: holds the fire bit while a condition holds. Decisions are pure so the suite pins them; tbot.cpp
// gathers engine answers and ORs one bit into the tic's input record (never clears it: the player may hold fire).

#include "aimsolve.h"
#include "playerstate.h"
#include "wallstate.h"

const int TBOT_WHEN_CROSSHAIR = 0;
const int TBOT_WHEN_AIMBOT    = 1;

const int TBOT_DELAY_MS_MIN = 0, TBOT_DELAY_MS_MAX = 500;
const int TBOT_DELAY_MS_DEFAULT = TBOT_DELAY_MS_MIN;
const int TBOT_TICS_PER_SECOND = 30;

const int TBOT_HELD_MS_CAP = 1000000;

// Most one frame may add to the held clock: past four tics a gap is a pause or load, not a held condition.
const int TBOT_FRAME_MS_MAX = (int)EKM_LEAD_GAP_MAX_MS;

// Crosshair cone, an approximation of a hitscan: it misses thin geometry between the cansee rungs, glass and grates
// (cansee sees through, bullets do not) and non-wall blockers. 24 units = half AUTO_AIM_ANGLE (48); erring tight costs
// a held trigger.
const int TBOT_CROSSHAIR_YAW_UNITS = 24;

// Vertical cone in whole horiz units; horiz is a slope: q16horiz = F16(100) + F16(128) * tan(pitch), so 4.2 degrees is
// 9.4.
const int TBOT_CROSSHAIR_HORIZ_UNITS = 10;

// Ints, not bools: MSVC /O2 lost a bool member of a struct returned by an inline function (Megaton port).
struct TbotState { int heldMs; int heldTics; int firing; };

struct TbotCond { int holds; const char* reason; };

inline int tbot_when_clamped(int when) {
  return when == TBOT_WHEN_AIMBOT ? TBOT_WHEN_AIMBOT : TBOT_WHEN_CROSSHAIR;
}

inline int tbot_delay_ms_clamped(int ms) {
  return ms < TBOT_DELAY_MS_MIN ? TBOT_DELAY_MS_MIN
                                : (ms > TBOT_DELAY_MS_MAX ? TBOT_DELAY_MS_MAX : ms);
}

// Milliseconds to whole tics, nearest. Integer arithmetic: 30 tics a second is exact in thousandths.
inline int tbot_delay_tics(int ms) {
  const int c = tbot_delay_ms_clamped(ms);
  return (c * TBOT_TICS_PER_SECOND + 500) / 1000;
}

// Tics a condition held for `heldMs` has been up, counting this one (the +1 fires a zero delay on the first frame). Tic
// clock, not frame counter, so 100 ms is 100 ms at any fps. Integer: 500 / (1000.0/30.0) is 14.999..., one tic late.
inline int tbot_held_tics(int heldMs) {
  if (heldMs <= 0) return 1;
  return 1 + (heldMs * TBOT_TICS_PER_SECOND) / 1000;
}

// Out: knee and handremote fire nothing; the pipe bomb throws on RELEASE of the fire bit (rapid_fire_hold,
// player.cpp:4496), so the bot would throw it as the enemy leaves view; the trip bomb places on a wall.
inline bool tbot_weapon_usable(int weapon) {
  return ekm_weapon_is_hitscan(weapon) || ekm_weapon_proj_speed(weapon) > 0;
}

// `haveTarget` is chosen once a tic, but both offsets are re-solved each frame from one snapshot (ekm_aim_view_pair);
// the yaw is not a tic old (95 units at 300/tic crossing at range 1000).
inline TbotCond tbot_when_crosshair(bool haveTarget, int myYaw, int targetYaw,
                                    int myQ16Horiz, int targetQ16Horiz) {
  TbotCond c = { 0, "notarget" };
  if (!haveTarget) return c;
  if (ekm_angular_offset(myYaw, targetYaw) > TBOT_CROSSHAIR_YAW_UNITS) {
    c.reason = "offyaw";
    return c;
  }
  const int dh = targetQ16Horiz - myQ16Horiz;
  if ((dh < 0 ? -dh : dh) > ekm_f16(TBOT_CROSSHAIR_HORIZ_UNITS)) {
    c.reason = "offpitch";
    return c;
  }
  c.holds = 1;
  c.reason = "on";
  return c;
}

// A target is locked wherever the view points. Staleness is the caller's: the lock is published on the draw.
inline TbotCond tbot_when_aimbot(bool haveLock) {
  TbotCond c = { 0, "nolock" };
  if (!haveLock) return c;
  c.holds = 1;
  c.reason = "lock";
  return c;
}

// What the shot's own ray landed on: the cansee ladder looks through glass and past a dying enemy that still stops
// bullets, so tbot.cpp casts the real ray.
const int TBOT_RAY_UNKNOWN = 0;
const int TBOT_RAY_ENEMY   = 1;
const int TBOT_RAY_BREAK   = 2;
const int TBOT_RAY_BLOCKED = 3;
const int TBOT_RAY_SHIELD  = 4;   // a live enemy shield (ekm_ray_shield)
const int TBOT_RAY_ATTACHMENT = 5;   // an AA Sidegun (ekm_ray_attachment)

// Shared with acquisition's reach test so the two cannot disagree. `ask` answers lazily: an enemy costs one engine
// question, a floor or ceiling none.
template <class Ask>
inline int ekm_ray_class(bool castOk, int hitSprite, int hitWall, Ask& ask) {
  if (!castOk) return TBOT_RAY_UNKNOWN;
  if (hitSprite >= 0 && ask.targetable(hitSprite)) return TBOT_RAY_ENEMY;
  if (hitSprite >= 0 && ask.sprite_breaks(hitSprite)) return TBOT_RAY_BREAK;
  if (hitWall >= 0 && ask.wall_breaks(hitWall)) return TBOT_RAY_BREAK;
  return TBOT_RAY_BLOCKED;
}

// Only BLOCKED (and a shield) is unreachable; no answer is not a demotion.
inline bool ekm_ray_reachable(int ray) {
  return ray != TBOT_RAY_BLOCKED && ray != TBOT_RAY_SHIELD &&
         ray != TBOT_RAY_ATTACHMENT;
}

// A BLOCKED ray on a Sidegun; named so the log counts it. No last-resort verdict, unlike a shield.
inline int ekm_ray_attachment(int ray, int hitSprite, bool readOk, bool attachment) {
  if (ray != TBOT_RAY_BLOCKED || hitSprite < 0 || !readOk) return ray;
  return attachment ? TBOT_RAY_ATTACHMENT : ray;
}

// EBARRIER (AADEFS.CON:2204) is spawned in front of its owner; BARRICADE is the Armored Lizard Trooper's cover. Both
// break at extra 0 and never regenerate.
const int EK_PICNUM_EBARRIER  = 16560;
const int EK_PICNUM_BARRICADE = 16544;
const int EK_SHIELD_PAL_RED   = 21;

inline bool ekm_picnum_is_shield(int picnum) {
  return picnum == EK_PICNUM_EBARRIER || picnum == EK_PICNUM_BARRICADE;
}

// Refines a BLOCKED ray that hit a live shield; kept out of ekm_ray_class so acquisition pays no read.
inline int ekm_ray_shield(int ray, int hitSprite, bool readOk, int picnum,
                          int extra) {
  if (ray != TBOT_RAY_BLOCKED || hitSprite < 0 || !readOk) return ray;
  return (ekm_picnum_is_shield(picnum) && extra > 0) ? TBOT_RAY_SHIELD : ray;
}

// Whose shield: AA's `myspawner` (`gamevar myspawner -1 2`, AADEFS.CON:3973), else sprite.owner (unproven).
const int EKM_SHIELD_OWNER_NONE = 0, EKM_SHIELD_OWNER_SPAWNER = 1,
          EKM_SHIELD_OWNER_SPRITE = 2;
inline int ekm_shield_owner(bool spawnerOk, int spawner, bool recordOk,
                            int spriteOwner, int* out) {
  if (spawnerOk) { if (out) *out = spawner; return EKM_SHIELD_OWNER_SPAWNER; }
  if (recordOk)  { if (out) *out = spriteOwner; return EKM_SHIELD_OWNER_SPRITE; }
  return EKM_SHIELD_OWNER_NONE;
}

// Shoot a shield only as a last resort: acquisition's winner was unreachable, the shield is its own and the weapon can
// hurt it.
const int TBOT_SHIELD_FIRE           = 0;
const int TBOT_SHIELD_HELD_REACHABLE = 1;
const int TBOT_SHIELD_HELD_UNREAD    = 2;
const int TBOT_SHIELD_HELD_OWNER     = 3;
const int TBOT_SHIELD_HELD_RED       = 4;
inline int tbot_shield_verdict(bool winnerReachable, bool ownerOk, int owner,
                               int lockedSprite, bool hitscanWeapon,
                               int shieldPal) {
  if (winnerReachable) return TBOT_SHIELD_HELD_REACHABLE;
  if (!ownerOk) return TBOT_SHIELD_HELD_UNREAD;
  if (lockedSprite < 0 || owner != lockedSprite) return TBOT_SHIELD_HELD_OWNER;
  if (!hitscanWeapon && shieldPal == EK_SHIELD_PAL_RED) return TBOT_SHIELD_HELD_RED;
  return TBOT_SHIELD_FIRE;
}

// Only widens a refusal into a hold, never narrows: earlier refusals pass through with their reason. UNKNOWN keeps the
// old behaviour. BREAKING holds the trigger (the first bullet opens a grate), so it is tested after BLOCKED.
inline TbotCond tbot_ray_cond(TbotCond base, int ray) {
  if (!base.holds) return base;
  // A shield with no verdict is still cover: only tbot_shield_cond opens it.
  if (ray == TBOT_RAY_BLOCKED || ray == TBOT_RAY_SHIELD) {
    TbotCond c = { 0, "blocked" };
    return c;
  }
  if (ray == TBOT_RAY_ATTACHMENT) { TbotCond c = { 0, "sidegun" }; return c; }
  if (ray == TBOT_RAY_BREAK)   { TbotCond c = { 1, "breaking" }; return c; }
  return base;
}

// A shield ray's condition; same widening discipline as above.
inline TbotCond tbot_shield_cond(TbotCond base, int verdict) {
  if (!base.holds) return base;
  TbotCond c = { 0, "shield-owner" };
  switch (verdict) {
    case TBOT_SHIELD_FIRE:           c.holds = 1; c.reason = "shield"; break;
    case TBOT_SHIELD_HELD_REACHABLE: c.reason = "shield-reachable"; break;
    case TBOT_SHIELD_HELD_UNREAD:    c.reason = "shield-unread"; break;
    case TBOT_SHIELD_HELD_RED:       c.reason = "shield-red"; break;
    default: break;
  }
  return c;
}

// Can the shot land at all? The quantum, not the height: pitch rounds to a whole horiz unit (P_PreFireHitscan), moving
// the impact range2d * EKM_Z_PER_XY / EKM_HORIZ_PER_SLOPE (3040-tall turret at 5873: 734 z vs margin 380; 39 rounds, 3
// hits). The rounding's leftover (F16(100) - q16horiz - q16horizoff) times the quantum must stay within marginZ.
inline bool ekm_shot_placeable(double marginZ, int range2d, int q16horiz,
                               int q16horizoff) {
  const double quantum = ekm_pitch_quantum_z(range2d);
  if (quantum <= 0.0) return true;
  // The ideal pitch in whole horiz units: what the engine is about to round.
  const double ideal = (double)q16horiz / 65536.0 + (double)q16horizoff / 65536.0;
  // Round half away from zero (as fix16_to_int does); no <math.h> because the test host pins this header.
  const double rounded = (ideal >= 0.0) ? (double)(long long)(ideal + 0.5)
                                        : (double)(long long)(ideal - 0.5);
  double leftover = ideal - rounded;
  if (leftover < 0.0) leftover = -leftover;
  return leftover * quantum <= marginZ;
}

// Same widening discipline as tbot_ray_cond; may only turn a hold into a refusal.
inline TbotCond tbot_placed_cond(TbotCond base, double marginZ, int range2d,
                                int q16horiz, int q16horizoff) {
  if (!base.holds) return base;
  if (!ekm_shot_placeable(marginZ, range2d, q16horiz, q16horizoff)) {
    TbotCond c = { 0, "noroom" };
    return c;
  }
  return base;
}

// Cover one bullet opens: a case in A_DamageObject_Duke3D (sector.cpp:1995-2492) clearing both blocking and hitscan
// bits on any damage: GRATE1 595, FANSPRITE 407, TOILET 569, STALL 571, CIRCLEPANNEL 1113, PANNEL1-3 342/343/4099.
// TREE1/2, TIRE, CONE, BOX, CACTUS need explosives or flame, so they are out. Walls: ekm_wall_breaks_open.
inline bool ekm_picnum_breaks_open(int picnum) {
  switch (picnum) {
    case 595:
    case 407:
    case 569:
    case 571:
    case 1113:
    case 342:
    case 343:
    case 4099:
      return true;
    default:
      return false;
  }
}

// Walls whose damage case clears blocking bits with no weapon test (A_DamageWall_Internal, sector.cpp:1570): GLASS 503,
// STAINGLASS1 510, FANSPRITE 407. Forcefields stay solid; the rest only change picnum. Reads overpicnum (masked walls).
// The neighbour-floor guard (:1623) is unanswerable here: a bullet into a pane's base is lost.
inline bool ekm_wall_breaks_open(int overpicnum, int cstat, int nextsector) {
  if (nextsector < 0) return false;
  if ((cstat & EK_WALL_CSTAT_MASKED) == 0) return false;
  switch (overpicnum) {
    case 503:
    case 510:
    case 407:
      return true;
    default:
      return false;
  }
}

// First refusal wins and resets the held clock, so a delay never survives a broken condition. Pause is ud.pause_on
// alone (screens.cpp:1300; gm stays MODE_GAME), so `paused` is its own term: else the delay charges while frozen.
inline bool tbot_decide(bool on, bool menuOpen, bool paused, bool dead, bool frozen,
                        bool panelOpen, bool weaponUsable, TbotCond cond,
                        int delayTics, int elapsedMs,
                        TbotState* st, const char** reason) {
  if (!st) { if (reason) *reason = "nostate"; return false; }

  const char* why = 0;
  if (!on)                why = "off";
  else if (menuOpen)      why = "menu";
  else if (paused)        why = "paused";
  else if (dead)          why = "dead";
  else if (frozen)        why = "frozen";
  else if (panelOpen)     why = "panel";
  else if (!weaponUsable) why = "weapon";
  else if (!cond.holds)   why = cond.reason ? cond.reason : "cond";
  if (why) {
    st->heldMs = 0; st->heldTics = 0; st->firing = 0;
    if (reason) *reason = why;
    return false;
  }

  int add = elapsedMs;
  if (add < 0) add = 0;
  if (add > TBOT_FRAME_MS_MAX) add = TBOT_FRAME_MS_MAX;
  if (st->heldMs < TBOT_HELD_MS_CAP) st->heldMs += add;

  st->heldTics = tbot_held_tics(st->heldMs);
  // Strictly greater: at delay 0 heldTics 1 fires at once; >= would fire a tic early elsewhere.
  st->firing = st->heldTics > delayTics ? 1 : 0;
  if (reason) *reason = st->firing ? "fire" : "delay";
  return st->firing != 0;
}

// --- tbot.cpp (DLL only): reads live engine memory and writes the input record ---

void tbot_frame(void);

// ekm_ray_class with the engine's answers. `admitted` is acquisition's per-pass bitmap (EK_MAXSPRITES bits) and answers
// "targetable" with no read; NULL (the triggerbot) asks targets_sprite_targetable. SwapBuffers hook only.
int tbot_ray_class_of(bool castOk, int hitSprite, int hitWall,
                      const unsigned char* admitted);

bool tbot_firing(void);

// Instrumentation only. Where the ray was cast from, so the FIRE instant can be diffed against it: the weapon fires
// during the tic (P_GetInput -> G_DrawRooms -> videoNextPage) and the cast happens at the swap after it. Same thread;
// published only with debuglog.
struct TbotCastRecord {
  int ox, oy, oz;
  int posZ;
  int pyoff;
  int pyoffOk;
  int osect;
  int q16ang, q16horiz, horizoff;
  // 1 when those two are silent aim's solution, not the player's view (A_ShootWithZvel's detour already wrote it)
  int usedSolution;
  int targetSprite;
  int range2d;
  int fired;
  unsigned ms;
};

bool tbot_cast_record(TbotCastRecord* out);
