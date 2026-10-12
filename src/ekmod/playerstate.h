#pragma once

// DukePlayer_t, the local player's state (the sprite array cannot give the 16.16 view angle and horizon, weapon,
// sector). #pragma pack(1) covers it (player.h:170-292; sizeof 640 static-asserted, :267), so offsets are the packed
// ones.
const int EK_PS_SIZE       = 640;
const int EK_PS_POS        = 0;
const int EK_PS_OPOS       = 12;    // the PREVIOUS tic's pos (player.h:185)
const int EK_PS_Q16HORIZ   = 64;
const int EK_PS_Q16HORIZOFF= 68;    // which the engine ADDS to q16horiz
const int EK_PS_OQ16HORIZ  = 72;
const int EK_PS_OQ16HORIZOFF = 76;
const int EK_PS_Q16ANG     = 80;
const int EK_PS_OQ16ANG    = 84;
const int EK_PS_CURSECTNUM = 404;
const int EK_PS_I          = 516;   // the player's own sprite index
const int EK_PS_PYOFF      = 488;   // the view bob (player.h:221)

// vel.z, the fall velocity, bracketed: pos(0), opos(12), vel(24) put it at 32; npos, bobpos, fric land on q16horiz 64.
// The teleport zeroes it.
const int EK_PS_VELZ = 32;

// bobpos (vec2_t) at 48, same walk. The teleport must write it: player.cpp:5308 derives the sprite's xvel from pos -
// bobpos, which wraps if too great, spiking bobcounter, pyoff and weapon_sway.
const int EK_PS_BOBPOS = 48;

// Bracketed between measured offsets by counting packed fields across player.h:190-224 (no probe); each walk lands on
// its far anchor. Kept as near anchors of live offsets: 76 oq16horizoff, 372 olook_ang (16 int16 back from cursectnum
// 404), 406 look_ang, 490 opyoff.
const int EK_PS_OLOOK_ANG  = 372;
const int EK_PS_LOOK_ANG   = 406;
const int EK_PS_OPYOFF     = 490;
const int EK_PS_GM         = 598;
// AA's level-end signal, player.h:194 `truefz, truecz, player_par`: BOBPOS 48 +16 = 64 (q16horiz); +16 = 80 (q16ang),
// 84; q16angvel 88; truefz 92, truecz 96, player_par 100.
const int EK_PS_PLAYER_PAR = 100;   // AA's level clock (PODHEAD.CON:434)

// Bracketed, far anchor exact: 492 newowner (after opyoff 490, twelve int16 back from i 516: jumping_counter ..
// on_crane); 608 over_shoulder_on (ten uint8 on from gm 598, 25 back from weapon_pos 633).
const int EK_PS_NEWOWNER   = 492;
const int EK_PS_OVER_SHOULDER_ON = 608;

// got_access is the seventh int16 after newowner (492); the walk continues to i (516): twelve int16 span 492..516. If
// wrong it lands on `i` and the likelier failure suppresses the hint on a lock the player cannot open; the mark's shape
// comes from picnum and lotag, only the card's colour reads this.
const int EK_PS_GOT_ACCESS = 506;
const int EK_PS_AUTO_AIM   = 590;
const int EK_PS_CURR_WEAPON = 635;
// The two per-tic weapon counters (weaponspeed.h), measured by compiling DukePlayer_t standalone.
const int EK_PS_KICKBACK    = 597;
const int EK_PS_WEAPON_POS  = 633;

// ammo_amount[13] and reloading by bracket: cursectnum 404 .. subweapon 410 reach 412; max_ammo_amount[13] (26 bytes)
// to 438; ammo_amount to 464; inv_amount[11] to 486; wackedbyactor to 488 = proven PYOFF. MAX_WEAPONS 13, GET_MAX 11
// (inv.h).
const int EK_PS_AMMO_AMOUNT_BASE = 438;
// inv.h's MAX_WEAPONS, static_asserted against EK_WD_MAX_WEAPONS (gamevars.cpp:104).
const int EK_PS_MAX_WEAPONS = 13;

// Rest of the item-take span, no probe: max_player_health follows player_par (100): runspeed 104, max_player_health
// 108; max_shield 112 ... loogie[6] 144, filler[100] 168, floorzrebound 368, floorzcutoff 370 land on olook_ang (372).
// If wrong ekm_takestate_plausible refuses it.
const int EK_PS_MAX_PLAYER_HEALTH = 108;
// gotweapon: max_actors_killed 132, actors_killed 134, gotweapon 136, zoom 138 (player.h:202).
const int EK_PS_GOTWEAPON         = 136;
const int EK_PS_MAX_AMMO_BASE     = 412;
const int EK_PS_INV_AMOUNT_BASE   = 464;
const int EK_PS_GET_MAX           = 11;

