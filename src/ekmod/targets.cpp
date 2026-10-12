// The one list of things that can be shot, shared by the ESP and the aimbot so a filter fix cannot reach only one of
// them.

#include "targets.h"
#include "gamestate.h"
#include "tiles.h"
#include "badguy.h"
#include "actorstate.h"
#include "playerstate.h"
#include "projmath.h"
#include "config.h"
#include "log.h"
#include "iat.h"
#include "targets_census.h"

#include <windows.h>
#include <stdio.h>
#include <math.h>

// Why a sprite is NOT a target. A refusal is silent, so this names the rejecting term (instrumentation only). Rate
// limited PER PICNUM, with a shared overflow slot, so one corpse cannot spend the line.
static const int EK_REFUSE_SLOTS = 32;
static const unsigned EK_REFUSE_MS = 1000;

// `extra` is the value the rejecting rule judged (deferred-damage discount applied). A sprite that is not enemy-like is
// counted by its caller and takes no slot, so level-start props cannot starve enemy lines.
static void targets_log_refusal(const unsigned char* r, int sprite, int picnum,
                                int statnum, int extra, const char* term,
                                bool enemyLike) {
  static short s_pic[EK_REFUSE_SLOTS];
  static unsigned s_ms[EK_REFUSE_SLOTS];
  static int s_used = 0;

  const int usedBefore = s_used;
  const int slot = ekm_refusal_slot(s_pic, &s_used, EK_REFUSE_SLOTS, picnum, enemyLike);
  if (slot < 0) return;
  if (s_used != usedBefore) s_ms[slot] = 0;

  const unsigned now = GetTickCount();
  if (s_ms[slot] != 0 && (unsigned)(now - s_ms[slot]) < EK_REFUSE_MS) return;
  s_ms[slot] = now;

  ek_log("target refused: sprite %d picnum %d statnum %d -- %s "
         "(cstat 0x%04X extra %d pal %d xrep %d yrep %d)",
         sprite, picnum, statnum, term,
         (unsigned)*(const unsigned short*)(r + EK_SPR_CSTAT), extra,
         (int)r[EK_SPR_PAL], (int)r[EK_SPR_XREPEAT], (int)r[EK_SPR_YREPEAT]);
}

// Cumulative beside the rate-limited samples: a sample shows one refusal, only a count shows a rate. Printed on the
// aimbot's `aim acquire:` line; incremented only when the Debug log row is on.
static unsigned s_refuseNotEnemy = 0, s_refuseCritter = 0;
static unsigned s_refuseZeroRep  = 0, s_refuseDown    = 0;

// One counter per gate, not summed: aim.cpp refuses in its candidate loop and targets_sprite_targetable in the trigger,
// and a sum would hide which is wrong. Each is its own kill switch: climbing while the player kills things means a
// damageable actor sits at -1. STAT_ACTOR sprites only.
static unsigned s_refuseMonstatusAim = 0, s_refuseMonstatusTrigger = 0;

void targets_note_monstatus_refusal(void) {
  if (g_cfg.debuglog) s_refuseMonstatusAim++;
}

static unsigned s_refuseAsleepAim = 0, s_refuseAsleepTrigger = 0;

void targets_note_asleep_refusal(void) {
  if (g_cfg.debuglog) s_refuseAsleepAim++;
}

static unsigned s_refuseSelfDestructAim = 0, s_refuseSelfDestructTrigger = 0;

void targets_note_selfdestruct_refusal(void) {
  if (g_cfg.debuglog) s_refuseSelfDestructAim++;
}

static unsigned s_refuseWormAim = 0, s_refuseWormTrigger = 0;

void targets_note_worm_refusal(void) {
  if (g_cfg.debuglog) s_refuseWormAim++;
}

static unsigned s_refuseAttachmentAim = 0, s_refuseAttachmentTrigger = 0;

void targets_note_attachment_refusal(void) {
  if (g_cfg.debuglog) s_refuseAttachmentAim++;
}

// Friendlies SEEN, not dropped: targets_collect_split re-homes the sprite into the other array, targets_collect refuses
// it. One counter for both says whether AA's team var read works at all. A companion bot the monstflags rule lists
// counts here too.
static unsigned s_friendlySeen = 0;

void targets_refusal_summary(char* buf, int n) {
  if (!buf || n <= 0) return;
  buf[0] = 0;
  _snprintf_s(buf, (size_t)n, _TRUNCATE,
              "notenemy %u, critter %u, friendly %u, zerorep %u, down %u, "
              "monstatus-aim %u, monstatus-trigger %u, "
              "asleep-aim %u, asleep-trigger %u, "
              "selfdestruct-aim %u, selfdestruct-trigger %u, "
              "worm-shielded-aim %u, worm-shielded-trigger %u, "
              "sidegun-aim %u, sidegun-trigger %u",
              s_refuseNotEnemy, s_refuseCritter, s_friendlySeen,
              s_refuseZeroRep, s_refuseDown,
              s_refuseMonstatusAim, s_refuseMonstatusTrigger,
              s_refuseAsleepAim, s_refuseAsleepTrigger,
              s_refuseSelfDestructAim, s_refuseSelfDestructTrigger,
              s_refuseWormAim, s_refuseWormTrigger,
              s_refuseAttachmentAim, s_refuseAttachmentTrigger);
}

// The enemy-like census: one walk a second, a line only when counts changed. The statnum filter had no counter and
// refusal samples were starved by props. Kept per walk kind, clock included: the plain walk refuses the friendlies the
// split one keeps and runs first each frame.
static EkmCensus s_censusCur;
static EkmCensus s_censusLast[2];
static bool s_censusHaveLast[2] = { false, false };
static unsigned s_censusMs[2] = { 0, 0 };

