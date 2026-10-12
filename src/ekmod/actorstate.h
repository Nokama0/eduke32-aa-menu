#pragma once

#include "esp_names.h"
#include "gamestate.h"

// actor_t actor[MAXSPRITES]: the state sprite[] lacks, queued damage (htextra) and the running CON action (t_data[4]).
// A direct .bss array (global.h:67), packed (actors.h:128-146, 96 bytes), fixed-width so x64 moves nothing.
const int EK_ACTOR_STRIDE = 96;

const int EK_ACT_T_DATA    = 0;
// t_data[1] is AC_MOVE_ID (actors.h:98), the move offset set by CON_AI/CON_MOVE (gameexec.cpp:3478, :3860).
const int EK_ACT_MOVE_ID   = EK_ACT_T_DATA + 1 * 4;
// t_data[3] is AC_CURFRAME (actors.h:100), one of four terms of the drawn tile (esp_drawntile.h).
const int EK_ACT_CURFRAME  = EK_ACT_T_DATA + 3 * 4;
// t_data[4] is AC_ACTION_ID (actors.h:103): an offset into apScript in intptr_t units (gameexec.cpp:6940-6944).
const int EK_ACT_ACTION_ID = 16;
// t_data[5] is AC_AI_ID (actors.h:104): zero until the actor has run a tic to an `ai` command (`ifai 0`).
const int EK_ACT_AI_ID     = 20;
const int EK_ACT_FLAGS     = 40;
// bpos: the sprite position at the START of this tic.
const int EK_ACT_BPOS_X    = 44;
const int EK_ACT_BPOS_Y    = 48;
const int EK_ACT_BPOS_Z    = 52;
const int EK_ACT_FLOORZ    = 56;
const int EK_ACT_CEILINGZ  = 60;
const int EK_ACT_LASTV_X   = 64;
const int EK_ACT_LASTV_Y   = 68;
const int EK_ACT_HTPICNUM  = 72;
const int EK_ACT_HTANG     = 74;
// htextra: damage QUEUED against this actor, not yet applied.
const int EK_ACT_HTEXTRA   = 76;
const int EK_ACT_HTOWNER   = 78;
const int EK_ACT_MOVFLAG   = 80;
const int EK_ACT_TEMPANG   = 82;
const int EK_ACT_TIMETOSLEEP = 84;
const int EK_ACT_STAYPUT   = 86;
// dispicnum is the tile the engine LAST DREW the sprite as (game.cpp:4536, inside G_DrawRooms :1148, before
// videoNextPage :6508), valid at our hook. 0 = not drawn since spawn; an unvisited sprite keeps its last tile.
const int EK_ACT_DISPICNUM = 92;

// G_RecordOldSpritePos (actors.cpp:9286-9331) sets bpos = xyz for EVERY sprite each G_MoveWorld, so sprites that did
// not move match exactly; a mover lags a tic (a miss, never a false accept). `htowner == i` is NOT an invariant.

// Live, and at a position zeroed memory cannot fake: both x and y nonzero.
inline bool ekm_actor_sample_usable(int statnum, int maxStatus, int x, int y) {
  return statnum >= 0 && statnum < maxStatus && x != 0 && y != 0;
}

const int EK_ACTOR_SAMPLES     = 128;
const int EK_ACTOR_MIN_SAMPLES = 32;

// Three quarters: movers lag a tic, non-movers outnumber them.
const int EK_ACTOR_MATCH_NUM = 3;
const int EK_ACTOR_MATCH_DEN = 4;

// The minimum-sample clause is load-bearing: 0 of 0 would accept zeroed .bss.
inline bool ekm_actor_scan_accept(int sampled, int matched) {
  if (sampled < EK_ACTOR_MIN_SAMPLES) return false;
  if (matched < 0 || matched > sampled) return false;
  return matched * EK_ACTOR_MATCH_DEN >= sampled * EK_ACTOR_MATCH_NUM;
}


