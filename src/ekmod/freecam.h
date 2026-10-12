#pragma once
// The free camera: pure state and its step integration. Ported in shape from World Tour's freecam.h, with every rate
// re-derived for this game's 30 Hz tic (that fork's is 26).

#include "esp.h"
#include "ui/common.h"
#include "shotray.h"
#include "aimsolve.h"
#include "projmath.h"
#include "gamestate.h"

const int FREECAM_SPEED_MIN     = 1;
const int FREECAM_SPEED_MAX     = 500;
const int FREECAM_SPEED_DEFAULT = 100;

// 100% = 128 units a tic: 3840 units/s = 7.5 m/s at 30 Hz (15 with the fast key).
const int FREECAM_BASE_SPEED = BUILD_UNITS_PER_METRE / 4;

// Build's z axis is 16x x/y, aliased from projmath.h. PHEIGHT (38<<8) is the sprite's z offset (premap.cpp:664), NOT
// the eye height (floorzoffset = 40<<8); see EK_TP_PHEIGHT.
const int FREECAM_Z_PER_XY = (int)EKM_Z_PER_XY;

const int FREECAM_ANG_UNITS = EKM_YAW_UNITS;

const int FREECAM_SIN_SCALE = 16384;

// Free pitch, not the player's -99..299 horiz clamp; short of vertical so horiz never divides by a zero cosine. 500
// units is ~87.9 degrees.
const int FREECAM_PITCH_MAX = 500;

const int FREECAM_HORIZ_LEVEL = EKM_HORIZ_LEVEL;

// Milli units per mouse count, so a fractional rate survives an integer; yawRem/pitchRem carry the remainder.
const int FREECAM_SENS_MILLI_UNIT = 1000;
const int FREECAM_SENS_MILLI_MIN  = 1;
const int FREECAM_SENS_MILLI_MAX  = 10000;

struct FreecamSens { int yawMilli, pitchMilli; };

inline int freecam_sens_milli_clamped(int m) {
  return clamp_int(m, FREECAM_SENS_MILLI_MIN, FREECAM_SENS_MILLI_MAX);
}

inline FreecamSens freecam_sens_default() {
  FreecamSens s = { FREECAM_SENS_MILLI_UNIT, FREECAM_SENS_MILLI_UNIT };
  return s;
}

// Pose, previous tic's pose, mouse remainder. The p-fields are the engine's opos/oq16ang: the camera steps at 30 Hz, so
// at 240 fps it changes every eighth frame and looks like low FPS; G_DrawRooms interpolates the player
// (game.cpp:1001-1029), this pair does the same.
struct FreecamState {
  int x, y, z;
  int ang;
  int pitch;
  int px, py, pz;
  int pang, ppitch;
  int yawRem, pitchRem;
  bool active;
};

// Collapse the previous POSITION only: a transport is a jump (no streak), the turn pair stays for the bracket to
// smooth.
inline void freecam_pose_carry_pos(FreecamState* s) {
  if (!s) return;
  s->px = s->x;
  s->py = s->y;
  s->pz = s->z;
}

// Collapse the previous pose at each tic start and at every discontinuity, so a frame cannot interpolate a streak
// across a jump.
inline void freecam_pose_carry(FreecamState* s) {
  if (!s) return;
  freecam_pose_carry_pos(s);
  s->pang = s->ang;
  s->ppitch = s->pitch;
}

struct FreecamInput { int dx, dy; bool fwd, back, left, right, up, down, fast, slow; };

inline int freecam_speed_clamped(int speedPct) {
  return clamp_int(speedPct, FREECAM_SPEED_MIN, FREECAM_SPEED_MAX);
}

// base * percent / 100, doubled by fast, halved by slow, min 1. Both together CANCEL (one net factor).
inline int freecam_speed(int speedPct, bool fast, bool slow) {
  int v = FREECAM_BASE_SPEED * speedPct / 100;
  if (fast && !slow) v *= 2;
  else if (slow && !fast) v /= 2;
  return v < 1 ? 1 : v;
}

// horiz = 100 + 128*sin/cos of pitch (player.cpp:3084's relation, sin/cos from ekm_sintable). The camera's horiz never
// passes P_UpdateAngles' -99..299 clamp (player.cpp:3179), so it can look past the player's limit; aimsolve.h's shot
// bound rests on the same fact.
inline int freecam_pitch_to_horiz(int pitch) {
  const int sinP = ekm_sintable(pitch);
  const int cosP = ekm_sintable(pitch + FREECAM_ANG_UNITS / 4);
  // The search below probes this range directly, so avoid a zero cosine.
  if (cosP == 0) return sinP >= 0 ? 32767 : -32768;
  return FREECAM_HORIZ_LEVEL + (128 * sinP) / cosP;
}

// A linear search over the forward function's output (consistent by construction; once per activation). Ties keep the
// candidate closest to pitch 0 (truncation flattens a plateau near 0), so freecam_horiz_to_pitch(FREECAM_HORIZ_LEVEL)
// == 0.
inline int freecam_horiz_to_pitch(int horiz) {
  int best = 0;
  int bestDiff = -1;
  for (int p = -FREECAM_PITCH_MAX; p <= FREECAM_PITCH_MAX; p++) {
    const int h = freecam_pitch_to_horiz(p);
    int diff = h - horiz;
    if (diff < 0) diff = -diff;
    const int absP = p < 0 ? -p : p;
    const int absBest = best < 0 ? -best : best;
    if (bestDiff < 0 || diff < bestDiff ||
        (diff == bestDiff && absP < absBest)) {
      bestDiff = diff; best = p;
    }
  }
  return best;
}

