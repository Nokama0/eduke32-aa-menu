// The aimbot: acquire a target, solve for it, write the view angle. No hook is needed: drawframe_do
// (game.cpp:6478-6511) runs P_GetInput, G_DrawRooms, then videoNextPage, which reaches our SwapBuffers hook on the game
// thread after the view angle's only writer. The write lands after the draw, so smoothing is per frame.

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "aim.h"
#include "config.h"
#include "targets.h"
#include "gamestate.h"
#include "aimsolve.h"
#include "cansee.h"
#include "hitscan.h"
#include "shotray.h"
#include "tbot.h"
#include "wallstate.h"
#include "playerstate.h"
#include "projlead.h"
#include "gamevars.h"
#include "weapondata.h"
#include "gamehooks.h"
#include "log.h"
#include "ui/bind.h"
#include "input.h"
#include "localinput.h"
#include "camera.h"
#include "aa_headshot.h"
#include "tilenames.h"
#include "tiles.h"

static EkTarget s_targets[EK_TARGET_MAX];

static int s_candIdx[EK_TARGET_MAX];
static int s_candDist[EK_TARGET_MAX];

// The sprites this pass admitted as targets (every term before range and cone), so the reach test's "did it hit an
// enemy" is a bit test, not a guarded read per candidate.
static unsigned char s_admitted[(EK_MAXSPRITES + 7) / 8];

// No probe budget; the cheap terms are the gate. A 64-probe cap was a hard ceiling on RANGE (a log showed 46
// candidates, 16 probed, nothing accepted past 13432 units while the nearest enemy sat at 16849). Cost is bounded by
// the cadence instead: acquisition runs once per tic (EKM_ACQUIRE_GAP_MS), not per rendered frame.

static const unsigned EK_AIM_SPAN_FULL_MS = 1000;
static const unsigned EK_AIM_SPAN_PARTIAL_MS = 100;


// QueryPerformanceCounter, not GetTickCount or timeGetTime: smoothing and the velocity divisor are functions of elapsed
// TIME, and a frame at 144 fps (7 ms) is under the 15.6 ms default system-timer step, which would quantise it. A
// negative return means the clock failed and the caller assumes one tic per frame.
static double aim_now_ms(void) {
  static double freqMs = 0.0;
  static bool broken = false;
  if (broken) return -1.0;
  if (freqMs == 0.0) {
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) {
      broken = true;
      ek_log("aim: QueryPerformanceFrequency failed, smoothing assumes 30 fps");
      return -1.0;
    }
    freqMs = 1000.0 / (double)f.QuadPart;
  }
  LARGE_INTEGER now;
  if (!QueryPerformanceCounter(&now)) {
    broken = true;
    ek_log("aim: QueryPerformanceCounter failed, smoothing assumes 30 fps");
    return -1.0;
  }
  return (double)now.QuadPart * freqMs;
}

static unsigned aim_tick_of(double ms) {
  return ms >= 0.0 ? (unsigned)(unsigned long long)ms : GetTickCount();
}

unsigned aim_clock_ms(void) { return aim_tick_of(aim_now_ms()); }

// Where every candidate was when it last moved, and how fast: velocity is a difference, so leading needs the previous
// position and its time. Indexed by sprite slot, not hashed: a shared bucket would hand one enemy another's motion.
// Zero-filled BSS.
struct EkAimSample {
  int x, y, z;            // the position as of lastChangeMs: this tic's
  // Where it was the tic before: the engine's own actor[].bpos, which G_DoSpriteAnimations interpolates the drawn
  // sprite FROM (game.cpp:3882). Nothing new is sampled for it.
  int bx, by, bz;
  int vx, vy, vz;         // Build units PER TIC
  unsigned lastChangeMs;  // wall clock, so unsigned subtraction survives a wrap
  short picnum;
  unsigned char used;
  unsigned char stopped;  // sticky: see the stale branch below. Free, in padding.
};
static EkAimSample s_prev[EK_MAXSPRITES];

// Where the RENDERER is drawing this target now, and its velocity in Build units PER TIC. Updated only when the
// position changes and divided by TICS, not frames (actors move at 30 Hz; per-frame sampling under-leads by 30/fps). A
// first sighting or a recycled slot (a changed picnum is a different enemy) gets zero velocity.
static void aim_track(const EkTarget& t, unsigned nowMs,
                      int* ix, int* iy, int* iz,
                      int* vx, int* vy, int* vz) {
  *ix = t.x; *iy = t.y; *iz = t.z;
  *vx = 0; *vy = 0; *vz = 0;
  if (t.sprite < 0 || t.sprite >= EK_MAXSPRITES) return;
  EkAimSample& s = s_prev[t.sprite];

  if (!s.used || s.picnum != t.picnum) {
    s.used = 1; s.picnum = t.picnum;
    s.x = t.x; s.y = t.y; s.z = t.z;
    s.bx = t.x; s.by = t.y; s.bz = t.z;
    s.vx = 0; s.vy = 0; s.vz = 0;
    s.stopped = 0;
    s.lastChangeMs = nowMs;
    return;
  }

  if (t.x != s.x || t.y != s.y || t.z != s.z) {
    const int tics = ekm_lead_divisor(nowMs - s.lastChangeMs);
    if (tics <= 0) {
      // Too long a gap to call motion: the slot is only sampled while the target survives the cone and ladder, so an
      // enemy can walk unobserved for seconds; dividing that walk by the cap threw the aim point into the floor.
      // Treated as a first sighting.
      s.x = t.x; s.y = t.y; s.z = t.z;
      s.bx = t.x; s.by = t.y; s.bz = t.z;
      s.vx = 0; s.vy = 0; s.vz = 0;
      s.stopped = 0;
      s.lastChangeMs = nowMs;
      return;
    }
    s.stopped = 0;
    s.vx = (t.x - s.x) / tics;
    s.vy = (t.y - s.y) / tics;
    // Grounded actors report fake vertical speed: a standing enemy shows 272..320 units per tic of z from the mover's
    // gravity-snap cycle (World Tour target.h:763), which led rockets into the floor. There is no floorz here, so vz is
    // taken only when horizontal speed is non-zero, from the per-tic values actually used.
    s.vz = (s.vx != 0 || s.vy != 0) ? (t.z - s.z) / tics : 0;
    // One tic back, not one observation back: the previous position is where we LAST LOOKED but the renderer's bpos is
    // always one tic back, so bpos is the current position less one tic of velocity. z skips the grounded-actor gate of
    // s.vz: that rule stops fake gravity-snap z being LED, but the renderer lerps z unconditionally.
    s.bx = t.x - s.vx; s.by = t.y - s.vy; s.bz = t.z - (t.z - s.z) / tics;
    s.x = t.x; s.y = t.y; s.z = t.z;
    s.lastChangeMs = nowMs;
  } else if (s.stopped || ekm_velocity_stale(nowMs - s.lastChangeMs)) {
    // An enemy that has stopped stops being led, or a standing target keeps its last velocity for ever. The stamp moves
    // with it every frame (`stopped` is sticky), so the first real step after a pause is measured over one frame;
    // restamping only when staleness fires would divide it by two or three.
    s.stopped = 1;
    s.vx = 0; s.vy = 0; s.vz = 0;
    s.bx = s.x; s.by = s.y; s.bz = s.z;
    s.lastChangeMs = nowMs;
  }

  const double r = ekm_smoothratio((double)(nowMs - s.lastChangeMs));
  *ix = ekm_lerp_pos(s.bx, s.x, r);
  *iy = ekm_lerp_pos(s.by, s.y, r);
  *iz = ekm_lerp_pos(s.bz, s.z, r);
  *vx = s.vx; *vy = s.vy; *vz = s.vz;
}

// The player's eye, interpolated as the camera is: DukePlayer_t carries opos (the previous tic's pos) after pos and
// G_DrawRooms interpolates between them by smoothratio; this only supplies WHEN the tic boundary was, by noticing the
// pair change. A failed opos read falls back to the raw position.
static void aim_eye_interp(const PlayerSnap& me, unsigned nowMs,
                           int* ex, int* ey, int* ez) {
  *ex = me.x; *ey = me.y; *ez = me.z;

  int ox = 0, oy = 0, oz = 0;
  if (!player_read_opos(&ox, &oy, &oz)) return;

  static bool s_seen = false;
  static int s_px = 0, s_py = 0, s_pz = 0;
  static int s_ox = 0, s_oy = 0, s_oz = 0;
  static unsigned s_ticMs = 0;
  if (!s_seen || me.x != s_px || me.y != s_py || me.z != s_pz ||
      ox != s_ox || oy != s_oy || oz != s_oz) {
    s_seen = true;
    s_px = me.x; s_py = me.y; s_pz = me.z;
    s_ox = ox;   s_oy = oy;   s_oz = oz;
    s_ticMs = nowMs;
  }

  const double r = ekm_smoothratio((double)(nowMs - s_ticMs));
  *ex = ekm_lerp_pos(ox, me.x, r);
  *ey = ekm_lerp_pos(oy, me.y, r);
  *ez = ekm_lerp_pos(oz, me.z, r);
}

// The gate's evidence: every step of the MODE_MENU gate reads correct (Menu_Open sets the bit, menus.cpp:5509; offset
// 598 verified) yet the aimbot aimed over the game's menu, so this prints what the bytes say. Expected with the menu
// open: 0x05 (MODE_GAME | MODE_MENU).
static void aim_log_gate(int gm, bool readOk, bool bailed) {
  if (!g_cfg.debuglog) return;
  static unsigned lastMs = 0;
  static bool logged = false;
  const unsigned now = GetTickCount();
  if (logged && now - lastMs < 1000) return;
  logged = true; lastMs = now;
  ek_log("aim gate: gm=0x%02X read=%d bailed=%d slotrva=0x%08X",
         readOk ? (unsigned)(gm & 0xFF) : 0u, readOk ? 1 : 0, bailed ? 1 : 0,
         player_rva());
}

bool aim_available(void) { return player_ready() && cansee_ready(); }