// A sitting trooper has NO `ifhitweapon` (GAME.CON:4750-4777), so hits sit in htextra up to 24-30 tics until `cactor
// LIZTROOP`, while sprite.extra reads full. Only these picnums gate the handover on `ifcount`; others hand over on tic
// one (:4721-4748), so a general discount would retire live enemies.
const int EK_PICNUM_TROOP_ON_TOILET = 1741;
const int EK_PICNUM_TROOP_JUST_SIT  = 1742;

// AA sitters have stock shape (NEWTROOP.CON:2238-2315, :2035-2076), so the discount holds.
const int EK_PICNUM_NEWTROOP_ON_TOILET = 16993;
const int EK_PICNUM_NEWTROOP_JUST_SIT  = 16895;
const int EK_PICNUM_NIPTROOP_ON_TOILET = 24417;
const int EK_PICNUM_NIPTROOP_JUST_SIT  = 24439;
inline bool ekm_actor_defers_damage(int picnum) {
  return picnum == EK_PICNUM_TROOP_ON_TOILET || picnum == EK_PICNUM_TROOP_JUST_SIT ||
         picnum == EK_PICNUM_NEWTROOP_ON_TOILET || picnum == EK_PICNUM_NEWTROOP_JUST_SIT ||
         picnum == EK_PICNUM_NIPTROOP_ON_TOILET || picnum == EK_PICNUM_NIPTROOP_JUST_SIT;
}

// Instrumentation: a shot sitting trooper regained its ESP box for a few frames after leaving the toilet. Hypothesis:
// the discount is scoped to 1741/1742. Samples several frames EITHER SIDE of the change.
const int EK_PICNUM_TROOP_STANDING = 1680;
const int EK_PICNUM_NEWTROOP_STANDING = 16896;
const int EK_PICNUM_NIPTROOP_STANDING = 24320;

// Sitter family a tile belongs to, as the sticky flag stores it, so a recycled slot cannot widen the discount across
// families. STOCK is 1.
const int EKM_DEFER_NONE  = 0;
const int EKM_DEFER_STOCK = 1;
const int EKM_DEFER_NEWT  = 2;
const int EKM_DEFER_NIP   = 3;
inline int ekm_defer_family(int picnum) {
  switch (picnum) {
    case EK_PICNUM_TROOP_ON_TOILET: case EK_PICNUM_TROOP_JUST_SIT:
    case EK_PICNUM_TROOP_STANDING:
      return EKM_DEFER_STOCK;
    case EK_PICNUM_NEWTROOP_ON_TOILET: case EK_PICNUM_NEWTROOP_JUST_SIT:
    case EK_PICNUM_NEWTROOP_STANDING:
      return EKM_DEFER_NEWT;
    case EK_PICNUM_NIPTROOP_ON_TOILET: case EK_PICNUM_NIPTROOP_JUST_SIT:
    case EK_PICNUM_NIPTROOP_STANDING:
      return EKM_DEFER_NIP;
    default:
      return EKM_DEFER_NONE;
  }
}

// One direction: a sitter becomes a walker (GAME.CON:4750-4777), never back.
inline bool ekm_toilet_transition(int oldPicnum, int newPicnum) {
  return ekm_actor_defers_damage(oldPicnum) && !ekm_actor_defers_damage(newPicnum) &&
         ekm_defer_family(newPicnum) == ekm_defer_family(oldPicnum);
}

const int EK_TOILET_TRACE_SLOTS  = 4;
const int EK_TOILET_TRACE_BEFORE = 4;
const int EK_TOILET_TRACE_AFTER  = 8;
const int EK_TOILET_TRACE_LINES  = 260;

// Oldest-first walk of the before-ring; -1 for anything unanswerable, never slot 0.
inline int ekm_ring_oldest(int head, int count, int n, int k) {
  if (n <= 0 || count <= 0 || count > n) return -1;
  if (k < 0 || k >= count) return -1;
  int idx = (head - count + k) % n;
  if (idx < 0) idx += n;
  return idx;
}

