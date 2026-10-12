// See tbot.h. Runs in the SwapBuffers hook right after aim_frame: P_GetInput (game.cpp:6485) precedes videoNextPage
// (:6508) and the record is cleared at tic end (:6542), so a bit can be ORed in here (localinput.cpp). Writes one bit,
// clears nothing. The delay is counted in TIME, not frames (no tic hook).

#include <windows.h>
#include <stdio.h>
#include "tbot.h"
#include "shotray.h"
#include "hitscan.h"
#include "targets.h"
#include "gamevars.h"
#include "aim.h"
#include "gamehooks.h"
#include "config.h"
#include "gamestate.h"
#include "wallstate.h"
#include "impact.h"
#include "tracer.h"
#include "projmath.h"
#include "tiles.h"
#include "playerstate.h"
#include "input.h"
#include "udstate.h"
#include "localinput.h"
#include "tbotswitch.h"
#include "tbotdual.h"
#include "tbotspin.h"
#include "gamearrays.h"
#include "weaponspeed.h"
#include "camera.h"
#include "projlead.h"
#include "weapondata.h"
#include "log.h"
#include "wallclock.h"

static TbotState s_state = { 0, 0, 0 };
static bool s_haveLast = false;
static unsigned s_lastMs = 0;

// The player sprite carries both answers the DukePlayer_t table cannot (no `frozen` field); player_read_body reads it
// (playerstate.h). It once used the cached sprite index, but ps->i is reassigned at respawn and a failed read counts as
// `dead`, so the triggerbot died after a level transition for the session.
static const int EK_PLAYER_PAL_FROZEN = 1;

// Instrumentation: the ray cast and the engine bullet disagreed and the log could not say where. `stage` says how far
// tbot_shot_ray_hit got (a fire can happen with no cast).
const int TBOT_TRACE_NOCAST = 1;
const int TBOT_TRACE_RAY    = 2;
const int TBOT_TRACE_HIT    = 3;
struct TbotRayTrace {
  int stage;
  int usedSolution;
  int q16ang, q16horiz, horizoff;
  int vx, vy, vz;
  int ox, oy, oz, osect;
  int hitSprite, hitWall, hitSect, hx, hy, hz;
};
static const TbotRayTrace TBOT_TRACE_BLANK =
    { TBOT_TRACE_NOCAST, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, 0, 0, 0 };
static TbotRayTrace s_trace = TBOT_TRACE_BLANK;

// The cast on record for the fire-time diff (TbotCastRecord, tbot.h). s_castThisFrame ensures `fired` is stamped only
// on a record published by THIS frame's cast.
static TbotCastRecord s_cast;
static bool s_castHave = false;
static bool s_castThisFrame = false;

const unsigned TBOT_TRACE_MIN_MS = 250;

// Counters count EVENTS, not log lines: the fire trace is rate limited and consecutive lines were misread as
// consecutive frames.
struct TbotTally {
  unsigned fires;
  unsigned casts;
  unsigned enemy;
  unsigned enemyLocked;
  unsigned breaking;
  unsigned blocked;
  // Of blocked: the hit sprite was the PLAYER'S OWN (the ray starts at the muzzle, inside it). ek_hitscan now clears
  // the shooter's blocking bits around each cast as Proj_DoHitscan does; this going to zero confirms the fix.
  unsigned blockedOwn;
  unsigned unknown;
  unsigned unknownNoRay;
  // Frames the angular condition held and the PITCH QUANTUM refused: the shot could not be placed on that body. Counted
  // before the ray cast, which such a frame never pays for.
  unsigned noroom;
  unsigned emptySpin;
};
static TbotTally s_tally = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// Shield rays, shots the last-resort option let through, and every hold by tbot_shield_verdict's reason. Debug log
// only.
struct TbotShieldTally {
  unsigned seen, fired, heldReachable, heldOwner, heldRed, heldUnread;
};
static TbotShieldTally s_shield = { 0, 0, 0, 0, 0, 0 };

struct TbotShieldHit { int sprite, picnum, pal, extra, owner, ownerSrc; };
static TbotShieldHit s_shieldHit = { -1, 0, 0, 0, -1, EKM_SHIELD_OWNER_NONE };

static unsigned s_attachmentRays = 0;

static int tbot_tally_ray(int ray, int hitSprite, int lockedSprite, int rayBuilt) {
  if (!g_cfg.debuglog) return ray;
  if (ray == TBOT_RAY_ENEMY) {
    s_tally.enemy++;
    if (hitSprite >= 0 && hitSprite == lockedSprite) s_tally.enemyLocked++;
  } else if (ray == TBOT_RAY_BREAK) {
    s_tally.breaking++;
  } else if (ray == TBOT_RAY_BLOCKED) {
    s_tally.blocked++;
  } else if (ray == TBOT_RAY_SHIELD) {
    s_tally.blocked++;
    s_shield.seen++;
  } else if (ray == TBOT_RAY_ATTACHMENT) {
    s_tally.blocked++;
    s_attachmentRays++;
  } else {
    s_tally.unknown++;
    if (!rayBuilt) s_tally.unknownNoRay++;
  }
  return ray;
}

// What a blocked ray hit: the sprite or wall the engine stopped at, its picnum, and whether it was the player's own
// sprite. Time rate limited, Debug log only; the counter is incremented at the event.
static const unsigned TBOT_BLOCKED_MIN_MS = 1000;
static unsigned s_blockedMs = 0;
static bool s_blockedHave = false;

static void tbot_log_blocked(const EkHitData& hit, bool own) {
  const unsigned now = wallclock_ms();
  if (s_blockedHave && (unsigned)(now - s_blockedMs) < TBOT_BLOCKED_MIN_MS) return;
  s_blockedHave = true;
  s_blockedMs = now;

  int picnum = 0;
  if (hit.sprite >= 0) {
    const bool havePic = targets_sprite_picnum(hit.sprite, &picnum);
    ek_log("tbot blocked: the ray stopped at SPRITE %d picnum %s%d%s "
           "(sect %d, at %d,%d,%d) -- %s",
           (int)hit.sprite, havePic ? "" : "UNREADABLE(", picnum,
           havePic ? "" : ")", (int)hit.sect, hit.x, hit.y, hit.z,
           own ? "IT IS THE PLAYER'S OWN SPRITE" : "not the player's own sprite");
    return;
  }

  EkWallFace face;
  if (hit.wall >= 0) {
    const bool haveWall = wallstate_face(hit.wall, &face);
    if (haveWall)
      ek_log("tbot blocked: the ray stopped at WALL %d picnum %d overpicnum %d "
             "cstat 0x%04X nextsector %d (sect %d, at %d,%d,%d)",
             (int)hit.wall, face.picnum, face.overpicnum,
             (unsigned)face.cstat, face.nextsector, (int)hit.sect,
             hit.x, hit.y, hit.z);
    else
      ek_log("tbot blocked: the ray stopped at WALL %d, whose record COULD NOT "
             "BE READ (sect %d, at %d,%d,%d)",
             (int)hit.wall, (int)hit.sect, hit.x, hit.y, hit.z);
    return;
  }

  ek_log("tbot blocked: the ray stopped at the FLOOR or CEILING of sect %d "
         "(at %d,%d,%d) -- no sprite and no wall were named",
         (int)hit.sect, hit.x, hit.y, hit.z);
}