// What the acquisition produces for the winner, from one ladder pass: the view angles that face it and the world points
// the shot aims at. The VIEW gets an angle (it tracks what the player SEES, interpolated); the SHOT gets a RAW point,
// since the engine hitscans raw positions and fires a tic later from a different muzzle.
struct EkAimPick {
  int yaw;        // Build units, from the INTERPOLATED eye: what the aimbot writes
  int horiz;      // q16horiz from the interpolated eye, less horizoff
  // The winner's plain, UN-LED geometry from the interpolated eye, and which sprite it is: the triggerbot's question is
  // where the player is POINTING. There is no shot-angle pair: silent aim publishes the POINT and solves at the muzzle
  // that fires (rawX/Y/Z and ledX/Y/Z below).
  int sprite;
  int viewYaw;
  int viewHoriz;
  int rawX, rawY, rawZ;
  int ledX, ledY, ledZ;
  // What the refinement pass measured, for the triggerbot's hold-fire rule: marginZ is the z between the chosen point
  // and the nearer edge of the visible run, range2d the range it was measured at (tbot.h's ekm_shot_placeable). Both
  // ZERO when the refinement pass did not run = no measurement.
  int marginZ;
  int range2d;
  int spanLo, spanHi, spanMid, spanHeight;
  int reachable;
  // The view's fallback: the best candidate whose pitch the VIEW can hold, or fallbackHave 0. Read by aim_frame only;
  // the lock, marker and silent aim's point stay the winner's.
  int fallbackHave;
  int fallbackYaw, fallbackHoriz, fallbackSprite;
  // The winner's own record, so the cheap terms can be re-run every frame: selection runs once a tic, the arithmetic
  // that turns the winner into an angle every frame (aim_refresh). Copied, since s_targets is rebuilt by the next
  // targets_collect.
  EkTarget target;
};

// Silent aim's answer is a world point, not an angle. Selection needs ek_cansee (legal only in SwapBuffers) but
// A_ShootWithZvel runs mid-tic, and an angle is valid only for the muzzle it was solved from (on a jetpack the player
// rises 2048 z a tic, so a published angle landed 2048 high). aim_solution_at solves at the firing muzzle.
static int s_shotRawX = 0, s_shotRawY = 0, s_shotRawZ = 0;
static int s_shotLedX = 0, s_shotLedY = 0, s_shotLedZ = 0;
static bool s_shotHave = false;
static unsigned s_shotMs = 0;

// The triggerbot's lock, published on the same frame and cleared on the same paths. Separate statics: the shot solution
// is published only while silent aim is on, and a triggerbot gated on that would quietly do nothing.
static bool s_lockHave = false;
static unsigned s_lockMs = 0;
static int s_lockSprite = -1;
static int s_lockYaw = 0, s_lockHoriz = 0;
static int s_lockMyYaw = 0, s_lockMyHoriz = 0;
static int s_lockRawX = 0, s_lockRawY = 0, s_lockRawZ = 0;
static int s_lockLedX = 0, s_lockLedY = 0, s_lockLedZ = 0;
static int s_lockMarginZ = 0, s_lockRange2d = 0;
static int s_lockSpanLo = -1, s_lockSpanHi = -1, s_lockSpanMid = -1,
           s_lockSpanH = 0;
static int s_lockHorizoff = 0;
static int s_lockReachable = 1;   // the winner's reach flag

// Why a candidate never became the answer, in numbers: the old 64-probe cap was a hard ceiling on range (46 candidates,
// 16 probed, nothing accepted past 13432 units) and is gone; `never probed` at 0 beside a rising `farthest ever`
// confirms it. Every term keeps CUMULATIVE counts (rate-limited samples were read as rates) and its FARTHEST refusal.
struct AimFar {
  int dist, sprite, pic;
  int d1, d2;
};

static void aim_far_note(AimFar* f, bool first, int* ever, int dist,
                         int sprite, int pic, int d1, int d2) {
  if (dist > *ever) *ever = dist;
  if (!first && dist <= f->dist) return;
  f->dist = dist; f->sprite = sprite; f->pic = pic; f->d1 = d1; f->d2 = d2;
}

struct AimFrameStats {
  int cand;        // survivors of the cheap filters
  int probed;      // how many of those the probe loop reached: now always all
  int probes;      // selection's cansee probes, uncapped and reported as a cost
  int spanProbes;  // the refinement's casts, on the refinement's OWN budget
  int spanHitscan; // 1 when those casts were hitscans, 0 when they were cansee
  int rangeSkips, rangeDist, rangeSprite, rangePic;
  int coneSkips, coneDist, coneSprite, conePic, coneOff, coneLimit;
  int crosshair;   // 0 in Closest mode, where the cone is never asked
  // The terms that used to refuse in silence: hittable (a Recon is absent from A_Spawn's cstat |= 257 list,
  // game.cpp:2020-2043, and gets the bits only inside `if (canSeePlayer)`, actors.cpp:1247-1249, so an inactive one is
  // refused here) and the cansee ladder.
  int hitSkips, hitDist, hitSprite, hitPic, hitCstat, hitXrep;
  int seeSkips, seeDist, seeSprite, seePic, seeRungs, seeLadder;
  int pitchSkips, pitchDist, pitchSprite, pitchPic, pitchNeed, pitchDz;
  AimFar hitFar, rangeFar, coneFar, seeFar, pitchFar;
  int acceptHave, acceptDist, acceptSprite, acceptPic;
  // This pass's reach casts, the NEAREST candidate a shot could not reach and what its ray stopped at, and the winner
  // today's order would have picked, so the log says whether reach changed the answer.
  int reachCasts;
  int nuHave, nuDist, nuSprite, nuPic, nuHitSprite, nuHitWall, nuHitSect;
  int legHave, legTier, legOff, legDist, legSprite;
};

// Cumulative reach totals, never reset, advanced only with the Debug log on. `unknown` is a subset of `reachable`;
// `decided` counts passes whose winner differs from today's order.
struct AimReachTotals {
  unsigned casts, reachable, unknown, unreachable, decided;
};
static AimReachTotals s_reach = {};

// Cumulative: every line above is rate limited, and one `aim skip cone:` a second says nothing about 1 refusal or 400.
// Never reset; advanced only with the Debug log on.
struct AimRefuseTotals {
  unsigned hittable, range, cone, cansee, pitch;
};
static AimRefuseTotals s_refuse = {};

struct AimFarTotals {
  int hittable, range, cone, cansee, pitch, accepted;
};
static AimFarTotals s_far = {};

static int aim_far_append(char* buf, int cap, int at, const char* name,
                          int skips, const AimFar& f, int ever,
                          const char* detail) {
  if (at < 0 || at >= cap) return at;
  const char* sep = at > 0 ? ", " : "";
  const int n = skips <= 0
      ? snprintf(buf + at, (size_t)(cap - at), "%s%s none (ever %d)",
                 sep, name, ever)
      : snprintf(buf + at, (size_t)(cap - at),
                 "%s%s sprite %d picnum %d at %d%s (ever %d)",
                 sep, name, f.sprite, f.pic, f.dist, detail, ever);
  if (n < 0) return at;
  return at + n >= cap ? cap - 1 : at + n;
}

