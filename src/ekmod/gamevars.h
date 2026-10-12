#pragma once

#include <stdint.h>
#include "gamestate.h"

// EDuke32's CON gamevar registry and AA's per-actor `team`/`mtype`: AA reuses the engine's badguy AI for allies and
// civilians, so A_CheckEnemyTile is true for them and a CON gamevar is the discriminator. gamevar_t members are
// pointer-sized, so the stride is 32 ON X64 (x86: 16, szLabel at +12): the wrong stride reads the NEXT label.
const int EK_GV_STRIDE  = 32;
const int EK_GV_GLOBAL  = 0;
const int EK_GV_DEFAULT = 8;
const int EK_GV_FLAGS   = 16;
const int EK_GV_LABEL   = 24;

const int EK_MAXGAMEVARS = 2048;
const int EK_MAXVARLABEL = 26;

const unsigned EK_GAMEVAR_PERPLAYER = 0x00000001u;
const unsigned EK_GAMEVAR_PERACTOR  = 0x00000002u;
const unsigned EK_GAMEVAR_RESET     = 0x00000008u;
const unsigned EK_GAMEVAR_SYSTEM    = 0x00000800u;
const unsigned EK_GAMEVAR_READONLY  = 0x00001000u;
const unsigned EK_GAMEVAR_INT32PTR  = 0x00002000u;
const unsigned EK_GAMEVAR_INT16PTR  = 0x00008000u;
const unsigned EK_GAMEVAR_NORESET   = 0x00020000u;
const unsigned EK_GAMEVAR_SPECIAL   = 0x00040000u;
const unsigned EK_GAMEVAR_Q16PTR    = 0x00100000u;

// THE THREE FLAGS THAT SAY +0 IS A POINTER (GAMEVAR_PTR_MASK). COOP holds &ud.coop, so a global read ignoring them
// would print an address. The engine's mask has a fourth term (RAWQ16PTR) left out on purpose: the nineteen SPECIAL
// anchor vars hold VALUES, so folding it in would refuse the anchor.
const uintptr_t EK_GV_PTR_MASK = (uintptr_t)EK_GAMEVAR_INT32PTR |
                                 (uintptr_t)EK_GAMEVAR_INT16PTR |
                                 (uintptr_t)EK_GAMEVAR_Q16PTR;

const uintptr_t EK_GV_WANT_GLOBAL = 0;

// A fourth: the POINTER-TYPED GLOBAL (e.g. viewingrange, GAMEVAR_INT32PTR) holds the ADDRESS at +0, a double
// indirection that must never reach the plain global read (the COOP hazard). This want IS the engine's own bit.
const uintptr_t EK_GV_WANT_INT32PTR = (uintptr_t)EK_GAMEVAR_INT32PTR;

// Which shape a cached index may still be read as, from the record's flags at READ time: a CON re-parse reassigns
// indices, and reading a global the per-player way (or the reverse) returns an address or garbage. The INT32PTR arm
// also tests the ABSENCE of the indexed bits.
inline bool ekm_gv_flags_accept(uintptr_t flags, uintptr_t want) {
  if (want == EK_GV_WANT_GLOBAL)
    return (flags & ((uintptr_t)EK_GAMEVAR_PERPLAYER |
                     (uintptr_t)EK_GAMEVAR_PERACTOR | EK_GV_PTR_MASK)) == 0;
  if (want == EK_GV_WANT_INT32PTR)
    return (flags & ((uintptr_t)EK_GAMEVAR_PERPLAYER |
                     (uintptr_t)EK_GAMEVAR_PERACTOR)) == 0 &&
           (flags & EK_GV_WANT_INT32PTR) != 0;
  return (flags & want) != 0;
}

// THE ANCHOR: Gv_AddSystemVars registers nineteen SPECIAL vars first, each with the same flags word and default -1
// that Gv_NewVar also stores into `global`: three KNOWN 8-byte words per record, only szLabel varies.
const unsigned EK_GV_SPECIAL_FLAGS =
    EK_GAMEVAR_READONLY | EK_GAMEVAR_SYSTEM | EK_GAMEVAR_SPECIAL;
