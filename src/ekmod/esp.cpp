// The ESP: a box and a label over every live actor. SIZE COMES FROM THE SOURCE: polymer.cpp:3983-3999 builds a facing
// sprite's quad with xratio = xrepeat*0.20 and yratio = yrepeat*0.25, and polymer-y is -z/16, so in Build z the
// height is 4*yrepeat*tilesiz.y and the half-width 0.1*xrepeat*tilesiz.x.

#include <windows.h>
#include <stdio.h>
#include "imgui/imgui.h"
#include "gamestate.h"
#include "project.h"
#include "esp.h"
#include "projmath.h"
#include "targets.h"
#include <math.h>
#include "config.h"
#include "log.h"
#include "ui/common.h"
#include "ui/canvas.h"
#include "ui/font.h"
#include "ui/wallclock.h"
#include "esp_layout.h"
#include "esp_names.h"
#include "badguy.h"
#include "esp_names_aa.h"
#include "esp_names_chain.h"
#include "esp_names_derived_aa.h"
#include "firstsight.h"
#include "esp_maxhp.h"
#include "gamevars.h"
#include "tilenames.h"
#include "esp_items.h"
#include "esp_items_runtime_aa.h"
#include "gamearrays.h"
#include "esp_itembox.h"
#include "esp_drawntile.h"
#include "itemcensus.h"
#include "esp_breakables.h"
#include "esp_cash_aa.h"
#include "esp_figurines_aa.h"
#include "esp_chests_aa.h"
#include "esp_itemtake_aa.h"
#include "esp_switches.h"
#include "esp_tint.h"
#include "worldquad.h"
#include "tiles.h"
#include "actorstate.h"
#include "playerstate.h"
#include "aimsolve.h"
#include "perf.h"
#include "aim.h"
#include "camera.h"
#include "mapextent.h"
#include "sectorstate.h"
#include "wallstate.h"
#include "secretvol.h"
#include "secrettrig.h"
#include "secretpath.h"
#include "tracer.h"

// THE MAXIMUM IS OBSERVED, NOT TABULATED: EDuke32's strengths come from CON and any mod (AA) may change them, so the
// highest `extra` seen for a sprite index is its maximum. NO BAR UNTIL THERE IS A REAL MAXIMUM (a bar against a guess
// is worse than none). KEYED ON THE OCCUPANT (ekm_health_latch): the engine recycles sprite indices.
static EkmHealthLatch s_health[EK_MAXSPRITES];

void esp_forget_health(void) {
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    s_health[i].max = 0;
    s_health[i].picnum = 0;
    s_health[i].pal = 0;
  }
}

static const char* ekm_actor_display_name(int picnum, int pal, int sprite);

// ONE TIER-READOUT LINE PER SPRITE, NOT PER FRAME, keyed on the slot index. The whole compared tuple is the throttle
// key; monstatus is a live state machine, so a per-slot CEILING (not a time limit) keeps the opening transitions.
const int EK_ACTOR_LOG_MAX = 12;

struct EkActorLogState {
  bool      said;
  unsigned char lines;
  short     picnum;
  int       mlevel;
  bool      haveLevel;
  int       initHp;
  bool      haveInitHp;
  short     extra;
  EkMaxTier tier;
  int       monstatus;
  bool      haveMonstatus;
};
static EkActorLogState s_actorLog[EK_MAXSPRITES];

static void esp_forget_actor_log(void) {
  for (int i = 0; i < EK_MAXSPRITES; i++) s_actorLog[i] = EkActorLogState();
}

static void esp_log_actor_tier(int sprite, short picnum, int pal,
                               int mlevel, bool haveLevel,
                               int initHp, bool haveInitHp,
                               short extra,
                               int ruledMax, int tableMax, int seen,
                               EkMaxTier tier,
                               int monstatus, bool haveMonstatus) {
  if (!g_cfg.debuglog || sprite < 0 || sprite >= EK_MAXSPRITES) return;

  EkActorLogState& st = s_actorLog[sprite];
  if (st.said && st.picnum == picnum && st.mlevel == mlevel &&
      st.haveLevel == haveLevel && st.initHp == initHp &&
      st.haveInitHp == haveInitHp && st.extra == extra && st.tier == tier &&
      st.monstatus == monstatus && st.haveMonstatus == haveMonstatus)
    return;

  if (st.lines >= (unsigned char)EK_ACTOR_LOG_MAX) return;
  st.lines = (unsigned char)(st.lines + 1);
  const bool last = st.lines >= (unsigned char)EK_ACTOR_LOG_MAX;

  const char* const name = ekm_actor_display_name(picnum, pal, sprite);
  ek_log("actor tier: sprite %d picnum %d \"%s\" | mlevel=%d read=%s | "
         "inithp=%d read=%s | extra=%d | candidates inithp=%d ruled=%d "
         "table=%d seen=%d | WON: %s | monstatus=%d read=%s%s",
         sprite, (int)picnum, name ? name : "?",
         mlevel, haveLevel ? "yes" : "NO-ANSWER",
         initHp, haveInitHp ? "yes" : "NO-ANSWER",
         (int)extra, initHp, ruledMax, tableMax, seen,
         ekm_actor_max_tier_name(tier),
         monstatus, haveMonstatus ? "yes" : "NO-ANSWER",
         last ? " | LAST LINE FOR THIS SLOT (ceiling reached; later changes are NOT reported)" : "");

  st.said = true;
  st.picnum = picnum;
  st.mlevel = mlevel;
  st.haveLevel = haveLevel;
  st.initHp = initHp;
  st.haveInitHp = haveInitHp;
  st.extra = extra;
  st.tier = tier;
  st.monstatus = monstatus;
  st.haveMonstatus = haveMonstatus;
}

static int health_max_for(int index, short hp, int picnum, int pal) {
  if (index < 0 || index >= EK_MAXSPRITES) return 0;
  return ekm_health_latch(&s_health[index], picnum, pal, (int)hp);
}

static int health_max_seen(int index, int picnum, int pal) {
  if (index < 0 || index >= EK_MAXSPRITES) return 0;
  const EkmHealthLatch& slot = s_health[index];
  if (!ekm_same_enemy(picnum, pal, (int)slot.picnum, (int)slot.pal)) return 0;
  return (int)slot.max;
}

static int esp_dist2d(int cx, int cy, int x, int y) {
  const double dx = (double)x - (double)cx, dy = (double)y - (double)cy;
  return clamp_double_to_int(sqrt(dx * dx + dy * dy) + 0.5);
}

int esp_dist2d_public(int cx, int cy, int x, int y) { return esp_dist2d(cx, cy, x, y); }

static int esp_distance_metres(int cx, int cy, int x, int y) {
  const double dx = (double)x - (double)cx, dy = (double)y - (double)cy;
  const double d = sqrt(dx * dx + dy * dy) / (double)BUILD_UNITS_PER_METRE;
  return clamp_double_to_int(d + 0.5);
}

static bool s_hivis = false;
static EspStrokes s_st = esp_strokes(0, false);
static void esp_frame_style(void) {
  s_hivis = g_cfg.hivis != 0;
  s_st = esp_strokes(canvas_frame_height(), s_hivis);
}

static void esp_draw_brackets(int x, int y, int w, int h, unsigned colour) {
  const int cx = esp_bracket_arm(w);
  const int cy = esp_bracket_arm(h);
  const int t  = esp_bracket_thickness(cx, cy, s_st.bracketT);
  if (t <= 0) return;
  const int x1 = x + w, y1 = y + h;
  const EspRect r[8] = {
    { x,       y,       cx, t  }, { x,       y,       t,  cy },
    { x1 - cx, y,       cx, t  }, { x1 - t,  y,       t,  cy },
    { x,       y1 - t,  cx, t  }, { x,       y1 - cy, t,  cy },
    { x1 - cx, y1 - t,  cx, t  }, { x1 - t,  y1 - cy, t,  cy } };
  esp_keyed_rects(r, 8, colour, s_st.keyline);
}

static void esp_collect(EkMarker* markers, int* count, int* seq, int* overflow,
                        int kind, int index,
                        const EkScreenPt& p, const EkScreenBox& box) {
  if (*count >= EK_MARKER_COLLECT_MAX) { (*overflow)++; return; }
  EkMarker& m = markers[(*count)++];
  m.kind = kind; m.index = index; m.dist2d = p.dist2d; m.seq = (*seq)++;
  m.p = p; m.box = box;
}

// THE FRAME'S CAMERA, FOR THE METRES LABEL ONLY. NOT DERIVED FROM m.dist2d: that is already rounded to whole Build
// units, and rounding again moves the printed metres at half-metre boundaries.
static int s_frameCamX = 0, s_frameCamY = 0;

// WHICH FOUR ROWS A CATEGORY DRAWS BY, PASSED RATHER THAN READ: enemies and Friendlies each have their own set, built
// in one place so the collect's "anything switched on" gate and the draw cannot disagree.
struct EspActorRows { int boxes, healthbar, name, distance, level; };

static EspActorRows esp_enemy_rows(void) {
  const EspActorRows r = { g_cfg.espboxes, g_cfg.esphealthbar,
                           g_cfg.espname, g_cfg.espdistance, g_cfg.esplevel };
  return r;
}
static EspActorRows esp_friendly_rows(void) {
  const EspActorRows r = { g_cfg.espfriendlyboxes, g_cfg.espfriendlyhealthbar,
                           g_cfg.espfriendlyname, g_cfg.espfriendlydistance,
                           g_cfg.espfriendlylevel };
  return r;
}
static bool esp_rows_any(const EspActorRows& r) {
  return r.boxes || r.healthbar || r.name || r.distance || r.level;
}

// THE NAME CHAIN, one place so precedence cannot drift: curated table, AA's nameplates (pal/mtype/team overrides),
// AA's `default` arm (team 3 = civilian), the names.h fallback, then null (the number). Gamevar reads are COST-GATED.
// NOT USED BY ekm_same_enemy: alias-heavy tables would fold distinct enemies into one ceiling.
static unsigned char s_espNamesSeen[(EK_MAXTILES + 7) / 8];
static int  s_espNamesTier4 = 0;
static int  s_espNamesTier5 = 0;
static int  s_espNamesOOR = 0;
static bool s_espNamesOORLogged = false;

static void esp_log_name_tier(int picnum, const char* name, int pal,
                              bool haveTeam, int team) {
  if (!g_cfg.debuglog) return;

  if (picnum < 0 || picnum >= EK_MAXTILES) {
    s_espNamesOOR++;
    if (!s_espNamesOORLogged) {
      s_espNamesOORLogged = true;
      ek_log("espnames: out-of-range picnum %d, %d out-of-range so far",
             picnum, s_espNamesOOR);
    }
    return;
  }
  if (!ekm_first_sight(s_espNamesSeen, EK_MAXTILES, picnum)) return;

  if (name) s_espNamesTier4++; else s_espNamesTier5++;
  char teamBuf[8];
  if (haveTeam) snprintf(teamBuf, sizeof(teamBuf), "%d", team);
  else          snprintf(teamBuf, sizeof(teamBuf), "?");
  ek_log("espnames: tier %s for picnum %d (\"%s\"), pal %d, team %s, "
         "%d distinct so far (tier4 %d, tier5 %d)",
         name ? "4 code name" : "5 number",
         picnum, name ? name : "none", pal, teamBuf,
         s_espNamesTier4 + s_espNamesTier5, s_espNamesTier4, s_espNamesTier5);
}

static const char* ekm_actor_display_name(int picnum, int pal, int sprite) {
  int mtype = 0;
  bool haveMtype = false;
  if (ekm_enemy_name_aa_has_mtype(picnum)) {
    haveMtype = gamevars_mtype(sprite, &mtype);
  }
  int team = 0;
  bool haveTeam = false;
  if (ekm_enemy_name_aa_has_team(picnum)) {
    haveTeam = gamevars_team(sprite, &team);
  }
  const char* const named = ekm_actor_name_rules(picnum, pal, haveMtype, mtype,
                                                 haveTeam, team);
  if (named) return named;

  if (!haveTeam) haveTeam = gamevars_team(sprite, &team);
  if (haveTeam) {
    const char* const civilian = ekm_enemy_name_aa_team(team);
    if (civilian) return civilian;
  }

  // TIER 3.5: a picnum with no entry of its own may be a pose/spawn variant of one that does, or a civilian the team
  // read missed. A derived STRING answers directly; a derived BASE PICNUM reruns tiers 1-2 on that base. A null falls
  // through to tier 4 for the ORIGINAL picnum.
  const char* const derivedString = ekm_enemy_name_derived_aa_string(picnum);
  if (derivedString) return derivedString;
  const int derivedBase = ekm_enemy_name_derived_aa_base(picnum);
  if (derivedBase) {
    int baseMtype = 0;
    bool haveBaseMtype = false;
    if (ekm_enemy_name_aa_has_mtype(derivedBase)) {
      haveBaseMtype = gamevars_mtype(sprite, &baseMtype);
    }
    int baseTeam = 0;
    bool haveBaseTeam = false;
    if (ekm_enemy_name_aa_has_team(derivedBase)) {
      haveBaseTeam = gamevars_team(sprite, &baseTeam);
    }
    const char* const baseNamed = ekm_actor_name_rules_via_derived_base(
        derivedBase, pal, haveBaseMtype, baseMtype, haveBaseTeam, baseTeam);
    if (baseNamed) return baseNamed;
  }

  const char* const codeName = tilenames_name(picnum);
  esp_log_name_tier(picnum, codeName, pal, haveTeam, team);
  return codeName;
}

// One actor marker, enemy or friendly. `m.index` is the slot in THIS frame's own list for that kind. THE GEOMETRY IS
// SHARED: a friendly is drawn exactly as an enemy, in a different colour. Returns 1 when something reached the
// canvas.
static int esp_draw_actor_marker(const EkMarker& m, const EkTarget* targets,
                                 unsigned colour, const EspActorRows& rows) {
  const EkTarget& tgt = targets[m.index];
  const short picnum = tgt.picnum;
  const int bx = m.box.x, by = m.box.y;
  const int bw = m.box.w, bh = m.box.h;
  int drew = 0;

  if (rows.boxes) {
    esp_draw_brackets(bx, by, bw, bh, colour);
    drew = 1;
  }

  const short hp = tgt.hp;
  const short shownHp = tgt.hpShown;

  const int ruledMax = ekm_enemy_ruled_max((int)picnum, (int)tgt.pal);
  const int shown = ekm_display_hp((int)hp,
                                   ekm_enemy_max_strength((int)picnum),
                                   ruledMax,
                                   actorstate_has_run(tgt.sprite));

  const int seen = health_max_seen(tgt.sprite, (int)picnum, (int)tgt.pal);

  // ONE MAXIMUM FOR THE BAR AND THE LABEL (they once disagreed: a pal'd Cycloid printed 1/4500). The precedence is
  // esp_maxhp.h's, AA's own inithp first; `tgt.initHp` is 0 for stock Duke and World Tour.
  const int tableMax = ekm_enemy_max_strength((int)picnum);
  const int seenMax = ekm_actor_resolved_max(tgt.initHp, ruledMax, tableMax,
                                             seen, shown);

  // THE MEASUREMENT LINE: the same inputs the resolve used plus the read flags, via a pure tier lookup so it cannot
  // name a different tier than the one that fired.
  esp_log_actor_tier(tgt.sprite, picnum, (int)tgt.pal,
                     tgt.level, tgt.haveLevel, tgt.initHp, tgt.haveInitHp, hp,
                     ruledMax, tableMax, seen,
                     ekm_actor_max_tier(tgt.initHp, ruledMax, tableMax, seen, shown),
                     tgt.monstatus, tgt.haveMonstatus);

  // THE QUEUED DAMAGE COMES OFF THE SUBSTITUTED NUMBER, not the sprite's stale one (a Captain woken by a shot in
  // flight ends the tic on 60 less the damage). Clamped: a corpse's negative health can leave hpShown above hp.
  const int pendingSeen = (int)hp - (int)shownHp;
  const int displayHp = ekm_actor_shown_extra(shown, pendingSeen);

  if (rows.healthbar && hp > 0 && seenMax > 0) {
    const int barW = s_st.barW;
    const int barX = esp_bar_x(bx, barW);
    const int fill = canvas_bar_fill(bh, displayHp, seenMax);
    const EspRect well = { barX, by, barW, bh };
    esp_keyed_rects(&well, 1, C_WELL, s_st.keyline);
    if (fill > 0)
      canvas_rect(barX, by + (bh - fill), barW, fill,
                  canvas_health_colour(displayHp, seenMax));
    drew = 1;
  }

  // Name and health above the box in two runs (name FONT_SMALL, number FONT_MONO so digits align). AA'S LEVEL, ONLY
  // WHERE AA HAS ONE: `haveLevel` false leaves NO trace (not a blank, not "L0").
  const bool showLevel = rows.level != 0 && tgt.haveLevel;

  if (rows.name || showLevel) {
    char nmBuf[16];
    const char* nm = 0;
    if (rows.name) {
      // THE NUMBER IS THE FALLBACK, NOT THE FORMAT: an unlisted picnum prints the number rather than a wrong name.
      // Inside the name row's own gate: up to two guarded gamevar reads per label, which the Level row must not pay.
      nm = ekm_actor_display_name((int)picnum, (int)tgt.pal, (int)tgt.sprite);
      if (!nm) { wsprintfA(nmBuf, "%d", (int)picnum); nm = nmBuf; }
    }
    char labBuf[ESP_ACTOR_LABEL_BUFSIZE];
    const bool haveLabel = rows.name &&
                           esp_actor_label(labBuf, ESP_ACTOR_LABEL_BUFSIZE, nm);
    const int nameW = haveLabel ? canvas_text_width(labBuf, FONT_SMALL) : 0;

    char numBuf[ESP_HEALTH_NUM_BUFSIZE];
    const int maxhp = seenMax;
    // THE STATE LABEL REPLACES THE NUMBER: "Playing dead" where a corpse would print nothing, "Choking" where a
    // live-at-zero trooper would print 0/30. With only the Level row on, no number appears.
    const bool haveNum = rows.name &&
                         esp_health_number(numBuf, displayHp, maxhp,
                                           ekm_actor_state_label(tgt.state));
    const int numW = haveNum ? canvas_text_width(numBuf, FONT_MONO) : 0;

    const EspLabelRuns runs = esp_label_runs(bx, bw, nameW, numW);
    const int nameY = esp_actor_name_label_y(by, showLevel);
    if (haveLabel) esp_draw_label(s_hivis, runs.nameX, nameY, labBuf, C_BONE, FONT_SMALL);
    if (haveNum)
      esp_draw_label(s_hivis, runs.numX,
                     esp_label_mono_y(nameY, font_height[FONT_SMALL], font_height[FONT_MONO]),
                     numBuf, C_BONE, FONT_MONO);

    if (showLevel) {
      char levBuf[ESP_LEVEL_LABEL_BUFSIZE];
      if (esp_level_label(levBuf, ESP_LEVEL_LABEL_BUFSIZE, tgt.level)) {
        const int levW = canvas_text_width(levBuf, FONT_SMALL);
        esp_draw_label(s_hivis, esp_label_x(bx, bw, levW), esp_level_label_y(by), levBuf,
                       C_BONE, FONT_SMALL);
      }
    }

    if (haveLabel || haveNum || showLevel) drew = 1;
  }

  if (rows.distance) {
    char dm[24];
    wsprintfA(dm, "%d m", esp_distance_metres(s_frameCamX, s_frameCamY, tgt.x, tgt.y));
    esp_draw_label(s_hivis, esp_label_x(bx, bw, canvas_text_width(dm, FONT_SMALL)),
                   esp_dist_label_y(by, bh), dm, C_BONEDIM, FONT_SMALL);
    drew = 1;
  }

  return drew;
}