static void aim_log_frame(const AimFrameStats& st, unsigned nowMs) {
  if (!g_cfg.debuglog) return;
  // Written whenever any term refused anything or a target was acquired: gated on the budget alone, the counters were
  // invisible in exactly the rooms the reports describe.
  const bool anySkip = st.hitSkips > 0 ||
                       st.rangeSkips > 0 || st.coneSkips > 0 ||
                       st.seeSkips > 0 || st.pitchSkips > 0 ||
                       st.acceptHave > 0;
  if (anySkip) {
    static unsigned lastMs = 0;
    if (lastMs == 0 || (unsigned)(nowMs - lastMs) >= 1000u) {
      lastMs = nowMs;
      // 512 since 15 counters at ten digits need 382 with the NUL. The tail matters: the monstatus counters are the
      // notenemy rule's kill switch and print LAST, and _TRUNCATE drops the tail silently.
      char collect[512];
      targets_refusal_summary(collect, sizeof(collect));
      // The mode is on the line because Closest does not ask the cone, so a missing `aim skip cone:` line could not
      // otherwise be told from a cone that refused nothing. The reach is early on the line: it is the quantity under
      // dispute and ek_log truncates at 1024.
      char accepted[96];
      if (st.acceptHave)
        snprintf(accepted, sizeof(accepted),
                 "sprite %d picnum %d at %d units, farthest ever %d",
                 st.acceptSprite, st.acceptPic, st.acceptDist, s_far.accepted);
      else
        snprintf(accepted, sizeof(accepted), "nothing, farthest ever %d",
                 s_far.accepted);

      char farthest[512];
      char detail[72];
      int at = 0;
      snprintf(detail, sizeof(detail), ", cstat 0x%04X xrep %d",
               (unsigned)st.hitFar.d1, st.hitFar.d2);
      at = aim_far_append(farthest, (int)sizeof(farthest), at, "hittable",
                          st.hitSkips, st.hitFar, s_far.hittable, detail);
      at = aim_far_append(farthest, (int)sizeof(farthest), at, "range",
                          st.rangeSkips, st.rangeFar, s_far.range, "");
      snprintf(detail, sizeof(detail), ", off %d units", st.coneFar.d1);
      at = aim_far_append(farthest, (int)sizeof(farthest), at, "cone",
                          st.coneSkips, st.coneFar, s_far.cone, detail);
      snprintf(detail, sizeof(detail), ", %d of %d rungs tried",
               st.seeFar.d1, st.seeFar.d2);
      at = aim_far_append(farthest, (int)sizeof(farthest), at, "cansee",
                          st.seeSkips, st.seeFar, s_far.cansee, detail);
      snprintf(detail, sizeof(detail), ", need q16horiz %d whole, dz %d",
               st.pitchFar.d1, st.pitchFar.d2);
      at = aim_far_append(farthest, (int)sizeof(farthest), at, "pitch",
                          st.pitchSkips, st.pitchFar, s_far.pitch, detail);
      // `never probed` stays on the line and reads zero on every frame: that zero beside the candidate count, with
      // `farthest ever` rising past 13432, confirms the ceiling is gone. The collect tail must fit 1024.
      ek_log("aim acquire: %s, %d candidates, %d probed, %d never probed; "
             "accepted %s; %d probes (uncapped, once per tic); reach %d casts "
             "(so far %u: %u ok incl %u unknown, %u blocked, %u decided); "
             "pass 2 took "
             "%d of %d (%s); refused so far: hittable %u, "
             "range %u, cone %u, cansee %u, pitch %u; farthest "
             "refused (Build units): %s; "
             "collect: %s",
             st.crosshair ? "crosshair" : "closest",
             st.cand, st.probed, st.cand - st.probed, accepted, st.probes,
             st.reachCasts, s_reach.casts, s_reach.reachable, s_reach.unknown,
             s_reach.unreachable, s_reach.decided,
             st.spanProbes, EKM_SPAN_BUDGET,
             st.spanHitscan ? "hitscan" : "cansee",
             s_refuse.hittable, s_refuse.range, s_refuse.cone,
             s_refuse.cansee, s_refuse.pitch, farthest, collect);
    }
  }
  if (st.hitSkips > 0) {
    static unsigned lastMs = 0;
    if (lastMs == 0 || (unsigned)(nowMs - lastMs) >= 1000u) {
      lastMs = nowMs;
      ek_log("aim skip hittable: %d not hittable; nearest sprite %d picnum %d "
             "at %d units, cstat 0x%04X xrep %d (wants (cstat & 0x8100) == "
             "0x0100 and xrep != 0)",
             st.hitSkips, st.hitSprite, st.hitPic, st.hitDist,
             (unsigned)st.hitCstat, st.hitXrep);
    }
  }
  if (st.rangeSkips > 0) {
    static unsigned lastMs = 0;
    if (lastMs == 0 || (unsigned)(nowMs - lastMs) >= 1000u) {
      lastMs = nowMs;
      ek_log("aim skip range: %d past range; nearest sprite %d picnum %d "
             "at %d units, limit %d",
             st.rangeSkips, st.rangeSprite, st.rangePic, st.rangeDist,
             g_cfg.aimrange);
    }
  }
  if (st.coneSkips > 0) {
    static unsigned lastMs = 0;
    if (lastMs == 0 || (unsigned)(nowMs - lastMs) >= 1000u) {
      lastMs = nowMs;
      ek_log("aim skip cone: %d outside the cone; nearest sprite %d picnum %d "
             "at %d units, off %d units (%d deg), limit %d units (%d deg)",
             st.coneSkips, st.coneSprite, st.conePic, st.coneDist,
             st.coneOff, st.coneOff * 360 / EKM_YAW_UNITS,
             st.coneLimit, g_cfg.aimcone);
    }
  }
  if (st.seeSkips > 0) {
    static unsigned lastMs = 0;
    if (lastMs == 0 || (unsigned)(nowMs - lastMs) >= 1000u) {
      lastMs = nowMs;
      ek_log("aim skip cansee: %d with no visible rung; nearest sprite %d "
             "picnum %d at %d units, %d of %d rungs tried and all refused "
             "(eye to body, aim point %d)",
             st.seeSkips, st.seeSprite, st.seePic, st.seeDist,
             st.seeRungs, st.seeLadder, g_cfg.aimpoint);
    }
  }
  if (st.pitchSkips > 0) {
    static unsigned lastMs = 0;
    if (lastMs == 0 || (unsigned)(nowMs - lastMs) >= 1000u) {
      lastMs = nowMs;
      ek_log("aim drop: %d past the shot's pitch; nearest sprite %d picnum %d "
             "at %d units -- need q16horiz %d whole, the shot reaches %d..%d; "
             "dz=%d",
             st.pitchSkips, st.pitchSprite, st.pitchPic, st.pitchDist,
             st.pitchNeed, EKM_SHOT_HORIZ_MIN, EKM_SHOT_HORIZ_MAX, st.pitchDz);
    }
  }
}

// The nearest candidate no shot could reach, what stopped it and who won; one line a second, only on a pass that had
// one. The blocker's picnum is read here, not per candidate.
static void aim_log_reach(const AimFrameStats& st, unsigned nowMs, int winSprite,
                          int winDist, int winReach) {
  if (!g_cfg.debuglog || !st.nuHave) return;
  static unsigned lastMs = 0;
  if (lastMs != 0 && (unsigned)(nowMs - lastMs) < 1000u) return;
  lastMs = nowMs;

  char blocker[128];
  if (st.nuHitSprite >= 0) {
    int pic = 0;
    const bool ok = targets_sprite_picnum(st.nuHitSprite, &pic);
    snprintf(blocker, sizeof(blocker), "SPRITE %d picnum %s%d%s", st.nuHitSprite,
             ok ? "" : "UNREADABLE(", pic, ok ? "" : ")");
  } else if (st.nuHitWall >= 0) {
    EkWallFace face;
    const bool ok = wallstate_face(st.nuHitWall, &face);
    if (ok)
      snprintf(blocker, sizeof(blocker), "WALL %d picnum %d overpicnum %d",
               st.nuHitWall, face.picnum, face.overpicnum);
    else
      snprintf(blocker, sizeof(blocker), "WALL %d (record unreadable)", st.nuHitWall);
  } else {
    snprintf(blocker, sizeof(blocker), "the FLOOR or CEILING of sect %d", st.nuHitSect);
  }

  const char* outcome = !winReach ? "nothing was reachable, so today's pick stands"
                      : winDist > st.nuDist ? "a FARTHER reachable enemy won"
                                            : "a nearer reachable enemy won";
  ek_log("aim reach: nearest unreachable sprite %d picnum %d at %d -- the ray "
         "from the muzzle stopped at %s; winner sprite %d at %d: %s%s%d)",
         st.nuSprite, st.nuPic, st.nuDist, blocker, winSprite, winDist, outcome,
         (st.legHave && st.legSprite != winSprite) ? " (today's order picked sprite "
                                                   : " (today's order agrees, sprite ",
         st.legHave ? st.legSprite : -1);
}

// The whitelist's picnums are confirmed against AA's own names.h before any is used: a picnum that means something else
// in another mod must not have its head aimed at. Until names.h is parsed no target has a band (today's Head). 0
// unchecked, 1 confirmed, 2 dropped.
static unsigned char s_headOk[EK_AA_HEAD_TILE_COUNT];
static bool s_headChecked = false;

static void aim_head_verify(void) {
  if (s_headChecked || !tilenames_ready()) return;
  s_headChecked = true;
  int ok = 0, named = 0;
  for (int i = 0; i < EK_AA_HEAD_TILE_COUNT; i++) {
    const EkAaHeadTile& row = k_ek_aa_head_tiles[i];
    const char* const nm = tilenames_name(row.picnum);
    if (ekm_aa_head_name_ok(row, nm)) {
      s_headOk[i] = 1;
      ok++;
      if (row.inNamesH) named++;
    } else {
      s_headOk[i] = 2;
      ek_log("aim head: DROPPED %s -- names.h calls picnum %d \"%s\", expected %s",
             row.name, (int)row.picnum, nm ? nm : "(no name)",
             row.inNamesH ? "that name" : "none (AADEFS.CON only)");
    }
  }
  ek_log("aim head: %d of %d headshot picnums confirmed against names.h "
         "(%d named there, %d AADEFS.CON-only and absent as expected)",
         ok, EK_AA_HEAD_TILE_COUNT, named, ok - named);
}

static EkAaHeadBand aim_head_band(const EkTarget& t) {
  int idx = -1;
  if (g_cfg.aimpoint) {
    aim_head_verify();
    idx = ekm_aa_head_index((int)t.picnum);
    if (idx >= 0 && s_headOk[idx] != 1) idx = -1;
  }
  bool haveMtype = false;
  int mtype = 0;
  if (idx >= 0 && k_ek_aa_head_tiles[idx].mtypeYsize > 0) {
    haveMtype = gamevars_mtype(t.sprite, &mtype);
  }
  return ekm_aa_head_band(g_cfg.aimpoint, idx, (unsigned)t.cstat, t.th,
                          haveMtype, mtype);
}

static unsigned char s_headLogged[EK_MAXTILES_ARR];

static void aim_head_log(const EkTarget& t, const EkAaHeadBand& b, int rawZ) {
  if (!g_cfg.debuglog || !g_cfg.aimpoint) return;
  if (t.picnum < 0 || t.picnum >= EK_MAXTILES_ARR) return;
  const int idx = ekm_aa_head_index((int)t.picnum);
  const bool moved = idx >= 0 && b.line >= 0.0f &&
                     b.line != (float)(k_ek_aa_head_tiles[idx].dive ? 7 : 6) / 8.0f;
  const unsigned char bit = !b.banded ? 1 : moved ? 4 : 2;
  if (s_headLogged[t.picnum] & bit) return;
  s_headLogged[t.picnum] |= bit;
  const double h = t.heightZ > 0.0f ? (double)t.heightZ : 0.0;
  const double finalFrac = h > 0.0 ? (double)(t.feetZ - rawZ) / h : 0.0;
  const char* why = b.banded ? "AA headshot band"
                  : idx < 0 ? "not on AA's list"
                  : s_headOk[idx] != 1 ? "unconfirmed by names.h"
                  : (t.cstat & 128u) ? "centred sprite, no band"
                  : "line too near the top";
  ek_log("aim head: picnum %d%s%s line %.4f, Head asks %.4f (floor %.4f), "
         "shot at %.4f -- %s",
         (int)t.picnum, idx >= 0 ? " " : "", idx >= 0 ? k_ek_aa_head_tiles[idx].name : "",
         (double)b.line, (double)b.headFrac, (double)b.floor, finalFrac, why);
}