// Integrates one tic in place. Yaw takes dx, pitch -dy clamped; travel follows the full 3-D look direction (xy =
// v*cos(pitch), z = -FREECAM_Z_PER_XY*v*sin(pitch)); strafe stays horizontal. `sens` is applied as (count*milli + rem)
// / FREECAM_SENS_MILLI_UNIT with the modulo kept.
inline void freecam_step(FreecamState* s, const FreecamInput& in,
                          int speedPct, const FreecamSens& sens) {
  if (!s || !s->active) return;

  const int yawNum = in.dx * sens.yawMilli + s->yawRem;
  s->ang = (s->ang + yawNum / FREECAM_SENS_MILLI_UNIT) & (FREECAM_ANG_UNITS - 1);
  s->yawRem = yawNum % FREECAM_SENS_MILLI_UNIT;

  const int pitchNum = -in.dy * sens.pitchMilli + s->pitchRem;
  int pitch = s->pitch + pitchNum / FREECAM_SENS_MILLI_UNIT;
  s->pitchRem = pitchNum % FREECAM_SENS_MILLI_UNIT;
  s->pitch = clamp_int(pitch, -FREECAM_PITCH_MAX, FREECAM_PITCH_MAX);

  const int speed = freecam_speed(speedPct, in.fast, in.slow);

  const int cosA = ekm_sintable(s->ang + FREECAM_ANG_UNITS / 4);
  const int sinA = ekm_sintable(s->ang);
  const int cosP = ekm_sintable(s->pitch + FREECAM_ANG_UNITS / 4);
  const int sinP = ekm_sintable(s->pitch);

  int moveFwd = 0, moveStrafe = 0, moveUp = 0;
  if (in.fwd)  moveFwd += speed;
  if (in.back) moveFwd -= speed;
  if (in.right) moveStrafe += speed;
  if (in.left)  moveStrafe -= speed;
  if (in.up)   moveUp += speed;
  if (in.down) moveUp -= speed;

  const int fwdXY = moveFwd * cosP / FREECAM_SIN_SCALE;
  s->x += (fwdXY * cosA) / FREECAM_SIN_SCALE - (moveStrafe * sinA) / FREECAM_SIN_SCALE;
  s->y += (fwdXY * sinA) / FREECAM_SIN_SCALE + (moveStrafe * cosA) / FREECAM_SIN_SCALE;
  s->z += -(moveFwd * sinP / FREECAM_SIN_SCALE) * FREECAM_Z_PER_XY
          - moveUp * FREECAM_Z_PER_XY;
}

// Following a teleporter or water surface: G_MoveTransports (actors.cpp:3908) carries the player across a paired SE 7,
// so without this the camera comes out in the wrong half. Ported in SHAPE from World Tour; none of its four z constants
// survived.
const int FREECAM_TRANSPORT_STAT = 9;
// The z window an airborne player is carried inside, and the distance from the far effector he lands at
// (actors.cpp:3993, :4001-4004).
const int FREECAM_TRANSPORT_ZBAND = 6144;

// P_Emerge's band below the ceiling (actors.cpp:3843); P_Submerge tests the floor plane itself (:3814).
const int FREECAM_TRANSPORT_EMERGE = 1080;
// The hold in tics; its SIGN is which arm armed it, the engine's own encoding: a paired surface sets -2
// (actors.cpp:3878) and a negative hold counts UP (player.cpp:5236-5237), out of reach of the positive top-up
// (:5233-5234). Applying the top-up to a water pair made it one-way.
const int FREECAM_TRANSPORT_WATER_HOLD_TICS = -2;

// The top-up's length (player.cpp:5234).
const int FREECAM_TRANSPORT_WARP_HOLD_TICS = 2;

// The visible teleporter (SE 7 on its sector's floor, lotag 0) is designed, not ported: World Tour and Megaton refuse
// it. The engine's gate (on_ground, actors.cpp:3947) is meaningless for a camera, so the POSITION it stands for is
// used: at most floorzoffset (40<<8, premap.cpp:660) above the floor.
const int FREECAM_TRANSPORT_STAND = 10240;
// The engine's hold for this branch (actors.cpp:3972) plus the top-up: the landing's z edge alone refuses only the
// landing POINT; one unit of drift is back in the far pad's window.
const int FREECAM_TRANSPORT_FLOOR_HOLD_TICS = 13;

// One candidate effector. `lotag` is its SECTOR's (0 none, 1 above water, 2 underwater, game.h:535), -1 if unreadable.
// `tempData4` is t_data[4], the ONFLOORZ flag for an SE 7 (game.cpp:2930, actors.cpp:3924).
struct FreecamTransport { int lotag, seX, seY, seZ, ownerX, ownerY, ownerZ,
                          ownerCeilZ, ownerFloorZ, secFloorZ, secCeilZ, tempData4; };
struct FreecamJump { int hit, x, y, z, hold; };

// Whether this effector carries a camera at (x,y,z), and to where; `dz` (positive is DOWN) stands in for vel.z. Four
// disjoint cases: airborne (actors.cpp:3993), floor teleporter (:3947), two water cases (:4018-4023). The direction
// term is needed: each water landing is the paired plane EXACTLY (P_Submerge :3815, P_Emerge vel.z <= 0 :3843).
inline FreecamJump freecam_transport_jump(const FreecamTransport& t, int x, int y,
                                          int z, int dz) {
  FreecamJump j = { 0, x, y, z, 0 };
  if (t.seX == t.ownerX && t.seY == t.ownerY && t.seZ == t.ownerZ) return j;
  // The z windows are tested against the SEGMENT travelled this tic: a fast camera (up to 40960 z-units a tic) steps
  // clean over a 12288 band. Strict edges survive, so neither pair ping-pongs.
  const long long zPrev = (long long)z - (long long)dz;
  const long long zLo = (zPrev < (long long)z) ? zPrev : (long long)z;
  const long long zHi = (zPrev < (long long)z) ? (long long)z : zPrev;

  int hit = 0, zTo = z, hold = 0;
  if (t.tempData4 == 0) {
    hit = (zLo < (long long)t.seZ + FREECAM_TRANSPORT_ZBAND &&
           zHi > (long long)t.seZ - FREECAM_TRANSPORT_ZBAND) ? 1 : 0;
    // Landing sign is the camera's direction (actors.cpp:4001-4004): rising lands ABOVE the far effector, else below;
    // exactly ZBAND away, which the far half's strict test refuses.
    zTo = (dz < 0) ? t.ownerZ - FREECAM_TRANSPORT_ZBAND
                   : t.ownerZ + FREECAM_TRANSPORT_ZBAND;
  } else if (t.lotag == 0) {
    // On the pad and no higher than a standing viewpoint; no velocity term (actors.cpp:3947). Lands on the far pad's
    // window edge, 512 above the engine's (:3975-3976), where its strict test refuses.
    hit = (zLo <= (long long)t.seZ &&
           zHi > (long long)t.seZ - FREECAM_TRANSPORT_STAND) ? 1 : 0;
    zTo = t.ownerZ - FREECAM_TRANSPORT_STAND;
    hold = FREECAM_TRANSPORT_FLOOR_HOLD_TICS;
  } else if (t.lotag == 1) {
    hit = (z >= t.secFloorZ && dz > 0) ? 1 : 0;
    zTo = t.ownerCeilZ;
    hold = FREECAM_TRANSPORT_WATER_HOLD_TICS;
  } else if (t.lotag == 2) {
    hit = (z < t.secCeilZ + FREECAM_TRANSPORT_EMERGE && dz <= 0) ? 1 : 0;
    zTo = t.ownerFloorZ;
    hold = FREECAM_TRANSPORT_WATER_HOLD_TICS;
  }
  if (!hit) return j;
  j.hit = 1;
  // All four arms keep the pair's separation, a deliberate divergence on the floor arm (the engine ASSIGNS the far
  // position, actors.cpp:3975): crossing a pad dead centre lands dead centre. The top-up holds the camera (tb_0123).
  j.x = clamp_ll_to_int((long long)x + t.ownerX - t.seX);
  j.y = clamp_ll_to_int((long long)y + t.ownerY - t.seY);
  j.z = zTo;
  j.hold = hold;
  return j;
}