const unsigned TBOT_TALLY_MIN_MS = 3000;
static TbotTally s_tallyPrinted = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static unsigned s_impactsPrinted = 0;
static unsigned s_tallyMs = 0;
static bool s_tallyHave = false;
static int s_tallySprite = -1;

// The locked target health settles whether rounds land. RAW EK_SPR_EXTRA, not targets.cpp effective value. Damage is
// deferred up to a tic (A_DamageObject into actor[].htextra), so the fall lags the shot by a frame or two.
static int s_hpSprite = -1;
static int s_hpExtra = 0;
static bool s_hpHave = false;
// Health DROPS on the locked target = landed damage; compared with gamehooks_bullets_fired() in the totals line.
static unsigned s_hpDrops = 0;
static bool s_hpUnreadSaid = false;

// Read in the SwapBuffers hook, never the tic-side detour. gamestate_sprite bounds-checks and NULL is reported
// unreadable, not zero. statnum too: 2 on a hit says the engine discarded it.
static bool tbot_read_target_hp(int sprite, int* extra, int* picnum, int* statnum) {
  if (sprite < 0 || sprite >= EK_MAXSPRITES) return false;
  const unsigned char* const r = gamestate_sprite(sprite);
  if (!r) return false;
  *extra   = (int)*(const short*)(r + EK_SPR_EXTRA);
  *picnum  = (int)*(const short*)(r + EK_SPR_PICNUM);
  *statnum = (int)*(const short*)(r + EK_SPR_STATNUM);
  return true;
}

// Target geometry (ceiling-turret investigation): sparks landed 2-7 horizontal units from the turret but 3216 z away,
// with zero bullet holes, i.e. on ceiling or floor. A ceiling sprite is y-flipped (cstat bit 3) and hangs DOWNWARD from
// its z, but ekm_feet_z handles only the centred bit 7.
static const unsigned EK_CSTAT_YFLIP    = 8u;
static const unsigned EK_CSTAT_CENTRED  = 128u;
static const unsigned EK_CSTAT_WALLALIGN = 16u;
static const unsigned EK_CSTAT_FLOORALIGN = 32u;

static void tbot_log_target_geometry(int sprite) {
  if (!g_cfg.debuglog || sprite < 0 || sprite >= EK_MAXSPRITES) return;
  static int s_saidFor = -1;
  if (s_saidFor == sprite) return;
  const unsigned char* const r = gamestate_sprite(sprite);
  if (!r) {
    ek_log("tbot geom: sprite %d -- the record refused; no geometry read", sprite);
    s_saidFor = sprite;
    return;
  }
  const int picnum = (int)*(const short*)(r + EK_SPR_PICNUM);
  const unsigned cstat = *(const unsigned short*)(r + EK_SPR_CSTAT);
  const int xrep = r[EK_SPR_XREPEAT], yrep = r[EK_SPR_YREPEAT];
  const int z = *(const int*)(r + EK_SPR_Z);
  int tw = 0, th = 0;
  tile_size(picnum, &tw, &th);
  const float heightZ = ekm_sprite_height_z(yrep, th);
  const int feetZ = ekm_feet_z(z, heightZ, cstat);

  AimMarkerPoints mp;
  const bool haveAim = aim_marker_points(&mp) && mp.sprite == sprite;

  ek_log("tbot geom: sprite %d picnum %d | cstat 0x%04X ="
         "%s%s%s%s | tile %dx%d xrep %d yrep %d | sprite z %d, our heightZ %d, "
         "our feetZ %d (%s) | our aim z %s%d -- %d above the sprite z. "
         "A y-flipped sprite hangs DOWNWARD and ekm_feet_z does not know bit 3.",
         sprite, picnum, cstat,
         (cstat & EK_CSTAT_YFLIP) ? " YFLIP(bit3)" : "",
         (cstat & EK_CSTAT_CENTRED) ? " CENTRED(bit7)" : "",
         (cstat & EK_CSTAT_WALLALIGN) ? " WALLALIGN(bit4)" : "",
         (cstat & EK_CSTAT_FLOORALIGN) ? " FLOORALIGN(bit5)" : "",
         tw, th, xrep, yrep, z, (int)heightZ, feetZ,
         (cstat & EK_CSTAT_CENTRED) ? "centred, so z + heightZ/2"
                                    : "taken as the sprite z itself",
         haveAim ? "" : "(none published) ", haveAim ? mp.rawZ : 0,
         haveAim ? (z - mp.rawZ) : 0);
  s_saidFor = sprite;
}

// Where the rounds actually landed, per presented frame over the engine's own sprite array (rules: impact.h). A second
// walk rather than a spawn hook, which would be a code patch on the firing path for a measurement; it is behind
// g_cfg.debuglog. The previous frame's class per slot is the entire state: 16 KB of .bss, this thread only.
static unsigned char s_impactPrev[EK_MAXSPRITES];
// Until the table is filled once, EVERY live impact looks new (bullet holes already on the walls). The first walk after
// Debug log goes on records nothing, and priming drops whenever the scan is skipped, so toggling cannot fabricate a
// burst.
static bool s_impactPrimed = false;

// The last target, not only the current: the engine spawns an impact on the TIC, so a burst tail lands after the lock
// is gone. Not cleared: the line prints the picnum so a recycled index is visible.
static int s_impactTarget = -1;

