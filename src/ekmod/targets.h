#pragma once

#include "esp_names.h"
#include "actorstate.h"
#include "gamevars.h"
#include "aa_notenemy.h"
#include "aa_worm.h"
#include "aa_polypad.h"
#include "aa_companion.h"

// The one list of things that can be shot, shared by the ESP and the aimbot so a filter fix cannot land in one and
// not the other. See targets.cpp's header.

const int EK_TARGET_MAX = 256;

// The friendly list's own ceiling, a second budget rather than a share of the first: one cap spent by both would let
// a crowd of civilians push enemies out of the list the aimbot reads.
const int EK_FRIENDLY_MAX = EK_TARGET_MAX;

// A FULL LIST KEEPS THE NEAREST, not the lowest sprite numbers (one AA level holds ~485 enemy-like sprites): 2D
// squared distance, in 64-bit so a map-wide dx*dx cannot overflow.
inline long long ekm_target_dist2(int dx, int dy) {
  return (long long)dx * dx + (long long)dy * dy;
}

inline int ekm_target_farthest(const long long* dist2, int n) {
  int best = -1;
  for (int i = 0; i < n; i++)
    if (best < 0 || dist2[i] > dist2[best]) best = i;
  return best;
}

// Where a candidate goes in a list holding n of cap: n while there is room, the farthest entry's slot only when
// strictly nearer (a tie keeps the incumbent), else -1 (dropped).
inline int ekm_target_insert_slot(const long long* dist2, int n, int cap,
                                  int farthest, long long d2) {
  if (n < cap) return n;
  if (cap <= 0 || !dist2 || farthest < 0 || farthest >= n) return -1;
  return d2 < dist2[farthest] ? farthest : -1;
}

struct EkTarget {
  int   sprite;
  int   x, y, z;
  short sect;
  short hp;
  // WHAT THE ESP PRINTS, AND NOTHING ELSE: hp less the damage already queued in actor[].htextra, so the number and bar
  // move during a pain animation. No targeting rule may read this (ekm_actor_shown_extra).
  short hpShown;
  short picnum;
  int   feetZ;
  float heightZ;
  unsigned char xrep, yrep;
  unsigned short cstat;
  unsigned char pal;
  int   tw, th;
  // EKA_PLAYDEAD, EKA_CRASHING and EKA_SELFDESTRUCT are in this list and are NOT targets: both gates refuse any
  // drawn-but-not-targetable state, and the ESP labels them. So are the worm's three refused states and a growing pad.
  int   state;
  // AA's two per-actor answers, CARRIED RATHER THAN RE-READ: guarded reads can start or stop answering between two
  // calls, so they are read once here. `initHp` (AA's `inithp`) is the REAL maximum health, 0 = no answer. `level`
  // (`mlevel`) needs its own flag because its default is 1, not 0.
  int   initHp;
  int   level;
  bool  haveLevel;
  bool  haveInitHp;
  // AA's `monstatus`, carried the same way, and it DECIDES a refusal: aim.cpp refuses a monstatus -1 actor, whose CON
  // body breaks before `ifhitweapon` (NEWTROOP.CON:2012). `haveMonstatus` is its own flag: 0 is a real value.
  int   monstatus;
  bool  haveMonstatus;
  bool  asleepDiscardsHits;
  short statnum;
  bool  attachment;
};

const int EKM_PAL_FROZEN = 1;

// The AIMBOT'S rule, not the ESP's: what can be SHOT needs the hitscan-sensitive cstat bit (BIT 8 ALONE: the sprite
// clip mask is 256, clip.cpp:2849/2671; bit 0 is movement collision) and no invisible bit (15); what should be DRAWN
// does not. xrepeat 0 is not drawn.
inline bool ekm_target_hittable(unsigned char xrepeat, unsigned cstat) {
  return xrepeat != 0 && (cstat & 0x8100u) == 0x100u;
}

// `monstatus -1` on an AWAKE actor means it exits before its own hit handling (`ife monstatus -1 break`), so a shot
// is wasted. Scoped to STAT_ACTOR: AA sets -1 on EVERY enemy at spawn, and dormant map-placed sprites never reach
// the break. Targeting only; a FAILED READ LEAVES THE TARGET SHOOTABLE.
inline bool ekm_target_monstatus_shootable(bool gotMonstatus, int monstatus,
                                           int statnum) {
  return !gotMonstatus || monstatus != -1 || statnum != EK_STAT_ACTOR;
}

// ASLEEP, AND NOT A FLAGGED BADGUY, SO EVERY HIT IS DISCARDED: the engine wakes a STAT_ZOMBIEACTOR sprite on a hit
// only inside A_CheckEnemySprite's branch (sector.cpp:2450-2459). Targeting only; with no badguy table the sprite
// stays shootable.
inline bool ekm_target_asleep_discards_hits(int statnum, bool badguyReady,
                                            bool flagged) {
  return statnum == EK_STAT_ZOMBIE && badguyReady && !flagged;
}