// One world point, projected, culled and collected. A point has no sprite box, so a degenerate box is stored. Returns
// 1 when collected, 0 when the range cull, projection or array ceiling turned it away.
static int esp_collect_point(const EkProjection& p, int kind, int index,
                             int wx, int wy, int wz,
                             EkMarker* markers, int* count, int* seq, int* overflow) {
  const int dist2d = esp_dist2d(p.cam.x, p.cam.y, wx, wy);
  if (!ekm_within_draw_distance(dist2d, g_cfg.espdrawdist, g_cfg.espnolimit)) return 0;
  float fx = 0.0f, fy = 0.0f;
  if (!project_point(p, wx, wy, wz, &fx, &fy)) return 0;
  const EkScreenPt pt = { (int)fx, (int)fy, dist2d };
  const EkScreenBox box = { (int)fx, (int)fy, 0, 0 };
  const int before = *count;
  esp_collect(markers, count, seq, overflow, kind, index, pt, box);
  return *count != before;
}

// Secret areas: one marker per sector still carrying the secret lotag. FOUND SECRETS DISAPPEAR ON THEIR OWN:
// sector.cpp:3293 zeroes the lotag when the player stands in it, so there is deliberately no found-set.
static void esp_collect_secrets(const EkProjection& p, EkMarker* markers,
                                int* count, int* seq, int* overflow) {
  const int ns = mapextent_numsectors();
  for (int sec = 0; sec < ns; sec++) {
    EkSectorInfo si = { 0, 0, 0, 0, 0, 0 };
    if (!sectorstate_info(sec, &si)) continue;
    if (!ekm_sector_is_secret(si.lotag)) continue;

    EkSectorCentre c = { 0, 0, 0, 0, 0, 0 };
    int accepted = 0;
    bool corrupt = false;
    for (int k = 0; k < si.wallnum; k++) {
      const int wallIndex = si.wallptr + k;
      int p2 = 0;
      if (!wallstate_point2(wallIndex, &p2)) { corrupt = true; break; }
      if (!ekm_point2_ok(p2, wallIndex, si.wallptr, si.wallnum)) { corrupt = true; break; }
      EkWallCorner wc = { 0, 0, 0 };
      if (!wallstate_corner(wallIndex, &wc)) { corrupt = true; break; }
      ekm_sector_centre_accumulate(&c, wc.x, wc.y);
      accepted++;
    }
    if (corrupt) continue;
    if (!ekm_sector_centre_finish(&c, accepted)) continue;

    esp_collect_point(p, EK_MARKER_SECRET, sec, c.x, c.y, si.florz,
                      markers, count, seq, overflow);
  }
}

// The secret volume's own five numbers per pass, FIVE because a screen with no panels has five causes that look
// identical: nothing staged, staged but none drawn, rooms skipped (caps), walls inexact, and quads pushed.
static int s_secvolStaged = 0, s_secvolDrawn = 0, s_secvolSkipped = 0,
           s_secvolUnreadable = 0, s_secvolInexact = 0, s_secvolQuads = 0;
int esp_secretvol_staged(void)     { return s_secvolStaged; }
int esp_secretvol_drawn(void)      { return s_secvolDrawn; }
int esp_secretvol_skipped(void)    { return s_secvolSkipped; }
int esp_secretvol_unreadable(void) { return s_secvolUnreadable; }
int esp_secretvol_inexact(void)    { return s_secvolInexact; }
int esp_secretvol_quads(void)      { return s_secvolQuads; }

// Every undiscovered secret room in range, one translucent panel per wall of its loop. IT IS DRAWN BY THE NEXT
// FRAME'S FLUSH: this runs inside SwapBuffers, which ends the frame the draw point's flush already ran in
// (worldquad.h).
static void esp_push_secret_volumes(const EkProjection& p) {
  if (!secretvol_ready()) return;

  // ACQUIRED ONCE FOR THE WHOLE PASS (secretvol.h's contract): the cache is double-buffered and a found secret
  // REPACKS the array, so a second acquisition mid-loop would hand back another room's wall.
  const int gen = secretvol_generation();

  s_secvolStaged     = secretvol_rooms(gen);
  s_secvolSkipped    = secretvol_skipped(gen);
  s_secvolUnreadable = secretvol_unreadable(gen);
  s_secvolInexact    = secretvol_inexact(gen);

  for (int i = 0; i < s_secvolStaged; i++) {
    int sectnum = 0, cx = 0, cy = 0, first = 0, n = 0;
    if (!secretvol_room(gen, i, &sectnum, &cx, &cy, &first, &n)) continue;

    // THE LOTAG IS RE-READ LIVE: the cache is up to a second old, so panels would OUTLIVE THE CROSS (sector.cpp:3293
    // zeroes the lotag when the player stands in it). Same predicate as the cross (sectorstate.h).
    EkSectorInfo si = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!sectorstate_info(sectnum, &si)) continue;
    if (!ekm_sector_is_secret(si.lotag)) continue;

    // THE WALL COUNT IS COMPARED because it is already in hand: a mitigation for the map-change window, in which a
    // sector secret in BOTH maps would draw the OLD map's walls. Same index and count with different vertices is the
    // known residue.
    if (si.wallnum != n) continue;

    const int dist2d = esp_dist2d(p.cam.x, p.cam.y, cx, cy);
    if (!ekm_within_draw_distance(dist2d, g_cfg.espdrawdist, g_cfg.espnolimit))
      continue;

    s_secvolDrawn++;
    for (int k = 0; k < n; k++) {
      const EkSecretWall* const w = secretvol_wall(gen, first + k);
      if (!w) continue;

      double x4[4] = { 0, 0, 0, 0 }, y4[4] = { 0, 0, 0, 0 };
      int z4[4] = { 0, 0, 0, 0 };
      if (ekm_secvol_quad_corners(*w, x4, y4, z4) != 4) continue;

      EkWorldQuad q;
      for (int c = 0; c < 4; c++) { q.x[c] = x4[c]; q.y[c] = y4[c]; q.z[c] = z4[c]; }
      q.argb = C_SECRET_VOLUME;
      worldquad_push_at(q, EK_WQ_KIND_SECRET, sectnum);
      s_secvolQuads++;
    }
  }
}

// ITEM-BOX TRACE: boxes both too big and too small point at a PER-ITEM input (~7000 unsized tilesiz entries silently
// get the 64x64 fallback). ekm_tile_size_is_fallback is the decisive field, never the printed w/h. KEYED ON THE
// INPUTS: a size logged before `tilesiz` was located must be corrected later.
static unsigned s_itemPicnumSig[EK_MAXTILES_ARR];
static unsigned char s_itemPicnumLines[EK_MAXTILES_ARR];
static int s_itemPicnumsSeen = 0;
static int s_itemPicnumsFallback = 0;
static int s_itemPicnumsVoxel = 0;
static int s_itemTraceLines = 0;
static int s_itemTraceCapped = 0;
static int s_itemDrawnDiffLines = 0;
static int s_itemBoxSuppressed = 0;

// A CEILING, BECAUSE THE VERDICT IS IN THE SIGNATURE: an item parked where its projected height crosses the 2-pixel
// floor would flip and print every few frames. The count of picnums that reached it is printed.
static const int EK_ITEM_TRACE_MAX = 8;

// The signature: never zero, so a first sighting is distinguishable. It carries `rejected` and THE YAW BUCKET (128
// Build units, 16 buckets of 22.5 degrees), or a box that changes as the player walks around an item would print
// once.
static const int EK_ITEM_YAW_BUCKET = 128;

static unsigned esp_item_sig(int rawTw, int rawTh, unsigned xrep, unsigned yrep,
                             unsigned cstat, bool vox, bool rejected,
                             int yawDelta, int drawn) {
  unsigned h = 2166136261u;
  const unsigned parts[9] = { (unsigned)(rawTw + 1), (unsigned)(rawTh + 1),
                              xrep, yrep, cstat & (32u | 16u | 128u),
                              vox ? 1u : 0u, rejected ? 1u : 0u,
                              (unsigned)((yawDelta & (EKM_ANGLE_UNITS - 1)) /
                                         EK_ITEM_YAW_BUCKET),
                              (unsigned)(drawn + 1) };
  for (int i = 0; i < 9; i++) { h ^= parts[i]; h *= 16777619u; }
  return h | 0x80000000u;
}

static int esp_upgrade_array_for_pchar(int pchar) {
  switch (pchar) {
    case 0:  return EK_GA_UP_DUKE;
    case 1:  return EK_GA_UP_SHELLY;
    case 2:  return EK_GA_UP_WES;
    default: return -1;
  }
}

static const int EK_ESP_LOCAL_PLAYER = 0;

// The frame's take state, filled once per frame: an AA gamevar costs two VirtualQuery, so each is read on the first
// picnum whose row needs it and never again that frame.
static_assert(EK_PS_MAX_WEAPONS == EKM_TAKE_MAX_WEAPONS && EK_PS_GET_MAX == EKM_TAKE_GET_MAX,
              "the span read and the rule table size the same arrays");
struct EspTakeCtx {
  EkTakeState st;
  int triedStack, triedUpg;
  int triedVar[EKM_TV_COUNT];
  int hidden;
  int hiddenCat[EKM_TAKE_CAT_COUNT];
};

static const char* const kEspTakeVarName[EKM_TV_COUNT] = {
  "LASERMODE", "pistolammo", "wristammo", "charsel", "nukeselect", "weap4ammo",
  "startguns", "gametype", "gotplasma", "plasmammo", "gotrailgun", "railgunammo",
  "gotbow", "arrows", "sawammo", "dbammo", "gotDB", "startmode", "stakes",
  "nukeamount", "LASERAMMO", "COMMAMMO", "gotsaw", "gotsaber", "attmode",
  "dukeup1", "shellyup1", "wesup1", "dukeup2", "shellyup2", "wesup2",
  "es_type", "es_battery", "es_batterymax" };