struct TbotImpactTally {
  unsigned seen[EKM_IMPACT_CLASSES];
  unsigned band[EKM_BAND_COUNT];
  unsigned sparkBand[EKM_BAND_COUNT];
  unsigned ours;
  unsigned notOurs;
  unsigned unmeasured;
};
static TbotImpactTally s_impacts = { { 0, 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, 0, 0, 0 };

// A cap per frame, not a time rate limit (time limiting misread this twice): every impact is COUNTED, the cap only
// bounds the lines spelled out, and the frame says how many it held back.
const int TBOT_IMPACT_LINES_MAX = 8;

static bool tbot_read_sprite_pos(int sprite, int* x, int* y, int* z, int* picnum) {
  if (sprite < 0 || sprite >= EK_MAXSPRITES) return false;
  const unsigned char* const r = gamestate_sprite(sprite);
  if (!r) return false;
  *x = *(const int*)(r + EK_SPR_X);
  *y = *(const int*)(r + EK_SPR_Y);
  *z = *(const int*)(r + EK_SPR_Z);
  *picnum = (int)*(const short*)(r + EK_SPR_PICNUM);
  return true;
}

static void tbot_impact_scan(int lockedSprite) {
  // The Debug log wants the LINES, the tracers want the impact POSITIONS, independently; the log gates only the
  // logging.
  const bool say = g_cfg.debuglog != 0;
  const bool feed = tracer_impacts_wanted();
  if ((!say && !feed) || !gamestate_ready()) { s_impactPrimed = false; return; }

  if (lockedSprite >= 0) s_impactTarget = lockedSprite;
  const int target = lockedSprite >= 0 ? lockedSprite : s_impactTarget;
  const char* const which = lockedSprite >= 0
      ? "the locked target" : "the LAST locked target (the lock has dropped)";

  int tx = 0, ty = 0, tz = 0, tpic = 0;
  const bool haveTarget = target >= 0 &&
                          tbot_read_sprite_pos(target, &tx, &ty, &tz, &tpic);

  const bool prime = !s_impactPrimed;
  int lines = 0, newThisFrame = 0;

  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* const r = gamestate_sprite(i);
    if (!r) { s_impactPrimed = false; return; }

    const int statnum = (int)*(const short*)(r + EK_SPR_STATNUM);
    const int picnum  = (int)*(const short*)(r + EK_SPR_PICNUM);
    const int cls = ekm_impact_live_class(picnum, statnum);
    const int prev = (int)s_impactPrev[i];
    s_impactPrev[i] = (unsigned char)cls;

    if (prime || !ekm_impact_is_new(prev, cls)) continue;

    newThisFrame++;
    if (say) s_impacts.seen[cls]++;

    const int ix = *(const int*)(r + EK_SPR_X);
    const int iy = *(const int*)(r + EK_SPR_Y);
    const int iz = *(const int*)(r + EK_SPR_Z);
    const int owner = (int)*(const short*)(r + EK_SPR_OWNER);
    const bool mine = player_sprite_matches(owner);
    if (say) { if (mine) s_impacts.ours++; else s_impacts.notOurs++; }

    // The tracers' half: SHOTSPARK1 only, since Proj_InsertShotspark puts one at hitData->x,y,z for every hitscan round
    // (impact.h). Whose round it was is passed, not filtered: refusing another actor's spark is the tracers' rule.
    if (feed && cls == EKM_IMPACT_SHOTSPARK1)
      tracer_note_impact(ix, iy, iz, mine ? 1 : 0);

    if (!haveTarget) {
      if (!say) continue;
      s_impacts.unmeasured++;
      if (lines < TBOT_IMPACT_LINES_MAX) {
        lines++;
        ek_log("tbot impact: %s (picnum %d) sprite %d at (%d,%d,%d) statnum %d, "
               "owner %d (%s) -- NO TARGET to measure against (none has been "
               "locked yet, or its record refused)%s",
               ekm_impact_name(cls), picnum, i, ix, iy, iz, statnum, owner,
               mine ? "OUR OWN ROUND" : "not ours",
               cls == EKM_IMPACT_BULLETHOLE
                   ? " | BULLETHOLE means this round hit a WALL: the decal is"
                     " spawned on the wall branch only" : "");
      }
      continue;
    }

    if (!say) continue;

    const double dx = (double)ix - (double)tx;
    const double dy = (double)iy - (double)ty;
    const int dist2d = (int)sqrt(dx * dx + dy * dy);
    const int dz = iz - tz;
    const int dzAbs = dz < 0 ? -dz : dz;
    const int band = ekm_impact_band(dist2d, dzAbs);

    s_impacts.band[band]++;
    if (cls == EKM_IMPACT_SHOTSPARK1) s_impacts.sparkBand[band]++;

    if (lines < TBOT_IMPACT_LINES_MAX) {
      lines++;
      ek_log("tbot impact: %s (picnum %d) sprite %d at (%d,%d,%d) statnum %d, "
             "owner %d (%s) | %s: sprite %d picnum %d at (%d,%d,%d) "
             "| HORIZONTAL %d units, z %+d (%d in horizontal units) -- %s%s",
             ekm_impact_name(cls), picnum, i, ix, iy, iz, statnum, owner,
             mine ? "OUR OWN ROUND" : "not ours",
             which, target, tpic, tx, ty, tz,
             dist2d, dz, dz / (int)EKM_Z_PER_XY, ekm_impact_band_name(band),
             cls == EKM_IMPACT_BULLETHOLE
                 ? " | BULLETHOLE means this round hit a WALL: the decal is"
                   " spawned on the wall branch only" : "");
    }
  }

  if (prime) {
    s_impactPrimed = true;
    if (!say) return;
    ek_log("tbot impact: the impact scan is primed -- this frame's live "
           "BULLETHOLE, SMALLSMOKE and SHOTSPARK1 sprites were recorded and "
           "NOT reported, so nothing already on the walls is counted as a hit.");
    return;
  }
  if (say && newThisFrame > lines)
    ek_log("tbot impact: ...and %d more impact(s) this frame, counted in the "
           "totals but not listed (the per-frame line cap is %d)",
           newThisFrame - lines, TBOT_IMPACT_LINES_MAX);
}

static int tbot_classify_shield(int sprite) {
  const unsigned char* const r = gamestate_sprite(sprite);
  const bool recordOk = r != NULL;
  int picnum = 0, extra = 0, pal = 0, spriteOwner = -1;
  if (recordOk) {
    picnum = (int)*(const short*)(r + EK_SPR_PICNUM);
    extra = (int)*(const short*)(r + EK_SPR_EXTRA);
    pal = (int)r[EK_SPR_PAL];
    spriteOwner = (int)*(const short*)(r + EK_SPR_OWNER);
  }
  const int attached = ekm_ray_attachment(TBOT_RAY_BLOCKED, sprite, recordOk,
                                          ekm_picnum_is_attachment(picnum));
  if (attached != TBOT_RAY_BLOCKED) return attached;
  const int ray = ekm_ray_shield(TBOT_RAY_BLOCKED, sprite, recordOk, picnum, extra);
  if (ray != TBOT_RAY_SHIELD) return ray;

  int spawner = -1;
  const bool spawnerOk = gamevars_myspawner(sprite, &spawner);
  int owner = -1;
  const int src = ekm_shield_owner(spawnerOk, spawner, recordOk, spriteOwner, &owner);
  s_shieldHit.sprite = sprite; s_shieldHit.picnum = picnum;
  s_shieldHit.pal = pal; s_shieldHit.extra = extra;
  s_shieldHit.owner = owner; s_shieldHit.ownerSrc = src;
  return ray;
}