// The whole acquisition: cheap filters, nearest-first order, probe ladder, leading and the solve. Selection is driven
// by the EYE solve alone (`muzzleZ` reaches the refinement pass only), so switching silent aim on cannot change which
// enemy the aimbot picks: WHICH enemy is asked from the eye, WHERE on it to shoot from the muzzle.
static bool aim_acquire(const PlayerSnap& me, int eyeX, int eyeY, int eyeZ,
                        int muzzleZ, unsigned nowTickMs, int projSpeed,
                        EkAimPick* out) {
  const int count = targets_collect(s_targets, EK_TARGET_MAX);
  if (count <= 0) return false;

  const int myYaw    = (me.ang >> 16) & (EKM_YAW_UNITS - 1);
  const int cone     = g_cfg.aimcone * EKM_YAW_UNITS / 360;
  const bool crosshair = (g_cfg.aimtarget == 0);

  const bool leadThis = projSpeed > 0;

  int probes = 0;
  bool have = false;
  int bestReach = 0, bestTier = 0, bestOff = 0, bestDist = 0;
  const bool reachByRay = hitscan_ready();
  int bestYaw = 0, bestHoriz = 0;
  int bestRawX = 0, bestRawY = 0, bestRawZ = 0, bestRawDist = 0;
  int bestVx = 0, bestVy = 0, bestVz = 0;
  int bestSprite = -1, bestViewYaw = 0, bestViewHoriz = 0;
  int bestCand = -1, bestZShift = 0, bestBaseX = 0, bestBaseY = 0;
  int bestBaseDist = 0, bestAimDist = 0, bestAimYaw = 0;
  EkAaHeadBand bestBand = ekm_aa_head_band(0, -1, 0, 0, false, 0);

  // The best candidate the VIEW can actually be pointed at, beside the winner and changing nothing about it: selection
  // uses the SHOT's bound, so the winner's pitch may be one P_GetInput clamps away and the aimbot then did not turn at
  // all. A second accumulator run through ekm_cand_better so the two orders cannot drift.
  bool fbHave = false;
  int fbReach = 0, fbTier = 0, fbOff = 0, fbDist = 0;
  int fbYaw = 0, fbHoriz = 0, fbSprite = -1;

  AimFrameStats stats = {};
  int nCand = 0;
  memset(s_admitted, 0, sizeof(s_admitted));
  for (int i = 0; i < count; i++) {
    const EkTarget& t = s_targets[i];

    if (!ekm_target_hittable(t.xrep, t.cstat)) {
      // The range is measured inside the refusal, not before the test: hoisting it would pay a sqrt for every sprite
      // the cheapest term rejects, with the Debug row off too.
      if (g_cfg.debuglog) {
        const int hdx = t.x - me.x, hdy = t.y - me.y;
        const int hd = (int)sqrt((double)hdx * hdx + (double)hdy * hdy);
        if (stats.hitSkips == 0 || hd < stats.hitDist) {
          stats.hitDist = hd; stats.hitSprite = t.sprite;
          stats.hitPic = (int)t.picnum; stats.hitCstat = (int)t.cstat;
          stats.hitXrep = (int)t.xrep;
        }
        aim_far_note(&stats.hitFar, stats.hitSkips == 0, &s_far.hittable, hd,
                     t.sprite, (int)t.picnum, (int)t.cstat, (int)t.xrep);
        stats.hitSkips++;
        s_refuse.hittable++;
      }
      continue;
    }

    // A hostile Sidegun is collected for the ESP and never a candidate; also kept out of s_admitted, so a reach ray
    // that meets one reads BLOCKED.
    if (t.attachment) {
      if (g_cfg.debuglog) targets_note_attachment_refusal();
      continue;
    }

    // Asleep and unflagged, so the engine discards the hit (targets.h); ahead of the monstatus term as in the trigger
    // gate.
    if (t.asleepDiscardsHits) {
      if (g_cfg.debuglog) targets_note_asleep_refusal();
      continue;
    }

    // `monstatus -1` on an AWAKE actor means it runs no hit handling (targets.h), so a shot at it is always wasted.
    // Targeting only: targets_collect still returns it so the ESP draws it. A failed read leaves it shootable. Narrowed
    // to STAT_ACTOR: a dormant STAT_ZOMBIE at -1 has never run its CON and is engaged.
    if (!ekm_target_monstatus_shootable(t.haveMonstatus, t.monstatus, t.statnum)) {
      if (g_cfg.debuglog) targets_note_monstatus_refusal();
      continue;
    }

    // A crashing Recon is collected for the ESP's "Crashing" label but keeps a hittable cstat at 1 HP, so only its
    // state refuses it here (as targets_sprite_targetable does for the trigger). An armed torso is counted apart.
    if (t.state == EKA_SELFDESTRUCT) {
      if (g_cfg.debuglog) targets_note_selfdestruct_refusal();
      continue;
    }
    if (ekm_actor_state_worm_refused(t.state)) {
      if (g_cfg.debuglog) targets_note_worm_refusal();
      continue;
    }
    if (!ekm_actor_state_targetable(t.state)) continue;
    ekm_bit_set(s_admitted, EK_MAXSPRITES, t.sprite);

    const int dx = t.x - me.x, dy = t.y - me.y;
    const int dist2d = (int)sqrt((double)dx * dx + (double)dy * dy);
    if (dist2d > g_cfg.aimrange) {
      if (g_cfg.debuglog) {
        if (stats.rangeSkips == 0 || dist2d < stats.rangeDist) {
          stats.rangeDist = dist2d; stats.rangeSprite = t.sprite;
          stats.rangePic = (int)t.picnum;
        }
        aim_far_note(&stats.rangeFar, stats.rangeSkips == 0, &s_far.range,
                     dist2d, t.sprite, (int)t.picnum, 0, 0);
        stats.rangeSkips++;
        s_refuse.range++;
      }
      continue;
    }

    if (crosshair) {
      const int off = ekm_angular_offset(myYaw, ekm_aim_yaw(dx, dy));
      if (off > cone) {
        if (g_cfg.debuglog) {
          if (stats.coneSkips == 0 || dist2d < stats.coneDist) {
            stats.coneDist = dist2d; stats.coneSprite = t.sprite;
            stats.conePic = (int)t.picnum; stats.coneOff = off;
          }
          aim_far_note(&stats.coneFar, stats.coneSkips == 0, &s_far.cone,
                       dist2d, t.sprite, (int)t.picnum, off, 0);
          stats.coneSkips++;
          s_refuse.cone++;
        }
        continue;
      }
    }

    s_candIdx[nCand] = i;
    s_candDist[nCand] = dist2d;
    nCand++;
  }

  // Nearest first: this no longer decides WHO is examined (everything that survived is) but still decides ties cheaply;
  // the ladder's early exit means the nearest is usually settled first.
  for (int a = 0; a < nCand; a++) {
    int pick = a;
    for (int b = a + 1; b < nCand; b++)
      if (s_candDist[b] < s_candDist[pick]) pick = b;
    if (pick != a) {
      const int ti = s_candIdx[a];  s_candIdx[a] = s_candIdx[pick];  s_candIdx[pick] = ti;
      const int td = s_candDist[a]; s_candDist[a] = s_candDist[pick]; s_candDist[pick] = td;
    }
  }

  for (int k = 0; k < nCand; k++) {
    stats.probed++;
    const EkTarget& t = s_targets[s_candIdx[k]];
    const int dist2d = s_candDist[k];
    const int dx = t.x - me.x, dy = t.y - me.y;
    const int yaw = ekm_aim_yaw(dx, dy);
    const int off = ekm_angular_offset(myYaw, yaw);

    // The ladder: the first rung the engine can see becomes the aim point; every probe is counted so `probes` is the
    // real cost. Selection probes from the EYE and stays there because it selects; the refinement pass asks from the
    // MUZZLE (ZOFFSET6 = 1024 z units plus pyoff, roughly chest-to-eye, the whole of the parallax it corrects).
    int aimZ = 0;
    bool visible = false;
    // The rung counter is hoisted so the refusal can report it. No sector clamp on the rungs (World Tour paid for that
    // rule, aimwt.cpp:326-331): an enemy whose head probe sits above its own sector ceiling loses its upper rungs and
    // is aimed at its obscured torso. Head's rungs come from this target's headshot band.
    const EkAaHeadBand band = aim_head_band(t);
    const int nRungs = ekm_aim_rung_count(g_cfg.aimpoint, band.headFrac, band.floor);
    int rung = 0;
    for (; rung < nRungs; rung++) {
      const int z = ekm_aim_z(t.feetZ, t.heightZ, rung, g_cfg.aimpoint,
                              band.headFrac, band.floor);
      probes++;
      if (ek_cansee(me.x, me.y, me.z, me.sect, t.x, t.y, z, t.sect)) {
        aimZ = z;
        visible = true;
        break;
      }
    }
    if (!visible) {
      // The suspect for the window report, no more: troopers on a ledge went unengaged through a broken window at the
      // same height; a window opening restricts which heights pass, so this term fits, but fitting a symptom is not a
      // diagnosis.
      if (g_cfg.debuglog) {
        if (stats.seeSkips == 0 || dist2d < stats.seeDist) {
          stats.seeDist = dist2d; stats.seeSprite = t.sprite;
          stats.seePic = (int)t.picnum; stats.seeRungs = rung;
          stats.seeLadder = nRungs;
        }
        aim_far_note(&stats.seeFar, stats.seeSkips == 0, &s_far.cansee, dist2d,
                     t.sprite, (int)t.picnum, rung, nRungs);
        stats.seeSkips++;
        s_refuse.cansee++;

        // Why all four rungs failed on a visible target (E1L3's wall turrets, ROTATEGUN 2360): every rung shares
        // t.x/t.y, so a face sprite flush on a wall can have its coordinates at or behind the wall plane; it renders,
        // but a ray to that point crosses solid geometry, intermittently by angle.
        static unsigned s_lastRungMs = 0;
        if (s_lastRungMs == 0 ||
            (unsigned)(nowTickMs - s_lastRungMs) >= 1000u) {
          s_lastRungMs = nowTickMs;
          const int z0 = ekm_aim_z(t.feetZ, t.heightZ, 0, g_cfg.aimpoint,
                                   band.headFrac, band.floor);
          char rungZ[96];
          int rzAt = 0;
          rungZ[0] = 0;
          for (int r = 0; r < nRungs && rzAt >= 0 && rzAt < (int)sizeof(rungZ); r++) {
            const int n = snprintf(rungZ + rzAt, sizeof(rungZ) - (size_t)rzAt, "%s%d",
                                   r ? " " : "",
                                   ekm_aim_z(t.feetZ, t.heightZ, r, g_cfg.aimpoint,
                                             band.headFrac, band.floor));
            if (n < 0) break;
            rzAt += n;
          }
          const int dx = me.x - t.x, dy = me.y - t.y;
          const int d  = dist2d > 0 ? dist2d : 1;
          int pull[2] = { 0, 0 };
          const int pullBy[2] = { 64, 192 };
          for (int p = 0; p < 2; p++) {
            const int px = t.x + (int)((long long)dx * pullBy[p] / d);
            const int py = t.y + (int)((long long)dy * pullBy[p] / d);
            pull[p] = ek_cansee(me.x, me.y, me.z, me.sect, px, py, z0, t.sect) ? 1 : 0;
          }
          ek_log("aim rung detail: sprite %d picnum %d at %d units -- eye "
                 "(%d,%d,%d) sect %d, target (%d,%d) sect %d, feetZ %d height "
                 "%d; rung z: %s; pulled toward eye: 64u=%s 192u=%s",
                 t.sprite, (int)t.picnum, dist2d, me.x, me.y, me.z, me.sect,
                 t.x, t.y, t.sect, t.feetZ, (int)t.heightZ, rungZ,
                 pull[0] ? "VISIBLE" : "blocked",
                 pull[1] ? "VISIBLE" : "blocked");
        }
      }
      continue;
    }

    // Leading decides where to shoot, never whether a target is visible: range, cone and the ladder ran on where the
    // enemy IS, so leading adds no cansee probe. Interpolate first, then lead from the interpolated point; leading from
    // the stale tic position would put the staircase back.
    int tix = t.x, tiy = t.y, tiz = t.z;
    int vx = 0, vy = 0, vz = 0;
    aim_track(t, nowTickMs, &tix, &tiy, &tiz, &vx, &vy, &vz);

    const int baseX = tix, baseY = tiy, baseZ = aimZ + (tiz - t.z);

    const int bdx = baseX - eyeX, bdy = baseY - eyeY;
    int baseYaw = yaw, baseDist = dist2d;
    if (bdx != 0 || bdy != 0) {
      baseDist = (int)sqrt((double)bdx * bdx + (double)bdy * bdy);
      baseYaw = ekm_aim_yaw(bdx, bdy);
    }

    int aimX = baseX, aimY = baseY, leadZ = baseZ;
    if (leadThis) {
      int lx = 0, ly = 0, lz = 0;
      ekm_lead_point(baseX, baseY, baseZ, vx, vy, vz, eyeX, eyeY,
                     projSpeed, &lx, &ly, &lz);
      aimX  = ekm_lead_scaled(baseX, lx, g_cfg.aimleadscale);
      aimY  = ekm_lead_scaled(baseY, ly, g_cfg.aimleadscale);
      leadZ = ekm_lead_scaled(baseZ, lz, g_cfg.aimleadscale);
    }
    const int adx = aimX - eyeX, ady = aimY - eyeY;
    int aimYaw = baseYaw, aimDist = baseDist;
    if (adx != 0 || ady != 0) {
      aimDist = (int)sqrt((double)adx * adx + (double)ady * ady);
      aimYaw = ekm_aim_yaw(adx, ady);
    }

    // The solved total, less what the engine already contributes: effective pitch is q16horiz PLUS q16horizoff. Asked
    // of the SHOT's bound, not the view's, whatever silent aim is set to (silent aim writes q16horiz inside the shot
    // call, after P_GetInput's clamp, player.cpp:3179). Leading moves the shot, it does not lose the target.
    const int ledYaw = aimYaw;
    int q16h = ekm_q16horiz_for(leadZ - eyeZ, aimDist) - me.horizoff;
    if (!ekm_shot_horiz_reachable(q16h)) {
      q16h = ekm_q16horiz_for(baseZ - eyeZ, baseDist) - me.horizoff;
      if (!ekm_shot_horiz_reachable(q16h)) {
        // Instrumented because the symptom is silence: a candidate dropped here draws no ESP state, publishes no lock
        // and moves no marker. The bound is the SHOT's, so a line means geometry past what a ray can express. Written
        // by aim_log_frame.
        if (g_cfg.debuglog) {
          if (stats.pitchSkips == 0 || dist2d < stats.pitchDist) {
            stats.pitchDist = dist2d; stats.pitchSprite = t.sprite;
            stats.pitchPic = (int)t.picnum; stats.pitchNeed = q16h >> 16;
            stats.pitchDz = baseZ - eyeZ;
          }
          aim_far_note(&stats.pitchFar, stats.pitchSkips == 0, &s_far.pitch,
                       dist2d, t.sprite, (int)t.picnum, q16h >> 16,
                       baseZ - eyeZ);
          stats.pitchSkips++;
          s_refuse.pitch++;
        }
        continue;
      }
      aimYaw = baseYaw;
    }

    // Two tiers: a FROZEN enemy and a CHOKING trooper arrive at hp <= 0, alive and worth a shot but not urgent; demoted
    // behind everything with health left rather than dropped, which is also what a bad hp read should cost.
    const int tier = t.hp > 0 ? 0 : 1;

    // Can a shot reach it? cansee tests no sprites (aimwt.cpp:17), so a near enemy behind a vehicle or fence sprite won
    // while no bullet could land. The triggerbot's origin (the muzzle) and hit rule, at this rung raw, quantised as
    // the refinement's rays are. No answer is reach-unknown and demotes nothing; demoted, never dropped.
    int reach = 1;
    if (reachByRay) {
      const EkShotRay rray = ekm_shot_ray_at(t.x - me.x, t.y - me.y,
                                             aimZ - muzzleZ, dist2d);
      EkHitData rhit = { 0, 0, 0, -1, -1, -1 };
      const bool rok = ek_hitscan(me.x, me.y, muzzleZ, me.sect,
                                  rray.vx, rray.vy, rray.vz, &rhit);
      const int rclass = tbot_ray_class_of(rok, rhit.sprite, rhit.wall, s_admitted);
      reach = ekm_ray_reachable(rclass) ? 1 : 0;
      stats.reachCasts++;
      if (g_cfg.debuglog) {
        s_reach.casts++;
        if (reach) s_reach.reachable++; else s_reach.unreachable++;
        if (rclass == TBOT_RAY_UNKNOWN) s_reach.unknown++;
        if (!reach && (!stats.nuHave || dist2d < stats.nuDist)) {
          stats.nuHave = 1; stats.nuDist = dist2d; stats.nuSprite = t.sprite;
          stats.nuPic = (int)t.picnum; stats.nuHitSprite = rhit.sprite;
          stats.nuHitWall = rhit.wall; stats.nuHitSect = rhit.sect;
        }
      }
    }

    if (g_cfg.debuglog &&
        ekm_cand_better(stats.legHave != 0, crosshair, tier, off, dist2d,
                        stats.legTier, stats.legOff, stats.legDist)) {
      stats.legHave = 1; stats.legTier = tier; stats.legOff = off;
      stats.legDist = dist2d; stats.legSprite = t.sprite;
    }

    if (ekm_horiz_reachable(q16h) &&
        ekm_cand_better(fbHave, crosshair, reach, tier, off, dist2d,
                        fbReach, fbTier, fbOff, fbDist)) {
      fbHave = true;
      fbReach = reach; fbTier = tier; fbOff = off; fbDist = dist2d;
      fbYaw = aimYaw; fbHoriz = q16h; fbSprite = t.sprite;
    }

    if (!ekm_cand_better(have, crosshair, reach, tier, off, dist2d,
                         bestReach, bestTier, bestOff, bestDist))
      continue;

    have = true;
    bestReach = reach; bestTier = tier; bestOff = off; bestDist = dist2d;
    bestYaw = aimYaw; bestHoriz = q16h;
    bestRawX = t.x; bestRawY = t.y; bestRawZ = aimZ;
    bestRawDist = dist2d;
    bestVx = vx; bestVy = vy; bestVz = vz;
    bestSprite = t.sprite;
    bestViewYaw = baseYaw;
    bestViewHoriz = ekm_q16horiz_for(baseZ - eyeZ, baseDist) - me.horizoff;
    bestCand = s_candIdx[k];
    bestBand = band;
    bestZShift = tiz - t.z;
    bestBaseX = baseX; bestBaseY = baseY;
    bestBaseDist = baseDist; bestAimDist = aimDist; bestAimYaw = ledYaw;
  }

  stats.cand = nCand;
  stats.probes = probes;
  stats.coneLimit = cone;
  stats.crosshair = crosshair ? 1 : 0;
  if (!have) {
    aim_log_frame(stats, nowTickMs);
    return false;
  }

  if (g_cfg.debuglog) {
    stats.acceptHave = 1;
    stats.acceptDist = bestDist;
    stats.acceptSprite = bestSprite;
    stats.acceptPic = bestCand >= 0 ? (int)s_targets[bestCand].picnum : 0;
    if (bestDist > s_far.accepted) s_far.accepted = bestDist;
    if (stats.legHave && stats.legSprite != bestSprite) s_reach.decided++;
    aim_log_reach(stats, nowTickMs, bestSprite, bestDist, bestReach);
  }

  // The refinement pass: refine the winner's HEIGHT, and only refine. Selection answers with the first rung it can see
  // (behind cover, the one nearest the silhouette edge), so the winner gets a finer ladder and aims at the middle of
  // the longest visible run. It may never reject a target, and uses all nine samples or none (EKM_SPAN_BUDGET).
  if (bestCand >= 0 && EKM_SPAN_SAMPLES > EKM_SPAN_BUDGET) {
    static bool saidOnce = false;
    if (!saidOnce) {
      saidOnce = true;
      ek_log("aim span: SKIPPED -- %d samples do not fit pass 2's budget of "
             "%d; the aim point stays pass 1's for the whole session",
             EKM_SPAN_SAMPLES, EKM_SPAN_BUDGET);
    }
  }
  int bestMarginZ = 0, bestMarginRange = 0;
  int bestSpanLo = -1, bestSpanHi = -1, bestSpanMid = -1, bestSpanH = 0;
  if (bestCand >= 0 && EKM_SPAN_SAMPLES <= EKM_SPAN_BUDGET) {
    const EkTarget& bt = s_targets[bestCand];
    unsigned char vis[EKM_SPAN_SAMPLES];
    // The ray, not the sightline, when the engine's hitscan is available; cansee is the fallback so a refused scan
    // costs precision, not the whole refinement.
    const bool byRay = hitscan_ready();
    int spanCasts = 0;
    for (int si = 0; si < EKM_SPAN_SAMPLES; si++) {
      // Probed from the MUZZLE, not the eye, which is this stage's whole point: the eye sees over a windowsill the
      // muzzle cannot, so the middle of an eye-visible span lands in the sill (shotgun and chaingun hit just below a
      // half-visible trooper; the pistol snaps onto the sprite, player.cpp:492-512). muzzleZ is me.z when pyoff failed.
      const int z = ekm_span_z(bt.feetZ, bt.heightZ, si, g_cfg.aimpoint,
                               bestBand.headFrac, bestBand.floor);
      spanCasts++;
      if (byRay) {
        // The ray that would actually be fired at this sample, quantised in yaw and pitch as the shot is (shotray.h). A
        // sample counts only when that ray lands on the target sprite itself, which answers glass, grates, other actors
        // and thin geometry for free.
        const EkShotRay ray = ekm_shot_ray_at(bt.x - me.x, bt.y - me.y,
                                              z - muzzleZ, bestRawDist);
        EkHitData hit;
        const bool ok = ek_hitscan(me.x, me.y, muzzleZ, me.sect,
                                   ray.vx, ray.vy, ray.vz, &hit);
        vis[si] = ekm_span_sample_hit(ok, ok ? (int)hit.sprite : -1, bt.sprite) ? 1 : 0;
      } else {
        vis[si] = ek_cansee(me.x, me.y, muzzleZ, me.sect, bt.x, bt.y, z, bt.sect) ? 1 : 0;
      }
    }
    stats.spanProbes = spanCasts;
    stats.spanHitscan = byRay ? 1 : 0;
    int spanFirst = -1, spanLast = -1, spanTight = 0;
    // The body and the range make the row's point affordable: the shot's pitch rounds to a whole horiz unit, so the
    // configured height is kept only with a quantum of room inside the visible run (aimsolve.h). On a target with a
    // headshot band only the band's samples may answer, down to its floor; with none visible this is today's rule.
    int spanInBand = 0;
    const int mid = ekm_span_mid_banded(vis, EKM_SPAN_SAMPLES, bestBand.floor,
                                        &spanInBand, &spanFirst, &spanLast,
                                        bt.heightZ, bestRawDist, g_cfg.aimpoint,
                                        &spanTight, bestBand.headFrac);
    const float spanHead = spanInBand ? bestBand.headFrac : EKM_HEAD_FRAC;
    const float spanFloor = spanInBand ? bestBand.floor : 0.0f;
    if (mid >= 0) {
      // The margin the chosen point actually has, carried out for the triggerbot: the quantity ekm_span_mid weighed
      // against the quantum, measured at the point it settled on. Unconditional; the decision it feeds is tbot.h's.
      bestMarginZ = (int)ekm_span_margin_z(
          mid, spanFirst, spanLast,
          bt.heightZ > 0.0f ? (double)bt.heightZ : 0.0, g_cfg.aimpoint,
          spanHead, spanFloor);
      bestMarginRange = bestRawDist;
      bestSpanLo = spanFirst; bestSpanHi = spanLast; bestSpanMid = mid;
      bestSpanH = (int)bt.heightZ;
      // Not even the midpoint has a quantum of margin: the target is smaller than the engine can be aimed at from this
      // range. The midpoint stands, but this case cost 36 of 39 chaingun rounds, so it gets a (rate limited) line.
      if (spanTight && g_cfg.debuglog) {
        static unsigned s_lastTightMs = 0;
        if (s_lastTightMs == 0 ||
            (unsigned)(nowTickMs - s_lastTightMs) >= EK_AIM_SPAN_PARTIAL_MS) {
          s_lastTightMs = nowTickMs;
          ek_log("aim quantum: sprite %d height %d at range %d -- one horiz "
                 "unit is %.0f z and the run %d..%d has less margin than that "
                 "anywhere; aiming at sample %d, the most forgiving point",
                 bt.sprite, (int)bt.heightZ, bestRawDist,
                 ekm_pitch_quantum_z(bestRawDist), spanFirst, spanLast, mid);
        }
      }
      const int spanZ = ekm_span_z(bt.feetZ, bt.heightZ, mid, g_cfg.aimpoint,
                                   spanHead, spanFloor);
      const int spanBaseZ = spanZ + bestZShift;
      int spanLeadZ = spanBaseZ;
      if (leadThis) {
        int lx = 0, ly = 0, lz = 0;
        ekm_lead_point(bestBaseX, bestBaseY, spanBaseZ, bestVx, bestVy, bestVz,
                       eyeX, eyeY, projSpeed, &lx, &ly, &lz);
        spanLeadZ = ekm_lead_scaled(spanBaseZ, lz, g_cfg.aimleadscale);
      }
      int spanHoriz = ekm_q16horiz_for(spanLeadZ - eyeZ, bestAimDist) - me.horizoff;
      int spanYaw = bestAimYaw;
      if (!ekm_shot_horiz_reachable(spanHoriz)) {
        spanHoriz = ekm_q16horiz_for(spanBaseZ - eyeZ, bestBaseDist) - me.horizoff;
        spanYaw = bestViewYaw;
      }
      if (ekm_shot_horiz_reachable(spanHoriz)) {
        if (g_cfg.debuglog) {
          const bool full = ekm_span_is_full(spanFirst, spanLast, EKM_SPAN_SAMPLES);
          static unsigned s_lastFullMs = 0, s_lastPartMs = 0;
          unsigned& lastMs = full ? s_lastFullMs : s_lastPartMs;
          const unsigned gap = full ? EK_AIM_SPAN_FULL_MS : EK_AIM_SPAN_PARTIAL_MS;
          if (lastMs == 0 || (unsigned)(nowTickMs - lastMs) >= gap) {
            lastMs = nowTickMs;
            const double bodyH = bt.heightZ > 0.0f ? (double)bt.heightZ : 0.0;
            const double was = bodyH > 0.0 ? (double)(bt.feetZ - bestRawZ) / bodyH : 0.0;
            ek_log("aim span: sprite %d run %d..%d of %d, aim at sample %d "
                   "(%.3f of body height, pass 1 had %.3f)%s",
                   bt.sprite, spanFirst, spanLast, EKM_SPAN_SAMPLES, mid,
                   (double)ekm_span_frac(mid, g_cfg.aimpoint, spanHead, spanFloor),
                   was, spanInBand ? ", inside AA's headshot band" : "");
          }
        }
        bestYaw = spanYaw;
        bestHoriz = spanHoriz;
        bestRawZ = spanZ;
        bestViewHoriz = ekm_q16horiz_for(spanBaseZ - eyeZ, bestBaseDist) - me.horizoff;
      }
    } else if (g_cfg.debuglog) {
      static unsigned s_lastBlindMs = 0;
      if (s_lastBlindMs == 0 ||
          (unsigned)(nowTickMs - s_lastBlindMs) >= EK_AIM_SPAN_PARTIAL_MS) {
        s_lastBlindMs = nowTickMs;
        ek_log("aim muzzle blind: sprite %d picnum %d -- the shot reaches none "
               "of %d samples (%s), keeping pass 1's height; muzzle is %d "
               "below the eye, feetZ %d height %d",
               bt.sprite, (int)bt.picnum, EKM_SPAN_SAMPLES,
               byRay ? "hitscan" : "cansee",
               muzzleZ - me.z, bt.feetZ, (int)bt.heightZ);
      }
    }
  }

  aim_log_frame(stats, nowTickMs);
  if (bestCand >= 0) aim_head_log(s_targets[bestCand], bestBand, bestRawZ);

  out->yaw = bestYaw;
  out->horiz = bestHoriz;
  out->sprite = bestSprite;
  out->viewYaw = bestViewYaw;
  out->viewHoriz = bestViewHoriz;

  // The shot's point, on raw positions throughout (raw target, raw lead origin): the engine hitscans raw sprite
  // positions. The angle is NOT solved here: it belongs to the muzzle the shot leaves from a tic later, where
  // ekm_shot_solve does it (led first, raw as fallback).
  int sx = bestRawX, sy = bestRawY, sz = bestRawZ;
  if (leadThis) {
    int lx = 0, ly = 0, lz = 0;
    ekm_lead_point(bestRawX, bestRawY, bestRawZ, bestVx, bestVy, bestVz,
                   me.x, me.y, projSpeed, &lx, &ly, &lz);
    sx = ekm_lead_scaled(bestRawX, lx, g_cfg.aimleadscale);
    sy = ekm_lead_scaled(bestRawY, ly, g_cfg.aimleadscale);
    sz = ekm_lead_scaled(bestRawZ, lz, g_cfg.aimleadscale);
  }

  out->rawX = bestRawX; out->rawY = bestRawY; out->rawZ = bestRawZ;
  out->ledX = sx; out->ledY = sy; out->ledZ = sz;
  out->marginZ = bestMarginZ;
  out->range2d = bestMarginRange;
  out->spanLo = bestSpanLo; out->spanHi = bestSpanHi;
  out->spanMid = bestSpanMid; out->spanHeight = bestSpanH;
  out->reachable = bestReach;
  out->fallbackHave = fbHave ? 1 : 0;
  out->fallbackYaw = fbYaw;
  out->fallbackHoriz = fbHoriz;
  out->fallbackSprite = fbSprite;
  if (bestCand >= 0) out->target = s_targets[bestCand];
  else               out->target.sprite = -1;
  return true;
}