// A TROOPER CHOKING ON GAS IS ALIVE AT ZERO HEALTH; the picnum scope is the safety argument (Enforcer, Octabrain and
// Protector Drone corpses also run strength 0 with cstat still 257). Needs exactly zero and the suffering action (not
// ATROOPDYING), no sitting picnum (1741, 1742), and not frozen (pal 1). EKM_SUFFER_UNKNOWN keeps the old behaviour.
inline bool ekm_trooper_choking(int picnum, int extra, int pal,
                                unsigned cstat, unsigned char xrepeat,
                                int suffering = EKM_SUFFER_UNKNOWN) {
  if (suffering == EKM_SUFFER_NO) return false;
  return extra == 0 && pal != EKM_PAL_FROZEN
      && ekm_picnum_is_trooper_family(picnum)
      && !ekm_actor_defers_damage(picnum)
      && ekm_target_hittable(xrepeat, cstat);
}

// DYING OR DEAD: not a target. An UNSCOPED `extra == 0 && !hittable` term made Enforcer, Octabrain and Protector Drone
// corpses permanent targets; keep the scoped form. Damage is deferred (A_DamageObject queues into actor[].htextra), so
// about 33 ms of lingering is a known floor.
inline bool ekm_target_dying(int extra, int pal, int picnum,
                             unsigned cstat, unsigned char xrepeat,
                             int suffering = EKM_SUFFER_UNKNOWN) {
  if (ekm_trooper_choking(picnum, extra, pal, cstat, xrepeat, suffering)) return false;
  return extra <= 0 && pal != EKM_PAL_FROZEN;
}

// ONE STATE PER SPRITE, AND THE ORDER OF THE TESTS IS THE RULE. playDead OUTRANKS EVERYTHING (a trooper playing dead
// has cleared its cstat at zero health): shown with a label, NOT targeted. Choking is shown and targeted; anything the
// dying rule refuses is EKA_DOWN and not drawn.
inline int ekm_worm_actor_state(int wormState) {
  switch (wormState) {
    case EKM_WORM_VULNERABLE: return EKA_WORM_VULNERABLE;
    case EKM_WORM_STUN_IT:    return EKA_WORM_STUNIT;
    case EKM_WORM_STUNNED:    return EKA_WORM_STUNNED;
    case EKM_WORM_RECOVERING: return EKA_WORM_RECOVERING;
    case EKM_WORM_SHIELDED:   return EKA_WORM_SHIELDED;
    default:                  return EKA_ALIVE;
  }
}

inline int ekm_actor_state(int picnum, int extra, int pal, unsigned cstat,
                           unsigned char xrepeat, bool playDead,
                           int suffering = EKM_SUFFER_UNKNOWN,
                           int dyingRecord = EKM_DYING_UNKNOWN,
                           bool crashing = false,
                           bool selfDestruct = false,
                           int wormState = EKM_WORM_NONE,
                           bool growing = false) {
  if (playDead) return EKA_PLAYDEAD;
  if (crashing) return EKA_CRASHING;
  // An armed torso explodes even if killed first (TERMINATOR.CON:1268-1273), so it outranks the death terms.
  if (selfDestruct) return EKA_SELFDESTRUCT;
  // THE DEATH RECORD OUTRANKS THE HEALTH NUMBER: below the dying rule and above every verdict that reads health >= 0
  // as alive. EKM_DYING_UNKNOWN is the default degrade.
  if (dyingRecord == EKM_DYING_YES) return EKA_DOWN;
  if (ekm_trooper_choking(picnum, extra, pal, cstat, xrepeat, suffering)) return EKA_CHOKING;
  if (ekm_target_dying(extra, pal, picnum, cstat, xrepeat, suffering)) return EKA_DOWN;
  if (growing) return EKA_GROWING;
  return ekm_worm_actor_state(wormState);
}

// A FRIENDLY IS NOT A TARGET, AND "NO ANSWER" IS NOT A FRIENDLY: gamevars_team answers false for stock Duke, World
// Tour, a CON with no `team` var and any failed read, and the verdict there must stay hostile.
inline bool ekm_target_is_friendly(bool haveTeam, int team) {
  return haveTeam && ekm_team_is_friendly(team);
}

// THE PICNUM-LEVEL SEQUENCE, written once because it is run twice (targets_collect, targets_sprite_targetable): a
// drifting order turns a target into a non-target. `haveTeam`/`team` come LAST so the team read can be skipped
// (ekm_target_refusal_awaits_team).
enum { EKM_REFUSE_NONE = 0, EKM_REFUSE_NOTENEMY = 1,
       EKM_REFUSE_CRITTER = 2, EKM_REFUSE_FRIENDLY = 3,
       EKM_REFUSE_ATTACHMENT = 4 };