// What the next shot would hit, cast along the ray it will leave on (a dying trooper or windowsill stops bullets;
// cansee looks through glass). Origin is the MUZZLE: pos.z + pyoff + ZOFFSET6 (player.cpp:1710-1711), pos.z if pyoff is
// unreadable. If silent aim will substitute, cast along ITS solution (gamehooks.cpp:344-353).
static int tbot_shot_ray_hit(int lockedSprite) {
  s_castThisFrame = false;
  if (g_cfg.debuglog) { s_trace = TBOT_TRACE_BLANK; s_tally.casts++; }
  if (!hitscan_ready())
    return tbot_tally_ray(TBOT_RAY_UNKNOWN, -1, lockedSprite, 0);

  PlayerSnap me;
  if (!player_snap(&me))
    return tbot_tally_ray(TBOT_RAY_UNKNOWN, -1, lockedSprite, 0);

  int pyoff = 0;
  const bool pyoffOk = player_read_pyoff(&pyoff);
  // aim_muzzle_z, not ekm_muzzle_z(weapon): the slot alone answers wrong for AA (projlead.h's ekm_shot_fires_hitscan),
  // and the ray must match aim_solution_at's muzzle.
  const int muzzleZ = pyoffOk ? aim_muzzle_z(me.z, pyoff) : me.z;

  int q16ang = me.ang, q16horiz = me.horiz;
  int usedSolution = 0;
  int solvedAng = 0, solvedHoriz = 0, curAng = 0;
  // aim_solution_at, not aim_solution_for_shot: silent aim now solves from a muzzle, so the ray and the shot agree only
  // if solved from the SAME muzzle and horizoff. pyoffOk is in the condition: with pyoff unreadable the detour refuses
  // to substitute, and a ray along a solution would verify a shot never fired.
  if (g_cfg.silenton != 0 && gamehooks_shoot_ready() && pyoffOk &&
      player_q16ang(&curAng) &&
      aim_solution_at(me.x, me.y, muzzleZ, me.horizoff,
                      &solvedAng, &solvedHoriz) &&
      ekm_redirect_within((curAng >> 16) & (EKM_YAW_UNITS - 1),
                          (solvedAng >> 16) & (EKM_YAW_UNITS - 1),
                          g_cfg.silentmaxangle)) {
    // horizoff is NOT replaced: silent aim writes q16horiz and the engine subtracts both (player.cpp:498); aim.cpp
    // solves q16horiz with the offset removed.
    q16ang = solvedAng;
    q16horiz = solvedHoriz;
    usedSolution = 1;
  }

  const EkShotRay ray = ekm_shot_ray(q16ang, q16horiz, me.horizoff);
  if (g_cfg.debuglog) {
    s_trace.stage = TBOT_TRACE_RAY;
    s_trace.usedSolution = usedSolution;
    s_trace.q16ang = q16ang; s_trace.q16horiz = q16horiz;
    s_trace.horizoff = me.horizoff;
    s_trace.vx = ray.vx; s_trace.vy = ray.vy; s_trace.vz = ray.vz;
    s_trace.ox = me.x; s_trace.oy = me.y; s_trace.oz = muzzleZ;
    s_trace.osect = me.sect;

    // Published where the ray is built, not after the answer: a cast hitscan refuses still gated a shot.
    s_cast.ox = me.x; s_cast.oy = me.y; s_cast.oz = muzzleZ;
    s_cast.posZ = me.z;
    s_cast.pyoff = pyoffOk ? pyoff : 0;
    s_cast.pyoffOk = pyoffOk ? 1 : 0;
    s_cast.osect = me.sect;
    s_cast.q16ang = q16ang; s_cast.q16horiz = q16horiz;
    s_cast.horizoff = me.horizoff;
    s_cast.usedSolution = usedSolution;
    s_cast.targetSprite = -1;
    s_cast.range2d = -1;
    s_cast.fired = 0;
    s_cast.ms = aim_clock_ms();
    s_castHave = true;
    s_castThisFrame = true;
  }
  EkHitData hit;
  if (!ek_hitscan(me.x, me.y, muzzleZ, me.sect, ray.vx, ray.vy, ray.vz, &hit))
    return tbot_tally_ray(TBOT_RAY_UNKNOWN, -1, lockedSprite, 1);
  if (g_cfg.debuglog) {
    s_trace.stage = TBOT_TRACE_HIT;
    s_trace.hitSprite = hit.sprite; s_trace.hitWall = hit.wall;
    s_trace.hitSect = hit.sect;
    s_trace.hx = hit.x; s_trace.hy = hit.y; s_trace.hz = hit.z;

    // The range is measured here and carried: the muzzle-to-hit distance hitscan returned, which for a sprite hit IS
    // the target distance. Only recorded when a sprite was named.
    s_cast.targetSprite = hit.sprite;
    if (hit.sprite >= 0) {
      const double dx = (double)hit.x - (double)me.x;
      const double dy = (double)hit.y - (double)me.y;
      s_cast.range2d = (int)sqrt(dx * dx + dy * dy);
    }
  }

  // The rule is ekm_ray_class's (tbot.h), shared with acquisition's reach test; TbotRayAsk holds the engine's answers.
  int landed = tbot_ray_class_of(true, hit.sprite, hit.wall, NULL);
  if (landed == TBOT_RAY_BLOCKED && hit.sprite >= 0)
    landed = tbot_classify_shield(hit.sprite);

  if (landed == TBOT_RAY_BLOCKED && g_cfg.debuglog) {
    const bool own = player_sprite_matches(hit.sprite);
    if (own) s_tally.blockedOwn++;
    tbot_log_blocked(hit, own);
  }
  return tbot_tally_ray(landed, hit.sprite, lockedSprite, 1);
}

// The engine answers to ekm_ray_class. targetable: ANY live or choking enemy. admitted is acquisition per-pass bitmap
// (NULL asks the engine). wall_breaks: a wall hit asks the wall (Duke3D glass is a wall); a failed read falls to
// blocked.
struct TbotRayAsk {
  const unsigned char* admitted;
  bool targetable(int s) {
    return admitted ? ekm_bit_test(admitted, EK_MAXSPRITES, s)
                    : targets_sprite_targetable(s);
  }
  bool sprite_breaks(int s) {
    int picnum = 0;
    const bool ok = targets_sprite_picnum(s, &picnum);
    return ok && ekm_picnum_breaks_open(picnum);
  }
  bool wall_breaks(int w) {
    EkWallFace face;
    const bool ok = wallstate_face(w, &face);
    return ok && ekm_wall_breaks_open(face.overpicnum, face.cstat, face.nextsector);
  }
};

int tbot_ray_class_of(bool castOk, int hitSprite, int hitWall,
                      const unsigned char* admitted) {
  TbotRayAsk ask = { admitted };
  return ekm_ray_class(castOk, hitSprite, hitWall, ask);
}

// The player's weapon key wins over a held fire bit (tbotswitch.h holds the rule). All on the game thread inside the
// SwapBuffers hook, so nothing crosses to input.cpp's window thread. The replay is the write the engine already makes:
// P_GetInput ORs this field into this record (player.cpp:3386), only into an empty field.
static TswState s_sw = tsw_blank();
static int s_swTicPrev = 0;
static bool s_swTicHave = false;

static const char* tbot_switch_outcome(int o) {
  switch (o) {
    case TSW_OUT_ENGINE:   return "switched by the engine itself, no replay";
    case TSW_OUT_REPLAYED: return "REPLAYED";
    case TSW_OUT_TIMEOUT:  return "TIMED OUT";
    case TSW_OUT_REPLACED: return "replaced by a newer request";
    case TSW_OUT_DROPPED:  return "dropped (the triggerbot stood down or a read failed)";
    default:               return "?";
  }
}

static void tbot_switch_log(const TswStep& r, int weaponNow) {
  if (!g_cfg.debuglog || r.outcome == TSW_OUT_NONE) return;
  ek_log("tbot switch: request field %d (1-10 a slot key, 11 prev, 12 next, "
         "13 alt, 14 last) at weapon %d -- %s; weapon now %d",
         r.slot, r.weaponAt, tbot_switch_outcome(r.outcome), weaponNow);
}