// reloading (player.h:241), bracketed backward from auto_aim 590: aim_mode 589, weapreccnt 588, reloading 587.
const int EK_PS_RELOADING = 587;

const int EK_ZOFFSET6 = 4 << 8;

// auto_aim during a silent-aim shot. 0: A_FindTargetSprite returns nothing (player.cpp:220), so the shot lands on our
// line. At 2 GetAutoAimAng snaps with safeldist's approximate distance (:380): ~5% long at 11432 units, 308 rounds for
// 8 damage events.
const int EK_SILENT_AUTO_AIM = 0;

const int EK_APLAYER     = 1405;
const int EK_STAT_PLAYER = 10;

// A live player has both fields right: the statnum is load-bearing, since map authors place APLAYER sprites for
// multiplayer starts and game.cpp:2786 sends those to STAT_MISC in single player.
inline bool ekm_is_player_sprite(int picnum, int statnum) {
  return picnum == EK_APLAYER && statnum == EK_STAT_PLAYER;
}

// Bit 0 of the first byte whatever the field's width (player.h:120-128).
const int EK_MODE_MENU = 0x01;

const int EK_MODE_GAME = 0x04;

// Is the level loaded now? Exact equality, not "MODE_GAME is clear": setlevel ORs MODE_EOL (gameexec.cpp:4413), restart
// MODE_RESTART (cheats.cpp:571), reload-after-death keeps the menu (menus.cpp:4541). A failed read is false and not a
// level change; per-sprite-index memory resets on the load (gm 0, premap.cpp:1465), not on !esp_level_loaded.
inline bool ekm_level_loading(int gm, bool gmOk) { return gmOk && gm == 0; }

inline bool esp_level_loaded(int gm, bool gmOk) {
  return gmOk && gm == EK_MODE_GAME;
}

// player_par only counts up within a level and G_EnterLevel zeroes it (premap.cpp:816), so a drop is a level entry,
// including finish-into-next where gm stays MODE_EOL and never reads 0.
inline bool ekm_level_par_dropped(bool gotPar, int par, bool havePrev, int prevPar) {
  if (!gotPar || !havePrev) return false;
  return par < prevPar;
}

struct EkLevelEpochState { int started, prevLoading, havePar, prevPar; };
const int EKM_EPOCH_NONE = 0, EKM_EPOCH_GM0 = 1, EKM_EPOCH_PAR_DROP = 2;

inline int ekm_level_epoch_step(EkLevelEpochState* s, bool gmOk, int gm,
                                bool gotPar, int par) {
  if (!s) return EKM_EPOCH_NONE;
  const bool loading = ekm_level_loading(gm, gmOk);
  int trig = EKM_EPOCH_NONE;
  if (s->started) {
    if (loading && !s->prevLoading) trig = EKM_EPOCH_GM0;
    else if (!loading && ekm_level_par_dropped(gotPar, par, s->havePar != 0, s->prevPar))
      trig = EKM_EPOCH_PAR_DROP;
  }
  if (gmOk) { s->started = 1; s->prevLoading = loading ? 1 : 0; }
  if (loading) s->havePar = 0;
  else if (gotPar) { s->prevPar = par; s->havePar = 1; }
  return trig;
}

// Inequality, so any number of missed bumps is one reset and a wrap is still a change.
inline bool ekm_level_epoch_changed(long saved, long current) { return saved != current; }

const int EK_WEAPON_PISTOL       = 1;
const int EK_WEAPON_SHOTGUN      = 2;
const int EK_WEAPON_CHAINGUN     = 3;
const int EK_WEAPON_RPG          = 4;
const int EK_WEAPON_SHRINKER     = 6;
const int EK_WEAPON_DEVISTATOR   = 7;
const int EK_WEAPON_FREEZE       = 9;
const int EK_WEAPON_GROW         = 11;
const int EK_WEAPON_FLAMETHROWER = 12;