const int EK_GV_SPECIAL_COUNT = 19;

bool gamevars_scan(void);
bool gamevars_ready(void);

unsigned gamevars_rva(void);

int gamevars_walked(void);

int gamevars_vq_calls(void);

int gamevars_team_index(void);

// ONE MECHANISM, FOUR PER-ACTOR VARS (team, mtype, mlevel, inithp; AADEFS.CON:3830, :3977, :4022, :4078): each name
// is resolved once by the label walk, ONE guarded read serves all. False means no answer. Give each call a statement
// of its own: an out-param read in the argument list that fills it is indeterminately sequenced (MSVC gets it wrong).
bool gamevars_team(int sprite, int* outTeam);
bool gamevars_mtype(int sprite, int* outMtype);
bool gamevars_inithp(int sprite, int* outInitHp);
bool gamevars_mlevel(int sprite, int* outLevel);
// AA's `droptile`: the picnum THIS sprite spawns when it dies, filled at map load for breakables
// (AAPRELOAD.CON:1562). Scratch in non-breakables, so meaningless on an arbitrary sprite.
bool gamevars_droptile(int sprite, int* outDropTile);
bool gamevars_monstatus(int sprite, int* outMonStatus);

// AA's `bottarget` (AADEFS.CON:3975): the sprite AA's Aimbot Goggles lock onto, set in APLAYER's context so it is
// read at the PLAYER'S SPRITE index. -1 is "no lock".
bool gamevars_bottarget(int sprite, int* outTarget);

// AA's `countvarc` (AADEFS.CON:3962), a scratch var; YES on a Dukinator torso arms its self-destruct.
bool gamevars_countvarc(int sprite, int* outCountvarc);

bool gamevars_countvar(int sprite, int* outCountvar);
bool gamevars_dashtime(int sprite, int* outDashtime);

bool gamevars_myspawner(int sprite, int* outSpawner);

// AA's `initflags` (AADEFS.CON:3983): 2 on a Polymorph pad spawned over a body. Meaningful only through the picnum
// scope of ekm_polypad_growing_aa and ekm_aa_mando_mannequin.
bool gamevars_initflags(int sprite, int* outInitflags);

bool gamevars_monstflags(int sprite, int* outMonstflags);

// A PER-ACTOR ARRAY VALIDATED ONCE PER WALK, not per sprite (each read costs two VirtualQuery calls). VALID ONLY
// UNTIL THE ENGINE CAN RUN AGAIN: Gv_Free (re-parse, new game, load) frees pValues on the game thread, so use a view
// only inside one SwapBuffers-hook call, never across frames or threads.
struct EkGvView {
  const intptr_t* values;
  bool ok;
};

bool gamevars_team_view(EkGvView* out);
bool gamevars_inithp_view(EkGvView* out);
bool gamevars_mlevel_view(EkGvView* out);
bool gamevars_monstatus_view(EkGvView* out);
bool gamevars_countvarc_view(EkGvView* out);
bool gamevars_countvar_view(EkGvView* out);
bool gamevars_dashtime_view(EkGvView* out);
bool gamevars_initflags_view(EkGvView* out);
bool gamevars_monstflags_view(EkGvView* out);
bool gamevars_mtype_view(EkGvView* out);

inline bool ekm_gv_view_get(const EkGvView& v, int sprite, int* out) {
  if (!out || !v.ok || !v.values) return false;
  if (sprite < 0 || sprite >= EK_MAXSPRITES) return false;
  *out = (int)v.values[sprite & (EK_MAXSPRITES - 1)];
  return true;
}

// THE PER-PLAYER SIDE, same record and guard: GAMEVAR_PERPLAYER (0x1), pValues holds MAXPLAYERS entries. `weapon` is
// 0..MAX_WEAPONS-1, `field` one of weapondata.h's EK_WD_*; the 130 indices come from the SAME single walk.
bool gamevars_weapon_field(int weapon, int field, int playerNum, int* out);