// The cheap terms, re-run every frame for the winner already chosen: once-a-tic selection left the triggerbot's two
// published yaws describing different instants and the aim point stepping at 30 Hz. No ek_cansee, ek_hitscan,
// ek_updatesector or candidate loop here, just one sprite's lerp and the lead. Re-solves, never re-selects.
static EkAimViewPair aim_refresh(const PlayerSnap& me, int eyeX, int eyeY,
                                 int eyeZ, unsigned nowTickMs, int projSpeed,
                                 EkAimPick* pick) {
  const EkTarget& t = pick->target;

  int tix = t.x, tiy = t.y, tiz = t.z;
  int vx = 0, vy = 0, vz = 0;
  aim_track(t, nowTickMs, &tix, &tiy, &tiz, &vx, &vy, &vz);

  const int baseX = tix, baseY = tiy, baseZ = pick->rawZ + (tiz - t.z);

  // The un-led line and both of the triggerbot's angles out of one call. The degenerate fallback is the acquisition's
  // yaw and a range of EKM_YAW_UNITS: a target exactly on the eye has no direction, and a range of zero would solve the
  // pitch as level.
  const EkAimViewPair pair = ekm_aim_view_pair(me.ang, me.horizoff,
                                               eyeX, eyeY, eyeZ,
                                               baseX, baseY, baseZ,
                                               pick->viewYaw, EKM_YAW_UNITS);
  pick->viewYaw = pair.yaw;
  pick->viewHoriz = pair.q16horiz;

  int aimX = baseX, aimY = baseY, leadZ = baseZ;
  if (projSpeed > 0) {
    int lx = 0, ly = 0, lz = 0;
    ekm_lead_point(baseX, baseY, baseZ, vx, vy, vz, eyeX, eyeY,
                   projSpeed, &lx, &ly, &lz);
    aimX  = ekm_lead_scaled(baseX, lx, g_cfg.aimleadscale);
    aimY  = ekm_lead_scaled(baseY, ly, g_cfg.aimleadscale);
    leadZ = ekm_lead_scaled(baseZ, lz, g_cfg.aimleadscale);
  }
  const EkAimViewPair led = ekm_aim_view_pair(me.ang, me.horizoff,
                                              eyeX, eyeY, eyeZ,
                                              aimX, aimY, leadZ,
                                              pair.yaw, pair.dist2d);
  if (ekm_shot_horiz_reachable(led.q16horiz)) {
    pick->yaw = led.yaw;
    pick->horiz = led.q16horiz;
  } else if (ekm_shot_horiz_reachable(pair.q16horiz)) {
    pick->yaw = pair.yaw;
    pick->horiz = pair.q16horiz;
  }
  return pair;
}