static void targets_census_note(EkmCensus* c, bool enemyLike, int reason,
                                int picnum, int statnum, int exA, int exB) {
  if (c && enemyLike) ekm_census_add(c, reason, picnum, statnum, exA, exB);
}

static void targets_census_unjudged(EkmCensus* c, const unsigned char* r) {
  if (!c || !r) return;
  const int statnum = *(const short*)(r + EK_SPR_STATNUM);
  if (statnum == EK_MAXSTATUS) return;
  const int picnum = *(const short*)(r + EK_SPR_PICNUM);
  if (!ekm_enemy_like(picnum, picnum_is_enemy(picnum))) return;
  ekm_census_add(c, EKM_CEN_STATNUM, picnum, statnum,
                 *(const short*)(r + EK_SPR_EXTRA), (int)r[EK_SPR_PAL]);
}

// Counted when the walk ends, because a nearer sprite may still evict it; an evicted one counts FULL. A listed hostile
// is classed by the first aim.cpp refusal it meets.
static void targets_census_listed(EkmCensus* c, const EkTarget& t, bool friendly,
                                  bool evicted) {
  if (!c) return;
  const int picnum = (int)t.picnum;
  if (!ekm_enemy_like(picnum, badguy_ready() && picnum_is_enemy(picnum))) return;
  int reason = EKM_CEN_FULL;
  if (!evicted)
    reason = friendly
        ? EKM_CEN_FRIENDLY
        : ekm_census_hostile_reason(
              ekm_target_hittable(t.xrep, t.cstat), t.attachment, t.asleepDiscardsHits,
              ekm_target_monstatus_shootable(t.haveMonstatus, t.monstatus, t.statnum),
              ekm_actor_state_targetable(t.state));
  ekm_census_add(c, reason, picnum, (int)t.statnum,
                 reason == EKM_CEN_STATE ? t.state : (int)t.hp, (int)t.pal);
}

static int targets_far_units(const long long* dist2, int n, bool haveMe) {
  const int idx = haveMe ? ekm_target_farthest(dist2, n) : -1;
  if (idx < 0) return -1;
  const double d = sqrt((double)dist2[idx]);
  return d >= 2147483647.0 ? 2147483647 : (int)d;
}

// Each list entry's squared distance, beside the caller's array. Render thread only; refilled from 0 every walk.
static long long s_hostDist2[EK_TARGET_MAX];
static long long s_friendDist2[EK_FRIENDLY_MAX];

static EkmCensus* targets_census_begin(bool split) {
  if (!g_cfg.debuglog) return NULL;
  const int k = split ? 1 : 0;
  const unsigned now = GetTickCount();
  if (s_censusMs[k] != 0 && (unsigned)(now - s_censusMs[k]) < EK_REFUSE_MS) return NULL;
  s_censusMs[k] = now;
  ekm_census_clear(&s_censusCur);
  return &s_censusCur;
}

static void targets_census_end(const EkmCensus* c, bool split) {
  if (!c) return;
  const int k = split ? 1 : 0;
  if (s_censusHaveLast[k] && ekm_census_same(*c, s_censusLast[k])) return;
  s_censusLast[k] = *c;
  s_censusHaveLast[k] = true;
  char line[EKM_CENSUS_LINE_MAX];
  ekm_census_format(*c, split ? "split" : "plain", line, (int)sizeof(line));
  ek_log("%s", line);
}

// The toilet transition, instrumentation only (hypothesis: actorstate.h). Sampled once per FRAME: targets_collect runs
// twice a frame (aim_frame then esp_draw), and iat_swap_frames is the only honest frame id.
struct ToiletSample {
  long frame;
  int  sprite_index, picnum, rawExtra, judgedExtra, pending, t4, state, dyingRecord;
  int  cstat;
  EkAction rec;
};
struct ToiletTrack {
  int  used, sprite, lastPicnum, after, head, count;
  long lastFrame;
  ToiletSample ring[EK_TOILET_TRACE_BEFORE];
};
static ToiletTrack s_toilet[EK_TOILET_TRACE_SLOTS];
static int s_toiletLines = 0;

static void toilet_log_sample(const ToiletSample& s, const char* when) {
  if (s_toiletLines >= EK_TOILET_TRACE_LINES) return;
  s_toiletLines++;
  ek_log("toilet trace %-6s frame %ld sprite %d picnum %d cstat 0x%04X "
         "extra %d (raw %d) htextra %d | t4 %d rec (%d,%d,%d,%d,%d) readable %d "
         "| dyingRecord %d state %d shown %d",
         when, s.frame, s.sprite_index, s.picnum, (unsigned)s.cstat,
         s.judgedExtra, s.rawExtra, s.pending, s.t4,
         s.rec.startframe, s.rec.numframes, s.rec.viewtype, s.rec.incval,
         s.rec.delay, s.rec.readable, s.dyingRecord, s.state,
         ekm_actor_state_shown(s.state) ? 1 : 0);
}