// Nothing pending is -1, not 0 (A_IncurDamage, actors.cpp:1305-1327; game.cpp:1389, :1485); zero is a pending hit for
// no damage, so the rule is `pending > 0`.
const int EK_HTEXTRA_NONE = -1;

// Health the engine will have a moment from now. Caveat: htextra holds damage of ANY kind and a freeze blast is handled
// by `ifwasweapon FREEZEBLAST` (GAME.CON:4372-4379), so a frozen sitter can read dead. The deferral follows the ACTOR,
// not its picnum (after `cactor LIZTROOP` a picnum-keyed discount stopped), via a per-sprite flag.
inline bool ekm_actor_discounts_pending(int picnum, int sticky) {
  return ekm_actor_defers_damage(picnum) ||
         (sticky != EKM_DEFER_NONE && sticky == ekm_defer_family(picnum));
}

// The flag after one observation: (1) identity: deletesprite (engine.cpp:8825-8839) keeps picnum at MAXSTATUS, so
// another tile means a different occupant; (2) a failed read guesses neither way; (3) queue cleared; (4) set ONLY by a
// sitter. A new level clears it (game.cpp:1389, :1485).
inline int ekm_defer_sticky_next(int prev, int picnum, int statnum,
                                 int pendingKnown, int pending) {
  if (statnum < 0 || statnum >= EK_MAXSTATUS) return 0;
  const int family = ekm_defer_family(picnum);
  if (family == EKM_DEFER_NONE) return 0;
  const int carried = prev == family ? family : 0;
  if (!pendingKnown) return carried;
  if (pending <= 0) return 0;
  if (ekm_actor_defers_damage(picnum)) return family;
  return carried;
}

inline int ekm_actor_effective_extra(int picnum, int extra, int pending,
                                     int sticky = 0) {
  if (!ekm_actor_discounts_pending(picnum, sticky) || pending <= 0) return extra;
  const int left = extra - pending;
  return left > 0 ? left : 0;
}


// DISPLAY ONLY: the same arithmetic for EVERY enemy for the number and bar. It must never reach targeting: every actor
// lags a tic before its `ifhitweapon`. A negative `extra` passes untouched.
inline int ekm_actor_shown_extra(int extra, int pending) {
  if (pending <= 0) return extra;
  const int left = extra - pending;
  return left > 0 ? left : 0;
}


const int EK_ACTION_UNIT = 8;

// Six stored params, not the five an `action` line declares (actors.h:106-114); the compiler pads zeros
// (gamedef.cpp:3183-3188).
const int EK_ACTION_PARAMS = 6;

// Stride between declarations is 8 (CON_ACTION, six params, CON_END; gamedef.cpp:3150-3190), so `t4 +/- stride` are the
// neighbours.
const int EK_ACTION_STRIDE = 8;

// int flags, not bool: MSVC 19.50 /O2 loses a bool member of a struct returned by value.
struct EkAction {
  int startframe, numframes, viewtype, incval, delay;
  int readable;
  int plausible;
};

// Does the decoder read a run of action declarations (not "is this legal"; all zeros is legal). Bounds from this game
// GAME.CON, all 402 declarations: viewtype {1,4,5,7}, numframes 1..20, startframe -10..395.
inline bool ekm_action_plausible(int startframe, int numframes, int viewtype) {
  const bool viewtypeOk = viewtype == 1 || viewtype == 4 || viewtype == 5 || viewtype == 7;
  const bool numframesOk = numframes >= 1 && numframes <= 64;
  const bool startframeOk = startframe >= -64 && startframe <= 1023;
  return viewtypeOk && numframesOk && startframeOk;
}

// Never AND plausibility into a record test: the World Tour fork did and silently retired its playing-dead indicator,
// whose record (54,0,0,0,0) is deliberately implausible (tb_0044).
inline bool ekm_action_is(const EkAction& a, int sf, int nf, int vt, int iv, int dl) {
  return a.readable != 0 && a.startframe == sf && a.numframes == nf &&
         a.viewtype == vt && a.incval == iv && a.delay == dl;
}