// The early refusals skip the step, so the frames they cover are tics unseen.
static void tbot_switch_drop(void) {
  const TswStep r = tsw_drop(&s_sw);
  s_swTicHave = false;
  tbot_switch_log(r, -1);
}

static bool tbot_switch_step(bool botHolding, bool live, int elapsedMs) {
  int tic = 0;
  const bool ticOk = weaponspeed_engine_tic(&tic);
  unsigned bits = 0;
  const bool bitsOk = localinput_read_bits(&bits);
  int weapon = -1, kick = 0, pos = 0, reloading = 0;
  const bool animOk = player_read_weapon_anim(&weapon, &kick, &pos);
  const bool reloadOk = player_read_reloading(&reloading);

  TswFrame f;
  f.clockOk = (ticOk && bitsOk) ? 1 : 0;
  f.ticAdvanced = (ticOk && s_swTicHave && tic != s_swTicPrev) ? 1 : 0;
  f.field = tsw_field(bits);
  f.botHolding = botHolding ? 1 : 0;
  f.live = live ? 1 : 0;
  f.animOk = (animOk && reloadOk) ? 1 : 0;
  f.weapon = weapon;
  f.kickback = kick;
  f.weaponPos = pos;
  f.reloading = reloading;
  f.elapsedMs = elapsedMs;
  s_swTicHave = ticOk;
  if (ticOk) s_swTicPrev = tic;

  const TswStep r = tsw_step(&s_sw, f);
  if (r.orField != 0)
    localinput_or_bits((unsigned)r.orField << EK_SK_WEAPON_SHIFT);
  tbot_switch_log(r, animOk ? weapon : -1);
  return r.suppressFire != 0;
}

// Mando's dual shotgun, one barrel per tic (tbotdual.h holds the gate and alternation). Called only on a frame that
// fires; the four gamevar reads are paid only with the shotgun up.
static const int EK_TBOT_LOCAL_PLAYER = 0;
static int s_dbLast = EKM_DB_NONE;

// `text` gets the gate's inputs for the fire line, only with Debug log on.
static int tbot_dual_side(char* text, size_t textSize) {
  int weapon = -1, kick = 0, pos = 0;
  const bool animOk = player_read_weapon_anim(&weapon, &kick, &pos);
  int pchar = -1, gotdb = 0;
  bool pcharOk = false, gotdbOk = false;
  if (animOk && weapon == EK_WEAPON_SHOTGUN) {
    pcharOk = gamevars_pchar(EK_TBOT_LOCAL_PLAYER, &pchar);
    gotdbOk = gamevars_gotdb(EK_TBOT_LOCAL_PLAYER, &gotdb);
  }
  const bool wants =
      ekm_tbot_wants_altfire(animOk && pcharOk && gotdbOk, pchar, weapon, gotdb);

  int dbammo = 0, rightCount = 0;
  bool ammoOk = false, countOk = false, bitsOk = false, extOk = false;
  unsigned bits = 0, ext = 0;
  if (wants) {
    ammoOk = gamevars_dbammo(EK_TBOT_LOCAL_PLAYER, &dbammo);
    countOk = gamevars_rightdb_weapcount(EK_TBOT_LOCAL_PLAYER, &rightCount);
    bitsOk = localinput_read_bits(&bits);
    extOk = localinput_read_extbits(&ext);
  }
  const bool rightTerms = ekm_dualbarrel_right_ready(ammoOk && countOk, rightCount,
                                                     pos, dbammo);

  EkmDbFrame f;
  f.fire = 1;
  f.wants = wants ? 1 : 0;
  f.recordOk = bitsOk ? 1 : 0;
  f.recordFire = (int)((bits >> EK_SK_FIRE) & 1u);
  f.recordAlt = extOk ? (int)((ext >> EK_EXT_ALT_FIRE) & 1u) : 0;
  f.leftReady = (animOk && kick == 0) ? 1 : 0;
  // An unarmed extbits cannot take the write, so the right barrel is not ready.
  f.rightReady = (extOk && rightTerms) ? 1 : 0;
  const int side = ekm_dualbarrel_step(&s_dbLast, f);

  if (text && textSize) {
    text[0] = 0;
    if (g_cfg.debuglog)
      _snprintf_s(text, textSize, _TRUNCATE,
                  " | dual: alt=%d (%s), gate %d: weapon %d pchar %d gotDB %d "
                  "(reads anim %d pchar %d gotDB %d) | left ready %d (kick %d), "
                  "right ready %d (rightDB %d pos %d dbammo %d, reads %d %d, "
                  "extbits %s) | record fire %d alt %d",
                  side == EKM_DB_RIGHT ? 1 : 0,
                  side == EKM_DB_RIGHT ? "right" : side == EKM_DB_LEFT ? "left"
                                                                       : "none",
                  wants ? 1 : 0, weapon, pchar, gotdb, animOk ? 1 : 0,
                  pcharOk ? 1 : 0, gotdbOk ? 1 : 0, f.leftReady, kick,
                  f.rightReady, rightCount, pos, dbammo, ammoOk ? 1 : 0,
                  countOk ? 1 : 0,
                  extOk ? "ok" : wants ? "UNARMED OR UNREADABLE" : "not read",
                  f.recordFire,
                  f.recordAlt);
  }
  return side;
}

// An empty spin-up gun holds fire (tbotspin.h). Called once per fire decision; the gamevar reads are paid only with a
// spin-up slot up and its pool at 0.
static bool tbot_empty_spin(bool weaponOk, int weapon) {
  EkmSpinIn s = {};
  s.weaponOk = weaponOk;
  s.weapon = weapon;
  if (weaponOk && (weapon == EK_WEAPON_CHAINGUN || weapon == EK_WEAPON_DEVISTATOR)) {
    s.ammoOk = player_read_ammo(weapon, &s.ammo);
    if (s.ammoOk && s.ammo == 0) {
      s.pcharOk = gamevars_pchar(EK_TBOT_LOCAL_PLAYER, &s.pchar);
      if (weapon == EK_WEAPON_CHAINGUN) {
        s.dukeUpOk = gamearrays_upgrade(EK_GA_UP_DUKE, EK_WEAPON_CHAINGUN, &s.dukeUp);
        s.gotshockOk = gamevars_gotshock(EK_TBOT_LOCAL_PLAYER, &s.gotshock);
      } else {
        s.railOk = gamevars_take(EK_GV_TK_GOTRAILGUN, EK_TBOT_LOCAL_PLAYER, &s.gotrailgun);
      }
    }
  }
  const bool hold = ekm_tbot_empty_spin(s);
  if (!hold || !g_cfg.debuglog) return hold;
  s_tally.emptySpin++;
  static unsigned s_lastSpinMs = 0;
  const unsigned nowMs = wallclock_ms();
  if (s_lastSpinMs != 0 && (unsigned)(nowMs - s_lastSpinMs) < 1000u) return hold;
  s_lastSpinMs = nowMs;
  ek_log("tbot hold: empty spin-up -- weapon %d, ammo_amount %d, pchar %d, "
         "dukeupgrades[3] %d (read %d), gotshock %d (read %d), gotrailgun %d "
         "(read %d); the fire bit is not written",
         weapon, s.ammo, s.pchar, s.dukeUp, s.dukeUpOk ? 1 : 0, s.gotshock,
         s.gotshockOk ? 1 : 0, s.gotrailgun, s.railOk ? 1 : 0);
  return hold;
}