bool aim_lock(AimLock* out) {
  if (!s_lockHave || !out) return false;
  if (!ekm_shot_fresh(aim_tick_of(aim_now_ms()) - s_lockMs)) return false;
  out->sprite = s_lockSprite;
  out->yaw = s_lockYaw;
  out->q16horiz = s_lockHoriz;
  out->myYaw = s_lockMyYaw;
  out->myQ16Horiz = s_lockMyHoriz;
  out->marginZ = s_lockMarginZ;
  out->range2d = s_lockRange2d;
  out->q16horizoff = s_lockHorizoff;
  out->spanLo = s_lockSpanLo; out->spanHi = s_lockSpanHi;
  out->spanMid = s_lockSpanMid; out->spanHeight = s_lockSpanH;
  out->reachable = s_lockReachable;
  return true;
}

bool aim_marker_points(AimMarkerPoints* out) {
  if (!s_lockHave || !out) return false;
  if (!ekm_shot_fresh(aim_tick_of(aim_now_ms()) - s_lockMs)) return false;
  out->sprite = s_lockSprite;
  out->rawX = s_lockRawX; out->rawY = s_lockRawY; out->rawZ = s_lockRawZ;
  out->ledX = s_lockLedX; out->ledY = s_lockLedY; out->ledZ = s_lockLedZ;
  return true;
}