static void toilet_trace(int sprite, int picnum, int cstat, int rawExtra,
                         int judgedExtra, int pending, int state,
                         int dyingRecord) {
  if (!g_cfg.debuglog) return;
  if (s_toiletLines >= EK_TOILET_TRACE_LINES) return;
  const bool sitting = ekm_actor_defers_damage(picnum);
  if (ekm_defer_family(picnum) == EKM_DEFER_NONE) return;

  ToiletTrack* t = NULL;
  for (int k = 0; k < EK_TOILET_TRACE_SLOTS; k++)
    if (s_toilet[k].used && s_toilet[k].sprite == sprite) { t = &s_toilet[k]; break; }
  if (!t) {
    // A slot is claimed only by a sitter, or every standing trooper would evict the one that matters.
    if (!sitting) return;
    for (int k = 0; k < EK_TOILET_TRACE_SLOTS; k++) {
      if (s_toilet[k].used) continue;
      ToiletTrack z = {};
      s_toilet[k] = z;
      s_toilet[k].used = 1;
      s_toilet[k].sprite = sprite;
      s_toilet[k].lastPicnum = picnum;
      t = &s_toilet[k];
      break;
    }
    if (!t) return;
  }

  const long frame = iat_swap_frames();
  // The second call this frame is dropped, even with an empty ring (the burst clears it). A fresh slot's lastFrame is 0
  // and iat_swap_frames starts at 1, so the sentinel cannot collide.
  if (t->lastFrame == frame) return;

  ToiletSample s = {};
  s.frame = frame;
  s.sprite_index = sprite;
  s.picnum = picnum;
  s.cstat = cstat;
  s.rawExtra = rawExtra;
  s.judgedExtra = judgedExtra;
  s.pending = pending;
  s.state = state;
  s.dyingRecord = dyingRecord;
  actorstate_action_at(sprite, &s.t4, &s.rec);

  if (t->after > 0) {
    toilet_log_sample(s, "after");
    if (--t->after == 0) { ToiletTrack z = {}; *t = z; return; }
  } else if (ekm_toilet_transition(t->lastPicnum, picnum)) {
    for (int k = 0; k < t->count; k++) {
      const int idx = ekm_ring_oldest(t->head, t->count, EK_TOILET_TRACE_BEFORE, k);
      if (idx >= 0) toilet_log_sample(t->ring[idx], "before");
    }
    toilet_log_sample(s, "change");
    t->count = 0;
    t->head = 0;
    t->after = EK_TOILET_TRACE_AFTER;
  } else if (sitting) {
    t->ring[t->head] = s;
    t->head = (t->head + 1) % EK_TOILET_TRACE_BEFORE;
    if (t->count < EK_TOILET_TRACE_BEFORE) t->count++;
  } else {
    ToiletTrack z = {};
    *t = z;
    return;
  }

  t->lastPicnum = picnum;
  t->lastFrame = frame;
}

// One byte per sprite slot; the rule and the rejected alternatives are in actorstate.h. Only the collector writes it,
// targets_sprite_targetable reads. .bss, 16 KB, render thread.
static unsigned char s_deferSticky[EK_MAXSPRITES];

// Cleared on level change so the recycling argument need not be right to be safe: the sweep below is the flag's real
// lifecycle, but a map load recycles every index at once.
static void targets_forget_deferrals(void) {
  for (int i = 0; i < EK_MAXSPRITES; i++) s_deferSticky[i] = 0;
}

static unsigned char s_crashLogged[EK_MAXSPRITES];

static bool targets_vehicle_crashing(int i, int picnum, short extra, bool logIt) {
  int moveId = 0, hvel = 0, vvel = 0;
  const bool haveMove = picnum == EK_PICNUM_NEWRECON &&
                        actorstate_move_at(i, &moveId, &hvel, &vvel);
  EkAiRead ai = {};
  if (picnum == EK_PICNUM_PIGSUV) actorstate_ai_at(i, &ai);
  const bool recon = ekm_actor_crashing_aa(picnum, haveMove ? 1 : 0, hvel, vvel);
  const bool suv = ekm_actor_suv_dying_aa(picnum, ai);
  const bool crashing = recon || suv;
  if (logIt && g_cfg.debuglog && crashing && !s_crashLogged[i]) {
    s_crashLogged[i] = 1;
    if (recon)
      ek_log("recon crashing: sprite %d extra %d move id %d (%d,%d) -- not a target",
             i, (int)extra, moveId, hvel, vvel);
    else
      ek_log("suv crashing: sprite %d extra %d ai id %d action id %d ai move id %d "
             "(%d,%d) -- not a target",
             i, (int)extra, ai.aiId, ai.actionId, ai.aiMoveId, ai.hvel, ai.vvel);
  }
  if (logIt && !crashing) s_crashLogged[i] = 0;
  return crashing;
}

// The once-per-sprite line for an armed torso. Cleared on level load, when a torso reads unarmed, and by the
// collector's sweep. Each caller does its own read and passes what came back.
static unsigned char s_selfDestructLogged[EK_MAXSPRITES];

static bool targets_torso_self_destructing(int i, int picnum, short extra,
                                           bool gotCountvarc, int countvarc,
                                           bool logIt) {
  const bool armed = ekm_torso_self_destructing(picnum, gotCountvarc, countvarc);
  if (logIt && g_cfg.debuglog && armed && !s_selfDestructLogged[i]) {
    s_selfDestructLogged[i] = 1;
    ek_log("torso self-destructing: sprite %d picnum %d extra %d countvarc %d "
           "-- not a target", i, picnum, (int)extra, countvarc);
  }
  if (logIt && !armed) s_selfDestructLogged[i] = 0;
  return armed;
}

// One line per worm-part slot per level, holding the part logged so a body promoted to tail (PODHEAD.CON:829-843) logs
// as a tail.
static unsigned char s_wormLogged[EK_MAXSPRITES];

static void targets_worm_first_sight(int i, int part, int countvar, int dashtime,
                                     int wormState) {
  if (!g_cfg.debuglog || s_wormLogged[i] == (unsigned char)part) return;
  s_wormLogged[i] = (unsigned char)part;
  const char* const label = ekm_actor_state_label(ekm_worm_actor_state(wormState));
  ek_log("worm part seen: sprite %d %s countvar %d dashtime %d -- %s",
         i, part == EKM_WORM_PART_HEAD ? "head" : "tail", countvar, dashtime,
         label ? label : "no verdict");
}