struct FreecamHold { int tics, expired; };

inline FreecamHold freecam_transport_hold_next(int hold) {
  FreecamHold h = { 0, 0 };
  if (hold > 0) {
    h.tics = hold - 1;
    h.expired = (h.tics == 0) ? 1 : 0;
  } else if (hold < 0) {
    h.tics = hold + 1;
  }
  return h;
}

// The engine's top-up (player.cpp:5233-5234). on_warping_sector is set for a player sharing a sector with a PAIRED
// effector (actors.cpp:3943) whether or not it fires; the camera's reading is as broad, since the narrower one is false
// at the landing edge.
inline int freecam_transport_hold_topup(int expired, int onWarping) {
  return (expired && onWarping) ? FREECAM_TRANSPORT_WARP_HOLD_TICS : 0;
}

// Which sector's effectors may carry it. The resolve is SEEDED from s_camSect and returns the seed when the point is
// inside it (engine.cpp:13517-13520), so a different answer means the camera LEFT it: refuse. Both answers are
// parameters because the call site once read the out-param in the argument list that filled it.
inline int freecam_transport_sector(bool inMap, int resolvedSect,
                                    int trackedSect, int maxSectors) {
  if (!inMap) return -1;
  if (resolvedSect < 0 || resolvedSect >= maxSectors) return -1;
  if (trackedSect < 0 || trackedSect >= maxSectors) return -1;
  if (resolvedSect != trackedSect) return -1;
  return trackedSect;
}


// Does the substitution reach the drawn view: newowner >= 0 (game.cpp:999) is a viewscreen drawn from the camera
// SPRITE; over_shoulder_on (game.cpp:1025-1040) runs G_DoThirdPerson on a moved camera. Both are REFUSALS: a view the
// player chose is theirs.
inline bool freecam_draw_reachable(int newowner, int overShoulderOn) {
  return newowner < 0 && overShoulderOn == 0;
}

// Which sector the frame hands the renderer. The camera does not clip, so past a wall updatesector answers "none";
// refusing there read as collision. Invalid is not stale: an index outside 0..maxSectors-1 is dangerous (the draw walks
// the sector graph), a stale valid one is what the engine hands on itself. -1 is the refusal.
struct FreecamSectorPick { int sect; int last; };

// `lastSect` seeds the next frame. The one refusal left is no valid sector of any kind, before a flight's first
// resolve.
inline FreecamSectorPick freecam_sector_pick(bool resolvedOk, int resolved,
                                             int lastSect, int maxSectors) {
  FreecamSectorPick r;
  if (resolvedOk && resolved >= 0 && resolved < maxSectors) {
    r.sect = resolved;
    r.last = resolved;
    return r;
  }
  if (lastSect >= 0 && lastSect < maxSectors) {
    r.sect = lastSect;
    r.last = lastSect;
    return r;
  }
  r.sect = -1;
  r.last = -1;
  return r;
}

// The sky is on one arm of polymer_drawrooms: the external-view arm (polymer.cpp:1214-1251) fires when the sector is
// out of range or z is outside its band (:1215-1217) and RETURNS at :1250, before polymer_drawsky (:1845): whole map,
// no sky. So prefer a reachable sector whose band CONTAINS z, every frame.

// A sector and its band. `exact` is 1 when the band is the whole truth: from getzsofslopeptr (camerascan.cpp), or
// neither surface is sloped (engine.cpp:14292). A sloped sector read FLAT is a guess in both directions.
struct FreecamBand { int sect; int ceilz; int florz; int exact; };

inline bool freecam_band_contains(const FreecamBand& b, int z) {
  return z >= b.ceilz && z <= b.florz;
}

// List order is preference; an EXACT band beats a guess; nothing containing z returns lastSect. A guessed candidate
// zero REFUSES the substitution (monotone): a guessed miss may be false. An EXACT miss means the renderer was going
// external. Candidates outside 0..maxSectors-1 are never chosen (game.cpp:1080-1082).
inline int freecam_band_pick(const FreecamBand* bands, int count, int z,
                             int lastSect, int maxSectors) {
  if (!bands || count <= 0) return lastSect;
  if (!bands[0].exact) return lastSect;
  int loose = -1;
  for (int i = 0; i < count; i++) {
    const FreecamBand& b = bands[i];
    if (b.sect < 0 || b.sect >= maxSectors) continue;
    if (!freecam_band_contains(b, z)) continue;
    if (b.exact) return b.sect;
    if (loose < 0) loose = b.sect;
  }
  return loose >= 0 ? loose : lastSect;
}


// When no band will do, widen the test's own input: high above the map nothing contains z and the external arm returns
// before the sky. polymer.cpp:1211 fills its two band locals from ONE getzsofslopeptr call, which camera.cpp redirects
// to a thunk of ours.

struct FreecamWidened { int ceilz; int florz; };