bool aim_solution_at(int muzzleX, int muzzleY, int muzzleZ, int q16horizoff,
                    int* q16ang, int* q16horiz) {
  if (!s_shotHave) return false;
  if (!ekm_shot_fresh(aim_tick_of(aim_now_ms()) - s_shotMs)) return false;

  // Pure arithmetic, deliberately: the fire detour runs mid-tic and may not touch ek_hitscan, ek_cansee or
  // ek_updatesector. ekm_shot_solve carries the reachability gate, applied to the pitch this muzzle produces, the only
  // pitch anything is fired on. A refusal means the player's own shot stands.
  const EkShotSolve sol = ekm_shot_solve(muzzleX, muzzleY, muzzleZ,
                                         s_shotLedX, s_shotLedY, s_shotLedZ,
                                         s_shotRawX, s_shotRawY, s_shotRawZ,
                                         q16horizoff);
  if (!sol.ok) return false;
  if (q16ang) *q16ang = ekm_f16(sol.yaw);
  if (q16horiz) *q16horiz = sol.q16horiz;
  return true;
}

static const int EK_AIM_LOCAL_PLAYER = 0;

// Once per (weapon, shoots) change: a separate table from the lead log, since is-hitscan and how-fast-does-it-fly
// change on different frames. Reached from the SwapBuffers path and the P_PreFireHitscan detour (one thread). Behind
// g_cfg.debuglog.
static void aim_log_muzzle_source(int weapon, bool gotShoots, int shoots,
                                  bool isHitscan) {
  if (!g_cfg.debuglog) return;
  if (weapon < 0 || weapon >= EK_WD_MAX_WEAPONS) return;
  static bool s_said[EK_WD_MAX_WEAPONS];
  static int s_lastShoots[EK_WD_MAX_WEAPONS];
  const int key = gotShoots ? shoots : -1;
  if (s_said[weapon] && s_lastShoots[weapon] == key) return;
  s_said[weapon] = true;
  s_lastShoots[weapon] = key;

  const char* source = !gotShoots ? "stock slot"
                     : ekm_con_proj_aa(shoots).found ? "AA con"
                                                      : "stock tile";
  ek_log("aim muzzle: w%02d shoots %d -> %s (source: %s)", weapon, shoots,
         isHitscan ? "hitscan" : "projectile", source);
}

// The one place the muzzle's hitscan question is asked, so aim_solution_for_shot, aim_frame, tbot.cpp's verification
// ray and gamehooks.cpp's staleness log cannot disagree. A failed weapon read keeps today's behaviour: weapon 0 is no
// slot of ekm_shot_fires_hitscan's stock arm, so it answers false and the offset is kept.
int aim_muzzle_z(int posZ, int pyoff) {
  int weapon = 0;
  bool isHitscan = false;
  if (player_read_weapon(&weapon)) {
    int shoots = 0;
    const bool gotShoots = gamevars_weapon_field(weapon, EK_WD_SHOOTS,
                                                 EK_AIM_LOCAL_PLAYER, &shoots);
    isHitscan = ekm_shot_fires_hitscan(weapon, gotShoots, shoots);
    aim_log_muzzle_source(weapon, gotShoots, shoots, isHitscan);
  }
  return ekm_muzzle_z(posZ, pyoff, isHitscan);
}

bool aim_solution_for_shot(int* q16ang, int* q16horiz) {
  // The live muzzle, read now, formed as aim_frame's probe muzzle and A_ShootWithZvel's startPos are
  // (player.cpp:1709-1711). A failed read refuses, not guesses. World Tour (silentwt.cpp:369): an eye-solved,
  // muzzle-fired shot cost twenty consecutive wall hits at full target health.
  PlayerSnap me;
  if (!player_snap(&me)) return false;
  int pyoff = 0;
  if (!player_read_pyoff(&pyoff)) return false;
  return aim_solution_at(me.x, me.y, aim_muzzle_z(me.z, pyoff),
                         me.horizoff, q16ang, q16horiz);
}

// Why this shot is led the way it is, once per (slot, projectile) pair: AA rewrites WEAPONn_SHOOTS on a character
// switch and on an upgrade, so a one-shot at startup would miss the cases worth reading. Prints the picnum, CON name,
// flag and where the number came from. Render thread only; behind g_cfg.debuglog.
static void aim_log_lead_source(int weapon, bool gotShoots, int shoots,
                                int speed) {
  if (!g_cfg.debuglog) return;
  if (weapon < 0 || weapon >= EK_WD_MAX_WEAPONS) return;
  static bool s_said[EK_WD_MAX_WEAPONS];
  static int s_lastShoots[EK_WD_MAX_WEAPONS];
  const int key = gotShoots ? shoots : -1;
  if (s_said[weapon] && s_lastShoots[weapon] == key) return;
  s_said[weapon] = true;
  s_lastShoots[weapon] = key;

  if (!gotShoots) {
    ek_log("aim lead: w%02d SHOOTS unread -- stock table, %d", weapon, speed);
    return;
  }
  const EkConProj p = ekm_con_proj_aa(shoots);
  if (!p.found) {
    ek_log("aim lead: w%02d shoots %d, not an AA projectile -- stock table, %d",
           weapon, shoots, speed);
    return;
  }
  ek_log("aim lead: w%02d shoots %d %s -- AA says %s, vel %d, lead %d",
         weapon, shoots, p.name, p.hitscan ? "HITSCAN" : "flying", p.vel, speed);
}