// Index 0 means "no opinion" (actionless actors; gameexec.cpp:6944), and below one stride there is no predecessor.
inline bool ekm_action_id_usable(int t4) {
  return t4 >= EK_ACTION_STRIDE;
}

// Wider than the name: AA troopers are NEWTROOP* and NIPTROOP* (AADEFS.CON:2239-2247, :2933-2940).
// ekm_picnum_is_trooper stays stock-only because it also NAMES them.
inline bool ekm_picnum_is_trooper_family(int picnum) {
  if (ekm_picnum_is_trooper(picnum)) return true;
  switch (picnum) {
    case 16896: case 16897: case 16898: case 16901:
    case 16926: case 16931: case 16993: case 16895:
    case 24320: case 24321: case 24322: case 24325:
    case 24350: case 24355: case 24417: case 24439:
      return true;
    default:
      return false;
  }
}

// Trooper down-states (GAME.CON:4018-4022; AA NEWTROOP.CON:29-33). Records: docs/re/actorstate.md.
inline bool ekm_action_is_suffering(const EkAction& a) {
  return ekm_action_is(a, 59, 2, 1, 1, 21) || ekm_action_is(a, 99, 2, 1, 1, 21);
}

// Three answers: "cannot tell" must not read as "not suffering".
const int EKM_SUFFER_UNKNOWN = 0;
const int EKM_SUFFER_YES     = 1;
const int EKM_SUFFER_NO      = -1;

// A play-dead trooper and a real corpse are identical in every sprite field and ATROOPDEAD/ATROOPPLAYDEAD records are
// identical; separable only by POSITION: a down record is playing dead iff its predecessor is also down and its
// successor is SUFFERDEAD.
inline bool ekm_action_is_playdead(const EkAction& self, const EkAction& before,
                                   const EkAction& after) {
  const bool stock = ekm_action_is(self, 54, 0, 0, 0, 0) &&
                     ekm_action_is(before, 54, 0, 0, 0, 0) &&
                     ekm_action_is(after, 58, 2, 1, -4, 24);
  const bool aa = ekm_action_is(self, 106, 0, 0, 0, 0) &&
                  ekm_action_is(before, 106, 0, 0, 0, 0) &&
                  ekm_action_is(after, 98, 1, 1, 1, 30);
  return stock || aa;
}

// The action record outranks `extra`, which moves only on the victim own `ifhitweapon` tic. Three answers, as
// EKM_SUFFER_*.
const int EKM_DYING_UNKNOWN = 0;
const int EKM_DYING_YES     = 1;
const int EKM_DYING_NO      = -1;

// COMMANDERSTAYPUT (1921) runs `cactor COMMANDER` on tic one (GAME.CON:7208-7211).
const int EK_PICNUM_COMMANDER = 1920;

// Dying records from this build GAME.CON (values: docs/re/actorstate.md). The 54-records are left out: ATROOPDEAD and
// ATROOPPLAYDEAD are bit-identical and would call a play-dead trooper dying. ANEWTSUFFERDEAD is out since both paths
// set `strength 0` and `cstat 0` first.
inline bool ekm_action_is_troop_dying(const EkAction& a) {
  return ekm_action_is(a, 50, 5, 1, 1, 16) || ekm_action_is(a, 58, 2, 1, -4, 24) ||
         ekm_action_is(a, 101, 6, 1, 1, 16);
}
// A dying commander is hittable ~3 s with health flickering (`cstat 0` only at `ifactioncount 8`, GAME.CON:7372-7392);
// a health threshold hid two commanders still being fought.
inline bool ekm_action_is_commander_dying(const EkAction& a) {
  return ekm_action_is(a, 30, 8, 1, 1, 12) || ekm_action_is(a, 38, 1, 1, 1, 1);
}