// Option C, the shield as a last resort: only on a frame whose ray landed on a shield. The hitscan answer is
// aim_muzzle_z's: the live WEAPONn_SHOOTS (AA reassigns what a slot fires). A failed weapon read reads as a projectile.
static const char* tbot_shield_verdict_name(int v) {
  switch (v) {
    case TBOT_SHIELD_FIRE:           return "FIRE";
    case TBOT_SHIELD_HELD_REACHABLE: return "held: a reachable target exists";
    case TBOT_SHIELD_HELD_UNREAD:    return "held: the owner would not read";
    case TBOT_SHIELD_HELD_OWNER:     return "held: not the locked target's shield";
    case TBOT_SHIELD_HELD_RED:       return "held: red shield and a projectile weapon";
    default:                         return "?";
  }
}

static int tbot_shield_decide(const AimLock& lock, bool weaponOk, int weapon) {
  bool hitscan = false;
  if (weaponOk) {
    int shoots = 0;
    const bool gotShoots = gamevars_weapon_field(weapon, EK_WD_SHOOTS,
                                                 EK_TBOT_LOCAL_PLAYER, &shoots);
    hitscan = ekm_shot_fires_hitscan(weapon, gotShoots, shoots);
  }
  const TbotShieldHit& h = s_shieldHit;
  const int v = tbot_shield_verdict(lock.reachable != 0,
                                    h.ownerSrc != EKM_SHIELD_OWNER_NONE, h.owner,
                                    lock.sprite, hitscan, h.pal);
  if (!g_cfg.debuglog) return v;
  if (v == TBOT_SHIELD_HELD_REACHABLE) s_shield.heldReachable++;
  else if (v == TBOT_SHIELD_HELD_OWNER) s_shield.heldOwner++;
  else if (v == TBOT_SHIELD_HELD_RED) s_shield.heldRed++;
  else if (v == TBOT_SHIELD_HELD_UNREAD) s_shield.heldUnread++;

  static unsigned s_lastShieldMs = 0;
  const unsigned nowMs = wallclock_ms();
  if (s_lastShieldMs != 0 && (unsigned)(nowMs - s_lastShieldMs) < 1000u) return v;
  s_lastShieldMs = nowMs;
  ek_log("tbot shield: the ray landed on sprite %d picnum %d (%s) pal %d extra %d, "
         "owner %d (from %s) | locked sprite %d, winner %s, weapon %d %s -- %s",
         h.sprite, h.picnum,
         h.picnum == EK_PICNUM_EBARRIER ? "EBARRIER" : "BARRICADE", h.pal,
         h.extra, h.owner,
         h.ownerSrc == EKM_SHIELD_OWNER_SPAWNER ? "myspawner"
         : h.ownerSrc == EKM_SHIELD_OWNER_SPRITE ? "sprite.owner, myspawner unread"
                                                 : "nothing, both unread",
         lock.sprite, lock.reachable ? "reachable" : "UNREACHABLE", weapon,
         !weaponOk ? "(unread, taken as projectile)" : hitscan ? "hitscan" : "projectile",
         tbot_shield_verdict_name(v));
  return v;
}