// Keyed on curr_weapon, not projecTile: World Tour renumbered its tiles, so a picnum list matched nothing
// (silentwt.cpp:73). A weapon index survives renumbering. Pure, out of gamehooks.cpp so the test host can pin it.
inline bool ekm_weapon_wants_accuracy(int weapon, int accPistol, int accShotgun,
                                      int accChaingun, int accExpander,
                                      int accIncinerator) {
  switch (weapon) {
    case EK_WEAPON_PISTOL:   return accPistol   != 0;
    case EK_WEAPON_SHOTGUN:  return accShotgun  != 0;
    case EK_WEAPON_CHAINGUN: return accChaingun != 0;
    case EK_WEAPON_GROW:     return accExpander != 0;
    // AA's Incinerator sits on stock FREEZE (9) but fires hitscan FIREBOLT: what the slot shoots decides.
    case EK_WEAPON_FREEZE:   return accIncinerator != 0;
    default:                 return false;
  }
}

inline bool ekm_accurate_shot_scope(bool isPlayer, bool weaponOk, bool wantsAccuracy) {
  return isPlayer && weaponOk && wantsAccuracy;
}

// Tiles whose EGS case moves ang/zvel through randtraj alone (AAGAME.CON:18734-18745; head :17539 writes other
// sprites). Other projectiles get deliberate EGS changes (ARROWPROJ zvel -= 512 :19071, GRENADEPROJ's lob :18984,
// BLADEPROJ :18701) never restored. Tiles: AADEFS.CON:2704, :2778, :2779.
inline bool ekm_egs_randtraj_tile(int pic) {
  return pic == 20654 || pic == 21940 ||
         pic == 21955;
}

const int EKM_EGS_RESTORE_ANG  = 1;
const int EKM_EGS_RESTORE_ZVEL = 2;
inline int ekm_egs_restore(bool scopeOpen, int scopeTile, int pic, int argStat,
                           int argAng, int argZvel, int argXvel,
                           int curAng, int curZvel, int curXvel) {
  if (!scopeOpen || pic != scopeTile || !ekm_egs_randtraj_tile(pic) || argStat != 4)
    return 0;
  int w = 0;
  if ((short)curAng != (short)argAng) w |= EKM_EGS_RESTORE_ANG;
  if ((short)curZvel != (short)argZvel && (short)curXvel == (short)argXvel)
    w |= EKM_EGS_RESTORE_ZVEL;
  return w;
}

// The four weapons P_PreFireHitscan routes through in stock Duke; the accuracy map is a superset (AA's Incinerator,
// FREEZE 9). Binds the stock table only: AA's pistol fires MANDOLASER at 768, the Devastator the hitscan BARRETPROJ;
// WEAPONn_SHOOTS decides (ekm_weapon_lead_speed, projlead.h).
inline bool ekm_weapon_is_hitscan(int weapon) {
  switch (weapon) {
    case EK_WEAPON_PISTOL:
    case EK_WEAPON_SHOTGUN:
    case EK_WEAPON_CHAINGUN:
    case EK_WEAPON_GROW:
      return true;
    default:
      return false;
  }
}

// Where the shot leaves the player: A_ShootWithZvel adds ZOFFSET6 (player.cpp:1710), P_PreFireHitscan removes it
// (:528), so hitscan fires from pos.z + pyoff (adding it everywhere put impacts 1024 high). Projectile arm approximate:
// RPG/Devastator -(1<<8), freezer +(3<<8), shrinker +(2<<8), knee +(6<<8).
inline int ekm_muzzle_z(int posZ, int pyoff, bool isHitscan) {
  return posZ + pyoff + (isHitscan ? 0 : EK_ZOFFSET6);
}

// Projectile speed per TIC (0 = do not lead). RPG/Devastator/Freezer xvel 644 (player.cpp:1360); Shrinker 768 (:1597);
// Flamethrower FIREBALL 840 (:1290), unconfirmed in game (needs CON version >= 16). Zero: hitscan, the arcing pipe bomb
// (:4496), the placed trip bomb, knee, handremote. Fallback only (projlead.h).
inline int ekm_weapon_proj_speed(int weapon) {
  switch (weapon) {
    case EK_WEAPON_RPG:
    case EK_WEAPON_DEVISTATOR:
    case EK_WEAPON_FREEZE:         return 644;
    case EK_WEAPON_SHRINKER:       return 768;
    case EK_WEAPON_FLAMETHROWER:   return 840;
    default:                       return 0;
  }
}

// playerdata_t holds `DukePlayer_t *ps` first (player.h:271); g_player = &g_player_s[1] (global.h:151). Measured by
// compiling under pack(1): 133, odd, so a stride-8 search could never reach base + 133.
const int EK_PD_SIZE = 133;
static_assert(EK_PD_SIZE % 8 != 0,
              "playerdata_t is not a multiple of 8, so the pointer search "
              "must stride 1; a stride of 8 cannot reach g_player[0].ps");