// The engine is asked first and its answer only EXTENDED: neither plane moves inwards, so an accepted z is never
// rejected.
inline FreecamWidened freecam_band_widen(int ceilz, int florz, int z) {
  FreecamWidened r;
  r.ceilz = z < ceilz ? z : ceilz;
  r.florz = z > florz ? z : florz;
  return r;
}

inline bool freecam_redirect_locate_ok(int sites, int faulted) {
  return sites == 1 && faulted == 0;
}

// Sprites no longer gate the install: forcing the normal arm loses actors in sweep-only sectors, a smaller loss than a
// black sky.
inline bool freecam_redirect_install_ok(bool siteOk, bool sweepOk) {
  return siteOk && sweepOk;
}

// The sweep pays for the sky; without it widening gives a sky over an empty world.
inline bool freecam_redirect_arm(bool wholeMapOn, bool sweepOk) {
  return wholeMapOn && sweepOk;
}

// Multipass spike seed (throwaway): half the map from the first sector, crude on purpose so a null result stays
// readable. -1 means no second pass for every degenerate input.
inline int freecam_secondpass_seed(int firstSect, int numsectors,
                                   int maxSectors) {
  if (numsectors < 2 || numsectors > maxSectors) return -1;
  if (firstSect < 0 || firstSect >= numsectors) return -1;
  return (firstSect + numsectors / 2) % numsectors;
}

// Widens ONLY while our own substituted draw is in flight; the game's own view must get the engine's answer unchanged.
inline bool freecam_widen_now(bool inFlight) {
  return inFlight;
}

// Nothing that aims or fires while the player is elsewhere: the triggerbot kept ORing its fire bit near his body off
// screen. The camera flying is the whole condition.
inline bool freecam_bots_run(bool cameraActive) {
  return !cameraActive;
}

// The panel and the camera share one keyboard. release_held_keys forges a WM_KEYUP for each key GetKeyboardState
// reports down but may never forge one the game has no press to pair with; the release is owed once, on the edge where
// the mod takes the keyboard, never on a swap between owners.
inline bool ekm_input_owned_edge(int wasOwned, int nowOwned) {
  return nowOwned != 0 && wasOwned == 0;
}

// Which drawrooms call is the player's view: the bracketed call is reached from TEN `call rel32` sites in six
// functions. The return address excludes five owners but not G_DrawRooms' own second call (game.cpp:870); the arguments
// exclude that but not another owner handed the same values. Neither half suffices.

struct FreecamViewArgs { int x, y, z, angFix, horizFix, sect; };

// Half-open (a RUNTIME_FUNCTION's EndAddress is PAST the function). An empty span matches NOTHING, or a refused locate
// (0/0) would substitute into every draw call.
inline bool freecam_rva_in_span(unsigned rva, unsigned begin, unsigned end) {
  if (end <= begin) return false;
  return rva >= begin && rva < end;
}

// All six or none.
inline bool freecam_same_view(const FreecamViewArgs& a, const FreecamViewArgs& b) {
  return a.x == b.x && a.y == b.y && a.z == b.z && a.angFix == b.angFix &&
         a.horizFix == b.horizFix && a.sect == b.sect;
}

inline bool freecam_is_player_view(unsigned retRva, unsigned drawBegin,
                                   unsigned drawEnd, const FreecamViewArgs& args,
                                   const FreecamViewArgs& cam) {
  return freecam_rva_in_span(retRva, drawBegin, drawEnd) &&
         freecam_same_view(args, cam);
}

// The TROR base level, drawn by yax_drawrooms after the armed call (a no-draw prepare pass) with the same position,
// angle, horizon and the sector re-read from ud.camerasect (engine.cpp:1203, game.cpp:1143). All six equal selects it;
// other levels (k+MAXSECTORS) and a mirror fail.
inline bool freecam_is_tror_base(bool located, bool rowOn, bool haveArmed,
                                 unsigned retRva, unsigned yaxBegin,
                                 unsigned yaxEnd, const FreecamViewArgs& args,
                                 const FreecamViewArgs& armed) {
  return located && rowOn && haveArmed &&
         freecam_rva_in_span(retRva, yaxBegin, yaxEnd) &&
         freecam_same_view(args, armed);
}

// "TROR support" is a mode stored in freecamtrornodraw so existing inis keep meaning (0 and 1 as before, else 1). Mode
// 2 is a diagnostic, not a fix.
const int FREECAM_TROR_OFF         = 0;
const int FREECAM_TROR_BASE        = 1;
const int FREECAM_TROR_SKIP_LEVELS = 2;

inline int freecam_tror_mode_clamped(int mode) {
  if (mode == FREECAM_TROR_OFF || mode == FREECAM_TROR_SKIP_LEVELS) return mode;
  return FREECAM_TROR_BASE;
}

inline bool freecam_tror_on(int mode) {
  return freecam_tror_mode_clamped(mode) != FREECAM_TROR_OFF;
}

inline const char* freecam_tror_mode_name(int mode) {
  switch (freecam_tror_mode_clamped(mode)) {
    case FREECAM_TROR_OFF:         return "Off";
    case FREECAM_TROR_SKIP_LEVELS: return "Base only (diagnostic)";
    default:                       return "Base level";
  }
}

// Mode 2 returns the other-level DRAW passes (engine.cpp:1160, g_nodraw 0), NOT the no-draw discovery passes (:1072)
// that fill the sprite lists yax_copytsprites reads.
inline bool freecam_tror_skip_level(int mode, bool located, bool haveArmed,
                                    bool inYax, bool drawPass, int sect) {
  return freecam_tror_mode_clamped(mode) == FREECAM_TROR_SKIP_LEVELS &&
         located && haveArmed && inYax && drawPass && sect >= EK_MAXSECTORS;
}