void tbot_frame(void) {
  const unsigned now = wallclock_ms();
  const int elapsedMs = s_haveLast ? (int)(now - s_lastMs) : 0;
  s_haveLast = true;
  s_lastMs = now;

  // Instrumentation, AHEAD of the feature's refusals: where rounds landed concerns the engine's bullets, so it runs on
  // every presented frame with Debug log on, hand-fired ones too. With the log off it is one bool read. With no lock,
  // impacts log without distances.
  if (g_cfg.debuglog || tracer_impacts_wanted()) {
  AimLock ilock = { -1, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, 0, 1 };
    tbot_impact_scan(aim_lock(&ilock) ? ilock.sprite : -1);
  } else {
    // Called with both consumers off rather than skipped: the gate inside drops the priming, while a skipped walk
    // leaves s_impactPrev stale and the first frame back would read every changed slot as an impact.
    tbot_impact_scan(-1);
  }

  const char* reason = "";

  // The cheap refusal first: a feature that is off costs one int read. tbot_decide still names the reason and resets
  // the held clock.
  if (g_cfg.tboton == 0) {
    TbotCond none = { 0, "off" };
    tbot_decide(false, false, false, false, false, false, false, none, 0, 0,
                &s_state, &reason);
    tbot_switch_drop();
    return;
  }

  // The free camera, second and ahead of every engine read: while it flies the player is across the map and a fire bit
  // would empty his gun into whatever stands near his body. Same shape as the off test (freecam.h owns the rule).
  if (!freecam_bots_run(camera_active())) {
    TbotCond flying = { 0, "camera" };
    tbot_decide(false, false, false, false, false, false, false, flying, 0, 0,
                &s_state, &reason);
    tbot_switch_drop();
    return;
  }

  // A failed read is a refusal: a feature that cannot tell whether the menu is open, the player alive or what they hold
  // must hold fire.
  int gm = 0;
  const bool gmOk = player_read_gm(&gm);
  const bool menuOpen = !gmOk || (gm & EK_MODE_MENU) != 0;

  // The pause is its own term: the Pause key leaves gm at MODE_GAME (screens.cpp:1300), so without it the held clock
  // charges through a pause. An unreadable &ud reads as paused.
  bool pauseOn = true;
  const bool paused = !ud_paused(&pauseOn) || pauseOn;

  int hp = 0, pal = 0;
  const bool bodyOk = player_read_body(&hp, &pal);
  const bool dead = !bodyOk || hp <= 0;
  const bool frozen = bodyOk && pal == EK_PLAYER_PAL_FROZEN;

  int weapon = 0;
  const bool weaponOk = player_read_weapon(&weapon);
  const bool weaponUsable = weaponOk && tbot_weapon_usable(weapon);

  // The lock is aim_frame's, published this frame; every gate that function has has already cleared it. Zeroed first:
  // with no lock the angles are never read.
  AimLock lock = { -1, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, 0, 1 };
  const bool haveLock = aim_lock(&lock);

  const TbotCond angular = tbot_when_clamped(g_cfg.tbotwhen) == TBOT_WHEN_AIMBOT
      ? tbot_when_aimbot(haveLock)
      : tbot_when_crosshair(haveLock, lock.myYaw, lock.yaw,
                            lock.myQ16Horiz, lock.q16horiz);

  // Can the shot be placed at all? BEFORE the ray, which is the expensive step; it also orders "noroom" ahead of
  // "blocked" in the logged reason.
  const TbotCond placed =
      tbot_placed_cond(angular, (double)lock.marginZ, lock.range2d,
                       lock.q16horiz, lock.q16horizoff);
  if (g_cfg.debuglog && angular.holds && !placed.holds) {
    s_tally.noroom++;
    static unsigned s_lastNoRoomMs = 0;
    const unsigned nowNoRoomMs = GetTickCount();
    if (s_lastNoRoomMs == 0 ||
        (unsigned)(nowNoRoomMs - s_lastNoRoomMs) >= 1000u) {
      s_lastNoRoomMs = nowNoRoomMs;
      const double quantum = ekm_pitch_quantum_z(lock.range2d);
      const double ideal = (double)lock.q16horiz / 65536.0 +
                           (double)lock.q16horizoff / 65536.0;
      const double rounded = (ideal >= 0.0) ? (double)(long long)(ideal + 0.5)
                                            : (double)(long long)(ideal - 0.5);
      double leftover = ideal - rounded;
      if (leftover < 0.0) leftover = -leftover;
      ek_log("tbot noroom: sprite %d at range %d -- pitch %.4f rounds to %.0f, "
             "the leftover %.4f moves the impact %.0f z and the body offers "
             "only %d | that margin came from run %d..%d of %d, aim sample %d, "
             "height %d (-1 means pass 2 never measured it)",
             lock.sprite, lock.range2d, ideal, rounded, leftover,
             leftover * quantum, lock.marginZ, lock.spanLo, lock.spanHi,
             EKM_SPAN_SAMPLES, lock.spanMid, lock.spanHeight);
    }
  }

  // The shot's own ray, only when the answer could change: one hitscan per frame at most. A shield answer goes through
  // option C's verdict; others through tbot_ray_cond.
  TbotCond cond = placed;
  int shieldVerdict = -1;
  if (placed.holds) {
    const int ray = tbot_shot_ray_hit(lock.sprite);
    if (ray == TBOT_RAY_SHIELD) {
      shieldVerdict = tbot_shield_decide(lock, weaponOk, weapon);
      cond = tbot_shield_cond(placed, shieldVerdict);
    } else {
      cond = tbot_ray_cond(placed, ray);
    }
  }

  const bool wasFiring = s_state.firing != 0;
  const bool decided = tbot_decide(true, menuOpen, paused, dead, frozen,
                                   input_panel_open(), weaponUsable, cond,
                                   tbot_delay_tics(g_cfg.tbotdelay),
                                   elapsedMs, &s_state, &reason);
  // A pending weapon request withholds the bit; with none, fire == decided. The decision's own state is left as it was.
  const bool withhold = tbot_switch_step(wasFiring || decided,
                                         !menuOpen && !paused && !dead, elapsedMs);
  // An empty spin-up gun would only spin; the decision's state is kept.
  const bool emptySpin = decided && !withhold && tbot_empty_spin(weaponOk, weapon);
  const bool fire = decided && !withhold && !emptySpin;
  // Stamped only on a record THIS frame's cast published, so a swallowed cast is never read at fire time as a shot the
  // bot asked for.
  if (g_cfg.debuglog && fire && s_castThisFrame) s_cast.fired = 1;

  if (g_cfg.debuglog) {
    if (fire) {
      s_tally.fires++;
      if (shieldVerdict == TBOT_SHIELD_FIRE) s_shield.fired++;

      // Logged only when the number moved (the fire bit goes down on every frame the condition holds); the delta stays
      // a true per-fire delta because the remembered value updates at every fire that could read it.
      int extra = 0, picnum = 0, statnum = 0;
      const bool hpOk = tbot_read_target_hp(lock.sprite, &extra, &picnum, &statnum);
      if (lock.sprite != s_hpSprite) {
        s_hpSprite = lock.sprite;
        s_hpHave = false;
        s_hpUnreadSaid = false;
      }
      if (!hpOk) {
        if (!s_hpUnreadSaid) {
          s_hpUnreadSaid = true;
          ek_log("tbot hp: locked sprite %d -- extra COULD NOT BE READ (the "
                 "sprite record refused), so no health is being tracked for it. "
                 "This is not a zero and not a stale value.", lock.sprite);
        }
        s_hpHave = false;
      } else {
        s_hpUnreadSaid = false;
        tbot_log_target_geometry(lock.sprite);
        if (!s_hpHave) {
          s_hpHave = true;
          s_hpExtra = extra;
          ek_log("tbot hp: locked sprite %d picnum %d statnum %d extra %d at fire #%u -- "
                 "first reading at this sprite, no delta yet",
                 lock.sprite, picnum, statnum, extra, s_tally.fires);
        } else if (extra != s_hpExtra) {
          // Drops only: a rise is a save being loaded (reloads showed as +33 and +39), which would inflate the ratio
          // this number measures.
          if (extra < s_hpExtra) s_hpDrops++;
          ek_log("tbot hp: locked sprite %d picnum %d statnum %d extra %d (delta %+d since "
                 "the previous fire at this sprite) at fire #%u",
                 lock.sprite, picnum, statnum, extra, extra - s_hpExtra, s_tally.fires);
          s_hpExtra = extra;
        }
      }
    }

    // Only fires and casts are tested for movement (every ray outcome increments with `casts`); impacts too, since
    // hand-fired rounds with the triggerbot held move neither; and D46's empty-spin holds.
    const unsigned impactsAll = s_impacts.seen[EKM_IMPACT_BULLETHOLE] +
                                s_impacts.seen[EKM_IMPACT_SMALLSMOKE] +
                                s_impacts.seen[EKM_IMPACT_SHOTSPARK1];
    const bool moved = s_tally.fires != s_tallyPrinted.fires ||
                       s_tally.casts != s_tallyPrinted.casts ||
                       s_tally.emptySpin != s_tallyPrinted.emptySpin ||
                       impactsAll != s_impactsPrinted;
    const bool due = !s_tallyHave ||
                     (unsigned)(now - s_tallyMs) >= TBOT_TALLY_MIN_MS;
    if (moved && (due || lock.sprite != s_tallySprite)) {
      char hpText[128];
      if (s_hpHave)
        _snprintf_s(hpText, sizeof(hpText), _TRUNCATE,
                    "sprite %d extra %d at the last fire", s_hpSprite, s_hpExtra);
      else if (s_hpSprite >= 0)
        _snprintf_s(hpText, sizeof(hpText), _TRUNCATE,
                    "sprite %d extra COULD NOT BE READ", s_hpSprite);
      else
        _snprintf_s(hpText, sizeof(hpText), _TRUNCATE,
                    "no target has been fired at yet");


      ek_log("tbot totals (CUMULATIVE since the Debug log went on -- running "
             "totals of EVERY event, not per-frame values and not a sample of "
             "the rate limited lines above): fire bit written %u time(s); rays "
             "cast %u = enemy %u (of which the locked target %u) + breaking %u "
             "+ blocked %u (of which THE PLAYER'S OWN SPRITE %u) + no answer "
             "%u (%u of those never reached hitscan) | held for NO ROOM "
             "(the pitch quantum, no cast paid) %u | "
             "currently locked sprite %d | ROUNDS ACTUALLY FIRED BY THE "
             "ENGINE %u, damage events landed on the locked target %u | "
             "target health: %s | IMPACT SPRITES SEEN: BULLETHOLE %u "
             "(every one of those is a round into a WALL) + SMALLSMOKE %u + "
             "SHOTSPARK1 %u, of which ours %u and someone else's %u | measured "
             "against the locked target: on it %u, a near miss %u, FAR %u "
             "(%u could not be measured) | SHOTSPARK1 alone, one per round: "
             "on %u, near %u, FAR %u",
             s_tally.fires, s_tally.casts, s_tally.enemy, s_tally.enemyLocked,
             s_tally.breaking, s_tally.blocked, s_tally.blockedOwn, s_tally.unknown,
             s_tally.unknownNoRay, s_tally.noroom,
             lock.sprite, gamehooks_bullets_fired(),
             s_hpDrops, hpText,
             s_impacts.seen[EKM_IMPACT_BULLETHOLE],
             s_impacts.seen[EKM_IMPACT_SMALLSMOKE],
             s_impacts.seen[EKM_IMPACT_SHOTSPARK1],
             s_impacts.ours, s_impacts.notOurs,
             s_impacts.band[EKM_BAND_ON], s_impacts.band[EKM_BAND_NEAR],
             s_impacts.band[EKM_BAND_FAR], s_impacts.unmeasured,
             s_impacts.sparkBand[EKM_BAND_ON],
             s_impacts.sparkBand[EKM_BAND_NEAR],
             s_impacts.sparkBand[EKM_BAND_FAR]);

      // Its own line: `tbot totals` is ~920 of ek_log 1024 bytes.
      if (s_shield.seen != 0)
        ek_log("tbot shield totals (CUMULATIVE, AA29): shield rays %u of blocked, shots "
               "fired into a shield %u; held for a reachable target %u, owner "
               "mismatch %u, red shield + projectile %u, owner unread %u",
               s_shield.seen, s_shield.fired, s_shield.heldReachable,
               s_shield.heldOwner, s_shield.heldRed, s_shield.heldUnread);
      if (s_attachmentRays != 0)
        ek_log("tbot sidegun totals (CUMULATIVE, D43): sidegun rays %u of blocked, "
               "every one held fire", s_attachmentRays);
      if (s_tally.emptySpin != 0)
        ek_log("tbot empty spin-up totals (CUMULATIVE, D46): fire decisions "
               "held because the spin-up gun in hand was empty %u",
               s_tally.emptySpin);

      s_impactsPrinted = impactsAll;
      s_tallyPrinted = s_tally;
      s_tallyMs = now;
      s_tallyHave = true;
      s_tallySprite = lock.sprite;
    }
  }

  if (!fire) return;

  // One line the first time the feature fires, so the log says the whole path ran.
  static int s_saidOnce = 0;
  if (!s_saidOnce && g_cfg.debuglog) {
    s_saidOnce = 1;
    ek_log("tbot: first write -- mode %d, delay %d ms (%d tic(s)), target sprite %d, "
           "held %d ms", tbot_when_clamped(g_cfg.tbotwhen), g_cfg.tbotdelay,
           tbot_delay_tics(g_cfg.tbotdelay), lock.sprite, s_state.heldMs);
  }

  // Which barrel this frame writes: fire, alt, or nothing when the record already holds this tic's barrel.
  char dualText[320];
  const int side = tbot_dual_side(dualText, sizeof(dualText));

  // The shot the bot asked for beside the fields that can move it afterwards. auto_aim has no other way to be seen:
  // GetAutoAimAng returns -1 only at 0, and otherwise the engine may snap a hitscan shot onto a sprite of its own
  // choosing.
  if (g_cfg.debuglog) {
    static unsigned s_traceMs = 0;
    static int s_traceHave = 0;
    if (!s_traceHave || (unsigned)(now - s_traceMs) >= TBOT_TRACE_MIN_MS) {
      s_traceHave = 1;
      s_traceMs = now;

      int autoAim = 0;
      const bool autoAimOk = player_read_auto_aim(&autoAim);
      int picnum = 0, fireExtra = 0, statnum = -1;
      const bool picOk = tbot_read_target_hp(lock.sprite, &fireExtra, &picnum, &statnum);
      // The target's `monstatus` at the instant of the shot, which the esp.cpp timeline cannot pin down. A statement of
      // its own.
      int monstatus = 0;
      const bool monstatusOk = lock.sprite >= 0 &&
                               gamevars_monstatus(lock.sprite, &monstatus);

      if (s_trace.stage == TBOT_TRACE_NOCAST) {
        ek_log("tbot fire: target sprite %d picnum %d statnum %d (read %s), auto_aim %d "
               "(read %s), monstatus %d (read %s) -- NO RAY WAS CAST this "
               "frame (hitscan or the player snapshot refused), so the shot "
               "went out unchecked%s",
               lock.sprite, picnum, statnum, picOk ? "ok" : "failed",
               autoAim, autoAimOk ? "ok" : "failed",
               monstatus, monstatusOk ? "ok" : "failed", dualText);
      } else {
        ek_log("tbot fire: target sprite %d picnum %d statnum %d (read %s), auto_aim %d "
               "(read %s), monstatus %d (read %s) | ray built from %s: "
               "q16ang %d q16horiz %d horizoff %d, handed to hitscan vx %d "
               "vy %d vz %d from (%d,%d,%d) sect %d | %s%s",
               lock.sprite, picnum, statnum, picOk ? "ok" : "failed",
               autoAim, autoAimOk ? "ok" : "failed",
               monstatus, monstatusOk ? "ok" : "failed",
               s_trace.usedSolution ? "SILENT AIM'S PUBLISHED SOLUTION"
                                    : "THE PLAYER'S OWN VIEW",
               s_trace.q16ang, s_trace.q16horiz, s_trace.horizoff,
               s_trace.vx, s_trace.vy, s_trace.vz,
               s_trace.ox, s_trace.oy, s_trace.oz, s_trace.osect,
               s_trace.stage == TBOT_TRACE_HIT ? "hit follows" : "HITSCAN GAVE NO ANSWER",
               dualText);
        if (s_trace.stage == TBOT_TRACE_HIT)
          ek_log("tbot fire: ...hit sprite %d wall %d sect %d at (%d,%d,%d)",
                 s_trace.hitSprite, s_trace.hitWall, s_trace.hitSect,
                 s_trace.hx, s_trace.hy, s_trace.hz);
      }
    }
  }

  // Off Mando's DB, side is LEFT on every firing frame: the old write.
  if (side == EKM_DB_LEFT) localinput_or_bits(1u << EK_SK_FIRE);
  else if (side == EKM_DB_RIGHT) localinput_or_extbits(1u << EK_EXT_ALT_FIRE);
}

bool tbot_firing(void) { return s_state.firing != 0; }

// A copy, never a pointer: the caller is in another translation unit on the firing path.
bool tbot_cast_record(TbotCastRecord* out) {
  if (!s_castHave || !out) return false;
  *out = s_cast;
  return true;
}