static bool esp_take_read_var(int k, int* v) {
  switch (k) {
    case EKM_TV_LASERMODE:   return gamevars_take(EK_GV_TK_LASERMODE, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_PISTOLAMMO:  return gamevars_take(EK_GV_TK_PISTOLAMMO, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_WRISTAMMO:   return gamevars_take(EK_GV_TK_WRISTAMMO, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_CHARSEL:     return gamevars_take(EK_GV_TK_CHARSEL, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_NUKESELECT:  return gamevars_take(EK_GV_TK_NUKESELECT, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_WEAP4AMMO:   return gamevars_take(EK_GV_TK_WEAP4AMMO, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_STARTGUNS:   return gamevars_take(EK_GV_TK_STARTGUNS, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_GAMETYPE:    return gamevars_take(EK_GV_TK_GAMETYPE, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_GOTPLASMA:   return gamevars_take(EK_GV_TK_GOTPLASMA, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_PLASMAMMO:   return gamevars_take(EK_GV_TK_PLASMAMMO, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_GOTRAILGUN:  return gamevars_take(EK_GV_TK_GOTRAILGUN, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_RAILGUNAMMO: return gamevars_take(EK_GV_TK_RAILGUNAMMO, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_GOTBOW:      return gamevars_take(EK_GV_TK_GOTBOW, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_ARROWS:      return gamevars_take(EK_GV_TK_ARROWS, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_SAWAMMO:     return gamevars_take(EK_GV_TK_SAWAMMO, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_DBAMMO:      return gamevars_dbammo(EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_GOTDB:       return gamevars_gotdb(EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_STARTMODE:   return gamevars_shot_guard(EK_GV_SG_STARTMODE, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_STAKES:      return gamevars_take(EK_GV_TK_STAKES, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_NUKEAMOUNT:  return gamevars_take(EK_GV_TK_NUKEAMOUNT, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_LASERAMMO:   return gamevars_take(EK_GV_TK_LASERAMMO, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_COMMAMMO:    return gamevars_take(EK_GV_TK_COMMAMMO, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_GOTSAW:      return gamevars_take(EK_GV_TK_GOTSAW, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_GOTSABER:    return gamevars_take(EK_GV_TK_GOTSABER, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_ATTMODE:     return gamevars_take(EK_GV_TK_ATTMODE, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_UP_DUKE_PISTOL:    return gamearrays_upgrade(EK_GA_UP_DUKE, 1, v);
    case EKM_TV_UP_SHELLY_PISTOL:  return gamearrays_upgrade(EK_GA_UP_SHELLY, 1, v);
    case EKM_TV_UP_WES_PISTOL:     return gamearrays_upgrade(EK_GA_UP_WES, 1, v);
    case EKM_TV_UP_DUKE_SHOTGUN:   return gamearrays_upgrade(EK_GA_UP_DUKE, 2, v);
    case EKM_TV_UP_SHELLY_SHOTGUN: return gamearrays_upgrade(EK_GA_UP_SHELLY, 2, v);
    case EKM_TV_UP_WES_SHOTGUN:    return gamearrays_upgrade(EK_GA_UP_WES, 2, v);
    case EKM_TV_ES_TYPE:           return gamevars_take(EK_GV_TK_ES_TYPE, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_ES_BATTERY:        return gamevars_take(EK_GV_TK_ES_BATTERY, EK_ESP_LOCAL_PLAYER, v);
    case EKM_TV_ES_BATTERYMAX:     return gamevars_take(EK_GV_TK_ES_BATTERYMAX, EK_ESP_LOCAL_PLAYER, v);
    default:                 return false;
  }
}

static void esp_take_begin(EspTakeCtx* t, bool gotPchar, int pchar) {
  *t = EspTakeCtx();
  t->triedStack = -1;
  t->triedUpg = -1;
  for (int k = 0; k < EKM_TV_COUNT; k++) t->triedVar[k] = -1;
  int hp = 0;
  const bool gotBody = player_read_body(&hp, NULL);
  t->st.gotBody = gotBody;
  t->st.hp = hp;
  PlayerTakeSpan span;
  if (player_read_take_span(&span)) {
    t->st.gotPs = true;
    t->st.maxhp = span.maxhp;
    for (int i = 0; i < EKM_TAKE_MAX_WEAPONS; i++) {
      t->st.maxAmmo[i] = span.maxAmmo[i];
      t->st.ammo[i] = span.ammo[i];
    }
    for (int i = 0; i < EKM_TAKE_GET_MAX; i++) t->st.inv[i] = span.inv[i];
    t->st.gotAccess = span.gotAccess;
    t->st.gotWeapon = span.gotWeapon;
  }
  t->st.gotPchar = gotPchar;
  t->st.pchar = pchar;
}

static void esp_take_lazy(EspTakeCtx* t, int picnum) {
  if (!t->st.gotPs) return;
  const unsigned long long mask = ekm_take_var_mask(picnum);
  for (int k = 0; mask && k < EKM_TV_COUNT; k++) {
    if (!(mask & (1ull << k)) || t->triedVar[k] >= 0) continue;
    int v = 0;
    const bool ok = esp_take_read_var(k, &v);
    t->triedVar[k] = ok ? 1 : 0;
    t->st.gotVar[k] = ok;
    t->st.var[k] = v;
  }
  if (picnum == 55 && t->triedStack < 0) {
    int v = 0;
    const bool ok = gamevars_steroidstack(EK_ESP_LOCAL_PLAYER, &v);
    t->triedStack = ok ? 1 : 0;
    t->st.gotStack = ok;
    t->st.steroidStack = v;
  }
  if ((picnum == 51 || picnum == 52) && t->triedUpg < 0) {
    int v = 0;
    const bool ok = gamevars_inven_upgrades(EK_ESP_LOCAL_PLAYER, &v);
    t->triedUpg = ok ? 1 : 0;
    t->st.gotUpg = ok;
    t->st.invenUpgrades = v;
  }
}

static void esp_take_log(const EspTakeCtx& t, int marked) {
  if (!g_cfg.debuglog) return;
  const EkTakeState& s = t.st;
  const int plausible = (s.gotPs && ekm_takestate_plausible(s.maxhp, s.maxAmmo,
                             EKM_TAKE_MAX_WEAPONS, s.gotBody, s.hp)) ? 1 : 0;
  const int nKey = 12 + EKM_TV_COUNT;
  int now[12 + EKM_TV_COUNT] = { t.hidden, t.hiddenCat[EKM_TAKE_CAT_HEALTH],
                        t.hiddenCat[EKM_TAKE_CAT_AMMO], t.hiddenCat[EKM_TAKE_CAT_INVENTORY],
                        t.hiddenCat[EKM_TAKE_CAT_KEYCARD], t.hiddenCat[EKM_TAKE_CAT_WEAPON],
                        s.gotBody ? 1 : 0, s.gotPs ? 1 : 0, plausible,
                        s.gotPchar ? 1 : 0, t.triedStack, t.triedUpg };
  for (int k = 0; k < EKM_TV_COUNT; k++) now[12 + k] = t.triedVar[k];
  static int last[12 + EKM_TV_COUNT];
  static bool seeded = false;
  if (!seeded) { for (int i = 0; i < nKey; i++) last[i] = -99; seeded = true; }
  bool moved = false;
  for (int i = 0; i < nKey; i++) if (now[i] != last[i]) moved = true;
  if (!moved) return;
  static DWORD lastMs = 0;
  static bool logged = false;
  const DWORD ms = GetTickCount();
  if (logged && ms - lastMs < 1000) return;
  logged = true; lastMs = ms;
  for (int i = 0; i < nKey; i++) last[i] = now[i];
  char vars[512];
  vars[0] = 0;
  int at = 0;
  for (int k = 0; k < EKM_TV_COUNT && at < (int)sizeof(vars) - 1; k++) {
    if (t.triedVar[k] < 0) continue;
    const int n = (t.triedVar[k] == 1)
        ? _snprintf_s(vars + at, sizeof(vars) - at, _TRUNCATE, " %s=%d",
                      kEspTakeVarName[k], s.var[k])
        : _snprintf_s(vars + at, sizeof(vars) - at, _TRUNCATE, " %s=?",
                      kEspTakeVarName[k]);
    if (n < 0) break;
    at += n;
  }
  ek_log("itemtake: hidden %d (whole level; health %d, ammo %d, inventory %d, "
         "keycard %d, weapon %d), marked %d (on screen, a different population); "
         "state maxhp=%d hp=%d read ok=body %d struct %d plausible %d pchar %d "
         "stack %d upg %d (-1 not needed); ammo/max 1:%d/%d 2:%d/%d 3:%d/%d "
         "4:%d/%d 5:%d/%d 6:%d/%d 7:%d/%d 8:%d/%d 9:%d/%d 11:%d/%d "
         "gotweapon=0x%X; vars tried:%s",
         t.hidden, now[1], now[2], now[3], now[4], now[5], marked, s.maxhp, s.hp,
         now[6], now[7], plausible, now[9], t.triedStack, t.triedUpg,
         s.ammo[1], s.maxAmmo[1], s.ammo[2], s.maxAmmo[2], s.ammo[3], s.maxAmmo[3],
         s.ammo[4], s.maxAmmo[4], s.ammo[5], s.maxAmmo[5], s.ammo[6], s.maxAmmo[6],
         s.ammo[7], s.maxAmmo[7], s.ammo[8], s.maxAmmo[8], s.ammo[9], s.maxAmmo[9],
         s.ammo[11], s.maxAmmo[11], (unsigned)s.gotWeapon, vars[0] ? vars : " none");
}

// The pickups: A SEPARATE WALK FROM targets_collect, so a pickup is never in an EkTarget and can never be a candidate
// for the aimbot or triggerbot. Gated on g_cfg.espitems. The four live name terms are the frame's, never read per
// sprite.
static void esp_collect_items(const EkProjection& p,
                              bool gotPchar, int pchar,
                              bool gotUpg, int upgBits,
                              EspTakeCtx* take,
                              EkMarker* markers,
                              int* count, int* seq, int* overflow) {
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;

    const short statnum = *(const short*)(r + EK_SPR_STATNUM);
    if (statnum != EK_STAT_ACTOR && statnum != EK_STAT_ZOMBIE) continue;

    const short picnum = *(const short*)(r + EK_SPR_PICNUM);

    if (ekm_item_census_is_figurine((int)picnum)) continue;

    const unsigned char pal = r[EK_SPR_PAL];
    const char* nm = ekm_item_name_pal((int)picnum, (int)pal);
    if (!nm) continue;

    char nameBuf[32];
    nm = ekm_item_label_aa((int)picnum, (int)pal, nm, gotPchar, pchar,
                           gotUpg, upgBits, nameBuf, (int)sizeof nameBuf);

    const int cstat = (int)*(const short*)(r + EK_SPR_CSTAT);
    const unsigned char xrep = r[EK_SPR_XREPEAT];
    const unsigned char yrep = r[EK_SPR_YREPEAT];
    if (!ekm_item_present(cstat, (int)xrep)) continue;

    if (take) {
      esp_take_lazy(take, (int)picnum);
      if (ekm_item_refused((int)picnum, (int)pal, take->st)) {
        take->hidden++;
        take->hiddenCat[ekm_item_take_category((int)picnum)]++;
        continue;
      }
    }

    if (picnum == EKM_PIC_HEAVYHBOMB) {
      const unsigned char* const act = actorstate_record(i);
      if (!act) continue;
      const int tdata0 = *(const int*)(act + EK_ACT_T_DATA);
      const int xvel   = (int)*(const short*)(r + EK_SPR_XVEL);
      if (!ekm_pipebomb_takeable(tdata0, xvel)) continue;
    }

    const int x = *(const int*)(r + EK_SPR_X);
    const int y = *(const int*)(r + EK_SPR_Y);
    const int z = *(const int*)(r + EK_SPR_Z);

    const int dist2d = esp_dist2d(p.cam.x, p.cam.y, x, y);
    if (!ekm_within_draw_distance(dist2d, g_cfg.espdrawdist, g_cfg.espnolimit))
      continue;

    // WHICH TILE THE ENGINE ACTUALLY DRAWS THIS SPRITE AS, read HERE because everything below is sized from it. Three
    // sources in order, each refusal falling through, the last being the stored picnum (esp_drawntile.h).
    int dispicnum = EKM_DISPICNUM_UNREADABLE, curFrame = 0;
    actorstate_drawn_tile_inputs(i, &dispicnum, &curFrame);

    int actStart = 0, actView = 0;
    bool actUsable = false;
    {
      // THE ENGINE SKIPS THE WHOLE OFFSET BLOCK UNLESS THE TILE HAS A CON ACTOR (G_TileHasActor), and `cactor`
      // changes a picnum while leaving t_data[4] alone, so a stale action id would add a startframe the engine does
      // not.
      int actionId = 0;
      EkAction act;
      const bool hasActor = (badguy_tile_record((int)picnum) != 0);
      const bool decoded = hasActor && actorstate_action_at(i, &actionId, &act);
      if (decoded && ekm_action_frameoffset_known(act.viewtype)) {
        actStart  = act.startframe;
        actView   = act.viewtype;
        actUsable = true;
      }
    }
    const int drawn = ekm_item_drawn_tile((int)picnum, dispicnum, actUsable,
                                          actStart, actView, curFrame, 0,
                                          EK_MAXTILES_ARR);

    // The same four facts targets.cpp derives for an enemy. FROM THE DRAWN TILE, not the stored one: `drawn` is the
    // stored picnum whenever nothing better was found.
    int tw = 0, th = 0;
    tile_size(drawn, &tw, &th);
    // Captured BEFORE the fallback overwrites tw/th: the measurement needs the raw tile_size answer to tell a real
    // 64x64 tile from the fallback (ekm_tile_size_is_fallback).
    const int rawTw = tw, rawTh = th;
    if (tw <= 0 || th <= 0) { tw = 64; th = 64; }

    // TILE OR VOXEL: AA draws most pickups as voxels, which stand proud of the flat plane their tile describes, so
    // the tile is the wrong ruler for 51 of 125 rows (esp_itembox.h). The voxel spins in WORLD space with the
    // sprite's own ang alone, so a screen-aligned box needs that ang against the camera's (ang is at 0x1C).
    const int sprAng = (int)*(const short*)(r + EK_SPR_ANG);
    const int yawDelta = sprAng - p.cam.ang;

    const EkItemBox ib = ekm_item_box(drawn, z, (int)xrep, (int)yrep,
                                      (unsigned)cstat, tw, th, yawDelta);
    const float heightZ = ib.heightZ;
    const int   feetZ   = ib.feetZ;
    const int   height  = (int)heightZ;

    float fx = 0.0f, fy = 0.0f;
    if (!project_point(p, x, y, feetZ, &fx, &fy)) continue;

    // Before tilesiz is located, tw/th are the substituted 64x64, so ekm_item_box_known reads the RAW reading: a
    // tile-path item that fails it never gets a box sized from the guess. The voxel path never guesses.
    const bool boxKnown = ekm_item_box_known(ib.fromVoxel, rawTw, rawTh);

    EkScreenBoxF sb = { 0.0f, 0.0f, 0.0f, 0.0f, false };
    if (boxKnown &&
        !ekm_project_sprite_box(p.g, (double)(x - p.cam.x), (double)(y - p.cam.y),
                                (double)ib.halfWidth,
                                (double)(feetZ - height - p.cam.z),
                                (double)(feetZ - p.cam.z),
                                p.vp.x, p.vp.y, &sb))
      continue;

    const float boxH = sb.h;
    const bool  rejected = boxKnown && (boxH < 2.0f || boxH > 4000.0f);

    // Item-box trace: one line per distinct picnum AND per change of the inputs the box derives from, never per
    // frame. It reports the box that was actually COLLECTED, with a rejection's reason, and keeps a running count
    // beside the enumeration.
    if (g_cfg.debuglog && picnum >= 0 && picnum < EK_MAXTILES_ARR) {
      const bool fellBack = ekm_tile_size_is_fallback(rawTw, rawTh);

      const unsigned sig = esp_item_sig(rawTw, rawTh, (unsigned)xrep,
                                        (unsigned)yrep, (unsigned)cstat,
                                        ib.fromVoxel, rejected, yawDelta,
                                        drawn);
      if (s_itemPicnumSig[picnum] != sig &&
          s_itemPicnumLines[picnum] < (unsigned char)EK_ITEM_TRACE_MAX) {
        const bool first = (s_itemPicnumSig[picnum] == 0u);
        s_itemPicnumSig[picnum] = sig;
        if (first) {
          s_itemPicnumsSeen++;
          if (fellBack) s_itemPicnumsFallback++;
          if (ib.fromVoxel) s_itemPicnumsVoxel++;
        }
        s_itemPicnumLines[picnum]++;
        if (s_itemPicnumLines[picnum] == (unsigned char)EK_ITEM_TRACE_MAX)
          s_itemTraceCapped++;
        s_itemTraceLines++;
        if (drawn != (int)picnum) s_itemDrawnDiffLines++;
        if (!boxKnown) s_itemBoxSuppressed++;
        const EkVoxAa v = ekm_vox_aa_find(drawn);
        // THE PAIR HAS FLIPPED: `tile`/`vox`/`r`/`z`/`s` are now the DRAWN tile's (what sized the box);
        // `stile`/`svox` are the STORED picnum's, kept so two runs compare. Computed UNCONDITIONALLY: a not-computed
        // `0x0` placeholder reads exactly like "this tile has no size". A field that means two things means nothing.
        int sTw = 0, sTh = 0;
        tile_size((int)picnum, &sTw, &sTh);
        const EkVoxAa sv =
            (drawn != (int)picnum) ? ekm_vox_aa_find((int)picnum) : v;
        ek_log("espitem: picnum %d %s dis=%d drawn=%d stile %dx%d svox=%s "
               "sr=%.1f src=%s tile %dx%d fallback=%s "
               "vox=%s r=%.1f z=%.0f s=%.2f yaw=%d xrep=%u yrep=%u cstat=%d "
               "halfw=%.1f h=%.1f %s=%dx%d seen=%d fallback_total=%d "
               "vox_total=%d lines=%d capped=%d drawndiff=%d boxsuppressed=%d",
               (int)picnum, nm, dispicnum, drawn,
               sTw, sTh, sv.found ? "yes" : "no", sv.radius,
               ib.fromVoxel ? "voxel" : "tile",
               rawTw, rawTh, fellBack ? "yes" : "no",
               ib.fromVoxel ? "on" : (v.found ? "floor-aligned" : "none"),
               v.radius, v.sizZ, v.scale,
               yawDelta & (EKM_ANGLE_UNITS - 1),
               (unsigned)xrep, (unsigned)yrep, cstat,
               ib.halfWidth, heightZ,
               !boxKnown ? "nobox"
                         : (rejected ? (boxH < 2.0f ? "reject-small"
                                                     : "reject-huge")
                                     : "box"),
               (int)sb.w, (int)boxH, s_itemPicnumsSeen, s_itemPicnumsFallback,
               s_itemPicnumsVoxel, s_itemTraceLines, s_itemTraceCapped,
               s_itemDrawnDiffLines, s_itemBoxSuppressed);
      }
    }

    if (rejected) continue;

    const EkScreenPt pt = { (int)fx, (int)fy, dist2d };
    const EkScreenBox box = boxKnown
        ? EkScreenBox{ (int)sb.x, (int)sb.y, (int)sb.w, (int)boxH }
        : EkScreenBox{ 0, 0, 0, 0 };
    esp_collect(markers, count, seq, overflow, EK_MARKER_ITEM, i, pt, box);
  }
}

// THE ITEM REFUSAL CENSUS: read-only, NOT a second collector. The walk above tests statnum before asking whether a
// sprite is a pickup, so a named pickup on another list is dropped silently; this inverts that order. Its own `break`
// is one of the hypotheses measured: the first unreadable slot is reported whether it fires or not.
static int s_censusDone = 0, s_censusSig = 0;
static unsigned s_censusMs = 0;

static void esp_census_items(void) {
  if (!g_cfg.debuglog) return;

  // mapextent supplies only the SIGNATURE here (this walk is bounded by EK_MAXSPRITES); not-ready means no census
  // yet. Both halves in one call: read apart, the pair can come from no single map.
  if (!mapextent_ready()) return;
  int ns = 0, nw = 0;
  if (!mapextent_extent(&ns, &nw)) return;

  const int sig = (ns << 16) ^ nw;
  const unsigned now = wallclock_ms();
  if (!ekm_item_census_due(s_censusDone, sig, s_censusSig, now, s_censusMs))
    return;
  s_censusMs = now;
  s_censusSig = sig;
  s_censusDone = 1;

  int walked = 0, firstUnreadable = -1;
  int named = 0, lines = 0, suppressed = 0, figures = 0;
  int byReason[4] = { 0, 0, 0, 0 };
  int figByReason[4] = { 0, 0, 0, 0 };

  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) { firstUnreadable = i; break; }
    walked++;

    const short picnum = *(const short*)(r + EK_SPR_PICNUM);
    const unsigned char pal = r[EK_SPR_PAL];
    const char* const nm = ekm_item_name_pal((int)picnum, (int)pal);
    if (!nm) continue;
    named++;

    const short statnum = *(const short*)(r + EK_SPR_STATNUM);
    const int cstat = (int)*(const short*)(r + EK_SPR_CSTAT);
    const unsigned char xrep = r[EK_SPR_XREPEAT];
    const unsigned char yrep = r[EK_SPR_YREPEAT];
    const int reason = ekm_item_census_reason((int)statnum, cstat, (int)xrep);
    if (reason >= 0 && reason < 4) byReason[reason]++;
    if (ekm_item_census_is_figurine((int)picnum)) {
      figures++;
      if (reason >= 0 && reason < 4) figByReason[reason]++;
    }

    if (lines >= EK_ITEM_CENSUS_LINE_MAX) { suppressed++; continue; }
    lines++;
    ek_log("itemcensus: slot %d picnum %d %s statnum %d cstat %d xrep %u "
           "yrep %u reason %s at %d,%d,%d",
           i, (int)picnum, nm, (int)statnum, cstat,
           (unsigned)xrep, (unsigned)yrep,
           ekm_item_census_reason_name(reason),
           *(const int*)(r + EK_SPR_X), *(const int*)(r + EK_SPR_Y),
           *(const int*)(r + EK_SPR_Z));
  }

  // THE TOTALS, AND THE ARITHMETIC THAT CHECKS THEM, IN THE SAME LINE. `first-unreadable none=-1` means no
  // truncation; anything else is the slot the real walk's `break` would have stopped on.
  ek_log("itemcensus: walked %d of %d slots first-unreadable %s=%d "
         "named %d = would-draw %d + statnum %d + hidden %d + zero-xrep %d "
         "(these four sum to named) figurines %d = would-draw %d + "
         "statnum %d + hidden %d + zero-xrep %d (these four sum to figurines) "
         "lines %d suppressed %d sig %d",
         walked, EK_MAXSPRITES,
         firstUnreadable < 0 ? "none" : "slot", firstUnreadable,
         named, byReason[EKM_CENSUS_WOULD_DRAW], byReason[EKM_CENSUS_STATNUM],
         byReason[EKM_CENSUS_HIDDEN], byReason[EKM_CENSUS_ZERO_XREP],
         figures, figByReason[EKM_CENSUS_WOULD_DRAW],
         figByReason[EKM_CENSUS_STATNUM], figByReason[EKM_CENSUS_HIDDEN],
         figByReason[EKM_CENSUS_ZERO_XREP], lines, suppressed, sig);
}

// Crack sprites: a walk of the STANDABLE list, sequential with the others. THE HITAG IS THE WHOLE LIVENESS TEST: the
// game deletes a crack the instant it detonates, and hitag zero is inert decoration (esp_breakables.h).
static void esp_collect_cracks(const EkProjection& p, EkMarker* markers,
                               int* count, int* seq, int* overflow) {
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;
    if (*(const short*)(r + EK_SPR_STATNUM) != EK_STAT_STANDABLE) continue;
    const short picnum = *(const short*)(r + EK_SPR_PICNUM);
    if (!ekm_crack_name((int)picnum)) continue;
    if (!ekm_crack_live((int)*(const short*)(r + EK_SPR_HITAG))) continue;
    esp_collect_point(p, EK_MARKER_CRACK, i,
                      *(const int*)(r + EK_SPR_X),
                      *(const int*)(r + EK_SPR_Y),
                      *(const int*)(r + EK_SPR_Z),
                      markers, count, seq, overflow);
  }
}

// THE BREAKABLES THAT PAY. THE TABLE IS A PRE-FILTER, THE GAMEVAR IS THE ANSWER: membership in esp_breakobj_aa.h says
// the tile's actor runs `state breakobject`, so its `droptile` MEANS something; the value says whether it pays.
// Asking the gamevar first would be a guarded read for all 16384 slots. Counters are cumulative, unsigned, VISITS.
static unsigned s_cashSeenActor = 0, s_cashSeenStand = 0;
static unsigned s_cashUnread = 0, s_cashMarkedActor = 0, s_cashMarkedStand = 0;
static unsigned s_cashZero = 0, s_cashOther = 0;
static int s_cashSumSig = 0, s_cashSumDone = 0;
static unsigned s_cashSumMs = 0;

static void esp_collect_cash(const EkProjection& p, EkMarker* markers,
                             int* count, int* seq, int* overflow) {
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;
    const int statnum = (int)*(const short*)(r + EK_SPR_STATNUM);
    if (!ekm_breakobj_statnum_ok(statnum)) continue;
    const short picnum = *(const short*)(r + EK_SPR_PICNUM);
    const bool inTable = ekm_breakobj_is_member((int)picnum);
    if (!inTable) continue;
    if (statnum == EK_STAT_ACTOR) s_cashSeenActor++; else s_cashSeenStand++;

    int droptile = 0;
    const bool gotDrop = gamevars_droptile(i, &droptile);
    if (!gotDrop) { s_cashUnread++; continue; }
    if (droptile == 0) s_cashZero++;

    if (!ekm_breakobj_pays_cash(inTable, gotDrop, droptile)) {
      if (droptile != 0) s_cashOther++;
      continue;
    }
    if (statnum == EK_STAT_ACTOR) s_cashMarkedActor++; else s_cashMarkedStand++;

    esp_collect_point(p, EK_MARKER_CASH, i,
                      *(const int*)(r + EK_SPR_X),
                      *(const int*)(r + EK_SPR_Y),
                      *(const int*)(r + EK_SPR_Z),
                      markers, count, seq, overflow);
  }
}

// ONE SUMMARY LINE, TOTALS UNCAPPED. The period rule is the item census's (ekm_item_census_due): a map change prints
// at once, a settled session once a minute. The statnum split measures the ACTOR-only narrowing rather than assuming
// it.
static void esp_log_cash_summary(void) {
  if (!g_cfg.debuglog) return;
  if (!mapextent_ready()) return;
  int ns = 0, nw = 0;
  if (!mapextent_extent(&ns, &nw)) return;

  const int sig = (ns << 16) ^ nw;
  const unsigned now = wallclock_ms();
  if (!ekm_item_census_due(s_cashSumDone, sig, s_cashSumSig, now, s_cashSumMs))
    return;
  s_cashSumMs = now;
  s_cashSumSig = sig;
  s_cashSumDone = 1;

  ek_log("espcash: table %d picnums; VISITS not sprites (one per in-table "
         "sprite per drawn frame) -- seen %d = actor %d + standable %d "
         "(these two sum to seen); droptile unread %d, zero %d, other %d, "
         "PAYS %d = actor %d + standable %d (these two sum to PAYS); "
         "seen = unread + zero + other + PAYS",
         EKM_BREAKOBJ_COUNT,
         s_cashSeenActor + s_cashSeenStand, s_cashSeenActor, s_cashSeenStand,
         s_cashUnread, s_cashZero, s_cashOther,
         s_cashMarkedActor + s_cashMarkedStand,
         s_cashMarkedActor, s_cashMarkedStand);
}

static EkFigurineCount s_figCount = { 0, 0 };

// WHAT THE CORNER SHOWS: the table rule's X / Y on a level the generated table knows, s_figCount (the visit rule)
// otherwise. Render thread, written by esp_collect_figurines, read by both corner draws.
static EkFigurineShown s_figShown = { 0, 0, false };
static EkFigurineShown s_figLogShown = { 0, 0, false };

static bool s_chestGotStart = false, s_chestGotMc = false;
static int s_chestStart = 0, s_chestMc = 0;
static bool s_chestHaveLevel = false;
static int s_chestVol = -1;
static int s_chestLevelMask = 0;
static int s_chestLog[7] = { -1, -1, -1, -1, -1, -1, -1 };

// The corner stack's bracket (esp_corner_text): Corner text size in the corner's own scaled-text slot, panel open or
// closed, so both draw the same glyphs. The end puts back exactly what the begin found.
struct EspCornerSaved { int scale; int slot; };
static EspCornerSaved esp_corner_begin(void) {
  const EspCornerSaved prev = { canvas_scale(), canvas_text_slot() };
  const EspCornerText t = esp_corner_text(g_cfg.cornerscale);
  canvas_set_scale(t.scale);
  canvas_set_text_slot(t.slot);
  return prev;
}
static void esp_corner_end(EspCornerSaved prev) {
  canvas_set_scale(prev.scale);
  canvas_set_text_slot(prev.slot);
}

static int s_figSigDone = 0, s_figSig = 0;

// The level-entry epoch the counter last reset under: the signature above can COLLIDE (httkb\aa05hub/aa05hub1/
// aa05hubfinal share 0xa883ffc), the epoch cannot.
static long s_figEpoch = 0;

static int s_figLogDone = 0, s_figLogSig = 0;
static unsigned s_figLogMs = 0;

// FOUR PATHS CAN RESET THE COUNTER: finishing a level into the next, menu then another level, the `map` command, and
// a save load. Loads set gm = 0, but FINISHING A LEVEL INTO THE NEXT NEVER READS gm 0 (P_EndLevel sets MODE_EOL). All
// four are the shared level-entry epoch (playerstate.h) ORed with mapextent's signature.
static void esp_figurine_maybe_reset(void) {
  int ns = 0, nw = 0;
  const bool haveSig = mapextent_ready() && mapextent_extent(&ns, &nw);
  const int sig = haveSig ? ((ns << 16) ^ nw) : s_figSig;
  const bool sigChanged = haveSig && (!s_figSigDone || sig != s_figSig);
  const long epoch = level_epoch();
  const bool newLevel = ekm_level_epoch_changed(s_figEpoch, epoch);
  s_figEpoch = epoch;

  if (newLevel || sigChanged) {
    const EkFigurineCount before = s_figCount;
    s_figCount.x = 0;
    s_figCount.y = 0;
    s_figShown.x = 0; s_figShown.y = 0; s_figShown.fromTable = false;
    if (g_cfg.debuglog && (before.x != 0 || before.y != 0 || !s_figSigDone))
      ek_log("figurines: reset (trigger=%s) x %d->0 y %d->0",
             newLevel ? "level-epoch" : "map-signature", before.x, before.y);
  }
  if (haveSig) { s_figSig = sig; s_figSigDone = 1; }
}

// THE CHESTS RIDE THIS WALK rather than a second one: `wantChests` adds two global reads a frame and one picnum test
// a sprite. With only the chest row on, the figurine arm and its reads are skipped.
static void esp_collect_figurines(const EkProjection& p, EkMarker* markers,
                                  int* count, int* seq, int* overflow,
                                  bool wantFigurines, bool wantChests) {
  int c1 = 0, c2 = 0;
  bool haveBits = false;
  EkGvView mtypeView = { 0, false };
  if (wantFigurines) {
    const bool gotC1 = gamevars_collectibles(EK_GV_CO_COLLECTIBLES1, &c1);
    const bool gotC2 = gamevars_collectibles(EK_GV_CO_COLLECTIBLES2, &c2);
    haveBits = gotC1 && gotC2;
    // mtype through ONE view a walk, so the hubs' display pieces are neither marked nor counted. A failed take leaves
    // it not ok, and every get then answers "unread" (ekm_figurine_is_find keeps the sprite).
    gamevars_mtype_view(&mtypeView);
  }
  int displaySprites = 0;

  s_chestStart = 0; s_chestMc = 0;
  s_chestGotStart = false; s_chestGotMc = false;
  if (wantChests) {
    s_chestGotStart = gamevars_collectibles(EK_GV_CO_STARTGUNS_PERMA, &s_chestStart);
    s_chestGotMc = gamevars_collectibles(EK_GV_CO_MCGUFFINS, &s_chestMc);
  }
  int chestLive = 0, chestEdf = 0, chestBomb = 0;
  int levelMask = 0;

  int vol = -1, lvl = -1;
  const bool haveLevel = ud_level_identity(&vol, &lvl);
  s_chestHaveLevel = haveLevel;
  s_chestVol = vol;

  unsigned long long seenMask = 0;
  int liveSprites = 0, hiddenSprites = 0;

  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;
    const int statnum = (int)*(const short*)(r + EK_SPR_STATNUM);
    const int picnum  = (int)*(const short*)(r + EK_SPR_PICNUM);
    if (wantChests && ekm_chest_statnum_live(statnum) &&
        (picnum == EKM_PIC_EDFCHEST || picnum == EKM_PIC_ITEMCHEST)) {
      chestLive++;
      const int hitag = (int)*(const short*)(r + EK_SPR_HITAG);
      // Fold into the level mask off the same LIVE sprite this walk found: ekm_edfchest_bit answers 0 for a non-EDF
      // hitag, and OR makes a duplicate hitag count once.
      if (picnum == EKM_PIC_EDFCHEST) levelMask |= ekm_edfchest_bit(hitag);
      const int kind = ekm_chest_marked(picnum, hitag, s_chestGotStart, s_chestStart,
                                        s_chestGotMc, s_chestMc);
      if (kind == EKM_CHEST_EDF) chestEdf++;
      if (kind == EKM_CHEST_BOMBPART) chestBomb++;
      if (kind != EKM_CHEST_NONE)
        esp_collect_point(p, EK_MARKER_CHEST, i,
                          *(const int*)(r + EK_SPR_X),
                          *(const int*)(r + EK_SPR_Y),
                          *(const int*)(r + EK_SPR_Z),
                          markers, count, seq, overflow);
      continue;
    }
    if (!wantFigurines) continue;
    if (!ekm_figurine_is_live(statnum, picnum)) continue;
    liveSprites++;
    int mtype = 0;
    const bool haveMtype = ekm_gv_view_get(mtypeView, i, &mtype);
    if (!ekm_figurine_is_find(haveMtype, mtype)) { displaySprites++; continue; }

    const EkFigurineBit fb = ekm_figurine_bit(picnum);
    const int bits = (fb.which == EK_GV_CO_COLLECTIBLES1) ? c1 : c2;
    if (ekm_figurine_collected(haveBits, bits, fb.bit)) continue;

    if ((int)*(const short*)(r + EK_SPR_CSTAT) & 0x8000) hiddenSprites++;
    seenMask = ekm_figurine_mark_seen(
        seenMask, ekm_figurine_flat_index(fb.which, fb.bit));

    if (g_cfg.espfigurines)
      esp_collect_point(p, EK_MARKER_FIGURINE, i,
                        *(const int*)(r + EK_SPR_X),
                        *(const int*)(r + EK_SPR_Y),
                        *(const int*)(r + EK_SPR_Z),
                        markers, count, seq, overflow);
  }
  s_chestLevelMask = levelMask;

  // One line on every change of what the chest row saw, beside the two bitfields it read; level= is the mask, so a
  // level's own count is checkable against the log.
  if (wantChests && g_cfg.debuglog) {
    const int now[7] = { chestLive, chestEdf, chestBomb,
                         s_chestGotStart ? s_chestStart : -1,
                         s_chestGotMc ? s_chestMc : -1, haveLevel ? vol : -1,
                         levelMask };
    bool changed = false;
    for (int k = 0; k < 7; k++) if (now[k] != s_chestLog[k]) changed = true;
    if (changed) {
      for (int k = 0; k < 7; k++) s_chestLog[k] = now[k];
      ek_log("chests: live=%d marked edf=%d bomb=%d (live = marked + opened/"
             "found/unknown hitag) startguns_perma=%d mcguffins=%d (-1 unread) "
             "volume=%d found=%d/%d level=%d",
             chestLive, chestEdf, chestBomb, now[3], now[4], now[5],
             s_chestGotStart ? ekm_edfchest_found(s_chestStart) : -1,
             EKM_EDFCHEST_TOTAL, now[6]);
    }
  }
  if (!wantFigurines) return;

  const int remaining = haveBits ? ekm_figurine_popcount(seenMask) : 0;
  s_figCount = ekm_figurine_count_update(s_figCount, remaining);

  // The table rule (esp_figurines_aa.h) off the level identity read above; the visit count keeps running underneath,
  // so a level the table lacks falls back with no gap.
  s_figShown = ekm_figurine_counter_shown(haveLevel, vol, lvl, haveBits, c1, c2,
                                          s_figCount, EKM_FIG_LEVELS,
                                          EKM_FIG_LEVELS_COUNT);

  if (g_cfg.debuglog && mapextent_ready()) {
    int ns = 0, nw = 0;
    if (mapextent_extent(&ns, &nw)) {
      const int sig = (ns << 16) ^ nw;
      const unsigned now = wallclock_ms();
      const bool due = ekm_item_census_due(s_figLogDone, sig, s_figLogSig, now, s_figLogMs);
      const bool changed = s_figShown.x != s_figLogShown.x || s_figShown.y != s_figLogShown.y ||
                           s_figShown.fromTable != s_figLogShown.fromTable;
      if (due || changed) {
        if (due) { s_figLogMs = now; s_figLogSig = sig; s_figLogDone = 1; }
        s_figLogShown = s_figShown;
        int inLevel = -1, found = -1;
        if (!gamevars_collectibles(EK_GV_CO_COLLECTINLEVEL, &inLevel)) inLevel = -1;
        if (!gamevars_collectibles(EK_GV_CO_COLLECTFOUND, &found)) found = -1;
        ek_log("figurines: x=%d y=%d remaining=%d live=%d display=%d hidden=%d "
               "bits=%s mtype=%s; shown %d/%d source=%s level=%s v%d l%d; AA "
               "collectfound=%d collectinlevel=%d (%s)",
               s_figCount.x, s_figCount.y, remaining, liveSprites, displaySprites,
               hiddenSprites, haveBits ? "read" : "unread",
               mtypeView.ok ? "read" : "unread",
               s_figShown.x, s_figShown.y, s_figShown.fromTable ? "table" : "visit",
               haveLevel ? "read" : "unread", vol, lvl, found, inLevel,
               due ? "level" : "change");
      }
    }
  }
}

// The wall marks, refreshed on a period: the walk is the most expensive thing in this file. WHAT IS CACHED IS ONLY
// WHICH WALL AND WHERE; the picnum is re-read live, so a stale cache can only be LATE TO NOTICE A NEW MARK, never
// wrong about one it holds. `sectnum` is the scan's loop sector (a wall's nextsector is the FAR side).
struct EkWallMark { int kind, wallnum, sectnum, x, y, z; };
static EkWallMark s_wallMarks[EK_WALL_MARK_MAX];
static int s_wallCount = 0, s_wallOverflow = 0, s_wallScanned = 0, s_wallSig = 0;
static unsigned s_wallScanMs = 0;

static void esp_scan_walls(void) {
  // OFF MEANS NO SCAN AND NO LATCHED MARKS: re-enabling a row must never start from a mark the level no longer has.
  // BOTH ROWS feed one walk: the gate is OR and the clear is AND, so turning Breakables off cannot take Switches with
  // it.
  if (!g_cfg.espbreakables && !g_cfg.espswitches) {
    s_wallCount = 0; s_wallOverflow = 0; s_wallScanned = 0; return;
  }

  if (!mapextent_ready()) return;

  int ns = 0, nw = 0;
  if (!mapextent_extent(&ns, &nw)) return;

  const int sig = (ns << 16) ^ nw;
  const unsigned now = wallclock_ms();
  if (!ekm_wall_scan_due(s_wallScanned, sig, s_wallSig, now, s_wallScanMs)) return;
  s_wallScanMs = now;
  s_wallSig = sig;
  s_wallScanned = 1;

  int count = 0, overflow = 0;
  for (int sec = 0; sec < ns; sec++) {
    EkSectorInfo si = { 0, 0, 0, 0, 0, 0 };
    if (!sectorstate_info(sec, &si)) continue;

    const int midz = ekm_wall_mid_z(si.florz, si.ceilz);

    for (int k = 0; k < si.wallnum; k++) {
      const int wallIndex = si.wallptr + k;
      EkWallFace f = { 0, 0, 0, 0 };
      if (!wallstate_face(wallIndex, &f)) continue;

      // WHICH KIND, OR NEITHER, with the tag read last (a second guarded read). THE LOTAG TERM IS LOAD-BEARING:
      // P_ActivateSwitch returns 0 on lotag zero (sector.cpp:1294) and a door tile is its own base, so every DOORTILE
      // face would take a cache slot and starve real switches.
      int kind = -1;
      if (ekm_breakable_name(f.picnum)) {
        kind = EK_WKIND_BREAKABLE;
      } else if (ekm_switch_base_for(f.picnum) >= 0) {
        EkWallTags t = { 0, 0, 0 };
        if (wallstate_tags(wallIndex, &t) && t.lotag != 0) kind = EK_WKIND_SWITCH;
      }
      if (kind < 0) continue;

      int p2 = 0;
      if (!wallstate_point2(wallIndex, &p2)) continue;
      if (!ekm_point2_ok(p2, wallIndex, si.wallptr, si.wallnum)) continue;

      EkWallCorner a = { 0, 0, 0 }, b = { 0, 0, 0 };
      if (!wallstate_corner(wallIndex, &a)) continue;
      if (!wallstate_corner(p2, &b)) continue;

      if (count >= EK_WALL_MARK_MAX) { overflow++; continue; }
      EkWallMark& wm = s_wallMarks[count++];
      wm.kind    = kind;
      wm.wallnum = wallIndex;
      wm.sectnum = sec;
      wm.x = ekm_wall_mid(a.x, b.x);
      wm.y = ekm_wall_mid(a.y, b.y);
      wm.z = midz;
    }
  }

  s_wallCount = count;
  s_wallOverflow = overflow;
}

// The breakable panels, out of the cache. The picnum is re-read LIVE for every mark: membership in the table is the
// liveness test, so a panel broken since the last scan drops out in the frame it broke.
static void esp_collect_breakables(const EkProjection& p, EkMarker* markers,
                                   int* count, int* seq, int* overflow) {
  const int n = ekm_wall_mark_count_clamped(s_wallCount);
  for (int i = 0; i < n; i++) {
    const EkWallMark& wm = s_wallMarks[i];
    if (wm.kind != EK_WKIND_BREAKABLE) continue;
    EkWallFace f = { 0, 0, 0, 0 };
    if (!wallstate_face(wm.wallnum, &f)) continue;
    if (!ekm_breakable_name(f.picnum)) continue;
    esp_collect_point(p, EK_MARKER_BREAKABLE, wm.wallnum, wm.x, wm.y, wm.z,
                      markers, count, seq, overflow);
  }
}

// THE COMBINATION LOCK'S TINT: a translucent BOX AROUND the lock's model, green for Flip, red for Leave (rules in
// esp_tint.h). A VOLUME AND NOT A WASH ON THE FACE: with r_models the HRP draws a switch as a model standing proud of
// its plane, and a coplanar quad lost four rounds to depth bias. A lock no model replaces gets one flat quad.
static int s_comboSeen = 0, s_comboPushed = 0, s_comboBoxed = 0, s_comboFlat = 0,
           s_comboQuads = 0;
int esp_combination_seen(void) { return s_comboSeen; }
int esp_combination_pushed(void) { return s_comboPushed; }
int esp_combination_boxed(void) { return s_comboBoxed; }
int esp_combination_flat(void) { return s_comboFlat; }
int esp_combination_quads(void) { return s_comboQuads; }

static void esp_push_combination_flat(const EkWorldQuad& q, int sect) {
  s_comboFlat++;
  if (ekm_world_quad_room(worldquad_count(), EK_WORLD_QUAD_MAX) < 1) return;
  worldquad_push_at(q, EK_WQ_KIND_OTHER, sect);
  s_comboQuads++;
}

// THE WALL-SPRITE ARM, for a lock no model replaces: AA ships its switch voxels commented out (duke3d.def:302), so
// its locks are drawn flat and a quad on the sprite's own plane is what the player sees (esp_tint.h's rule).
static void esp_push_combination_wall_sprite(const unsigned char* r, int cstat,
                                             int tw, int th, int verdict) {
  EkTintClamp clamp = { 0, 0, 0, 0 };
  const EkTintClamp* pc = 0;
  EkSectorInfo si;
  const int sect = (int)*(const short*)(r + EK_SPR_SECTNUM);
  if (sectorstate_info(sect, &si)) {
    clamp.ceilz = si.ceilz;
    clamp.ceilParallax = si.ceilparallax;
    clamp.florz = si.florz;
    clamp.florParallax = si.florparallax;
    pc = &clamp;
  }
  EkWorldQuad q;
  if (!ekm_tint_wall_sprite_quad(*(const int*)(r + EK_SPR_X), *(const int*)(r + EK_SPR_Y),
                                 *(const int*)(r + EK_SPR_Z),
                                 (int)*(const short*)(r + EK_SPR_ANG),
                                 (int)r[EK_SPR_XREPEAT], (int)r[EK_SPR_YREPEAT], tw, th,
                                 (int)(signed char)r[EK_SPR_XOFFSET],
                                 (int)(signed char)r[EK_SPR_YOFFSET], cstat, pc, &q,
                                 ekm_tint_colour(verdict, C_TINT_BOX_FLIP,
                                                 C_TINT_BOX_LEAVE)))
    return;
  esp_push_combination_flat(q, sect);
}

static void esp_push_combination_tint(const unsigned char* r,
                                      const EkSwitchInfo& si) {
  const int verdict = ekm_tint_verdict(si.family, si.line);
  if (verdict == EKM_TINT_NONE) return;

  const int cstat = (int)*(const unsigned short*)(r + EK_SPR_CSTAT);
  if ((cstat & 0x30) != 0x10) return;

  const int picnum = (int)*(const short*)(r + EK_SPR_PICNUM);
  int tw = 0, th = 0;
  tile_size(picnum, &tw, &th);
  if (tw <= 0 || th <= 0) return;

  // A LOCK NO MODEL REPLACES IS DRAWN FLAT, SO ITS TINT IS FLAT (under AA every lock). ONLY EK_MDL_NONE, NOT UNREADY.
  // NONE ALSO MEANS A VOXEL (modelbounds.cpp:1294): if switches.def is re-enabled the quad sits inside the voxel;
  // consult the engine's tiletovox table then.
  EkModelBox box;
  const int mdl = modelbounds_for(picnum, (int)r[EK_SPR_PAL], &box);
  if (mdl == EK_MDL_NONE) {
    esp_push_combination_wall_sprite(r, cstat, tw, th, verdict);
    return;
  }
  if (mdl != EK_MDL_OK) return;

  // THE MODEL'S ORIGIN, NOT tspr->z: polymer.cpp:4510-4511 translates a y-centred sprite's model down by
  // 2*tilesiz.y*yrepeat in build z, so the raw z would hang the box half a sprite-height ABOVE the lock (esp_tint.h).
  const int originZ = ekm_tint_model_origin_z(*(const int*)(r + EK_SPR_Z),
                                              (int)r[EK_SPR_YREPEAT], th,
                                              (cstat & 128) ? 1 : 0);

  // REPEATS GO IN WHOLE, not divided by 64: the 1/64 is already inside modelbounds.h's scale rule. Canvas colours go
  // in AS THEY STAND (`argb` is 0xAARRGGBB).
  EkWorldQuad faces[6];
  const int n = ekm_tint_box_faces(box, *(const int*)(r + EK_SPR_X),
                                   *(const int*)(r + EK_SPR_Y), originZ,
                                   (int)*(const short*)(r + EK_SPR_ANG),
                                   (int)r[EK_SPR_XREPEAT],
                                   (int)r[EK_SPR_YREPEAT], faces,
                                   ekm_tint_colour(verdict, C_TINT_BOX_FLIP,
                                                   C_TINT_BOX_LEAVE));
  if (n <= 0) return;
  s_comboBoxed++;

  // ALL SIX OR NONE, ASKED FOR BEFORE ANY ONE IS PUSHED: the secret volume pushes an arbitrary count of panels first,
  // so the box can start anywhere and a cap landing mid-box would leave stray faces floating in a doorway.
  // ekm_world_quad_room (worldquad.h) is the rule; the refusal is silent here because worldquad_push owns the count.
  if (ekm_world_quad_room(worldquad_count(), EK_WORLD_QUAD_MAX) < n) return;

  const int sect = (int)*(const short*)(r + EK_SPR_SECTNUM);
  for (int i = 0; i < n; i++) {
    worldquad_push_at(faces[i], EK_WQ_KIND_OTHER, sect);
    s_comboQuads++;
  }
}

// WHETHER THIS LEVEL HAS A SECRET EXIT AT ALL, set as the walks CLASSIFY, not as they draw: an unfound secret exit is
// exactly the one worth a note, and it is off screen by definition. Cleared once per frame.
static bool s_levelHasSecretExit = false;
bool esp_level_has_secret_exit(void) { return s_levelHasSecretExit; }

// Switch sprites: everything the classifier names, off the DEFAULT list (the spawn path has no case for any of them).
// `gotAccess` is the frame's, passed down, never read per sprite.
static void esp_collect_switch_sprites(const EkProjection& p, int gotAccess,
                                       EkMarker* markers, int* count, int* seq,
                                       int* overflow) {
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;
    if (*(const short*)(r + EK_SPR_STATNUM) != EK_STAT_DEFAULT) continue;

    const EkSwitchInfo si = ekm_switch_classify(
        (int)*(const short*)(r + EK_SPR_PICNUM), (int)r[EK_SPR_PAL],
        (int)*(const short*)(r + EK_SPR_HITAG), (int)*(const short*)(r + EK_SPR_LOTAG),
        gotAccess);
    if (si.family == EK_SW_NONE) continue;
    if (si.family == EK_SW_EXIT && si.exitKind == EKM_EXIT_SECRET)
      s_levelHasSecretExit = true;

    const int marked = esp_collect_point(p, EK_MARKER_SWITCH_SPRITE, i,
                                         *(const int*)(r + EK_SPR_X),
                                         *(const int*)(r + EK_SPR_Y),
                                         *(const int*)(r + EK_SPR_Z),
                                         markers, count, seq, overflow);

    // THE TINT FOLLOWS ITS OWN MARKER, and the culls are the reason: they live in esp_collect_point, so pushing ahead
    // of it put every lock in the LEVEL on the layer every frame. It follows the collect, not the final selection
    // (ekm_keep_nearest can still drop the marker; the layer's cap of 64 covers that).
    if (si.family == EK_SW_COMBINATION) s_comboSeen++;
    if (marked && g_cfg.esptint && si.family == EK_SW_COMBINATION) {
      s_comboPushed++;
      esp_push_combination_tint(r, si);
    }
  }
}

// The exit button needs a walk of its own: NUKEBUTTON is the one switch the spawn path moves, onto the MISC list.
// SEQUENTIAL WITH THE DEFAULT-LIST WALK, NEVER NESTED. `collect` SEPARATES SEEING THE EXIT FROM MARKING IT: with
// Secrets on and Switches off it still has to learn whether the level HAS a secret exit.
static void esp_collect_exit_buttons(const EkProjection& p, bool collect,
                                     EkMarker* markers,
                                     int* count, int* seq, int* overflow) {
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* r = gamestate_sprite(i);
    if (!r) break;
    if (*(const short*)(r + EK_SPR_STATNUM) != EK_STAT_MISC) continue;
    if (*(const short*)(r + EK_SPR_PICNUM) != EKM_PIC_NUKEBUTTON) continue;

    if (ekm_switch_exit_kind((int)*(const short*)(r + EK_SPR_PICNUM),
                             (int)r[EK_SPR_PAL]) == EKM_EXIT_SECRET)
      s_levelHasSecretExit = true;

    if (!collect) continue;

    esp_collect_point(p, EK_MARKER_SWITCH_SPRITE, i,
                      *(const int*)(r + EK_SPR_X),
                      *(const int*)(r + EK_SPR_Y),
                      *(const int*)(r + EK_SPR_Z),
                      markers, count, seq, overflow);
  }
}

// Switch walls, out of the cache the breakable walk fills, RE-READ AND RE-CLASSIFIED LIVE: a switch flipped or a lock
// opened since the last scan is classified from the tile it wears now. `gotAccess` only decides whether an ACCESS
// switch gets a card hint. A WALL-TEXTURE LOCK GETS THE WHOLE WALL FACE AS ITS TINT (a texture is always flat).
static void esp_push_combination_wall_face(const EkWallMark& wm,
                                           const EkSwitchInfo& sw) {
  const int verdict = ekm_tint_verdict(sw.family, sw.line);
  if (verdict == EKM_TINT_NONE) return;
  EkSectorInfo si;
  if (!sectorstate_info(wm.sectnum, &si)) return;
  int fp2 = 0, p2 = 0;
  EkWallCorner fw = { 0, 0, 0 }, fw2 = { 0, 0, 0 }, a = { 0, 0, 0 }, b = { 0, 0, 0 };
  if (!wallstate_corner(si.wallptr, &fw)) return;
  if (!wallstate_point2(si.wallptr, &fp2)) return;
  if (!ekm_point2_ok(fp2, si.wallptr, si.wallptr, si.wallnum)) return;
  if (!wallstate_corner(fp2, &fw2)) return;
  if (!wallstate_point2(wm.wallnum, &p2)) return;
  if (!ekm_point2_ok(p2, wm.wallnum, si.wallptr, si.wallnum)) return;
  if (!wallstate_corner(wm.wallnum, &a)) return;
  if (!wallstate_corner(p2, &b)) return;

  const EkTintPlane ceil = { si.ceilz, si.ceilStat, si.ceilHeinum };
  const EkTintPlane flor = { si.florz, si.florStat, si.florHeinum };
  EkWorldQuad q;
  if (!ekm_tint_wall_face_quad(a.x, a.y, b.x, b.y, ceil, flor, fw.x, fw.y, fw2.x, fw2.y,
                               &q, ekm_tint_colour(verdict, C_TINT_BOX_FLIP,
                                                   C_TINT_BOX_LEAVE)))
    return;
  esp_push_combination_flat(q, wm.sectnum);
}

static void esp_collect_switch_walls(const EkProjection& p, int gotAccess,
                                     EkMarker* markers, int* count, int* seq,
                                     int* overflow) {
  const int n = ekm_wall_mark_count_clamped(s_wallCount);
  for (int i = 0; i < n; i++) {
    const EkWallMark& wm = s_wallMarks[i];
    if (wm.kind != EK_WKIND_SWITCH) continue;
    EkWallFace f = { 0, 0, 0, 0 };
    EkWallTags t = { 0, 0, 0 };
    if (!wallstate_face(wm.wallnum, &f)) continue;
    if (!wallstate_tags(wm.wallnum, &t)) continue;
    const EkSwitchInfo sw = ekm_switch_classify(f.picnum, t.pal, t.hitag, t.lotag, gotAccess);
    if (sw.family == EK_SW_NONE) continue;
    const int marked = esp_collect_point(p, EK_MARKER_SWITCH_WALL, wm.wallnum,
                                         wm.x, wm.y, wm.z, markers, count, seq,
                                         overflow);
    if (sw.family == EK_SW_COMBINATION) s_comboSeen++;
    if (marked && g_cfg.esptint && sw.family == EK_SW_COMBINATION) {
      s_comboPushed++;
      esp_push_combination_wall_face(wm, sw);
    }
  }
}

// Items: the same corner brackets an enemy gets, in blue, with the name above; no health, no distance. `m.index` is
// the SPRITE index here, not a targets[] slot. Returns 1 when it drew: a slot recycled between collection and draw no
// longer holds a named pickup.
static int esp_draw_card_mark(const EkScreenPt& p, unsigned colour,
                              const char* label, const char* line2);

static int esp_draw_item_marker(const EkMarker& m,
                                bool gotPchar, int pchar,
                                bool gotUpg, int upgBits) {
  const unsigned char* r = gamestate_sprite(m.index);
  if (!r) return 0;
  const short picnum = *(const short*)(r + EK_SPR_PICNUM);
  const unsigned char pal = r[EK_SPR_PAL];
  const char* nm = ekm_item_name_pal((int)picnum, (int)pal);
  if (!nm) return 0;

  char nameBuf[32];
  nm = ekm_item_label_aa((int)picnum, (int)pal, nm, gotPchar, pchar,
                         gotUpg, upgBits, nameBuf, (int)sizeof nameBuf);

  // A KEYCARD WEARS ITS OWN COLOUR, EVERY OTHER PICKUP IS BLUE ON BONE. ekm_keycard_colour returns 0 for a pal with
  // no lock colour, so an odd-pal card falls back to the ordinary mark rather than drawing invisible.
  const unsigned card = (picnum == EKM_PIC_ACCESSCARD)
                      ? ekm_keycard_colour((int)pal) : 0u;
  const unsigned markColour = card ? card : canvas_esp_colour(false);
  const unsigned textColour = card ? card : C_BONE;

  // A 0x0 box is the collect walk's sentinel for "size not known yet": draw the label alone, anchored on the
  // projected sprite point, and skip the brackets rather than frame a guessed size.
  const bool boxKnown = (m.box.w > 0 && m.box.h > 0);

  if (esp_item_shape(card) == ESP_SHAPE_CARD) {
    const EkScreenPt c = boxKnown
        ? EkScreenPt{ clamp_ll_to_int((long long)m.box.x + m.box.w / 2),
                      clamp_ll_to_int((long long)m.box.y + m.box.h / 2), m.p.dist2d }
        : m.p;
    return esp_draw_card_mark(c, card, nm, 0);
  }

  if (boxKnown) {
    esp_draw_brackets(m.box.x, m.box.y, m.box.w, m.box.h, markColour);
    esp_draw_label(s_hivis, esp_label_x(m.box.x, m.box.w, canvas_text_width(nm, FONT_SMALL)),
                   esp_name_label_y(m.box.y), nm, textColour, FONT_SMALL);
  } else {
    esp_draw_label(s_hivis, esp_label_x(m.p.x, 0, canvas_text_width(nm, FONT_SMALL)),
                   esp_name_label_y(m.p.y), nm, textColour, FONT_SMALL);
  }
  return 1;
}

// The cross every point marker draws: two bars sharing one centre pixel, a label above and an optional second line
// below. Shared by the secret, crack and breakable marks and four of the five switch shapes. `shape`
// ESP_CROSS_SALTIRE draws the arms as two diagonals, for the explodables.
static int esp_draw_cross_mark(const EkScreenPt& p, unsigned colour,
                               const char* label, const char* line2,
                               int shape = ESP_CROSS_PLUS) {
  const int arm = s_st.secretArm, t = s_st.secretT, k = s_st.keyline;
  if (shape == ESP_CROSS_STAR) {
    const EspSaltire s = esp_saltire(p.x, p.y, arm);
    const EspRect r[2] = {
      { esp_secret_hbar_x(p.x, arm), esp_secret_hbar_y(p.y, t), esp_secret_bar_span(arm), t },
      { esp_secret_vbar_x(p.x, t), esp_secret_vbar_y(p.y, arm), t, esp_secret_bar_span(arm) } };
    if (k > 0) {
      const unsigned kc = esp_keyline_colour(colour);
      canvas_line_w(s.x0, s.y0, s.x1, s.y1, kc, t + 2 * k);
      canvas_line_w(s.x2, s.y2, s.x3, s.y3, kc, t + 2 * k);
      for (int j = 0; j < 2; j++) {
        const EspRect g = esp_keyline_rect(r[j].x, r[j].y, r[j].w, r[j].h, k);
        canvas_rect(g.x, g.y, g.w, g.h, kc);
      }
    }
    canvas_line_w(s.x0, s.y0, s.x1, s.y1, colour, t);
    canvas_line_w(s.x2, s.y2, s.x3, s.y3, colour, t);
    esp_keyed_rects(r, 2, colour, 0);
  } else if (shape == ESP_CROSS_SALTIRE) {
    const EspSaltire s = esp_saltire(p.x, p.y, arm);
    if (k > 0) {
      const unsigned kc = esp_keyline_colour(colour);
      canvas_line_w(s.x0, s.y0, s.x1, s.y1, kc, t + 2 * k);
      canvas_line_w(s.x2, s.y2, s.x3, s.y3, kc, t + 2 * k);
    }
    canvas_line_w(s.x0, s.y0, s.x1, s.y1, colour, t);
    canvas_line_w(s.x2, s.y2, s.x3, s.y3, colour, t);
  } else {
    const EspRect r[2] = {
      { esp_secret_hbar_x(p.x, arm), esp_secret_hbar_y(p.y, t), esp_secret_bar_span(arm), t },
      { esp_secret_vbar_x(p.x, t), esp_secret_vbar_y(p.y, arm), t, esp_secret_bar_span(arm) } };
    esp_keyed_rects(r, 2, colour, k);
  }
  if (label)
    esp_draw_label(s_hivis, esp_label_x(p.x, 0, canvas_text_width(label, FONT_SMALL)),
                   esp_secret_name_label_y(p.y, arm), label, colour, FONT_SMALL);
  if (line2)
    esp_draw_label(s_hivis, esp_label_x(p.x, 0, canvas_text_width(line2, FONT_SMALL)),
                   esp_secret_dist_label_y(p.y, arm), line2, C_BONEDIM, FONT_SMALL);
  return 1;
}

// A keycard lock draws a CARD, not a cross, so a lock reads as a lock: a rounded outline with a notch in that card's
// colour, hint beneath. Used for switch locks and for a keycard pickup.
static int esp_draw_card_mark(const EkScreenPt& p, unsigned colour,
                              const char* label, const char* line2) {
  const int k = s_st.keyline;
  const int cx = esp_card_x(p.x), cy = esp_card_y(p.y);
  const EspRect notch = { esp_card_notch_x(p.x), esp_card_notch_y(p.y),
                          ESP_CARD_NOTCH, ESP_CARD_NOTCH };
  if (k > 0) {
    const unsigned kc = esp_keyline_colour(colour);
    canvas_round_rect_outline(cx - k, cy - k, ESP_CARD_W + 2 * k, ESP_CARD_H + 2 * k,
                              ESP_CARD_R + k, s_st.cardT + 2 * k, kc);
    const EspRect g = esp_keyline_rect(notch.x, notch.y, notch.w, notch.h, k);
    canvas_rect(g.x, g.y, g.w, g.h, kc);
  }
  canvas_round_rect_outline(cx, cy, ESP_CARD_W, ESP_CARD_H, ESP_CARD_R, s_st.cardT, colour);
  canvas_rect(notch.x, notch.y, notch.w, notch.h, colour);
  if (label)
    esp_draw_label(s_hivis, esp_label_x(p.x, 0, canvas_text_width(label, FONT_SMALL)),
                   esp_card_name_label_y(p.y), label, colour, FONT_SMALL);
  if (line2)
    esp_draw_label(s_hivis, esp_label_x(p.x, 0, canvas_text_width(line2, FONT_SMALL)),
                   esp_card_dist_label_y(p.y), line2, colour, FONT_SMALL);
  return 1;
}

static int esp_draw_secret_marker(const EkMarker& m) {
  char dist[16];
  wsprintfA(dist, "%d m", (m.p.dist2d + BUILD_UNITS_PER_METRE / 2) / BUILD_UNITS_PER_METRE);
  return esp_draw_cross_mark(m.p, canvas_esp_secret_colour(), ESP_SECRET_LABEL, dist);
}

static int esp_draw_crack_marker(const EkMarker& m) {
  const unsigned char* r = gamestate_sprite(m.index);
  if (!r) return 0;
  const char* nm = ekm_crack_name((int)*(const short*)(r + EK_SPR_PICNUM));
  if (!nm) return 0;
  return esp_draw_cross_mark(m.p, canvas_esp_explodable_colour(), nm, 0,
                             esp_point_cross(m.kind));
}

// A cash container's verdict is re-derived from its LIVE picnum and a FRESH gamevar read: `droptile` is scratch in
// actors that are not breakables, so a recycled slot could look like money. THE LABEL CARRIES NO NUMBER
// (esp_cash_aa.h): the stack does not exist yet, so a credit figure would be invented.
static int esp_draw_cash_marker(const EkMarker& m) {
  const unsigned char* r = gamestate_sprite(m.index);
  if (!r) return 0;
  if (!ekm_breakobj_statnum_ok((int)*(const short*)(r + EK_SPR_STATNUM))) return 0;
  const bool inTable =
      ekm_breakobj_is_member((int)*(const short*)(r + EK_SPR_PICNUM));
  if (!inTable) return 0;
  int droptile = 0;
  const bool gotDrop = gamevars_droptile(m.index, &droptile);
  if (!ekm_breakobj_pays_cash(inTable, gotDrop, droptile)) return 0;
  return esp_draw_cross_mark(m.p, canvas_esp_cash_colour(),
                             EKM_CASH_INSIDE_LABEL, 0);
}

// A figurine's name is re-derived from its live picnum and pal, and the statnum re-checked, so a recycled slot stops
// drawing rather than mislabelling.
static int esp_draw_figurine_marker(const EkMarker& m) {
  const unsigned char* r = gamestate_sprite(m.index);
  if (!r) return 0;
  const int statnum = (int)*(const short*)(r + EK_SPR_STATNUM);
  const int picnum = (int)*(const short*)(r + EK_SPR_PICNUM);
  if (!ekm_figurine_is_live(statnum, picnum)) return 0;
  const int pal = (int)r[EK_SPR_PAL];
  const char* nm = ekm_item_name_pal(picnum, pal);
  if (!nm) nm = EKM_FIGURINE_FALLBACK_LABEL;
  return esp_draw_cross_mark(m.p, canvas_esp_figurine_colour(), nm, 0);
}

static int esp_draw_chest_marker(const EkMarker& m) {
  const unsigned char* r = gamestate_sprite(m.index);
  if (!r) return 0;
  if (!ekm_chest_statnum_live((int)*(const short*)(r + EK_SPR_STATNUM))) return 0;
  const int picnum = (int)*(const short*)(r + EK_SPR_PICNUM);
  const int hitag = (int)*(const short*)(r + EK_SPR_HITAG);
  const int kind = ekm_chest_marked(picnum, hitag, s_chestGotStart, s_chestStart,
                                    s_chestGotMc, s_chestMc);
  char label[EKM_CHEST_LABEL_BUFSIZE];
  if (!ekm_chest_label(label, (int)sizeof label, kind, hitag)) return 0;
  return esp_draw_cross_mark(m.p, canvas_esp_chest_colour(), label, 0,
                             esp_point_cross(m.kind));
}

static int esp_draw_breakable_marker(const EkMarker& m) {
  EkWallFace f = { 0, 0, 0, 0 };
  if (!wallstate_face(m.index, &f)) return 0;
  const char* nm = ekm_breakable_name(f.picnum);
  if (!nm) return 0;
  return esp_draw_cross_mark(m.p, canvas_esp_explodable_colour(), nm, 0,
                             esp_point_cross(m.kind));
}

// WHAT A CLASSIFIED SWITCH LOOKS LIKE, FOR SPRITES AND WALLS ALIKE: the same picnums appear on both, and a switch
// that drew as a card on a sprite and a cross on a wall would be a defect no pin could catch (esp.cpp does not link
// into test_host). The SHAPE is decided once, here, from the classifier's output plus the pal the card colour needs.
static int esp_draw_switch_shape(const EkScreenPt& p, const EkSwitchInfo& si, int pal) {
  if (si.family == EK_SW_ACCESS) {
    // ekm_keycard_colour returns 0 for a pal the game has no lock colour for, and 0 draws invisible: fall back to the
    // ordinary switch colour. The item mark refuses the same way.
    const unsigned card = ekm_keycard_colour(pal);
    return esp_draw_card_mark(p, card ? card : canvas_esp_switch_colour(),
                              EKM_SWITCH_LABEL, si.line);
  }

  // The exit's own line IS its name ("Exit" or "Secret exit"), so there is no second row. Its colour is the secret
  // mark's for both, as a normal exit is the other thing a player hunts for.
  if (si.family == EK_SW_EXIT)
    return esp_draw_cross_mark(p, canvas_esp_secret_colour(), si.line, 0);

  return esp_draw_cross_mark(p, canvas_esp_switch_colour(),
                             si.family == EK_SW_DOOR ? EKM_DOOR_LABEL : EKM_SWITCH_LABEL,
                             si.line);
}

// Re-classified from the live tile, so a switch flipped or a lock opened since collection draws its new state, and
// the keycard hint disappears in the frame the card is picked up.
static int esp_draw_switch_sprite_marker(const EkMarker& m, int gotAccess) {
  const unsigned char* r = gamestate_sprite(m.index);
  if (!r) return 0;
  const int pal = (int)r[EK_SPR_PAL];
  const EkSwitchInfo si = ekm_switch_classify(
      (int)*(const short*)(r + EK_SPR_PICNUM), pal,
      (int)*(const short*)(r + EK_SPR_HITAG), (int)*(const short*)(r + EK_SPR_LOTAG),
      gotAccess);
  if (si.family == EK_SW_NONE) return 0;
  return esp_draw_switch_shape(m.p, si, pal);
}

// The same three shapes over a WALL read instead of a sprite read; the shape choice is esp_draw_switch_shape's, so a
// lock cannot show a card on a sprite and a bare cross on a wall. `m.index` is a wall index.
static int esp_draw_switch_wall_marker(const EkMarker& m, int gotAccess) {
  EkWallFace f = { 0, 0, 0, 0 };
  EkWallTags t = { 0, 0, 0 };
  if (!wallstate_face(m.index, &f)) return 0;
  if (!wallstate_tags(m.index, &t)) return 0;
  const EkSwitchInfo si = ekm_switch_classify(f.picnum, t.pal, t.hitag, t.lotag, gotAccess);
  if (si.family == EK_SW_NONE) return 0;
  return esp_draw_switch_shape(m.p, si, t.pal);
}

// The secret trigger's own five numbers per draw pass, FIVE for the secret volume's reason: `seen` (the map's
// candidates; 0 = none or the scan has not answered), `staged`, `drawn` (after range cull, live reach test and
// near-plane clip), and `refused`/`dropped`, the scan's two capacity counters kept APART.
static int s_trigSeen = 0, s_trigStaged = 0, s_trigDrawn = 0,
           s_trigRefused = 0, s_trigDropped = 0;
int esp_secrettrig_seen(void)    { return s_trigSeen; }
int esp_secrettrig_staged(void)  { return s_trigStaged; }
int esp_secrettrig_drawn(void)   { return s_trigDrawn; }
int esp_secrettrig_refused(void) { return s_trigRefused; }
int esp_secrettrig_dropped(void) { return s_trigDropped; }

// Every staged chain in range: a line from the hidden trigger to the centre of the sector it operates, the effect
// named at the trigger end. SCREEN SPACE, NOT THE WORLD-QUAD LAYER (canvas_line is 2D). IT DOES NOT FEED esp_draw's
// `drawn`, which counts MARKERS; s_trigDrawn reports.
static void esp_draw_secret_triggers(const EkProjection& p) {
  // ACQUIRED ONCE FOR THE WHOLE PASS AND PASSED TO EVERY ACCESSOR (secrettrig.h's contract): the cache is
  // double-buffered and chains are REPACKED every scan, so a second acquisition mid-loop would return index i of the
  // OTHER generation. -1 before the first walk, which every accessor refuses.
  const int gen = secrettrig_generation();

  s_trigSeen    = secrettrig_seen(gen);
  s_trigStaged  = secrettrig_chains(gen);
  s_trigRefused = secrettrig_refused(gen);
  s_trigDropped = secrettrig_dropped(gen);

  for (int i = 0; i < s_trigStaged; i++) {
    const EkTrigChain* const ch = secrettrig_chain(gen, i);
    if (!ch) continue;

    // THE CULL IS AT THE TRIGGER END, where the player is standing when the line is worth anything. dist2d, never
    // depth. FIRST, so a chain out of range pays for none of the engine reads below.
    const int dist2d = esp_dist2d(p.cam.x, p.cam.y, ch->tx, ch->ty);
    if (!ekm_within_draw_distance(dist2d, g_cfg.espdrawdist, g_cfg.espnolimit))
      continue;

    // THE REACH IS RE-TESTED LIVE, the one check the scan cannot do: sector.cpp:3293 zeroes a secret sector's lotag
    // the instant the player stands in it, so the line would OUTLIVE THE CROSS. It is the REACH
    // (ekm_trig_reaches_secret), not the operated sector's own lotag (a door is not secret).
    EkSectorInfo si = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!sectorstate_info(ch->sectnum, &si)) continue;
    const int isSecret = ekm_sector_is_secret(si.lotag) ? 1 : 0;
    int adjoins = 0;
    for (int k = 0; !isSecret && !adjoins && k < si.wallnum; k++) {
      EkWallCorner wc = { 0, 0, 0 };
      if (!wallstate_corner(si.wallptr + k, &wc)) break;
      if (wc.nextsector < 0) continue;
      EkSectorInfo nsi = { 0, 0, 0, 0, 0, 0, 0, 0 };
      if (sectorstate_info(wc.nextsector, &nsi) && ekm_sector_is_secret(nsi.lotag))
        adjoins = 1;
    }
    if (!ekm_trig_reaches_secret(isSecret, adjoins)) continue;

    // CLIPPED AT THE NEAR PLANE, NOT REFUSED: demanding that BOTH endpoints project made a line vanish as the player
    // turned from its touchplate. worldline_draw (tracer.h) clips; a segment WHOLLY behind the eye draws nothing. THE
    // FAR END IS THE OPERATED SECTOR'S CENTRE, by the cross's own rule (for a door, the door).
    const unsigned colour = canvas_esp_secret_colour();
    EkWorldLine ln = { 0.0f, 0.0f, 0.0f, 0.0f, 0, 0 };
    const bool drew = worldline_draw(p, ch->tx, ch->ty, ch->tz,
                                     ch->cx, ch->cy, si.florz, colour, &ln,
                                     EKM_WL_CALLER_OTHER, s_st.lineW, s_st.keyline);
    if (!drew) continue;

    // THE LABEL ONLY WHEN THE TRIGGER END IS REALLY THERE: a clipped end is a point on the near plane, so a cross or
    // label there would claim the touchplate is at the edge of the view. The LINE is still drawn.
    if (ln.aFront) {
      const EkScreenPt pt = { (int)ln.ax, (int)ln.ay, dist2d };
      const char* const label = ekm_trig_label(ch->effect);
      if (ch->triggerKind == EK_TRIG_TOUCHPLATE) {
        // A TOUCHPLATE GETS A MARKER AND A SWITCH DOES NOT: the plate is an invisible sprite, while a switch already
        // carries its own cross and label from "Switches and locks".
        esp_draw_cross_mark(pt, colour, label, 0);
      } else {
        // ONE TEXT ROW ABOVE THE NAME ROW ONLY WHEN SOMETHING IS ON IT: "Switches and locks" draws its label there
        // but is an INDEPENDENT PARENT, so with it off an unconditional offset floats the label a row high. A switch
        // tile on STAT_ACTIVATOR is never marked, so with Switches on its row is bare.
        const int nameY = esp_secret_name_label_y(pt.y, s_st.secretArm);
        esp_draw_label(s_hivis, esp_label_x(pt.x, 0, canvas_text_width(label, FONT_SMALL)),
                       g_cfg.espswitches ? nameY - font_height[FONT_SMALL] - 1 : nameY,
                       label, colour, FONT_SMALL);
      }
    }

    s_trigDrawn++;
  }
}

// The secret path's own three numbers per draw pass: `points` (published), `segments` (reached the screen after the
// range cull and near-plane clip) and `crosses` (teleport hops marked). Points 0 means no route; segments 0 with
// points > 0 means the route is behind or beyond the player; the two read identically on screen.
static int s_pathPoints = 0, s_pathSegments = 0, s_pathCrosses = 0;
int esp_secretpath_points(void)   { return s_pathPoints; }
int esp_secretpath_segments(void) { return s_pathSegments; }
int esp_secretpath_crosses(void)  { return s_pathCrosses; }

// The cross at a teleport hop: the secret marker's shape at HALF its arm, since a full-size cross in the same yellow
// beside a secret marker reads as one smudge. Widened before subtracting and clamped back: the projection saturates.
static void esp_draw_path_cross(int cx, int cy, unsigned colour) {
  const int arm = s_st.secretArm / 2, t = s_st.secretT;
  const int span = arm * 2 + 1;
  const EspRect r[2] = {
    { clamp_ll_to_int((long long)cx - arm), clamp_ll_to_int((long long)cy - t / 2), span, t },
    { clamp_ll_to_int((long long)cx - t / 2), clamp_ll_to_int((long long)cy - arm), t, span } };
  esp_keyed_rects(r, 2, colour, s_st.keyline);
}

// The published route: a polyline through its turn points, a cross at each teleport hop, one label at the first turn.
// SCREEN SPACE. SEGMENTS ARE CLIPPED AND MARKS ARE PROJECTED: a segment with one end behind the eye is still a
// direction (worldline_draw), while a cross or label has to be on screen to mean anything. s_pathSegments reports.
static void esp_draw_secret_path(const EkProjection& p) {
  // ACQUIRED ONCE FOR THE WHOLE PASS (secretpath.h's contract): the route is REBUILT every scan, so a second
  // acquisition mid-loop would hand back turn i of the OTHER generation. -1 before the first walk, which every
  // accessor refuses.
  const int gen = secretpath_generation();

  s_pathPoints = secretpath_count(gen);
  if (s_pathPoints <= 0) return;

  const unsigned colour = canvas_esp_secret_colour();
  const int hops = secretpath_hops(gen);

  int prevX = 0, prevY = 0, prevZ = 0, prevOk = 0, prevKind = EK_SPATH_EDGE_WALK;
  for (int k = 0; k < s_pathPoints; k++) {
    const EkSpathPoint* const pt = secretpath_point(gen, k);
    if (!pt) { prevOk = 0; continue; }

    const int dist2d = esp_dist2d(p.cam.x, p.cam.y, pt->x, pt->y);
    const int inRange =
        ekm_within_draw_distance(dist2d, g_cfg.espdrawdist, g_cfg.espnolimit) ? 1 : 0;

    // NO SEGMENT OUT OF A TELEPORT HOP: p[k] is the source effector and p[k+1] a doorway in the DESTINATION room, so
    // the line between them is the engine's own move, not a route anyone walks; the cross replaces it. The segment
    // INTO a hop is a real walk and is kept.
    if (prevOk && inRange && prevKind != EK_SPATH_EDGE_TELEPORT) {
      EkWorldLine ln = { 0.0f, 0.0f, 0.0f, 0.0f, 0, 0 };
      const bool drew = worldline_draw(p, prevX, prevY, prevZ,
                                       pt->x, pt->y, pt->z, colour, &ln,
                                       EKM_WL_CALLER_OTHER, s_st.lineW, s_st.keyline);
      if (drew) s_pathSegments++;
    }

    if (inRange) {
      float sx = 0.0f, sy = 0.0f;
      const bool onScreen = project_point(p, pt->x, pt->y, pt->z, &sx, &sy);
      if (onScreen) {
        if (pt->kind == EK_SPATH_EDGE_TELEPORT) {
          esp_draw_path_cross((int)sx, (int)sy, colour);
          s_pathCrosses++;
        }
        // ONE LABEL, AT THE FIRST TURN, naming the SEARCH'S hop count. With the scan's own outCap that EQUALS the
        // published point count (a route past the cap reports no path, never a truncated one), so it can exceed the
        // SEGMENTS DRAWN.
        if (k == 0) {
          char lab[32];
          wsprintfA(lab, "Secret: %d %s", hops, ekm_spath_room_word(hops));
          esp_draw_label(s_hivis, esp_label_x((int)sx, 0, canvas_text_width(lab, FONT_SMALL)),
                         esp_secret_name_label_y((int)sy, s_st.secretArm), lab, colour,
                         FONT_SMALL);
        }
      }
    }

    prevX = pt->x; prevY = pt->y; prevZ = pt->z;
    prevOk = inRange; prevKind = pt->kind;
  }
}

// ONE FRAME'S SCRATCH, IN .bss FOR ITS SIZE ALONE: 1024 markers is ~45 KB, too much for the render thread's stack
// inside a hooked SwapBuffers on top of targets[256]. NOT cross-frame state: esp_draw refills it from index 0 every
// call.
static EkMarker s_markers[EK_MARKER_COLLECT_MAX];

static EkTarget s_friendlies[EK_FRIENDLY_MAX];

// ONE CATEGORY'S SPRITES, PROJECTED AND COLLECTED: the two categories are the same geometry over two lists, so this
// runs twice per frame. `kind` and `targets` must always match (esp_layout.h), which is why they arrive together.
static void esp_collect_actors(const EkProjection& p, const EkTarget* targets,
                               int count, int kind, const EspActorRows& rows,
                               EkMarker* markers, int* markerCount,
                               int* markerSeq, int* markerOverflow) {
  for (int t = 0; t < count; t++) {
    const EkTarget& tgt = targets[t];

    const int x = tgt.x;
    const int y = tgt.y;

    const int dist2d = esp_dist2d(p.cam.x, p.cam.y, x, y);
    if (!ekm_within_draw_distance(dist2d, g_cfg.espdrawdist, g_cfg.espnolimit)) continue;

    const short picnum = tgt.picnum;
    const unsigned char xrep = tgt.xrep;
    const unsigned char yrep = tgt.yrep;
    const int tw = tgt.tw;
    const int th = tgt.th;

    const int height = (int)tgt.heightZ;
    const int feetZ = tgt.feetZ;

    // FOUR CORNERS, NOT A HEIGHT AND A RATIO: Build's screen axes do not share a scale and each corner carries its
    // own f, so the corners are not a rectangle (ekm_project_sprite_box). The anchor stays a separate projection:
    // labels hang off the feet.
    float fx = 0.0f, fy = 0.0f;
    if (!project_point(p, x, y, feetZ, &fx, &fy)) continue;

    EkScreenBoxF sb = { 0.0f, 0.0f, 0.0f, 0.0f, false };
    if (!ekm_project_sprite_box(p.g, (double)(x - p.cam.x), (double)(y - p.cam.y),
                                (double)ekm_sprite_halfwidth(xrep, tw),
                                (double)(feetZ - height - p.cam.z),
                                (double)(feetZ - p.cam.z),
                                p.vp.x, p.vp.y, &sb))
      continue;

    const float boxH = sb.h;
    if (boxH < 2.0f || boxH > 4000.0f) continue;
    const float hy = sb.y;
    const float halfW = sb.w * 0.5f;

    // SPLIT INTO THE FOUR THINGS THE MENU SWITCHES; each row owns one piece. Colours come from canvas.h so the ESP
    // and the panel agree. THIS CATEGORY'S OWN FOUR ROWS: with Friendlies on and all four of ITS rows off there is
    // nothing to draw.
    if (!esp_rows_any(rows)) continue;

    const short hp = tgt.hp;

    // OBSERVED AT COLLECTION, NOT AT DRAW, SO THE CAP CANNOT COST A CEILING. GAME.CON'S FIRST-TIC OVERRIDE IS APPLIED
    // BEFORE ANYTHING ELSE READS THE NUMBER: a dormant Assault Captain says 30 when it has 60, a dormant pal'd
    // Battlelord has not yet set its strength to 1000 (esp_names.h).
    const int ruledMax = ekm_enemy_ruled_max((int)picnum, (int)tgt.pal);
    const int shown = ekm_display_hp((int)hp,
                                     ekm_enemy_max_strength((int)picnum),
                                     ruledMax,
                                     actorstate_has_run(tgt.sprite));

    // RECORDED ONCE, FOR BOTH READERS, and fed `shown`: it MUST see the substitution (or a dormant Captain reads
    // 60/30) and must NOT see the pending-damage discount (or one burst teaches it a maximum under the real one).
    health_max_for(tgt.sprite, (short)shown, (int)picnum, (int)tgt.pal);

    const int bx = (int)sb.x, by = (int)hy;
    const int bw = (int)sb.w, bh = (int)boxH;

    const EkScreenPt  pt  = { (int)fx, (int)fy, dist2d };
    const EkScreenBox box = { bx, by, bw, bh };
    esp_collect(markers, markerCount, markerSeq, markerOverflow,
                kind, t, pt, box);
  }
}

int esp_draw(int* notDrawn, int* refused, int* wallOverflow) {
  if (notDrawn)     *notDrawn     = 0;
  if (refused)      *refused      = 0;
  if (wallOverflow) *wallOverflow = 0;
  esp_frame_style();

  // THE QUAD LIST'S PASS BOUNDARY, AND IT CANNOT BE LEFT TO THE FLUSH: G_DrawRooms returns early on the automap, help
  // screen and cursectnum == -1 (game.cpp:799) while this collect keeps running, so resetting only in the flush let
  // the list LATCH. ABOVE EVERY GATE, so an early-returning pass still clears what the last one left.
  worldquad_reset();
  s_comboSeen = 0;
  s_comboPushed = 0;
  s_comboBoxed = 0;
  s_comboFlat = 0;
  s_comboQuads = 0;
  s_secvolStaged = 0;
  s_secvolDrawn = 0;
  s_secvolSkipped = 0;
  s_secvolUnreadable = 0;
  s_secvolInexact = 0;
  s_secvolQuads = 0;
  s_trigSeen = 0;
  s_trigStaged = 0;
  s_trigDrawn = 0;
  s_trigRefused = 0;
  s_trigDropped = 0;
  s_pathPoints = 0;
  s_pathSegments = 0;
  s_pathCrosses = 0;

  if (!gamestate_ready()) return 0;

  int gm = 0;
  const bool gmOk = player_read_gm(&gm);

  // THE PER-LEVEL RESET: s_healthMax is a maximum per SPRITE INDEX, or a slot that held a Battlelord prints a live
  // trooper as 30/4500. ABOVE THE ROW'S OWN GATE, or the maxima wait for the row to come back. ekm_level_loading, NOT
  // !esp_level_loaded (also false over the pause menu). The tier-readout throttle rides the same reset.
  if (ekm_level_loading(gm, gmOk)) { esp_forget_health(); esp_forget_actor_log(); }

  // The figurine counter's OWN RESET, unconditional like the block above: with the counter off through a level change
  // its totals would survive (esp_figurine_maybe_reset has the four paths).
  esp_figurine_maybe_reset();

  // FEATURES DEFAULT OFF. Enemies, Items, Secret areas and Explodables are INDEPENDENT PARENTS, so the early return
  // only fires when every one is off and each walk carries its own switch. g_cfg.esp decides WHETHER an enemy marker
  // draws, its four sub-rows decide WHAT. One term per line, so the next category joins the list.
  if (!g_cfg.esp &&
      !g_cfg.espfriendlies &&
      !g_cfg.espitems &&
      !g_cfg.espsecrets &&
      !g_cfg.espexplodables &&
      !g_cfg.espbreakables &&
      !g_cfg.espcash &&
      !g_cfg.espswitches &&
      !g_cfg.espfigurines &&
      !g_cfg.figurinecounter &&
      !g_cfg.espchests) return 0;
  if (!esp_level_loaded(gm, gmOk)) return 0;

  EkProjection p;
  if (!project_begin(&p, EK_PC_ESP)) return 0;
  s_frameCamX = p.cam.x;
  s_frameCamY = p.cam.y;

  int drawn = 0;

  EkTarget targets[EK_TARGET_MAX];
  // THE WALK BELONGS TO BOTH ACTOR ROWS, SO EITHER ONE RUNS IT: gated on g_cfg.esp alone, Enemies OFF and Friendlies
  // ON would silently kill the friendly category. Each parent still gates only its OWN markers, below.
  const bool wantEnemies = g_cfg.esp != 0;
  const bool wantFriendlies = g_cfg.espfriendlies != 0;
  const EspActorRows enemyRows = esp_enemy_rows();
  const EspActorRows friendlyRows = esp_friendly_rows();

  // WITH FRIENDLIES OFF, NO SECOND ARRAY IS PASSED and the collector refuses a friendly outright. esp.walk's items
  // are the VirtualQuery calls readable() made (the guard cost was the suspect, not the walk's size); the zone
  // brackets the call unconditionally so an all-off frame still reports a true span.
  int count = 0, friendlyCount = 0;
  perf_zone_begin(EK_PZ_ESP_WALK);
  const int vqBefore = gamevars_vq_calls();
  if (wantEnemies || wantFriendlies)
    count = targets_collect_split(targets, EK_TARGET_MAX,
                                  wantFriendlies ? s_friendlies : NULL,
                                  wantFriendlies ? EK_FRIENDLY_MAX : 0,
                                  &friendlyCount);
  perf_zone_end(EK_PZ_ESP_WALK, gamevars_vq_calls() - vqBefore);

  int markerCount = 0, markerSeq = 0, markerOverflow = 0;

  perf_zone_begin(EK_PZ_ESP_ACTORS);
  if (wantEnemies)
    esp_collect_actors(p, targets, count, EK_MARKER_ENEMY, enemyRows,
                       s_markers, &markerCount, &markerSeq, &markerOverflow);
  if (wantFriendlies)
    esp_collect_actors(p, s_friendlies, friendlyCount, EK_MARKER_FRIENDLY,
                       friendlyRows, s_markers, &markerCount, &markerSeq,
                       &markerOverflow);
  perf_zone_end(EK_PZ_ESP_ACTORS, count + friendlyCount);

  // ONCE PER FRAME, NOT ONCE PER SPRITE: the item walk visits every slot and neither value changes in a frame. A
  // FAILED READ LEAVES THE ROW AS IT DRAWS TODAY (its CON symbol): a wrong name is worse than none, while a missing
  // card hint is merely silence about a lock.
  bool gotPchar = false, gotUpg = false;
  int pchar = 0, upgBits = 0;
  if (g_cfg.espitems) {
    gotPchar = gamevars_pchar(EK_ESP_LOCAL_PLAYER, &pchar);
    const int which = gotPchar ? esp_upgrade_array_for_pchar(pchar) : -1;
    if (which >= 0) {
      const bool read = gamearrays_upgrade(which, EKM_AA_UPG_SLOT_DEVISTATOR,
                                           &upgBits);
      gotUpg = read;
    }
    // esp.items brackets ONLY this walk, not esp_census_items below (once a minute, debuglog only): folding it in
    // would let a rare spike pass as this zone's steady cost.
    const int markerCountBeforeItems = markerCount;
    perf_zone_begin(EK_PZ_ESP_ITEMS);
    EspTakeCtx take = EspTakeCtx();
    const bool hideFull = g_cfg.espitemshidefull != 0;
    if (hideFull) esp_take_begin(&take, gotPchar, pchar);
    esp_collect_items(p, gotPchar, pchar, gotUpg, upgBits,
                      hideFull ? &take : NULL,
                      s_markers, &markerCount, &markerSeq, &markerOverflow);
    perf_zone_end(EK_PZ_ESP_ITEMS, markerCount - markerCountBeforeItems);
    if (hideFull) esp_take_log(take, markerCount - markerCountBeforeItems);

    // INSIDE THIS ROW'S GATE ON PURPOSE: the census describes the walk that just ran, and with Items off it would
    // report guard verdicts for a walk that is not happening.
    esp_census_items();
  }

  // mapextent_ready() IS PART OF THE GATE: a category that cannot bound its walk must not walk. esp.cpp never calls
  // mapextent_scan() (it belongs to the scan thread), so the usual `if (!X_ready()) X_scan();` idiom would be wrong
  // here.
  if (g_cfg.espsecrets && mapextent_ready())
    esp_collect_secrets(p, s_markers, &markerCount, &markerSeq, &markerOverflow);

  // THE SUB-ROW'S OWN GATE, with no mapextent_ready() term: the panels come from secretvol.cpp's published cache,
  // which took its own bound at scan time. It reads only one live lotag per staged room, which sectorstate_info
  // bounds-checks.
  if (g_cfg.espsecrets && g_cfg.secretvolume)
    esp_push_secret_volumes(p);

  // THE SECOND SUB-ROW'S GATE, no mapextent_ready() term either. BEFORE the marker loop: canvas draws in call order,
  // so the lines go UNDER the crosses they point at.
  if (g_cfg.espsecrets && g_cfg.secrettriggers)
    esp_draw_secret_triggers(p);

  // THE THIRD SUB-ROW'S GATE, no mapextent_ready() term either. AFTER the trigger lines and BEFORE the marker loop:
  // the route goes under the crosses and over a trigger line where they share a room (the newer, coarser answer).
  if (g_cfg.espsecrets && g_cfg.secretpath)
    esp_draw_secret_path(p);

  if (g_cfg.espexplodables)
    esp_collect_cracks(p, s_markers, &markerCount, &markerSeq, &markerOverflow);

  // THE SCAN IS UNGATED AND THE COLLECTOR IS NOT: the scan's first branch clears the cache when BOTH rows are off,
  // and gating the call would make that branch unreachable and leave a level's marks latched. ekm_wall_scan_due
  // measures elapsed wall-clock, so a long all-off interval forces a rescan.
  esp_scan_walls();
  if (g_cfg.espbreakables)
    esp_collect_breakables(p, s_markers, &markerCount, &markerSeq, &markerOverflow);

  // AA12'S OWN GATE: a SPRITE walk with a per-actor gamevar read behind it, while the row above is walls out of the
  // cached scan; neither gates the other. The summary line sits INSIDE this gate: with the row off it would report a
  // walk that is not happening.
  if (g_cfg.espcash) {
    esp_collect_cash(p, s_markers, &markerCount, &markerSeq, &markerOverflow);
    esp_log_cash_summary();
  }

  // EITHER ROW RUNS THIS WALK (the figurine counter needs the totals with the marks off, and the marks need it with
  // the counter off); the chest row rides it too. The summary log sits inside the gate for esp_log_cash_summary's
  // reason.
  if (g_cfg.espfigurines || g_cfg.figurinecounter || g_cfg.espchests)
    esp_collect_figurines(p, s_markers, &markerCount, &markerSeq, &markerOverflow,
                          g_cfg.espfigurines || g_cfg.figurinecounter,
                          g_cfg.espchests != 0);

  // ONCE PER FRAME, NOT ONCE PER SPRITE: player_read_got_access resolves the player base and reads through it, and
  // the keycard bits cannot change in a frame. A FAILED READ PASSES 0, which makes EVERY card hint appear: noise on
  // an opened lock beats the ESP going quiet on the one the player cannot open.
  int gotAccess = 0;
  s_levelHasSecretExit = false;
  if (g_cfg.espswitches) {
    if (!player_read_got_access(&gotAccess)) gotAccess = 0;
    esp_collect_switch_sprites(p, gotAccess, s_markers,
                               &markerCount, &markerSeq, &markerOverflow);
    esp_collect_exit_buttons(p,true, s_markers,
                             &markerCount, &markerSeq, &markerOverflow);
    // The third source walks nothing: esp_scan_walls has run this frame and the switch walls sit in its cache beside
    // the panels. Gated on this row alone.
    esp_collect_switch_walls(p, gotAccess, s_markers,
                             &markerCount, &markerSeq, &markerOverflow);
  } else if (g_cfg.espsecrets) {
    // SECRETS WITHOUT SWITCHES: the corner note is still Secrets' business, so the exit-button walk runs to learn the
    // flag with collect=false (no marker for a layer this row did not ask for). NUKEBUTTON is on the MISC list, which
    // only that walk sees.
    esp_collect_exit_buttons(p,false, s_markers,
                             &markerCount, &markerSeq, &markerOverflow);
  }

  // SELECT, CLAMP, ORDER, IN THAT ORDER: clamping first handed ekm_keep_nearest `keep >= count`, so the cap was
  // lowest sprite index, never nearest. Selection is by distance, ordering is the painter's. The CAP'S DISCARD IS
  // COUNTED TOO: a frame past 1024 is not reachable on an ordinary level, while 257 is what the selection is for.
  ekm_keep_nearest(s_markers, markerCount, EK_MARKER_MAX);
  const int markerCapped = markerCount > EK_MARKER_MAX ? markerCount - EK_MARKER_MAX : 0;
  markerCount -= markerCapped;
  ekm_sort_far_to_near(s_markers, markerCount);

  for (int i = 0; i < markerCount; i++) {
    switch (s_markers[i].kind) {
      case EK_MARKER_ENEMY:
        drawn += esp_draw_actor_marker(s_markers[i], targets,
                                       canvas_esp_colour(true), enemyRows);   break;
      case EK_MARKER_FRIENDLY:
        drawn += esp_draw_actor_marker(s_markers[i], s_friendlies,
                                       C_FRIENDLY, friendlyRows);             break;
      case EK_MARKER_ITEM:
        drawn += esp_draw_item_marker(s_markers[i], gotPchar, pchar,
                                      gotUpg, upgBits);                       break;
      case EK_MARKER_SECRET: drawn += esp_draw_secret_marker(s_markers[i]);        break;
      case EK_MARKER_CRACK: drawn += esp_draw_crack_marker(s_markers[i]);          break;
      case EK_MARKER_BREAKABLE: drawn += esp_draw_breakable_marker(s_markers[i]);  break;
      case EK_MARKER_CASH: drawn += esp_draw_cash_marker(s_markers[i]);            break;
      case EK_MARKER_FIGURINE: drawn += esp_draw_figurine_marker(s_markers[i]);    break;
      case EK_MARKER_CHEST: drawn += esp_draw_chest_marker(s_markers[i]);          break;
      case EK_MARKER_SWITCH_SPRITE:
        drawn += esp_draw_switch_sprite_marker(s_markers[i], gotAccess);           break;
      case EK_MARKER_SWITCH_WALL:
        drawn += esp_draw_switch_wall_marker(s_markers[i], gotAccess);             break;
      default: break;
    }
  }

  // THE LEVEL NOTE, AFTER THE MARKERS so it is never painted over. Gate is Secrets OR Switches
  // (esp_secret_exit_note_wanted): World Tour gates on Secrets alone and HRP on Switches alone, but both rows collect
  // the flag here.
  const bool showSecretNote = esp_secret_exit_note_wanted(
      g_cfg.espsecrets, g_cfg.espswitches, s_levelHasSecretExit);

  // THE FIGURINE COUNTER, the corner stack's middle slot: Y == 0 is treated exactly like a switched-off row
  // (esp_figurine_counter_wanted). `false` for showPose here: this call wants only `.counter`.
  const bool showFigCounter =
      ekm_figurine_counter_wanted(g_cfg.figurinecounter, s_figShown.y);
  // "EDF chests: X / Y (episode E / 11)" under it, in httkb only. X/Y are THIS level's own mask; Y == 0 hides it like
  // the figurine row's y > 0 gate.
  const int chestY = ekm_popcount16(s_chestLevelMask);
  const int chestX = ekm_popcount16(s_chestLevelMask & s_chestStart);
  const bool showChestCounter = ekm_edfchest_counter_wanted(
      g_cfg.espchests, s_chestHaveLevel, s_chestVol, s_chestGotStart, chestY);
  if (showSecretNote || showFigCounter || showChestCounter) {
    const EspCornerSaved prev = esp_corner_begin();
    esp_draw_secret_exit_note(showSecretNote);
    const EspCornerY corner = esp_corner_lines(showSecretNote, showFigCounter, false,
                                               font_height[FONT_UI], font_height[FONT_UI],
                                               showChestCounter, font_height[FONT_UI]);
    char line[EKM_FIGURINE_COUNTER_BUFSIZE];
    if (showFigCounter &&
        ekm_figurine_counter_label(line, (int)sizeof(line), s_figShown.x, s_figShown.y))
      esp_draw_corner_line(FREECAM_POSE_X, corner.counter, line, C_FIGURINE_TEXT, FONT_UI);
    char chestLine[EKM_EDFCHEST_COUNTER_BUFSIZE];
    if (showChestCounter &&
        ekm_edfchest_counter_label(chestLine, (int)sizeof(chestLine), chestX, chestY,
                                   s_chestGotStart ? ekm_edfchest_found(s_chestStart) : 0))
      esp_draw_corner_line(FREECAM_POSE_X, corner.chests, chestLine, C_CHEST, FONT_UI);
    esp_corner_end(prev);
  }

  if (notDrawn)     *notDrawn     = markerOverflow + markerCapped;
  if (refused)      *refused      = markerOverflow;
  if (wallOverflow) *wallOverflow = s_wallOverflow;
  return drawn;
}

// The aim calibration marker: two squares over one target, World Tour's instrument. Gate first: aimdebug alone draws
// nothing, matching aim_frame's acquisition gate, since a marker for a point nothing published would be stale.
int aim_draw_marker(void) {
  if (!aim_marker_on(g_cfg.aimon, g_cfg.silenton, g_cfg.aimdebug)) return 0;

  AimMarkerPoints m;
  if (!aim_marker_points(&m)) return 0;

  EkProjection p;
  if (!project_begin(&p, EK_PC_AIM)) return 0;

  float rx = 0.0f, ry = 0.0f, lx = 0.0f, ly = 0.0f;
  const bool haveRaw = project_point(p, m.rawX, m.rawY, m.rawZ, &rx, &ry);
  const bool haveLed = project_point(p, m.ledX, m.ledY, m.ledZ, &lx, &ly);
  if (!haveRaw && !haveLed) return 0;

  const unsigned colour = canvas_aim_colour();
  esp_frame_style();
  const int half = s_st.aimHalf, t = s_st.aimT;
  const int span = aim_marker_span(half);

  if (haveRaw) {
    const int bx = aim_marker_x((int)rx, half), by = aim_marker_y((int)ry, half);
    const EspRect r[4] = {
      { bx, by, span, t },
      { bx, aim_marker_edge_far((int)ry, half, t), span, t },
      { bx, by, t, span },
      { aim_marker_edge_far((int)rx, half, t), by, t, span } };
    esp_keyed_rects(r, 4, colour, s_st.keyline);
  }

  if (haveLed) {
    const EspRect r = { aim_marker_x((int)lx, half), aim_marker_y((int)ly, half), span, span };
    esp_keyed_rects(&r, 1, colour, s_st.keyline);
    char lab[32];
    wsprintfA(lab, "%s %d", AIM_MARKER_LABEL, m.sprite);
    esp_draw_label(s_hivis, esp_label_x((int)lx, 0, canvas_text_width(lab, FONT_SMALL)),
                   aim_marker_label_y((int)ly, half), lab, colour, FONT_SMALL);
  }

  return 1;
}

// The free camera's "You" marker: a cyan diamond on the player's own body so it can be found once the view has
// detached (geometry: esp_layout.h). HERE AND NOT IN camera.cpp: that is the game thread's hook file, while this runs
// on the render thread inside SwapBuffers, alongside the port's other non-ESP overlay markers.
int camera_draw_player_marker(void) {
  // THE FIRST TERM IS THE LIVE CAMERA, NOT g_cfg.freecamon: a marker that appeared the moment the row was switched on
  // would sit on the player's body while he still looks out through his own eyes.
  const bool live = camera_active();
  const bool wantBody = freecam_marker_on(live, g_cfg.freecamshowplayer);
  const bool wantText = freecam_debug_on(live, g_cfg.freecamdebug);
  if (!wantBody && !wantText) return 0;
  int drew = 0;

  // THE READOUT FIRST: it needs neither the player nor the projection and must survive both refusals (a camera flown
  // where the player cannot be projected is when the numbers matter). camera_viewpoint_read, NOT the camera state:
  // the camera steps on the game thread, so the published seqlock read is the only safe one.
  if (wantText) {
    FreecamPose vp;
    if (camera_viewpoint_read(&vp)) {
      // THE SAME TWO ANSWERS esp_draw used for its note/counter draws, recomputed rather than carried over a frame
      // boundary, off the same statics, so the two esp_corner_lines calls always agree on where the note and counter
      // sit.
      const bool showSecretNote = esp_secret_exit_note_wanted(
          g_cfg.espsecrets, g_cfg.espswitches, s_levelHasSecretExit);
      const bool showFigCounter =
          ekm_figurine_counter_wanted(g_cfg.figurinecounter, s_figShown.y);
      const bool showChestCounter = ekm_edfchest_counter_wanted(
          g_cfg.espchests, s_chestHaveLevel, s_chestVol, s_chestGotStart,
          ekm_popcount16(s_chestLevelMask));
      const EspCornerY corner =
          esp_corner_lines(showSecretNote, showFigCounter, true,
                           font_height[FONT_UI], font_height[FONT_UI],
                           showChestCounter, font_height[FONT_UI]);
      char line[FREECAM_POSE_BUFSIZE];
      freecam_pose_line(line, vp.x, vp.y, vp.z, vp.ang, vp.horiz);
      const EspCornerSaved prev = esp_corner_begin();
      esp_draw_corner_line(FREECAM_POSE_X, corner.pose, line, C_CORNER_POSE, FONT_SMALL);
      esp_corner_end(prev);
      drew = 1;
    }
  }

  if (!wantBody) return drew;

  // ps->pos AND NOT THE PLAYER'S SPRITE (World Tour's choice): our camera substitutes ud.camera* and never writes
  // ps->pos, so nothing here can be confused with the camera's position. pos.z is the view height, so the diamond
  // sits on the body.
  PlayerSnap ps;
  if (!player_snap(&ps)) return drew;

  // THE SAME PROJECTION THE ESP USES, already registered with the flying camera: the bracket leaves ud.camera*
  // written on purpose, and project_begin projects from the pose recorded at that draw.
  EkProjection p;
  if (!project_begin(&p, EK_PC_CAM_MARKER)) return drew;

  float sx = 0.0f, sy = 0.0f;
  if (!project_point(p, ps.x, ps.y, ps.z, &sx, &sy)) return drew;

  const unsigned colour = canvas_player_colour();
  const int cx = (int)sx, cy = (int)sy;
  esp_frame_style();
  EspRect rows[PLAYER_MARKER_HALF * 2 + 1];
  const int n = player_marker_rows();
  for (int r = 0; r < n; r++) {
    const EspRect row = { player_marker_row_x(cx, r), player_marker_row_y(cy, r),
                          player_marker_row_w(r), 1 };
    rows[r] = row;
  }
  esp_keyed_rects(rows, n, colour, s_st.keyline);
  esp_draw_label(s_hivis, esp_label_x(cx, 0, canvas_text_width(PLAYER_MARKER_LABEL, FONT_SMALL)),
                 player_marker_label_y(cy), PLAYER_MARKER_LABEL, colour, FONT_SMALL);
  return 1;
}

// The teleport's landing column: four side quads and a cap, drawn from the same plan the key acts on (camera.cpp), so
// the marker and the key never disagree about the spot. QUADS THROUGH worldquad_push, NOT LINES: a filled translucent
// column.
static int s_tpQuads = 0, s_tpNotice = 0;
int esp_teleport_quads(void)  { return s_tpQuads; }
int esp_teleport_notice(void) { return s_tpNotice; }

static void esp_draw_teleport_mark(const EkTeleportPlan& plan) {
  // THE COLUMN STANDS ON THE FLOOR, NOT ON THE PLAN'S z, which is the landing EYE: ekm_tp_plan returns florz -
  // EK_TP_PHEIGHT and Build's z grows downward. Widened: the z arrived through engine memory this DLL does not own.
  const int floorz = clamp_ll_to_int((long long)plan.z + (long long)EK_TP_PHEIGHT);
  const EkMarkBox b = ekm_tp_mark_box(plan.x, plan.y, floorz);
  const unsigned colour = C_TELEPORT_MARK;

  for (int i = 0; i < 4; i++) {
    const int j = (i + 1) & 3;
    EkWorldQuad q;
    q.x[0] = b.c[i].x; q.y[0] = b.c[i].y; q.z[0] = b.zTop;
    q.x[1] = b.c[i].x; q.y[1] = b.c[i].y; q.z[1] = b.zBottom;
    q.x[2] = b.c[j].x; q.y[2] = b.c[j].y; q.z[2] = b.zBottom;
    q.x[3] = b.c[j].x; q.y[3] = b.c[j].y; q.z[3] = b.zTop;
    q.argb = colour;
    worldquad_push(q);
    s_tpQuads++;
  }

  // THE CAP CLOSES THE COLUMN WHEN IT IS LOOKED DOWN ON, the normal way to see it. ekm_tp_mark_box walks once around
  // the square, so consecutive corners are always a side.
  EkWorldQuad cap;
  for (int c = 0; c < 4; c++) { cap.x[c] = b.c[c].x; cap.y[c] = b.c[c].y; cap.z[c] = b.zTop; }
  cap.argb = colour;
  worldquad_push(cap);
  s_tpQuads++;
}

// The refusal line, centred under the crosshair for the two seconds ekm_tp_notice_visible allows. Only a refusal
// draws; EK_TP_NONE is the empty string. C_PLAYER AND NOT C_TELEPORT_MARK, though the same cyan: the column's
// constant carries alpha 0x40, and a one-pixel glyph at a quarter alpha cannot be read.
static void esp_draw_teleport_notice(const EkTeleportPlan& plan, int have,
                                     unsigned setMs, int screenW, int screenH) {
  if (!ekm_tp_has_notice(plan)) return;
  if (!ekm_tp_notice_visible(have, setMs, wallclock_ms())) return;

  const int w = canvas_text_width(plan.reason, FONT_SMALL);
  esp_draw_label(g_cfg.hivis != 0, (screenW - w) / 2, screenH / 2 + EK_TP_NOTICE_Y,
                 plan.reason, canvas_player_colour(), FONT_SMALL);
  s_tpNotice = 1;
}

// `active` is wantEsp, overlay.cpp's screenshot gate. THE CALL IS UNCONDITIONAL and the gate a parameter, so both
// counters are zeroed even when nothing may draw; a counter behind a gate has latched three times on this project.
void esp_draw_teleport(int active, int screenW, int screenH) {
  s_tpQuads = 0;
  s_tpNotice = 0;
  if (!active) return;

  // THE LIVE CAMERA, NOT g_cfg.freecamon: the row goes on before the camera does, and the last flight's plan is still
  // published, so the row alone would stand a column at a spot nobody is aiming at.
  if (!camera_active()) return;

  // THE COLUMN TAKES THE LIVE PLAN AND THE NOTICE TAKES THE LATCH: two accessors for two questions (camera.h);
  // reading one for both printed a refusal nobody asked for. NO PROJECTION HERE: worldquad_push takes world
  // coordinates. A refused plan draws nothing; the notice is screen space regardless.
  if (g_cfg.freecamteleportmarker) {
    EkTeleportPlan live = { 0, 0, 0, 0, -1, 0 };
    int haveLive = 0;
    const int gotLive = camera_teleport_plan_read(&live, &haveLive);
    if (gotLive && haveLive && live.ok) esp_draw_teleport_mark(live);
  }

  EkTeleportPlan said = { 0, 0, 0, 0, -1, 0 };
  unsigned setMs = 0;
  int have = 0;
  const int gotSaid = camera_teleport_outcome(&said, &setMs, &have);
  if (gotSaid) esp_draw_teleport_notice(said, have, setMs, screenW, screenH);
}