// One drawrooms pass cannot hold train or brainwash (MAXWALLSB), so a second depth-merged pass may run first.
enum { FREECAM_PASSES_ONE = 1, FREECAM_PASSES_TWO_WHEN_NEEDED = 2 };
inline int freecam_passes_clamped(int mode) {
  return mode == FREECAM_PASSES_ONE ? FREECAM_PASSES_ONE
                                    : FREECAM_PASSES_TWO_WHEN_NEEDED;
}
inline const char* freecam_passes_name(int mode) {
  return freecam_passes_clamped(mode) == FREECAM_PASSES_ONE ? "One"
                                                            : "Two when needed";
}
enum {
  FREECAM_EXTRA_YES = 0,
  FREECAM_EXTRA_NO_SETTING,
  FREECAM_EXTRA_NO_STOP,
  FREECAM_EXTRA_NO_PREPARE,
  FREECAM_EXTRA_NO_FLAG,
  FREECAM_EXTRA_NO_WHOLEMAP,
  FREECAM_EXTRA_NO_SPRITECOUNT,
  FREECAM_EXTRA_REASONS
};
// trorBase: the base level, where the engine already cleared depth (engine.cpp:1130). Otherwise the player-view call, a
// g_nodraw prepare pass that must never get a pass (it would rewrite gotsector). spriteCountOk: Polymost never resets
// spritesortcnt per pass, so the count must be vouched for; the base level collects no sprites (engine.cpp:1200).
inline int freecam_extra_pass_wanted(int passesMode, bool lastMainStopped,
                                     bool trorBase, int nodrawValue,
                                     bool zclearLocated, bool wholeMapOn,
                                     bool spriteCountOk) {
  if (freecam_passes_clamped(passesMode) == FREECAM_PASSES_ONE)
    return FREECAM_EXTRA_NO_SETTING;
  if (!wholeMapOn) return FREECAM_EXTRA_NO_WHOLEMAP;
  if (!lastMainStopped) return FREECAM_EXTRA_NO_STOP;
  if (trorBase) return FREECAM_EXTRA_YES;
  if (nodrawValue != 0) return FREECAM_EXTRA_NO_PREPARE;
  if (!zclearLocated) return FREECAM_EXTRA_NO_FLAG;
  if (!spriteCountOk) return FREECAM_EXTRA_NO_SPRITECOUNT;
  return FREECAM_EXTRA_YES;
}

// Out of bounds a ceiling is seen only from under its extended slope and pops, so a view from ABOVE drops them. Judged
// in 3D over every sector whose footprint holds the camera (updatesector keeps its seed on an x/y test,
// engine.cpp:13516-13520). int32 wraps as the engine does, hence unsigned forms.
struct FreecamEdge { int x1, y1, x2, y2; };
inline unsigned freecam_inside_term(int v1x, int v1y, int v2x, int v2y) {
  if ((v1x ^ v2x) >= 0) return (unsigned)v1x;
  return ((unsigned)v1x * (unsigned)v2y - (unsigned)v2x * (unsigned)v1y) ^
         (unsigned)v2y;
}
inline int freecam_inside(int x, int y, const FreecamEdge* e, int n) {
  if (!e || n <= 0) return 0;
  unsigned cnt1 = 0, cnt2 = 0;
  for (int i = 0; i < n; i++) {
    int v1x = (int)((unsigned)e[i].x1 - (unsigned)x);
    int v1y = (int)((unsigned)e[i].y1 - (unsigned)y);
    int v2x = (int)((unsigned)e[i].x2 - (unsigned)x);
    int v2y = (int)((unsigned)e[i].y2 - (unsigned)y);
    if ((v1x | v1y) == 0 || (v2x | v2y) == 0) return 1;
    if ((v1y ^ v2y) < 0) cnt1 ^= freecam_inside_term(v1x, v1y, v2x, v2y);
    v1y = (int)((unsigned)v1y - 1u);
    v2y = (int)((unsigned)v2y - 1u);
    if ((v1y ^ v2y) < 0) {
      v1x = (int)((unsigned)v1x - 1u);
      v2x = (int)((unsigned)v2x - 1u);
      cnt2 ^= freecam_inside_term(v1x, v1y, v2x, v2y);
    }
  }
  return (int)((cnt1 | cnt2) >> 31);
}

// Build z grows downward; standing on a plane is inside. A band not slope-exact counts as inside; MIXED (between
// levels) too: only ABOVE hides ceilings.
enum { FREECAM_Z_INSIDE = 0, FREECAM_Z_ABOVE, FREECAM_Z_BELOW, FREECAM_Z_MIXED,
       FREECAM_Z_CLASSES };
struct FreecamZSample { bool contained; int ceilz; int florz; bool exact; };
struct FreecamZTally { int contained, inside, above, below; };
inline void freecam_z_note(FreecamZTally* t, const FreecamZSample& s, int camz) {
  if (!s.contained) return;
  t->contained++;
  if (!s.exact || (camz >= s.ceilz && camz <= s.florz)) t->inside++;
  else if (camz < s.ceilz) t->above++;
  else t->below++;
}
inline int freecam_z_class(const FreecamZTally& t) {
  if (t.inside > 0) return FREECAM_Z_INSIDE;
  if (t.contained == 0 || t.above == t.contained) return FREECAM_Z_ABOVE;
  if (t.below == t.contained) return FREECAM_Z_BELOW;
  return FREECAM_Z_MIXED;
}
inline int freecam_z_classify(const FreecamZSample* s, int n, int camz) {
  FreecamZTally t = { 0, 0, 0, 0 };
  for (int i = 0; s && i < n; i++) freecam_z_note(&t, s[i], camz);
  return freecam_z_class(t);
}
enum { FREECAM_CEILINGS_SHOW = 0, FREECAM_CEILINGS_HIDE = 1 };
inline int freecam_hideceilings_clamped(int v) {
  return v == FREECAM_CEILINGS_SHOW ? FREECAM_CEILINGS_SHOW : FREECAM_CEILINGS_HIDE;
}
inline const char* freecam_hideceilings_name(int v) {
  return freecam_hideceilings_clamped(v) == FREECAM_CEILINGS_SHOW ? "Show" : "Hide";
}

inline bool freecam_sprite_count_ok(int n, int maxOnScreen) {
  return n >= 0 && n <= maxOnScreen;
}

// yax_drawrooms is located twice (the callee after G_DrawRooms' drawrooms call, and the one function with exactly three
// drawrooms calls); both must agree.
inline unsigned freecam_yax_agree(unsigned calleeAfterArmed, unsigned threeDrawFn) {
  if (calleeAfterArmed == 0 || threeDrawFn == 0) return 0;
  return calleeAfterArmed == threeDrawFn ? calleeAfterArmed : 0;
}

// Whether the bracketed draw is still reached: an OBSERVATION (no rendmode cvar), so revocable, else a camera key would
// keep swallowing the keyboard after a renderer switch. It does not prove which renderer drew (renderDrawRoomsQ16
// dispatches inside).