const int EK_MAXPLAYERS = 16;

// g_player_s itself: zeroth entry null, then sixteen distinct non-null allocations (game.cpp:6803 allocates all
// MAXPLAYERS even in single player).
inline bool ekm_slot_run_ok(const void* zeroth, const void* const* players, int count) {
  if (zeroth != nullptr || players == nullptr || count != EK_MAXPLAYERS) return false;
  for (int i = 0; i < count; i++) {
    if (players[i] == nullptr) return false;
    for (int j = 0; j < i; j++)
      if (players[i] == players[j]) return false;
  }
  return true;
}

inline int ekm_q16ang_degrees(int q16ang) {
  return (int)(((q16ang >> 16) & 2047) * 360 / 2048);
}

// Struct scan in two phases, because the player moves during it (an exact x/y match failed five times). 65536 is ~40x
// the fastest move between scans; it only thins candidates, `i` filters and the confirm proves.
const int EKM_PS_WINDOW = 65536;
const int EKM_PS_MAX_CANDS = 16384;
const int EKM_PS_CONFIRM_ROUNDS = 4;

inline bool ekm_ps_in_window(int v, int start) {
  const long long d = (long long)v - (long long)start;
  return d >= -EKM_PS_WINDOW && d <= EKM_PS_WINDOW;
}

// `i` first: an exact int16 rejects nearly every offset in one compare, where a window around a small position admits
// zeros.
inline bool ekm_ps_candidate(const unsigned char* rec, int spr, int px, int py) {
  if (*(const short*)(rec + EK_PS_I) != (short)spr) return false;
  if (!ekm_ps_in_window(*(const int*)(rec + EK_PS_POS + 0), px)) return false;
  return ekm_ps_in_window(*(const int*)(rec + EK_PS_POS + 4), py);
}

inline void ekm_ps_cands_push(const void** at, int cap, int* count, int* dropped,
                              const void* p) {
  if (*count < cap) at[(*count)++] = p;
  else (*dropped)++;
}
struct EkPsCands { const void* at[EKM_PS_MAX_CANDS]; int count; int dropped; };
inline void ekm_ps_cands_add(EkPsCands* c, const void* p) {
  if (c) ekm_ps_cands_push(c->at, EKM_PS_MAX_CANDS, &c->count, &c->dropped, p);
}

inline bool ekm_ps_pos_confirms(int cx, int cy, int sx, int sy) {
  return cx == sx && cy == sy;
}

// Phase 2 in rounds: every candidate once, then pause and re-check only mismatches (a tic may lie between reads). A
// false read marks the candidate dead.
const unsigned char EKM_PS_PENDING = 0, EKM_PS_CONFIRMED = 1, EKM_PS_DEAD = 2;
template <class Read, class Pause>
inline int ekm_ps_confirm_rounds(Read& read, Pause& pause, int n, unsigned char* st) {
  for (int k = 0; k < n; k++) st[k] = EKM_PS_PENDING;
  int confirmed = 0;
  for (int round = 0; round < EKM_PS_CONFIRM_ROUNDS; round++) {
    if (round) pause();
    int pending = 0;
    for (int k = 0; k < n; k++) {
      if (st[k] != EKM_PS_PENDING) continue;
      int cx = 0, cy = 0, sx = 0, sy = 0;
      if (!read(k, &cx, &cy, &sx, &sy)) { st[k] = EKM_PS_DEAD; continue; }
      if (ekm_ps_pos_confirms(cx, cy, sx, sy)) { st[k] = EKM_PS_CONFIRMED; confirmed++; }
      else pending++;
    }
    if (!pending) break;
  }
  return confirmed;
}

// Exactly one confirmed is the answer; several is a refusal (a fresh copy confirms while the player stands still). One
// with candidates dropped is used, logged as not proven unique.
const int EKM_PS_NONE = 0, EKM_PS_ONE = 1, EKM_PS_MANY = 2, EKM_PS_ONE_UNPROVEN = 3;
inline int ekm_ps_verdict(int confirmed, int dropped) {
  if (confirmed <= 0) return EKM_PS_NONE;
  if (confirmed > 1) return EKM_PS_MANY;
  return dropped > 0 ? EKM_PS_ONE_UNPROVEN : EKM_PS_ONE;
}
inline bool ekm_ps_verdict_usable(int v) { return v == EKM_PS_ONE || v == EKM_PS_ONE_UNPROVEN; }