void aim_frame(void) {
  // Cleared first, before any gate: every path below that bails must leave silent aim with no answer, or the detour
  // would shoot at where something used to be.
  s_shotHave = false;
  // And the triggerbot's lock, on the same line and for the same reason: every bail must leave it with nothing, so the
  // gates (the game's menu, our panel, no player struct, no cansee) are also the triggerbot's.
  s_lockHave = false;

  const bool wantAim = g_cfg.aimon != 0;
  // gamehooks_shoot_ready() is part of the cost condition, not just the row's label: otherwise a player who switched
  // the row on while the shoot hook failed to install would pay the probe cost every frame for a feature that can never
  // fire.
  const bool wantSilent = g_cfg.silenton != 0 && gamehooks_shoot_ready();
  // The triggerbot drives this acquisition too, a third independent reason: its "Aimbot locked" row names the lock this
  // function produces, not the view write, so gating one on the other would make two rows quietly reconfigure each
  // other. localinput_ready() is part of the condition: its only output is a bit OR'd into localInput.
  const bool wantTbot = g_cfg.tboton != 0 && localinput_ready();
  if (!wantAim && !wantSilent && !wantTbot) return;
  if (!aim_available()) return;

  if (input_panel_open()) return;

  // And the free camera stops us, for a sharper reason: the camera is a player who is not WHERE DUKE IS, so everything
  // below would turn Duke's view and publish the lock the triggerbot fires on at whatever stands near a body nobody is
  // looking at (freecam.h owns the rule).
  if (!freecam_bots_run(camera_active())) return;

  PlayerSnap me;
  if (!player_snap(&me)) return;

  int gm = 0;
  const bool gmOk = player_read_gm(&gm);
  const bool gmGate = !gmOk || (gm & EK_MODE_MENU) != 0;
  aim_log_gate(gm, gmOk, gmGate);
  if (gmGate) return;

  // An unbound hold row means the aimbot does nothing, not runs free. `vk != 0` did not implement that: an unmappable
  // vk (VK_SHIFT from a hand-edited ini) is nonzero, so the row drew "Unbound" while GetAsyncKeyState still fired it;
  // bind_hold_is_active is the live test. One syscall on a frame that does nothing.
  bool aimNow = wantAim;
  if (aimNow && g_cfg.aimhold) {
    const int vk = config_bind_get(BIND_ROW_AIM);
    aimNow = bind_hold_is_active(vk, (GetAsyncKeyState(vk) & 0x8000) != 0);
  }
  if (!aimNow && !wantSilent && !wantTbot) return;

  // How much SIMULATION time this frame covers, in tics: smoothing and the velocity divisor are per-tic rates, so this
  // makes a setting mean the same at 30 fps and 240. Capped at EKM_LEAD_TICS_MAX so a pause, load or alt-tab costs one
  // near-snap; one tic is assumed on the first frame and when the clock is unavailable.
  const double nowMs = aim_now_ms();
  const unsigned nowTickMs = aim_tick_of(nowMs);
  static double s_lastFrameMs = -1.0;
  double ticsElapsed = 1.0;
  if (nowMs >= 0.0) {
    if (s_lastFrameMs >= 0.0) {
      ticsElapsed = (nowMs - s_lastFrameMs) / EKM_MS_PER_TIC;
      if (ticsElapsed < 0.0) ticsElapsed = 0.0;
      if (ticsElapsed > (double)EKM_LEAD_TICS_MAX) ticsElapsed = (double)EKM_LEAD_TICS_MAX;
    }
    s_lastFrameMs = nowMs;
  }

  int eyeX = me.x, eyeY = me.y, eyeZ = me.z;
  aim_eye_interp(me, nowTickMs, &eyeX, &eyeY, &eyeZ);

  // The muzzle, not the eye; a failed pyoff read costs silent aim its answer. Measured from me.z, the raw position, not
  // the interpolated eye: A_ShootWithZvel builds startPos from pPlayer->pos itself (player.cpp:1709-1711). World Tour
  // (silentwt.cpp:369): an eye-solved, muzzle-fired shot is a parallel line about pyoff + 1024 low.
  int pyoff = 0;
  const bool muzzleOk = player_read_pyoff(&pyoff);
  const int muzzleZ = muzzleOk ? aim_muzzle_z(me.z, pyoff) : me.z;

  // How fast the shot will fly, asked once a frame, not per candidate. Zero means a straight-line lead is wrong
  // (hitscan, thrown, placed, nothing fired). The speed comes from AA's live WEAPONn_SHOOTS via ekm_weapon_lead_speed,
  // the stock table only as fallback (projlead.h); a failed weapon read does not lead.
  int projSpeed = 0;
  if (g_cfg.aimlead) {
    int weapon = 0;
    if (player_read_weapon(&weapon)) {
      int shoots = 0;
      const bool gotShoots = gamevars_weapon_field(weapon, EK_WD_SHOOTS,
                                                   EK_AIM_LOCAL_PLAYER, &shoots);
      projSpeed = ekm_weapon_lead_speed(weapon, gotShoots, shoots);
      aim_log_lead_source(weapon, gotShoots, shoots, projSpeed);
    }
  }

  // Once per tic, not per rendered frame (at 60-120 fps the loop ran two to four times a tic on actors the engine had
  // not moved). The tic boundary is wallclock-derived (EKM_ACQUIRE_GAP_MS from the engine's 30 Hz tic). The pick is
  // republished every frame, only the LOOP is skipped, so it is at most one tic old, inside ekm_shot_fresh's 133 ms.
  static EkAimPick s_pick;
  static bool s_pickHave = false, s_pickEver = false;
  static unsigned s_pickMs = 0;
  if (ekm_acquire_due(s_pickEver, nowTickMs - s_pickMs)) {
    s_pickEver = true;
    s_pickMs = nowTickMs;
    s_pickHave = aim_acquire(me, eyeX, eyeY, eyeZ, muzzleZ, nowTickMs,
                             projSpeed, &s_pick);
  }
  if (!s_pickHave) return;

  EkAimPick pick = s_pick;
  const EkAimViewPair pair =
      aim_refresh(me, eyeX, eyeY, eyeZ, nowTickMs, projSpeed, &pick);

  // The triggerbot's lock, published whenever an acquisition succeeded, with no reachability test: this pair describes
  // where the enemy IS, not a pitch anything is written to. All three angles come out of one ekm_aim_view_pair call, so
  // they describe one instant.
  s_lockSprite = pick.sprite;
  s_lockYaw = pair.yaw;
  s_lockHoriz = pair.q16horiz;
  s_lockMyYaw = pair.myYaw;
  s_lockMyHoriz = me.horiz;
  s_lockRawX = pick.rawX; s_lockRawY = pick.rawY; s_lockRawZ = pick.rawZ;
  s_lockLedX = pick.ledX; s_lockLedY = pick.ledY; s_lockLedZ = pick.ledZ;
  s_lockMarginZ = pick.marginZ; s_lockRange2d = pick.range2d;
  s_lockSpanLo = pick.spanLo; s_lockSpanHi = pick.spanHi;
  s_lockSpanMid = pick.spanMid; s_lockSpanH = pick.spanHeight;
  s_lockHorizoff = me.horizoff;
  s_lockReachable = pick.reachable;
  s_lockMs = s_pickMs;
  s_lockHave = true;

  // The point, published for the A_ShootWithZvel detour to solve from. The muzzle read and the reachability test are
  // applied in aim_solution_at, against the pitch the firing muzzle actually yields; that test is the SHOT's bound
  // (aimsolve.h), not P_GetInput's clamp, and silent aim with no answer leaves the player's own shot as it was.
  if (wantSilent) {
    s_shotRawX = pick.rawX; s_shotRawY = pick.rawY; s_shotRawZ = pick.rawZ;
    s_shotLedX = pick.ledX; s_shotLedY = pick.ledY; s_shotLedZ = pick.ledZ;
    s_shotMs = s_pickMs;   // the acquisition's, for the reason above
    s_shotHave = true;
  }

  if (!aimNow) return;

  // AA's own aimbot goggles stand over ours: only the VIEW write is skipped here. Acquisition, the lock, the marker,
  // silent aim and the triggerbot are published above, so goggles holding the view leave them intact; nothing
  // downstream accumulates state across frames, so returning leaves nothing stale.
  {
    int aimbotOnVal = 0, bottargetVal = -1, mySprite = 0;
    const bool gotOn = gamevars_aimbot_on(EK_AIM_LOCAL_PLAYER, &aimbotOnVal);
    const bool gotSprite = player_sprite_index(&mySprite);
    const bool gotTarget = gotSprite &&
                           gamevars_bottarget(mySprite, &bottargetVal);
    const bool hold = ekm_goggles_hold_view(gotOn, aimbotOnVal, gotTarget,
                                            bottargetVal);
    static bool s_held = false;
    if (hold != s_held) {
      s_held = hold;
      if (g_cfg.debuglog) {
        if (hold)
          ek_log("aim: goggles hold the view (bottarget %d) -- aimbot stands "
                 "down", bottargetVal);
        else
          ek_log("aim: goggles released the view -- aimbot resumes");
      }
    }
    if (hold) return;
  }

  // The view's own bound, asked here and nowhere else: acquisition judges by what the SHOT can reach, so the winner's
  // pitch may be one P_GetInput would clamp back out (player.cpp:3179). A winner the view cannot hold must not starve
  // the bot: it falls back to the best view-reachable candidate (fbHave); silent aim and the lock still use the winner.
  int aimYawTo = pick.yaw, aimHorizTo = pick.horiz;
  if (!ekm_horiz_reachable(pick.horiz)) {
    if (!pick.fallbackHave) return;
    aimYawTo = pick.fallbackYaw;
    aimHorizTo = pick.fallbackHoriz;
    if (g_cfg.debuglog) {
      static unsigned s_lastFbMs = 0;
      if (s_lastFbMs == 0 ||
          (unsigned)(nowTickMs - s_lastFbMs) >= EK_AIM_SPAN_PARTIAL_MS) {
        s_lastFbMs = nowTickMs;
        ek_log("aim fallback: sprite %d needs q16horiz %d, which the view "
               "cannot hold; turning to sprite %d instead (the lock and silent "
               "aim keep sprite %d)",
               pick.sprite, pick.horiz >> 16, pick.fallbackSprite, pick.sprite);
      }
    }
  }

  // The whole delta in 16.16, not a whole-unit delta widened: taking it from (me.ang >> 16) discarded the player's
  // fractional angle and re-applied it every frame, a sub-unit jitter floor. Per tic, not per frame: ekm_smooth_step
  // takes its percentage once a frame, so at 240 fps every setting converged inside 20 ms and snapped.
  const int stepYaw   = ekm_smooth_step_tics(ekm_yaw_delta_q16(me.ang, ekm_f16(aimYawTo)),
                                             g_cfg.aimsmooth, ticsElapsed);
  const int stepHoriz = ekm_smooth_step_tics(aimHorizTo - me.horiz,
                                             g_cfg.aimsmooth, ticsElapsed);
  if (stepYaw == 0 && stepHoriz == 0) return;

  const int newAng   = (me.ang + stepYaw) & (ekm_f16(EKM_YAW_UNITS) - 1);
  const int newHoriz = me.horiz + stepHoriz;

  // Four fields, not two: G_DrawRooms interpolates between oq16ang and q16ang by smoothratio, so leaving the old value
  // on a snap renders a frame of travel the player did not ask for; on a smoothed step the interpolation is wanted.
  player_write_view(newAng, newHoriz, g_cfg.aimsmooth == 0);
}