// Drawn frames the hooked draw may go idle before revocation: 300 is 2.5-5 s at 60-120 fps. Not a camera timeout: a
// flying camera zeroes it every frame.
const int FREECAM_DRAW_IDLE_FRAMES = 300;

struct FreecamDrawSeenVerdict { int idle; int revoke; };

// `revoke` is set only on the frame that spends the budget. A zero in means "the detour ran"; the rule keeps no memory,
// so switching back to Polymer restores the feature. A budget of zero or less revokes nothing.
inline FreecamDrawSeenVerdict freecam_draw_seen_verdict(int idle, int threshold) {
  FreecamDrawSeenVerdict r;
  r.idle = idle;
  r.revoke = 0;
  if (threshold <= 0 || idle >= threshold) return r;
  r.idle = idle + 1;
  r.revoke = (r.idle == threshold) ? 1 : 0;
  return r;
}

// One symmetric entry/exit rule left the camera stuck on with the input lockout engaged (World Tour freecam.h).

// Each term is a refusal: row off, held key (not an edge), a menu owning input, a dead or unreadable player.
inline bool freecam_enter_rule(bool on, bool keyEdge, bool menuOpen, bool dead,
                                bool valid) {
  return on && keyEdge && !menuOpen && !dead && valid;
}

// Easier than enter: a key edge is enough and the menu does not block. Being unable to leave is worse than leaving
// early.
inline bool freecam_exit_rule(bool active, bool keyEdge, bool menuOpen) {
  return active && keyEdge;
}

// Forced off regardless of keys: row off, player died, or the read stopped resolving (a level change).
inline bool freecam_force_off_rule(bool active, bool on, bool dead, bool valid) {
  return active && (!on || dead || !valid);
}

// Seqlock counter arithmetic (camera.cpp owns the volatile counter and barriers). A pose is five ints and not atomic;
// odd means a write is in flight.
struct FreecamPose { int x, y, z, ang, horiz; };

// smoothRatio's full scale (baselayer.h:364-375, game.cpp:1001); a value off a hooked argument is clamped.
const int FREECAM_SMOOTH_UNIT = 65536;

// The engine's own tic counter. smoothRatio cannot be the clock: calc_smoothratio CLAMPS to 65536 when a frame exceeds
// a tic (baselayer.h:364-375), so at or below 30 fps the camera stops stepping. ototalclock advances per tic
// (game.cpp:7122). ANY change is a tic (premap.cpp:1698, savegame.cpp:704); several tics step once.
inline bool freecam_tic_advanced(int prevTic, int curTic, bool havePrev) {
  if (!havePrev) return false;
  return curTic != prevTic;
}

// Accepting the two clocks. Shape: `totalclocklock = totalclock` (engine.cpp:9497) ends `ticksS32 &=
// 0xFFFFFFFFFFFF0000` (clockticks.hpp:155), emitted as `xor ax, ax`, ONE in the located drawrooms. Witness: G_DrawRooms
// has it (game.cpp:786). ototalclock: what the game copies INTO totalclock (game.cpp:4953-5145, screens.cpp:1023).
inline bool ekm_tclk_shape_ok(int xors, int loads, int refptrs) {
  return xors == 1 && loads == 1 && refptrs == 1;
}

inline bool ekm_tclk_witness_ok(int drawRoomsLoads) {
  return drawRoomsLoads >= 1;
}

inline bool ekm_otclk_witness_ok(int sites, int distinctSources) {
  return sites >= 1 && distinctSources == 1;
}

inline bool ekm_tclk_span_ok(size_t slotRva, size_t addrRva, size_t imageSize,
                             int inData) {
  if (!inData || imageSize == 0) return false;
  if (slotRva == 0 || slotRva + sizeof(void*) > imageSize) return false;
  if (addrRva == 0 || addrRva >= imageSize) return false;
  return imageSize - addrRva >= 16;
}

// The pose a FRAME draws between the tic before and now (game.cpp:1001-1029 does this for the player). Yaw takes the
// SHORT ARC, `((cur - prev + HALF) & (UNITS-1)) - HALF`; a plain blend spins the long way across zero. 64-bit
// arithmetic: z moves tens of thousands of units a tic.
inline FreecamPose freecam_pose_lerp(const FreecamPose& prev,
                                     const FreecamPose& cur, int ratio) {
  const int r = clamp_int(ratio, 0, FREECAM_SMOOTH_UNIT);
  const int half = FREECAM_ANG_UNITS / 2;
  const int dAng = ((cur.ang - prev.ang + half) & (FREECAM_ANG_UNITS - 1)) - half;
  FreecamPose p;
  p.x = prev.x + (int)((long long)(cur.x - prev.x) * r / FREECAM_SMOOTH_UNIT);
  p.y = prev.y + (int)((long long)(cur.y - prev.y) * r / FREECAM_SMOOTH_UNIT);
  p.z = prev.z + (int)((long long)(cur.z - prev.z) * r / FREECAM_SMOOTH_UNIT);
  p.ang = (prev.ang + (int)((long long)dAng * r / FREECAM_SMOOTH_UNIT))
          & (FREECAM_ANG_UNITS - 1);
  p.horiz = prev.horiz
            + (int)((long long)(cur.horiz - prev.horiz) * r / FREECAM_SMOOTH_UNIT);
  return p;
}

inline unsigned freecam_seq_write_begin(unsigned seq) { return seq + 1; }

inline unsigned freecam_seq_write_end(unsigned seq) { return seq + 1; }

// Good only if the counter was even at the start and did not move; a failed read means no pose.
inline bool freecam_seq_read_ok(unsigned before, unsigned after) {
  return (before & 1u) == 0u && before == after;
}

// Remove fog. Under REND_POLYMER the one distance darkening term scales with globalvisibility (polymer.cpp:5188, :5345;
// polymost.cpp:1300-1327). The field substituted is g_visibility, NOT globalvisibility, which engine.cpp:9460-9482
// recomputes at the head of renderDrawRoomsQ16; g_visibility is the engine's own input (game.cpp:817).

inline bool freecam_fog_substitute(bool cameraActive, bool fogSwitch) {
  return cameraActive && fogSwitch;
}