static unsigned char s_growLogged[EK_MAXSPRITES];

static void targets_polypad_growth_note(int i, int picnum, short extra,
                                        int initflags, int xrepeat, bool growing) {
  if (g_cfg.debuglog && growing && !s_growLogged[i]) {
    s_growLogged[i] = 1;
    ek_log("polymorph growing: sprite %d picnum %d extra %d initflags %d xrep %d "
           "-- not a target until xrep 32", i, picnum, (int)extra, initflags, xrepeat);
  }
  if (!growing) s_growLogged[i] = 0;
}

int targets_collect(EkTarget* out, int max) {
  return targets_collect_split(out, max, NULL, 0, NULL);
}

int targets_collect_split(EkTarget* out, int max,
                          EkTarget* friendlyOut, int friendlyMax,
                          int* friendlyCount) {
  int count = 0;
  int friendlies = 0;
  if (!friendlyOut || friendlyMax <= 0) { friendlyOut = NULL; friendlyMax = 0; }
  // The distance arrays are sized to these caps.
  if (max > EK_TARGET_MAX) max = EK_TARGET_MAX;
  if (friendlyMax > EK_FRIENDLY_MAX) friendlyMax = EK_FRIENDLY_MAX;
  int hostFar = -1, friendFar = -1;

  // The player's raw tic position, once a walk (the one aim.cpp's range measures from). With no read every distance is
  // 0 and a full list keeps slot order.
  PlayerSnap me = {};
  const bool haveMe = player_snap(&me);

  // ekm_level_loading, not !esp_level_loaded: a PAUSE must not clear the flag and hand a sitting trooper its health
  // back (playerstate.h).
  {
    int gm = 0;
    const bool gmOk = player_read_gm(&gm);
    if (ekm_level_loading(gm, gmOk)) {
      targets_forget_deferrals();
      for (int i = 0; i < EK_MAXSPRITES; i++) s_crashLogged[i] = 0;
      for (int i = 0; i < EK_MAXSPRITES; i++) s_selfDestructLogged[i] = 0;
      for (int i = 0; i < EK_MAXSPRITES; i++) s_wormLogged[i] = 0;
      for (int i = 0; i < EK_MAXSPRITES; i++) s_growLogged[i] = 0;
    }
  }

  // The four per-actor arrays are validated once per walk (8 VirtualQuery calls, not 8 per target). Safe because both
  // callers run in the SwapBuffers hook on the engine's thread (drawframe_do -> videoNextPage, game.cpp:6508), the only
  // one that runs Gv_Free.
  EkGvView teamView, initHpView, levelView, monstatusView;
  gamevars_team_view(&teamView);
  gamevars_inithp_view(&initHpView);
  gamevars_mlevel_view(&levelView);
  gamevars_monstatus_view(&monstatusView);
  EkGvView countvarcView = { NULL, false };
  bool countvarcViewTaken = false;
  EkGvView countvarView = { NULL, false }, dashtimeView = { NULL, false };
  bool wormViewsTaken = false;
  EkGvView initflagsView = { NULL, false };
  bool initflagsViewTaken = false;
  EkGvView monstflagsView = { NULL, false };
  bool monstflagsViewTaken = false;
  EkmCensus* const cen = targets_census_begin(friendlyOut != NULL);

  for (int i = 0; i < EK_MAXSPRITES; i++) {
    // No sweep-only state: a full list still takes a nearer sprite, so every slot is judged and the deferral sweep runs
    // for all.
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;

    const short statnum = *(const short*)(r + EK_SPR_STATNUM);

    // The deferral flag's lifecycle, swept for every slot: deletesprite parks a freed record at statnum MAXSTATUS, so a
    // flag left on a dead sitter's slot would be inherited by whatever spawns there and hide a live enemy. The flag
    // holds the family that set it.
    if (s_deferSticky[i])
      s_deferSticky[i] = (unsigned char)ekm_defer_sticky_next(
          s_deferSticky[i], (int)*(const short*)(r + EK_SPR_PICNUM), (int)statnum, 0, 0);

    if (s_selfDestructLogged[i] &&
        (!ekm_picnum_is_term_torso((int)*(const short*)(r + EK_SPR_PICNUM)) ||
         (statnum != EK_STAT_ACTOR && statnum != EK_STAT_ZOMBIE)))
      s_selfDestructLogged[i] = 0;
    if (s_growLogged[i] &&
        (!ekm_picnum_is_polyback((int)*(const short*)(r + EK_SPR_PICNUM)) ||
         (statnum != EK_STAT_ACTOR && statnum != EK_STAT_ZOMBIE)))
      s_growLogged[i] = 0;

    // The census counts what this term drops, enemy-like only, by the actual statnum; AA parks sprites in other status
    // lists.
    if (statnum != EK_STAT_ACTOR && statnum != EK_STAT_ZOMBIE) {
      if (cen) targets_census_unjudged(cen, r);
      continue;
    }

    const short picnum = *(const short*)(r + EK_SPR_PICNUM);

    // The three picnum-level terms in one order both sites run, which lives in targets.h (ekm_target_picnum_refusal): a
    // drifting sequence turns a target into a non-target.
    const bool ready = badguy_ready();
    const bool enemyTile = ready ? picnum_is_enemy(picnum) : false;
    const bool critter = picnum_is_harmless_critter(picnum);
    const bool enemyLike = g_cfg.debuglog && ekm_enemy_like(picnum, enemyTile);
    int refusal = ekm_target_picnum_refusal(picnum, ready, enemyTile, critter, false, 0);
    const char* friendlyTerm = "friendly (AA team 1 or 3)";
    if (ekm_target_refusal_awaits_team(refusal)) {
      // The team read, only where it can decide anything, through the view taken above the loop. A statement of its
      // own, never f(gamevars_team(i, &team), team).
      int team = 0;
      const bool haveTeam = ekm_gv_view_get(teamView, i, &team);
      // monstflags and monstatus only for a companion bot, which together tell it from a mimic.
      int monstflags = 0, botMonstatus = 0;
      bool gotMonstflags = false, gotBotMonstatus = false;
      if (ekm_picnum_is_aa_companion_bot(picnum)) {
        if (!monstflagsViewTaken) {
          monstflagsViewTaken = true;
          gamevars_monstflags_view(&monstflagsView);
        }
        gotMonstflags = ekm_gv_view_get(monstflagsView, i, &monstflags);
        gotBotMonstatus = ekm_gv_view_get(monstatusView, i, &botMonstatus);
        if (!ekm_target_is_friendly(haveTeam, team))
          friendlyTerm = "friendly (AA companion bot, not a mimic)";
      }
      int mandoInitflags = 0;
      bool gotMandoInitflags = false;
      if (ekm_picnum_is_aa_mando(picnum)) {
        if (!initflagsViewTaken) {
          initflagsViewTaken = true;
          gamevars_initflags_view(&initflagsView);
        }
        gotMandoInitflags = ekm_gv_view_get(initflagsView, i, &mandoInitflags);
        if (!ekm_target_is_friendly(haveTeam, team))
          friendlyTerm = "friendly (AA character-select Mando, initflags >= 32766)";
      }
      refusal = ekm_target_picnum_refusal(picnum, ready, enemyTile, critter, haveTeam, team,
                                          gotMonstflags, monstflags,
                                          gotBotMonstatus, botMonstatus,
                                          gotMandoInitflags, mandoInitflags);
    }
    // A hostile Sidegun stays in the ESP list, carried flagged for aim.cpp to refuse; only the trigger gate refuses it
    // outright.
    const bool attachment = (refusal == EKM_REFUSE_ATTACHMENT);
    if (ekm_target_refusal_hostile_listed(refusal)) refusal = EKM_REFUSE_NONE;

    const bool wormAdmitted = ekm_worm_admitted(picnum);
    int wormState = EKM_WORM_NONE;
    if (refusal == EKM_REFUSE_NONE && wormAdmitted) {
      if (!wormViewsTaken) {
        wormViewsTaken = true;
        gamevars_countvar_view(&countvarView);
        gamevars_dashtime_view(&dashtimeView);
      }
      int countvar = 0, dashtime = 0;
      const bool gotCountvar = ekm_gv_view_get(countvarView, i, &countvar);
      const bool gotDashtime = ekm_gv_view_get(dashtimeView, i, &dashtime);
      const int part = ekm_worm_part(picnum);
      wormState = ekm_worm_state_read(part, gotCountvar, countvar, gotDashtime,
                                      dashtime).state;
      if (wormState == EKM_WORM_NONE) refusal = EKM_REFUSE_NOTENEMY;
      else targets_worm_first_sight(i, part, countvar, dashtime, wormState);
    }

    if (refusal == EKM_REFUSE_NOTENEMY) {
      if (g_cfg.debuglog) {
        targets_log_refusal(r, i, picnum, statnum,
                            *(const short*)(r + EK_SPR_EXTRA),
                            wormAdmitted ? "worm part, countvar/dashtime unread"
                                         : "picnum is not an enemy tile (g_tile)",
                            enemyLike);
        s_refuseNotEnemy++;
        targets_census_note(cen, enemyLike, EKM_CEN_NOTENEMY, picnum, statnum,
                            *(const short*)(r + EK_SPR_EXTRA), (int)r[EK_SPR_PAL]);
      }
      continue;
    }
    if (refusal == EKM_REFUSE_CRITTER) {
      if (g_cfg.debuglog) {
        targets_log_refusal(r, i, picnum, statnum,
                            *(const short*)(r + EK_SPR_EXTRA),
                            "harmless critter", enemyLike);
        s_refuseCritter++;
        targets_census_note(cen, enemyLike, EKM_CEN_CRITTER, picnum, statnum,
                            *(const short*)(r + EK_SPR_EXTRA), (int)r[EK_SPR_PAL]);
      }
      continue;
    }

    // A friendly leaves the hostile list here and keeps walking: it is not refused when there is somewhere to put it,
    // and a "refused" line for a collected sprite would lie. The count is taken either way.
    const bool isFriendly = (refusal == EKM_REFUSE_FRIENDLY);
    if (isFriendly) {
      if (g_cfg.debuglog) {
        s_friendlySeen++;
        if (!friendlyOut) {
          targets_log_refusal(r, i, picnum, statnum,
                              *(const short*)(r + EK_SPR_EXTRA),
                              friendlyTerm, enemyLike);
          targets_census_note(cen, enemyLike, EKM_CEN_FRIENDLY_EXCL, picnum, statnum,
                              *(const short*)(r + EK_SPR_EXTRA), (int)r[EK_SPR_PAL]);
        }
      }
      if (!friendlyOut) continue;
    }

    // Each list has its own cap (targets.h). A full list takes this sprite only in place of its farthest entry, decided
    // before the reads below.
    const int sx = *(const int*)(r + EK_SPR_X);
    const int sy = *(const int*)(r + EK_SPR_Y);
    const long long d2 = haveMe ? ekm_target_dist2(sx - me.x, sy - me.y) : 0;
    const int slot = isFriendly
        ? ekm_target_insert_slot(s_friendDist2, friendlies, friendlyMax, friendFar, d2)
        : ekm_target_insert_slot(s_hostDist2, count, max, hostFar, d2);
    if (slot < 0) {
      targets_census_note(cen, enemyLike, EKM_CEN_FULL, picnum, statnum,
                          *(const short*)(r + EK_SPR_EXTRA), (int)r[EK_SPR_PAL]);
      continue;
    }

    const unsigned char xrep = r[EK_SPR_XREPEAT];
    const unsigned char yrep = r[EK_SPR_YREPEAT];
    if (xrep == 0 || yrep == 0) {
      if (g_cfg.debuglog) {
        targets_log_refusal(r, i, picnum, statnum,
                            *(const short*)(r + EK_SPR_EXTRA),
                            "xrepeat or yrepeat is zero", enemyLike);
        s_refuseZeroRep++;
        targets_census_note(cen, enemyLike, EKM_CEN_ZEROREP, picnum, statnum,
                            *(const short*)(r + EK_SPR_EXTRA), (int)r[EK_SPR_PAL]);
      }
      continue;
    }
    const unsigned short cstat = *(const unsigned short*)(r + EK_SPR_CSTAT);

    // Liveness uses the engine own dead predicate `extra <= 0` (CON_IFDEAD, gameexec.cpp:3325-3327), `pal != 1` carving
    // out the frozen enemy, plus a cstat term scoped to the trooper picnums (ekm_trooper_choking). Not every enemy
    // clears cstat when dying. A trooper PLAYING DEAD is dropped with corpses. History: docs/re/target-liveness.md.
    const short rawExtra = *(const short*)(r + EK_SPR_EXTRA);
    const unsigned char pal = r[EK_SPR_PAL];

    // The sitting trooper's queued damage is discounted HERE so the ESP, aimbot and triggerbot see one health number
    // (actorstate.h has the rule and the freezethrower caveat). Read for every enemy for the ESP's number;
    // ekm_actor_effective_extra applies it to the six sitting picnums only, so targeting is unchanged.
    int pending = EK_HTEXTRA_NONE;
    const bool pendingKnown = actorstate_pending_damage(i, &pending);
    if (!pendingKnown) pending = EK_HTEXTRA_NONE;

    // The flag is decided from this frame's own read, so a sitter that just became 1680 keeps the discount its queue
    // earns; a failed read guesses neither way.
    s_deferSticky[i] = (unsigned char)ekm_defer_sticky_next(
        s_deferSticky[i], picnum, (int)statnum, pendingKnown ? 1 : 0, pending);
    const int sticky = s_deferSticky[i];
    const short extra =
        (short)ekm_actor_effective_extra(picnum, rawExtra, pending, sticky);

    // Display only: where the discount already went into `extra`, nothing more comes off. One predicate decides both.
    // It must never reach a targeting rule (actorstate.h).
    const short hpShown = (short)ekm_actor_shown_extra(
        extra, ekm_actor_discounts_pending(picnum, sticky) ? EK_HTEXTRA_NONE
                                                           : pending);

    // A trooper playing dead stays in this list and is not a target: every sprite field says corpse (extra 0, cstat
    // cleared, frame 54), so only the CON action record can see it. The aimbot refuses it on its own hittable rule.
    const bool couldBeDown = ekm_picnum_is_trooper_family(picnum) && extra <= 0 &&
                             !ekm_target_hittable(xrep, cstat);
    const bool playDead = couldBeDown && actorstate_is_playing_dead(i, picnum);

    // Choking or falling? Only a trooper the choking rule could take (zero health, cstat still hittable) reaches the
    // decoder, so a room of live enemies pays nothing.
    const int suffering =
        (ekm_picnum_is_trooper_family(picnum) && extra == 0 &&
         ekm_target_hittable(xrep, cstat)) ? actorstate_suffering(i, picnum)
                                           : EKM_SUFFER_UNKNOWN;

    // A statement of its own. monstatus is read unconditionally: it decides the monstatus refusal
    // (ekm_target_monstatus_shootable, aim.cpp) and feeds the death record.
    int monstatus = 0;
    bool haveMonstatus = ekm_gv_view_get(monstatusView, i, &monstatus);
    if (!haveMonstatus) monstatus = 0;

    // The death record, only where it can change anything: `extra <= 0` is already DOWN, so the decode is gated on
    // health reading alive. ekm_action_is_dying covers the trooper family and the Assault Commander. Every other picnum
    // takes AA's monstatus 2, which only the admitted notenemy list reads (an SSLIME corpse at extra 30).
    const int dyingRecord =
        (extra > 0 && (ekm_picnum_is_trooper_family(picnum) || picnum == EK_PICNUM_COMMANDER))
            ? actorstate_dying_record(i, picnum)
            : ekm_aa_notenemy_dying_record(picnum, haveMonstatus, monstatus);

    const bool crashing = targets_vehicle_crashing(i, picnum, extra, true);

    bool selfDestruct = false;
    if (ekm_picnum_is_term_torso(picnum)) {
      if (!countvarcViewTaken) {
        countvarcViewTaken = true;
        gamevars_countvarc_view(&countvarcView);
      }
      int countvarc = 0;
      const bool gotCountvarc = ekm_gv_view_get(countvarcView, i, &countvarc);
      selfDestruct = targets_torso_self_destructing(i, picnum, extra, gotCountvarc,
                                                    countvarc, true);
    }

    bool growing = false;
    if (ekm_picnum_is_polyback(picnum)) {
      if (!initflagsViewTaken) {
        initflagsViewTaken = true;
        gamevars_initflags_view(&initflagsView);
      }
      int initflags = 0;
      const bool gotInitflags = ekm_gv_view_get(initflagsView, i, &initflags);
      growing = ekm_polypad_growing_aa(picnum, gotInitflags, initflags, (int)xrep);
      targets_polypad_growth_note(i, picnum, extra, initflags, (int)xrep, growing);
    }

    const int state = ekm_actor_state(picnum, extra, pal, cstat, xrep, playDead,
                                      suffering, dyingRecord, crashing, selfDestruct,
                                      wormState, growing);

    // Ahead of the filter: the frames that matter are where the verdict flips, and half are refusals. Instrumentation.
    toilet_trace(i, picnum, (int)cstat, (int)rawExtra, (int)extra, pending,
                 state, dyingRecord);

    if (!ekm_actor_state_shown(state)) {
      // A picnum with no CON `actor` line has no strength stamped into extra, so `extra <= 0` reads it as a corpse: the
      // first candidate for the Recon report.
      if (g_cfg.debuglog) {
        targets_log_refusal(r, i, picnum, statnum, extra,
                            "state is down (dying, dead or playing dead)", enemyLike);
        s_refuseDown++;
        targets_census_note(cen, enemyLike, EKM_CEN_DOWN, picnum, statnum, extra, pal);
      }
      continue;
    }

    // AA's maximum and level for this actor, read once for every consumer, below every refusal so only a sprite that
    // reaches a list pays. A failed inithp read is stored as 0, which AA's HUD calls no answer (esp_maxhp.h); `mlevel`
    // needs a flag because its default is 1.
    int initHp = 0;
    const bool haveInitHp = ekm_gv_view_get(initHpView, i, &initHp);
    if (!haveInitHp) initHp = 0;
    int level = 0;
    const bool haveLevel = ekm_gv_view_get(levelView, i, &level);
    if (!haveLevel) level = 0;

    // The one fill, so a field added to EkTarget reaches both lists. `slot` is the next free entry or the farthest one
    // displaced.
    EkTarget* const list = isFriendly ? friendlyOut : out;
    int* const listCount = isFriendly ? &friendlies : &count;
    if (slot < *listCount) targets_census_listed(cen, list[slot], isFriendly, true);
    EkTarget& t = list[slot];
    t.sprite = i;
    t.x = sx;
    t.y = sy;
    t.z = *(const int*)(r + EK_SPR_Z);
    t.sect = *(const short*)(r + EK_SPR_SECTNUM);
    t.hp = extra;
    t.hpShown = hpShown;
    t.picnum = picnum;
    t.state = state;
    t.xrep = xrep;
    t.yrep = yrep;
    t.cstat = cstat;
    t.pal = pal;
    t.initHp = initHp;
    t.level = level;
    t.haveLevel = haveLevel;
    t.haveInitHp = haveInitHp;
    t.monstatus = monstatus;
    t.haveMonstatus = haveMonstatus;
    t.asleepDiscardsHits = ekm_target_asleep_discards_hits(statnum, ready, enemyTile);
    t.statnum = statnum;
    t.attachment = attachment;

    int tw = 0, th = 0;
    tile_size(picnum, &tw, &th);
    if (tw <= 0 || th <= 0) { tw = 64; th = 64; }
    t.tw = tw;
    t.th = th;

    t.heightZ = ekm_sprite_height_z(yrep, th);
    t.feetZ = ekm_feet_z(t.z, t.heightZ, cstat);

    // The farthest is found once when the list fills and again after each replacement: at worst one 256-entry scan per
    // admitted sprite past the cap.
    long long* const dist2 = isFriendly ? s_friendDist2 : s_hostDist2;
    const int cap = isFriendly ? friendlyMax : max;
    dist2[slot] = d2;
    if (slot == *listCount) (*listCount)++;
    if (*listCount >= cap)
      *(isFriendly ? &friendFar : &hostFar) = ekm_target_farthest(dist2, *listCount);
  }

  if (cen) {
    for (int k = 0; k < count; k++) targets_census_listed(cen, out[k], false, false);
    for (int k = 0; k < friendlies; k++)
      targets_census_listed(cen, friendlyOut[k], true, false);
    cen->farHostile = targets_far_units(s_hostDist2, count, haveMe);
    cen->farFriendly = targets_far_units(s_friendDist2, friendlies, haveMe);
  }
  targets_census_end(cen, friendlyOut != NULL);

  if (friendlyCount) *friendlyCount = friendlies;
  return count;
}