bool gamevars_weapon_field_set(int weapon, int field, int playerNum, int value);

// AA's `pchar`, the character being driven (AADEFS.CON:3267: 0 duke, 1 bombshell, 2 wes, 3 mando, 4 deanova),
// NORESET|PERPLAYER. AA's weapon rewrites are keyed on it.
bool gamevars_pchar(int playerNum, int* out);

// AA's `gotshock` (AADEFS.CON:4334, NORESET|PERPLAYER): bit 2 means "executioner is on top (not chaingun)". They
// share slot 3, so curr_weapon cannot tell them apart. `_present` (declared at all: false on stock Duke) differs from
// the read (false = exists but unreadable); see firestops.h.
bool gamevars_gotshock_present(void);
bool gamevars_gotshock(int playerNum, int* out);

// AA's `aimbot_on` (AADEFS.CON:3548, PERPLAYER), toggled by night-vision. While it reads 1 AND `bottarget` names a
// sprite, AA's Aimbot Goggles write ang/horiz every tic and our aimbot must stand down (ekm_goggles_hold_view).
bool gamevars_aimbot_on(int playerNum, int* out);

// Reload-window vars, PERPLAYER, measurement only: THISCLIP (AADEFS.CON:4406), fastfire (:3794), perks (:3772; bit
// 2048 widens the refill window to kickback 25..26) and burstfired (:4418), which gates the same refill block.
bool gamevars_thisclip(int playerNum, int* out);
bool gamevars_fastfire(int playerNum, int* out);
bool gamevars_perks(int playerNum, int* out);
bool gamevars_burstfired(int playerNum, int* out);
bool gamevars_gotdb(int playerNum, int* out);
// AA's `dbammo` (AADEFS.CON:3554) and `rightDB_weapcount` (:4332): the right DB barrel fires only when the counter is
// 0 and dbammo > 0.
bool gamevars_dbammo(int playerNum, int* out);
bool gamevars_rightdb_weapcount(int playerNum, int* out);
// AA's `steroidstack` (AADEFS.CON:3549) and `inven_upgrades` (:3545), NORESET|PERPLAYER; read once per frame by the
// item-take rule only.
bool gamevars_steroidstack(int playerNum, int* out);
bool gamevars_inven_upgrades(int playerNum, int* out);
// The ammo rows' AA vars (a `want` column): twelve PERPLAYER (131073) and three GLOBAL (charsel 131072, startguns,
// gametype 0).
enum { EK_GV_TK_LASERMODE = 0, EK_GV_TK_PISTOLAMMO = 1, EK_GV_TK_WRISTAMMO = 2,
       EK_GV_TK_NUKESELECT = 3, EK_GV_TK_WEAP4AMMO = 4, EK_GV_TK_GOTPLASMA = 5,
       EK_GV_TK_PLASMAMMO = 6, EK_GV_TK_GOTRAILGUN = 7, EK_GV_TK_RAILGUNAMMO = 8,
       EK_GV_TK_GOTBOW = 9, EK_GV_TK_ARROWS = 10, EK_GV_TK_SAWAMMO = 11,
       EK_GV_TK_CHARSEL = 12, EK_GV_TK_STARTGUNS = 13, EK_GV_TK_GAMETYPE = 14,
       EK_GV_TK_STAKES = 15, EK_GV_TK_NUKEAMOUNT = 16, EK_GV_TK_LASERAMMO = 17,
       EK_GV_TK_COMMAMMO = 18, EK_GV_TK_GOTSAW = 19, EK_GV_TK_GOTSABER = 20,
       EK_GV_TK_ATTMODE = 21,
       EK_GV_TK_ES_TYPE = 22, EK_GV_TK_ES_BATTERY = 23, EK_GV_TK_ES_BATTERYMAX = 24,
       EK_GV_TK_COUNT = 25 };
bool gamevars_take(int which, int playerNum, int* out);