// Fullbright: indexed Polymost darkens by the surface's OWN shade byte (polymost.cpp:746-771), so shades are lowered
// around one G_DrawRooms and restored before it returns.
inline bool freecam_fullbright_wants(bool cameraActive, bool fbSwitch,
                                     bool available) {
  return cameraActive && fbSwitch && available;
}

inline int ekm_fullbright_shade(int s) { return s > 0 ? 0 : s; }

inline bool ekm_fullbright_records(int s) { return ekm_fullbright_shade(s) != s; }

// Still 0 means still ours; anything else was written during the draw and stands.
inline int ekm_fullbright_restore(int current, int saved) {
  return current == 0 ? saved : current;
}

// Rule 1: engine.cpp:9480's `* 150` is unique and three steps back are `(g_visibility<<2) * 150`: shl, dword read,
// pointer load. The refptr load may repeat (engine.cpp:9477, :9480): take the last, refuse only on zero.
inline bool ekm_gvis_shape_ok(int imuls, int shls, int loads, int refptrs) {
  return imuls == 1 && shls == 1 && loads == 1 && refptrs >= 1;
}

// Rule 2: every arm of engine.cpp:9460-9482 reads g_visibility, and game.cpp:817 writes it exactly ONCE in G_DrawRooms
// (located by its own anchor).
inline bool ekm_gvis_witness_ok(int drawroomsLoads, int drawStores) {
  return drawroomsLoads >= 2 && drawStores == 1;
}

inline bool ekm_gvis_span_ok(size_t slotRva, size_t addrRva, size_t imageSize,
                             bool slotInData) {
  if (!slotInData) return false;
  if (slotRva == 0 || addrRva == 0) return false;
  if (slotRva >= imageSize) return false;
  if (addrRva + sizeof(int) > imageSize) return false;
  return true;
}

// The teleport: decision here; camera.cpp casts the ray and writes, esp.cpp draws. From Megaton's freecam.h with `int
// ok` for `bool ok` (MSVC /O2).

const int EK_TP_CLEARANCE = 256;

// Resting eye height above the floor, Megaton's 0x2800: floorzoffset = 40<<8 = 10240 (premap.cpp:660), pos.z = floorZ -
// floorZOffset (player.cpp:5414). NOT PHEIGHT (spritezoffset, premap.cpp:664): 9728 refuses rooms the player fits in
// and lands 512 low.
const int EK_TP_PHEIGHT = 10240;

// Ceiling side, Megaton's PHEIGHT_MARGIN: P_ClampZ's ceilZ + PMINHEIGHT (player.cpp:5005-5006), PMINHEIGHT 1024
// (player.h:44). P_DoJetpack's 4608 (player.cpp:4939-4940) is a different clamp.
const int EK_TP_CEIL_MARGIN = 1024;

const unsigned EK_TP_NOTICE_MS = 2000;

// Marker column: 224 = 0.44 m across, 0x3000 = 1.5 m tall (xy unit 1/512 m, z 1/8192); under the real 2 m footprint,
// which read too big.
const int EK_TP_MARK_HALF   = 224;
const int EK_TP_MARK_HEIGHT = 0x3000;

const int EK_TP_NOTICE_Y = 24;

const char* const EK_TP_NOTHING   = "Nothing to teleport to";
const char* const EK_TP_NO_SECTOR = "Camera is outside the map";
const char* const EK_TP_TIGHT     = "Not enough room to stand";
// The write's failure gets its own words: an unverified binary, a bad player index or sector is an ENGINE fact, and
// "not enough room" would send the player elsewhere forever.
const char* const EK_TP_NO_WRITE  = "The player could not be moved";
// A sloped sector whose slope call did not answer: cam_band_fill fell back to its flat pair, so the floor height is not
// the floor there.
const char* const EK_TP_NO_FLOOR  = "Could not read the floor there";
const char* const EK_TP_NONE      = "";

// May the teleport fire this tic? keyEdge: pressed this tic, not held. menuOpen: the panel pauses the camera, so its
// pose is stale. The feature's own switch is deliberately absent: the teleport is a way OUT of the camera.
inline int ekm_tp_fire_rule(int active, int keyEdge, int menuOpen) {
  if (!active) return 0;
  if (!keyEdge) return 0;
  return menuOpen ? 0 : 1;
}

// Look direction in the hitscan convention: vx/vy from yaw, vz from pitch alone (no horizoff). Calls ekm_sintable
// (shotray.h:57), so no pointer to refuse.
struct EkHitscanDir { int vx, vy, vz; };

inline EkHitscanDir ekm_tp_direction(int ang, int horiz) {
  EkHitscanDir d = { 0, 0, 0 };
  d.vx = ekm_sintable(ang + FREECAM_ANG_UNITS / 4);
  d.vy = ekm_sintable(ang);
  // Multiplies, not shifts: (100 - horiz) is negative on upward looks (shotray.h:95-99).
  d.vz = ((100 - horiz) * 32) * 64;
  return d;
}

inline void ekm_tp_step_back(int hitx, int hity, int vx, int vy, int stepBack,
                             int* outX, int* outY) {
  if (!outX || !outY) return;
  if (!stepBack) { *outX = hitx; *outY = hity; return; }
  *outX = clamp_ll_to_int((long long)hitx - (long long)vx * EK_TP_CLEARANCE / FREECAM_SIN_SCALE);
  *outY = clamp_ll_to_int((long long)hity - (long long)vy * EK_TP_CLEARANCE / FREECAM_SIN_SCALE);
}

// Stage one: a raw hitscan result to the point the engine resolves a sector for (the caller runs ek_updatesector on it
// before stage two). A wall or sprite hit steps the point back; floor or ceiling does not.
struct EkTeleportAim { int ok; int x, y, z; const char* reason; };

inline EkTeleportAim ekm_tp_aim(int hitsect, int hitwall, int hitsprite,
                                int hitx, int hity, int hitz, int vx, int vy) {
  EkTeleportAim a = { 0, 0, 0, 0, EK_TP_NOTHING };
  if (hitsect < 0) return a;
  const int stepBack = (hitwall >= 0 || hitsprite >= 0) ? 1 : 0;
  int x = 0, y = 0;
  ekm_tp_step_back(hitx, hity, vx, vy, stepBack, &x, &y);
  a.ok = 1;
  a.x = x;
  a.y = y;
  a.z = hitz;
  a.reason = EK_TP_NONE;
  return a;
}