// One sprite, one question (targets.h). The terms are the collector's, in its order, with logging left out and the
// aimbot's hittable rule last. The first three terms are shared in ekm_target_picnum_refusal; from the health read down
// this is a deliberate second copy.
bool targets_sprite_targetable(int sprite) {
  if (sprite < 0 || sprite >= EK_MAXSPRITES) return false;
  const unsigned char* const r = gamestate_sprite(sprite);
  if (!r) return false;

  const short statnum = *(const short*)(r + EK_SPR_STATNUM);
  if (statnum != EK_STAT_ACTOR && statnum != EK_STAT_ZOMBIE) return false;

  const short picnum = *(const short*)(r + EK_SPR_PICNUM);

  const bool ready = badguy_ready();
  const bool enemyTile = ready ? picnum_is_enemy(picnum) : false;
  const bool critter = picnum_is_harmless_critter(picnum);
  int refusal = ekm_target_picnum_refusal(picnum, ready, enemyTile, critter, false, 0);
  if (refusal == EKM_REFUSE_NONE) {
    int team = 0;
    const bool haveTeam = gamevars_team(sprite, &team);
    int monstflags = 0, botMonstatus = 0;
    bool gotMonstflags = false, gotBotMonstatus = false;
    if (ekm_picnum_is_aa_companion_bot(picnum)) {
      gotMonstflags = gamevars_monstflags(sprite, &monstflags);
      gotBotMonstatus = gamevars_monstatus(sprite, &botMonstatus);
    }
    int mandoInitflags = 0;
    bool gotMandoInitflags = false;
    if (ekm_picnum_is_aa_mando(picnum))
      gotMandoInitflags = gamevars_initflags(sprite, &mandoInitflags);
    refusal = ekm_target_picnum_refusal(picnum, ready, enemyTile, critter, haveTeam, team,
                                        gotMonstflags, monstflags,
                                        gotBotMonstatus, botMonstatus,
                                        gotMandoInitflags, mandoInitflags);
  }
  // A Sidegun of either team is refused here, so its team is not read.
  if (refusal == EKM_REFUSE_ATTACHMENT) {
    if (g_cfg.debuglog) s_refuseAttachmentTrigger++;
    return false;
  }
  if (refusal != EKM_REFUSE_NONE) return false;

  int wormState = EKM_WORM_NONE;
  if (ekm_worm_admitted(picnum)) {
    const int part = ekm_worm_part(picnum);
    int countvar = 0, dashtime = 0;
    const bool gotCountvar =
        part == EKM_WORM_PART_HEAD && gamevars_countvar(sprite, &countvar);
    const bool gotDashtime = gamevars_dashtime(sprite, &dashtime);
    wormState = ekm_worm_state_read(part, gotCountvar, countvar, gotDashtime,
                                    dashtime).state;
    if (wormState == EKM_WORM_NONE) return false;
  }

  const unsigned char xrep = r[EK_SPR_XREPEAT];
  const unsigned char yrep = r[EK_SPR_YREPEAT];
  if (xrep == 0 || yrep == 0) return false;

  const unsigned short cstat = *(const unsigned short*)(r + EK_SPR_CSTAT);
  const short rawExtra = *(const short*)(r + EK_SPR_EXTRA);
  const unsigned char pal = r[EK_SPR_PAL];

  const int sticky = s_deferSticky[sprite];
  int pending = EK_HTEXTRA_NONE;
  if (ekm_actor_discounts_pending(picnum, sticky))
    if (!actorstate_pending_damage(sprite, &pending)) pending = EK_HTEXTRA_NONE;
  const short extra =
      (short)ekm_actor_effective_extra(picnum, rawExtra, pending, sticky);

  const bool couldBeDown = ekm_picnum_is_trooper_family(picnum) && extra <= 0 &&
                           !ekm_target_hittable(xrep, cstat);
  const bool playDead = couldBeDown && actorstate_is_playing_dead(sprite, picnum);

  const int suffering =
      (ekm_picnum_is_trooper_family(picnum) && extra == 0 &&
       ekm_target_hittable(xrep, cstat)) ? actorstate_suffering(sprite, picnum)
                                         : EKM_SUFFER_UNKNOWN;


  int monstatus = 0;
  const bool haveMonstatus = gamevars_monstatus(sprite, &monstatus);

  const int dyingRecord =
      (extra > 0 && (ekm_picnum_is_trooper_family(picnum) || picnum == EK_PICNUM_COMMANDER))
          ? actorstate_dying_record(sprite, picnum)
          : ekm_aa_notenemy_dying_record(picnum, haveMonstatus, monstatus);

  const bool crashing = targets_vehicle_crashing(sprite, picnum, extra, false);

  bool selfDestruct = false;
  if (ekm_picnum_is_term_torso(picnum)) {
    int countvarc = 0;
    const bool gotCountvarc = gamevars_countvarc(sprite, &countvarc);
    selfDestruct = targets_torso_self_destructing(sprite, picnum, extra, gotCountvarc,
                                                  countvarc, false);
  }

  bool growing = false;
  if (ekm_picnum_is_polyback(picnum)) {
    int initflags = 0;
    const bool gotInitflags = gamevars_initflags(sprite, &initflags);
    growing = ekm_polypad_growing_aa(picnum, gotInitflags, initflags, (int)xrep);
  }

  const int state = ekm_actor_state(picnum, extra, pal, cstat, xrep, playDead,
                                    suffering, dyingRecord, crashing, selfDestruct,
                                    wormState, growing);
  if (!ekm_actor_state_shown(state)) return false;
  if (state == EKA_SELFDESTRUCT) {
    if (g_cfg.debuglog) s_refuseSelfDestructTrigger++;
    return false;
  }
  if (ekm_actor_state_worm_refused(state)) {
    if (g_cfg.debuglog) s_refuseWormTrigger++;
    return false;
  }
  // Shown but not targetable: a crashing Recon, or a play-dead trooper. Ahead of the monstatus term so its kill-switch
  // counter does not absorb this.
  if (!ekm_actor_state_targetable(state)) return false;

  // Asleep and unflagged, so the engine discards the hit. Ahead of the monstatus term so neither kill-switch absorbs
  // the other's refusals.
  if (ekm_target_asleep_discards_hits(statnum, ready, enemyTile)) {
    if (g_cfg.debuglog) s_refuseAsleepTrigger++;
    return false;
  }

  // tbot.cpp:621 calls this on ANY live or choking enemy the ray hit, so refusing in aim.cpp alone would leave the
  // trigger firing on a monstatus -1 actor. Placed LAST so the kill-switch counter does not claim corpses or
  // non-hittable sprites. A failed read leaves the sprite SHOOTABLE. STAT_ACTOR only.
  if (!ekm_target_monstatus_shootable(haveMonstatus, monstatus, statnum)) {
    if (g_cfg.debuglog) s_refuseMonstatusTrigger++;
    return false;
  }

  return ekm_target_hittable(xrep, cstat);
}

bool targets_sprite_picnum(int sprite, int* out) {
  if (!out || sprite < 0 || sprite >= EK_MAXSPRITES) return false;
  const unsigned char* const r = gamestate_sprite(sprite);
  if (!r) return false;
  *out = (int)*(const short*)(r + EK_SPR_PICNUM);
  return true;
}