// The picnum scope is the safety argument: AOCTADEAD shares ACOMMDEAD five values. No plausibility gate.
inline bool ekm_action_is_dying(int picnum, const EkAction& a) {
  if (ekm_picnum_is_trooper_family(picnum)) return ekm_action_is_troop_dying(a);
  if (picnum == EK_PICNUM_COMMANDER) return ekm_action_is_commander_dying(a);
  return false;
}

// Crashing Recon: `ifdead` sets `strength 1` (VEHICLES.CON:1661-1667, :1715-1718), so only its move says it is dying.
const int EK_PICNUM_NEWRECON = 16865;

// `move RECONDYINGVELS 256 12` (VEHICLES.CON:1649); no other AA move declares it.
const int EK_RECON_DYING_HVEL = 256;
const int EK_RECON_DYING_VVEL = 12;

// Zero is "no move" (gameexec.cpp:743); a declared labelcode is >= 1 (gamedef.cpp:2956-2984).
inline bool ekm_move_id_usable(int t1) { return t1 >= 1; }

// Picnum-scoped; `haveMove` 0 (any failed read) is never crashing.
inline bool ekm_actor_crashing_aa(int picnum, int haveMove, int hvel, int vvel) {
  return picnum == EK_PICNUM_NEWRECON && haveMove != 0 &&
         hvel == EK_RECON_DYING_HVEL && vvel == EK_RECON_DYING_VVEL;
}

// Pig Cop SUV driving on dead: `ai AISUVDYING` (VEHICLES.CON:965-969); predamage resets negative extra to 1
// (AAGAME.CON:2048-2050).
const int EK_PICNUM_PIGSUV = 6374;

// `ai AISUVDYING APIGSUVDYING PIGSUVSPEED2 geth` (:19), 448 (:8); a live SUV has the same values, so they only confirm.
const int EK_SUV_DYING_HVEL = 448;
const int EK_SUV_DYING_VVEL = 0;

// The ai as the engine keeps it: apScript[ai], [ai+1] are action and move ids (gameexec.cpp:3474-3479).
struct EkAiRead {
  int aiId, actionId;
  int aiActionId, aiMoveId;
  EkAction aiAction;
  int hvel, vvel;
  int readable;
};

// Zero is "no ai yet"; a declared labelcode is >= 1.
inline bool ekm_ai_id_usable(int t5) { return t5 >= 1; }

// VEHICLES.CON:969 is the only place AISUVDYING is set; `actionId == aiActionId` compares ADDRESSES. A failed read is
// never dying.
inline bool ekm_actor_suv_dying_aa(int picnum, const EkAiRead& a) {
  return picnum == EK_PICNUM_PIGSUV && a.readable != 0 && ekm_ai_id_usable(a.aiId) &&
         a.actionId == a.aiActionId && ekm_action_is(a.aiAction, 0, 1, 5, 1, 10) &&
         a.hvel == EK_SUV_DYING_HVEL && a.vvel == EK_SUV_DYING_VVEL;
}

// A dying Dukinator spawns TERMTORSO armed with `countvarc YES` (TERMINATOR.CON:369-371), then TERMINATORCRAWLING
// (:1348), which explodes (:1181-1203); both tiles count.
const int EK_PICNUM_TERMTORSO          = 6227;
const int EK_PICNUM_TERMINATORCRAWLING = 6194;
const int EK_AA_YES = 1;

inline bool ekm_picnum_is_term_torso(int picnum) {
  return picnum == EK_PICNUM_TERMTORSO || picnum == EK_PICNUM_TERMINATORCRAWLING;
}

// A failed read is never self-destructing.
inline bool ekm_torso_self_destructing(int picnum, bool gotCountvarc, int countvarc) {
  return ekm_picnum_is_term_torso(picnum) && gotCountvarc && countvarc == EK_AA_YES;
}