const int EKM_PS_MIN_LIVE_PICNUMS = 8;
inline bool ekm_ps_level_loaded(int livePicnums) {
  return livePicnums >= EKM_PS_MIN_LIVE_PICNUMS;
}

bool player_scan(void);
bool player_ready(void);
unsigned player_rva(void);

// Health and palette from the player's SPRITE (DukePlayer_t keeps neither): hp <= 0 dead (player.cpp:2262), pal == 1
// frozen (:1796). Index re-read from ps->i every call.
bool player_read_body(int* hp, int* pal);

bool player_sprite_matches(int spriteNum);

bool player_sprite_index(int* out);

// One guarded read of everything, the only path to engine memory here. horizoff is included: the effective pitch is
// q16horiz + q16horizoff (shot zvel player.cpp:498, autoaim :304, camera game.cpp:1009).
struct PlayerSnap { int x, y, z, ang, horiz, horizoff, sect; };
bool player_snap(PlayerSnap* out);

bool player_read_draw_mode(int* newowner, int* overShoulderOn);

bool player_read_gm(int* out);

// player_par: zeroed on every G_EnterLevel (premap.cpp:816), incremented per tic (player.cpp:5028), so a drop is a
// level ending into the next, which gm==0 cannot see.
bool player_read_par(int* out);

// The level-entry epoch's one updater, once per frame from overlay_on_swap on the game thread before consumers;
// level_epoch is the interlocked read.
void level_epoch_tick(void);
long level_epoch(void);

bool player_read_weapon(int* out);

// The only write into engine memory, to fields the engine writes every frame (P_UpdateAngles, player.cpp:3080).
// `alsoOld` is for a snap, else G_DrawRooms interpolates from the old angle. Game thread.
bool player_write_view(int q16ang, int q16horiz, bool alsoOld);

bool player_read_weapon_anim(int* weapon, int* kickback, int* weaponPos);

bool player_read_ammo(int weapon, int* out);

bool player_read_reloading(int* out);

bool player_write_kickback(int v);
bool player_write_weapon_pos(int v);

bool player_read_pyoff(int* out);

bool player_read_auto_aim(int* out);

bool player_read_got_access(int* out);

// AA32 span: max_player_health, max_ammo_amount, ammo_amount, inv_amount, the UNMASKED got_access (AA keeps more cards
// in bits 3-9) and gotweapon in one guarded copy; all or nothing.
struct PlayerTakeSpan {
  int maxhp;
  int maxAmmo[EK_PS_MAX_WEAPONS];
  int ammo[EK_PS_MAX_WEAPONS];
  int inv[EK_PS_GET_MAX];
  int gotAccess;
  int gotWeapon;
};
bool player_read_take_span(PlayerTakeSpan* out);

bool player_read_opos(int* x, int* y, int* z);

// Silent aim's substitution and undo: q16ang, q16horiz, their o-prefixed pair (no frame interpolates through a view the
// player never had), auto_aim, and the sprite ang the actor paths read. Valid for one engine call; the restore is
// mandatory.
struct EkShotSave {
  int q16ang, oq16ang, q16horiz, oq16horiz;
  int autoAim;
  int sprAng;
  int sprOk;
};

bool player_write_shot_view(int q16ang, int q16horiz, int autoAim, EkShotSave* save);
bool player_restore_shot_view(const EkShotSave* save);

// The fourth write: the shooter's blocking bits, cleared around one cast as Proj_DoHitscan does (player.cpp:548;
// hitscan.h). The caller wraps the cast in __try/__finally; `rec` is where the clear wrote and `held` is zero when
// nothing was.
struct EkShooterCstat { unsigned char* rec; int saved; int held; };

bool player_clear_shooter_cstat(int mask, EkShooterCstat* save);

bool player_restore_shooter_cstat(const EkShooterCstat* save);

// Moves the local player to the planner's destination (ekm_tp_plan). Ten fields, all or refused before the first store:
// both members of each pair (Build interpolates from the o member), bobpos x/y, cursectnum, vel.z = 0 (the engine
// transporter's set, actors.cpp:3859-3881). Not rolled back on a fault. Tic clock only, from camera_frame.
struct EkTeleportPlan;
bool player_write_pos(const EkTeleportPlan& plan);

bool player_q16ang(int* out);
bool player_q16horiz(int* out);
bool player_pos(int* x, int* y, int* z);

void player_log_tick(void);