enum { EK_GV_RC_M4RELOAD = 0, EK_GV_RC_UZIRELOAD = 1, EK_GV_RC_M4CLIP = 2,
       EK_GV_RC_UZICLIP = 3, EK_GV_RC_COUNT = 4 };
bool gamevars_reload_counter(int which, int playerNum, int* out);
// A GUARDED WRITE for the two COUNTERS AA writes every tic: same record re-check plus write permission. The first
// failure logs once and later calls refuse without touching memory.
bool gamevars_reload_counter_set(int which, int playerNum, int value);

enum { EK_GV_RV_HORIZFALL = 0, EK_GV_RV_SAVEDHORIZ = 1, EK_GV_RV_COUNT = 2 };
bool gamevars_recoil(int which, int playerNum, int* out);
bool gamevars_recoil_set(int which, int playerNum, int value);
inline bool ekm_recoil_vars_present(bool ready, int fallIdx, int savedIdx) {
  return ready && fallIdx >= 0 && savedIdx >= 0;
}
bool gamevars_recoil_present(void);

enum { EK_GV_SG_BRUTALCOUNT = 0, EK_GV_SG_CRAWLCLOCK = 1, EK_GV_SG_VENDOR = 2,
       EK_GV_SG_VRMENU = 3, EK_GV_SG_STARTMODE = 4, EK_GV_SG_BONUS = 5,
       EK_GV_SG_EPSELECT = 6, EK_GV_SG_MATCHMENU = 7, EK_GV_SG_VRSCREEN = 8,
       EK_GV_SG_THISCAM = 9, EK_GV_SG_COUNT = 10 };
bool gamevars_shot_guard(int which, int playerNum, int* out);

// AA's scope zoom writes the engine's viewing range directly (`setaspect vr yxaspect`, AAGAME.CON:16996), not the fov
// cvar. `zoomin` (AADEFS.CON:4180) and `zoomlevel` (:4193) are PERPLAYER; `vr` (:4189), `vrhi`, `vrlo` are GLOBAL.
// Never treat `vr` == -1 as a viewing range: it is the "no zoom" sentinel.
bool gamevars_zoomin(int playerNum, int* out);
bool gamevars_zoomlevel(int playerNum, int* out);
bool gamevars_vr(int* out);
bool gamevars_vrhi(int* out);
bool gamevars_vrlo(int* out);

const int EK_GV_ZOOM_COUNT = 5;
int gamevars_zoom_resolved(void);

bool gamevars_viewingrange(int* out);
bool gamevars_yxaspect(int* out);

const int EK_GV_ENGINE_COUNT = 2;
int gamevars_engine_resolved(void);

// Two GLOBAL bitfields (AADEFS.CON:4367-4369): figurines collected in this SAVE, one bit per MINI* picnum.
// collectibles1 bit n = MINIDUKE+n (n 0..28, 16010..16038); collectibles2 bit n = MINIQUEEN+n (n 0..21,
// 16039..16060). esp_figurines_aa.h owns the arithmetic. startguns_perma and mcguffins are NORESET.
enum { EK_GV_CO_COLLECTIBLES1 = 0, EK_GV_CO_COLLECTIBLES2 = 1,
       EK_GV_CO_COLLECTINLEVEL = 2, EK_GV_CO_COLLECTFOUND = 3,
       EK_GV_CO_STARTGUNS_PERMA = 4, EK_GV_CO_MCGUFFINS = 5, EK_GV_CO_COUNT = 6 };
bool gamevars_collectibles(int which, int* out);
const int EK_GV_COLLECT_COUNT = 6;
int gamevars_collect_resolved(void);

int gamevars_weapon_resolved(void);

// FRIEND OR FOE, AA's own rule: AAGAME.CON:6793 `ifn team 1 ifn team 3 add killcount 1`. AADEFS.CON:3830: 0 aliens,
// 1 player and allies, 2 feral, 3 neutral non-combatant, 4+ other factions. team -1 (a leftover companion being
// killed) is hostile.
inline bool ekm_team_is_friendly(int team) { return team == 1 || team == 3; }