// A CHOKING trooper is shown and targeted; PLAYING-DEAD, CRASHING (Recon, SUV), SELF-DESTRUCTING torso and a GROWING
// Polymorph pad (aa_polypad.h) are shown, labelled and never shot; of the worm states only STUNIT and VULNERABLE count
// hits.
enum EkActorState { EKA_ALIVE = 0, EKA_CHOKING, EKA_PLAYDEAD, EKA_DOWN, EKA_CRASHING,
                    EKA_SELFDESTRUCT, EKA_WORM_STUNIT, EKA_WORM_STUNNED,
                    EKA_WORM_VULNERABLE, EKA_WORM_RECOVERING, EKA_WORM_SHIELDED,
                    EKA_GROWING };

inline bool ekm_actor_state_shown(int s) { return s != EKA_DOWN; }
inline bool ekm_actor_state_targetable(int s) {
  return s == EKA_ALIVE || s == EKA_CHOKING || s == EKA_WORM_STUNIT ||
         s == EKA_WORM_VULNERABLE;
}
inline bool ekm_actor_state_worm_refused(int s) {
  return s == EKA_WORM_STUNNED || s == EKA_WORM_RECOVERING || s == EKA_WORM_SHIELDED;
}
// Null for the two states that have an honest number to print instead.
inline const char* ekm_actor_state_label(int s) {
  switch (s) {
    case EKA_CHOKING:  return "Choking";
    case EKA_PLAYDEAD: return "Playing dead";
    case EKA_CRASHING: return "Crashing";
    case EKA_SELFDESTRUCT: return "Self-destructing";
    case EKA_WORM_STUNIT:     return "Stun it";
    case EKA_WORM_STUNNED:    return "Stunned";
    case EKA_WORM_VULNERABLE: return "Vulnerable";
    case EKA_WORM_RECOVERING: return "Recovering";
    case EKA_WORM_SHIELDED:   return "Shielded";
    case EKA_GROWING:         return "Growing";
    default:           return 0;
  }
}

// DLL only: everything below reads live engine memory.

// Find actor[] by content; needs a level and the sprite array, so retried like the sprite scan. Logs every attempt.
bool actorstate_scan(void);
bool actorstate_ready(void);

unsigned actorstate_rva(void);

const unsigned char* actorstate_record(int i);

// False is "nothing known", never zero pending.
bool actorstate_pending_damage(int i, int* out);

// Has this actor run its first tic (`ifai 0` overrides)? Display only; a refusal answers "has run", the safe way round.
bool actorstate_has_run(int i);

bool actorstate_script_scan(void);
bool actorstate_script_ready(void);
// apScript and the end of its allocation (AA31 scans the words between).
bool actorstate_script_span(const unsigned char** base, const unsigned char** end);

// False whenever anything is unknown: "cannot tell" must read as an ordinary corpse, the safe direction to be wrong in.
bool actorstate_is_playing_dead(int i, int picnum);

// One of the three EKM_SUFFER_* answers; UNKNOWN is not "no", so a caller that cannot tell keeps its behaviour.
// Troopers only.
int actorstate_suffering(int i, int picnum);

// One of the three EKM_DYING_* answers for the sprite's OWN family; a failed decode leaves the verdict the sprite
// fields would give.
int actorstate_dying_record(int i, int picnum);

// dispicnum and t_data[3] from the ONE record in one guarded read (read together every item every frame). False means
// nothing was read and leaves both untouched: "no answer", not "never drawn" (EKM_DISPICNUM_UNREADABLE vs 0,
// esp_drawntile.h).
bool actorstate_drawn_tile_inputs(int i, int* dispicnum, int* curFrame);

// The raw action id and its decoded record, for the trace alone. False leaves `*out` zeroed and unreadable.
bool actorstate_action_at(int i, int* t4, EkAction* out);

// Move id (t_data[1]) and the record's two velocities. False is "unknown", never "crashing".
bool actorstate_move_at(int i, int* moveId, int* hvel, int* vvel);

// Ai (t_data[5]), its action id, and the ai record's action and move decoded. False with `readable` 0 on any failed
// read; ekm_actor_suv_dying_aa reads that as alive.
bool actorstate_ai_at(int i, EkAiRead* out);