// Stage two: may the player stand there. The eye lands eye height above the floor, not at the ray's z. A tight space is
// a REFUSAL, not a clamp: a destination chosen by looking must never silently move them elsewhere.
struct EkTeleportPlan { int ok; int x, y, z; int sect; const char* reason; };

inline EkTeleportPlan ekm_tp_plan(int x, int y, int z, int sectFromEngine,
                                  int florz, int ceilz, int eyeHeight) {
  EkTeleportPlan p = { 0, 0, 0, 0, -1, EK_TP_NO_SECTOR };
  (void)z;
  if (sectFromEngine < 0) return p;
  const long long gap = (long long)florz - (long long)ceilz;
  const long long need = (long long)eyeHeight + (long long)EK_TP_CEIL_MARGIN;
  if (gap < need) { p.reason = EK_TP_TIGHT; return p; }
  p.ok = 1;
  p.x = x;
  p.y = y;
  // The player's z is their EYE: pos.z = floorZ - floorZOffset (player.cpp:5414), never florz.
  p.z = clamp_ll_to_int((long long)florz - (long long)eyeHeight);
  p.sect = sectFromEngine;
  p.reason = EK_TP_NONE;
  return p;
}

inline int ekm_tp_has_notice(const EkTeleportPlan& p) {
  return (p.reason != 0 && p.reason[0] != '\0') ? 1 : 0;
}

// Unsigned subtraction: exact across the 49.7 day tick wrap. `have` is 0 until something has been said.
inline int ekm_tp_notice_visible(int have, unsigned setMs, unsigned nowMs) {
  if (!have) return 0;
  return ((unsigned)(nowMs - setMs) < EK_TP_NOTICE_MS) ? 1 : 0;
}

struct EkMarkCorner { int x, y; };
struct EkMarkBox { EkMarkCorner c[4]; int zTop, zBottom; };

inline EkMarkBox ekm_tp_mark_box(int x, int y, int z) {
  const long long h = EK_TP_MARK_HALF;
  const int lx = clamp_ll_to_int((long long)x - h), hx = clamp_ll_to_int((long long)x + h);
  const int ly = clamp_ll_to_int((long long)y - h), hy = clamp_ll_to_int((long long)y + h);
  EkMarkBox b;
  b.c[0].x = lx; b.c[0].y = ly;
  b.c[1].x = hx; b.c[1].y = ly;
  b.c[2].x = hx; b.c[2].y = hy;
  b.c[3].x = lx; b.c[3].y = hy;
  b.zBottom = z;
  b.zTop    = clamp_ll_to_int((long long)z - (long long)EK_TP_MARK_HEIGHT);
  return b;
}

// View-substitution bisection: camera.cpp substitutes six values into renderDrawRoomsQ16, the ONLY thing the mod does
// there with "Draw the whole map" OFF. Five states: hold back ONE of z/horiz/ang/sect, never two; FREECAM_SUBST_ALL is
// the plain call. Pure selection; no cause is asserted.
const int FREECAM_SUBST_ALL        = 0;
const int FREECAM_SUBST_KEEP_SECT  = 1;
const int FREECAM_SUBST_KEEP_Z     = 2;
const int FREECAM_SUBST_KEEP_HORIZ = 3;
const int FREECAM_SUBST_KEEP_ANG   = 4;
const int FREECAM_SUBST_MAX        = FREECAM_SUBST_KEEP_ANG;

inline int freecam_subst_clamped(int state) {
  if (state < FREECAM_SUBST_ALL || state > FREECAM_SUBST_MAX) return FREECAM_SUBST_ALL;
  return state;
}

struct FreecamSubstArgs { int z; int ang; int horiz; short sect; };

// `state` passes the engine's OWN argument for the one value it names; out of range clamps to ALL.
inline FreecamSubstArgs freecam_subst_pick(int state, int substZ, int substAng,
                                           int substHoriz, short substSect,
                                           int engineZ, int engineAng,
                                           int engineHoriz, short engineSect) {
  FreecamSubstArgs a = { substZ, substAng, substHoriz, substSect };
  switch (freecam_subst_clamped(state)) {
    case FREECAM_SUBST_KEEP_SECT:  a.sect  = engineSect;  break;
    case FREECAM_SUBST_KEEP_Z:     a.z     = engineZ;     break;
    case FREECAM_SUBST_KEEP_HORIZ: a.horiz = engineHoriz; break;
    case FREECAM_SUBST_KEEP_ANG:   a.ang   = engineAng;   break;
    default: break;
  }
  return a;
}

// The ud.camera field bisection: the OTHER engine write on that path (ud_camera_write's four non-position fields). One
// field per state; FREECAM_UD_ALL is the plain call. No cause asserted.
const int FREECAM_UD_ALL        = 0;
const int FREECAM_UD_KEEP_SECT  = 1;
const int FREECAM_UD_KEEP_Z     = 2;
const int FREECAM_UD_KEEP_HORIZ = 3;
const int FREECAM_UD_KEEP_ANG   = 4;
const int FREECAM_UD_MAX        = FREECAM_UD_KEEP_ANG;

inline int freecam_ud_clamped(int state) {
  if (state < FREECAM_UD_ALL || state > FREECAM_UD_MAX) return FREECAM_UD_ALL;
  return state;
}

struct FreecamUdArgs { int z; int ang; int horiz; int sect; };

inline FreecamUdArgs freecam_ud_pick(int state, int camZ, int camAng,
                                     int camHoriz, int camSect, int engineZ,
                                     int engineAng, int engineHoriz,
                                     int engineSect) {
  FreecamUdArgs a = { camZ, camAng, camHoriz, camSect };
  switch (freecam_ud_clamped(state)) {
    case FREECAM_UD_KEEP_SECT:  a.sect  = engineSect;  break;
    case FREECAM_UD_KEEP_Z:     a.z     = engineZ;     break;
    case FREECAM_UD_KEEP_HORIZ: a.horiz = engineHoriz; break;
    case FREECAM_UD_KEEP_ANG:   a.ang   = engineAng;   break;
    default: break;
  }
  return a;
}