// AA's Sidegun dies when its host dies (ROLLYTURRET.CON:646-651), so a shot at it is one the host should take.
// Actions animate the tsprite only, so sprite.picnum stays 16510.
const int EK_PICNUM_SIDEGUN = 16510;
inline bool ekm_picnum_is_attachment(int picnum) {
  return picnum == EK_PICNUM_SIDEGUN;
}

inline bool ekm_target_refusal_awaits_team(int refusal) {
  return refusal == EKM_REFUSE_NONE || refusal == EKM_REFUSE_ATTACHMENT;
}

inline bool ekm_target_refusal_hostile_listed(int refusal) {
  return refusal == EKM_REFUSE_NONE || refusal == EKM_REFUSE_ATTACHMENT;
}

inline int ekm_target_picnum_refusal(bool badguyReady, bool enemyTile,
                                     bool critter, bool haveTeam, int team) {
  if (badguyReady && !enemyTile) return EKM_REFUSE_NOTENEMY;
  if (critter) return EKM_REFUSE_CRITTER;
  if (ekm_target_is_friendly(haveTeam, team)) return EKM_REFUSE_FRIENDLY;
  return EKM_REFUSE_NONE;
}

// The form both sites call, so AA's unflagged hostiles are admitted in one place. `flagged` is picnum_is_enemy's
// answer; the critter and team terms still run after admission (that refuses the player's own turret).
inline int ekm_target_picnum_refusal(int picnum, bool badguyReady, bool flagged,
                                     bool critter, bool haveTeam, int team,
                                     bool gotMonstflags = false, int monstflags = 0,
                                     bool gotMonstatus = false, int monstatus = 0,
                                     bool gotInitflags = false, int initflags = 0) {
  int refusal =
      ekm_target_picnum_refusal(badguyReady,
                                flagged || ekm_aa_hostile_notenemy(picnum) ||
                                    ekm_worm_admitted(picnum),
                                critter, haveTeam, team);
  if (refusal == EKM_REFUSE_NONE &&
      (ekm_aa_companion_bot(picnum, gotMonstflags, monstflags, haveTeam, team,
                            gotMonstatus, monstatus) ||
       ekm_aa_mando_mannequin(picnum, gotInitflags, initflags, haveTeam, team)))
    refusal = EKM_REFUSE_FRIENDLY;
  if (refusal == EKM_REFUSE_NONE && ekm_picnum_is_attachment(picnum))
    return EKM_REFUSE_ATTACHMENT;
  return refusal;
}

// AN SSLIME CORPSE READS ALIVE TO EVERY SPRITE TEST: its dead actions re-arm strength 30 with cstat 256
// (SSLIME.CON:164, :169), so only AA's monstatus 2 marks it. The Pig Cop SUV's wreck too (VEHICLES.CON:659, :680).
inline int ekm_aa_notenemy_dying_record(int picnum, bool haveMonstatus,
                                        int monstatus) {
  return (ekm_aa_hostile_notenemy(picnum) || picnum == EK_PICNUM_PIGSUV) && haveMonstatus &&
                 monstatus == EKM_AA_MONSTATUS_DEAD
             ? EKM_DYING_YES
             : EKM_DYING_UNKNOWN;
}

// Fills out[0..max-1] with the live, drawable, hostile sprites; returns the count. No range or visibility test.
// "The aimbot never shoots a friendly" is a property of what this list CONTAINS, not a filter at three call sites.
int targets_collect(EkTarget* out, int max);

// The same walk, filling a SECOND array with the friendlies it passes over. `friendlyCount` is an out-param: give it
// a statement of its own before it is read.
int targets_collect_split(EkTarget* out, int max,
                          EkTarget* friendlyOut, int friendlyMax,
                          int* friendlyCount);

// How many sprites each collection term has turned down, formatted into `buf`. Instrumentation, advanced only while
// the Debug log row is on.
void targets_refusal_summary(char* buf, int n);

void targets_note_monstatus_refusal(void);

void targets_note_asleep_refusal(void);

void targets_note_selfdestruct_refusal(void);

void targets_note_worm_refusal(void);

void targets_note_attachment_refusal(void);

// Is this one sprite worth a bullet right now? The triggerbot's question, asked about the sprite the ENGINE named
// (hitscan's `hit.sprite`), so no list lookup. Includes the aimbot's hittable rule. False for anything unreadable,
// which holds fire.
bool targets_sprite_targetable(int sprite);

bool targets_sprite_picnum(int sprite, int* out);
