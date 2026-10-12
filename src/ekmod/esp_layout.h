#pragma once
#include <stdio.h>
#include "ui/common.h"
#include "ui/canvas.h"
#include "ui/font.h"
#include "ui/ui.h"

// ESP marker layout: corner brackets, health bar, labels, given a box. The placement arithmetic is World Tour's
// verbatim: a marker nearly the same in two games is worse than one plainly different.

// Arm length is a fraction of the edge, so brackets read as corners at any box size.
const int ESP_BRACKET_T   = 2;
const int ESP_BRACKET_DIV = 3;
const int ESP_BRACKET_MIN = 3;

const int ESP_BAR_W   = 3;
const int ESP_BAR_GAP = 3;

const int ESP_LABEL_GAP = 17;
const int ESP_DIST_GAP  = 2;

// Arm length for an edge of `dim` pixels. Capped at (dim-1)/2, not dim/2: two opposing arms of dim/2 on an even edge
// would meet and close the brackets into an outline, merging markers into a grid in a crowd.
inline int esp_bracket_arm(int dim) {
  if (dim <= 0) return 0;
  int a = dim / ESP_BRACKET_DIV;
  if (a < ESP_BRACKET_MIN) a = ESP_BRACKET_MIN;
  const int cap = (dim - 1) / 2;
  if (a > cap) a = cap;
  return a < 0 ? 0 : a;
}

inline int esp_bracket_thickness(int armX, int armY, int t = ESP_BRACKET_T) {
  if (t > armX) t = armX;
  if (t > armY) t = armY;
  return t < 0 ? 0 : t;
}

inline int esp_label_x(int boxX, int boxW, int textW) {
  return clamp_ll_to_int((long long)boxX + (long long)boxW / 2 - (long long)textW / 2);
}

inline int esp_name_label_y(int boxY) {
  return clamp_ll_to_int((long long)boxY - ESP_LABEL_GAP);
}

inline int esp_dist_label_y(int boxY, int boxH) {
  return clamp_ll_to_int((long long)boxY + (long long)boxH + ESP_DIST_GAP);
}

inline int esp_bar_x(int boxX, int barW = ESP_BAR_W) {
  return clamp_ll_to_int((long long)boxX - ESP_BAR_GAP - barW);
}

// The health label is two runs in two faces (name in FONT_SMALL, number in FONT_MONO so digits align), centred as one
// pair; World Tour's shape, only the pixel gap is a number here.
const int ESP_LABEL_RUN_GAP = 4;
struct EspLabelRuns { int nameX, numX, totalW; };
inline EspLabelRuns esp_label_runs(int boxX, int boxW, int nameW, int numW) {
  EspLabelRuns r = { 0, 0, 0 };
  const int gap = (nameW > 0 && numW > 0) ? ESP_LABEL_RUN_GAP : 0;
  r.totalW = clamp_ll_to_int((long long)nameW + gap + numW);
  r.nameX = esp_label_x(boxX, boxW, r.totalW);
  r.numX = clamp_ll_to_int((long long)r.nameX + nameW + gap);
  return r;
}

inline int esp_label_mono_y(int nameY, int smallH, int monoH) {
  return clamp_ll_to_int((long long)nameY + smallH - monoH);
}

struct EkScreenPt  { int x, y, dist2d; };
struct EkScreenBox { int x, y, w, h; };

// `index` is kind-specific: for EK_MARKER_ENEMY it is the slot in the frame's own targets[] array, which already
// holds the hp, hpShown and actor state the enemy draw needs.
struct EkMarker { int kind, index, dist2d, seq; EkScreenPt p; EkScreenBox box; };

// Two ceilings, and they must differ: what one frame may gather has to exceed what it may draw, or ekm_keep_nearest's
// `keep >= count` guard is always true and the cap silently becomes first-come (lowest sprite index).
const int EK_MARKER_COLLECT_MAX = 1024;
const int EK_MARKER_MAX         = 256;
// Appended, never renumbered: the kind is stored in EkMarker::kind and dispatched on, so moving a value would send
// one category through another's draw and array.
enum { EK_MARKER_ENEMY = 0, EK_MARKER_ITEM = 1, EK_MARKER_SECRET = 2,
       EK_MARKER_CRACK = 3, EK_MARKER_BREAKABLE = 4,
       EK_MARKER_SWITCH_SPRITE = 5, EK_MARKER_SWITCH_WALL = 6,
       EK_MARKER_FRIENDLY = 7, EK_MARKER_CASH = 8, EK_MARKER_FIGURINE = 9,
       EK_MARKER_CHEST = 10 };

// Keep the nearest `keep`, then let the caller order them: far-to-near is the painter order, so a cap applied to it
// drops the near markers and keeps the distant ones (World Tour records conflating the two).
inline void ekm_keep_nearest(EkMarker* m, int count, int keep) {
  if (!m || count <= 0 || keep <= 0 || keep >= count) return;
  for (int i = 0; i < keep; i++) {
    int best = i;
    for (int j = i + 1; j < count; j++)
      if (m[j].dist2d < m[best].dist2d) best = j;
    if (best != i) { const EkMarker t = m[i]; m[i] = m[best]; m[best] = t; }
  }
}

// Painter order, farthest first. Takes the caller's count as given: clamping to EK_MARKER_MAX here was a second
// ceiling over a 1024-slot array that left the tail unordered with no diagnostic.
inline void ekm_sort_far_to_near(EkMarker* m, int count) {
  if (!m || count < 2) return;
  for (int i = 1; i < count; i++) {
    const EkMarker key = m[i];
    int j = i - 1;
    while (j >= 0 && m[j].dist2d < key.dist2d) { m[j + 1] = m[j]; j--; }
    m[j + 1] = key;
  }
}

// Aim calibration marker, World Tour's shape: a hollow square at the raw body point and a filled one at the led
// point. They coincide for a hitscan weapon and separate by the lead solver's amount for a projectile; that gap is
// the instrument.
const int AIM_MARKER_HALF = 4;
const int AIM_MARKER_T    = 1;
const char* const AIM_MARKER_LABEL = "Aim";

inline int aim_marker_span(int half = AIM_MARKER_HALF) { return half * 2 + 1; }

inline int aim_marker_x(int cx, int half = AIM_MARKER_HALF) { return clamp_ll_to_int((long long)cx - half); }
inline int aim_marker_y(int cy, int half = AIM_MARKER_HALF) { return clamp_ll_to_int((long long)cy - half); }

inline int aim_marker_edge_far(int c, int half = AIM_MARKER_HALF, int t = AIM_MARKER_T) {
  return clamp_ll_to_int((long long)c + half + 1 - t);
}

inline int aim_marker_label_y(int cy, int half = AIM_MARKER_HALF) {
  return clamp_ll_to_int((long long)cy - half - ESP_LABEL_GAP);
}

// The marker's gate, from World Tour: acquisition runs when either assist asks, so the marker needs its own switch
// and at least one assist live, or aimdebug alone would draw over a target nothing aims at.
inline bool aim_marker_on(int aimon, int silenton, int aimdebug) {
  return (aimon != 0 || silenton != 0) && aimdebug != 0;
}

// The free camera's own-body marker: a diamond on the player's sprite, labelled "You". Neither a box (enemy) nor a
// square (aim), so it never reads as a target; the camera's cyan keeps it unambiguous once the camera has detached.
const int PLAYER_MARKER_HALF = 6;

const char* const PLAYER_MARKER_LABEL = "You";

inline int player_marker_rows() { return PLAYER_MARKER_HALF * 2 + 1; }

inline int player_marker_row_halfwidth(int row) {
  const int d = row - PLAYER_MARKER_HALF;
  const int w = PLAYER_MARKER_HALF - (d < 0 ? -d : d);
  return w < 0 ? 0 : w;
}

inline int player_marker_row_x(int cx, int row) {
  return clamp_ll_to_int((long long)cx - player_marker_row_halfwidth(row));
}
inline int player_marker_row_y(int cy, int row) {
  return clamp_ll_to_int((long long)cy - PLAYER_MARKER_HALF + row);
}
inline int player_marker_row_w(int row) { return player_marker_row_halfwidth(row) * 2 + 1; }

inline int player_marker_label_y(int cy) {
  return clamp_ll_to_int((long long)cy - PLAYER_MARKER_HALF - ESP_LABEL_GAP);
}

// Draws only when the camera is actually flying AND the player's switch is on. The first term must be the live camera
// state, not the toggle: a marker appearing on enabling would sit on the player's body while looking out of their
// eyes.
inline bool freecam_marker_on(bool freecamActive, int freecamshowplayer) {
  return freecamActive && freecamshowplayer != 0;
}

inline bool freecam_debug_on(bool freecamActive, int freecamdebug) {
  return freecamActive && freecamdebug != 0;
}

// Five numbers, one mono line at the top left while the camera flies (World Tour's freecamdebug). horiz=, not pitch=.
const int ESP_SECRET_EXIT_NOTE_MARGIN = 8;
const char* const ESP_SECRET_EXIT_NOTE = "Secret exit in this level";
inline int esp_secret_exit_note_x() { return ESP_SECRET_EXIT_NOTE_MARGIN; }
inline int esp_secret_exit_note_y() { return ESP_SECRET_EXIT_NOTE_MARGIN; }

// The corner stack: the secret-exit note (passive, owns the corner), the figurine counter, then the free camera's
// pose readout (a debugging aid, last), each taking the first free slot from the top so switching one off closes the
// gap. -1 marks "not shown".
const int ESP_CORNER_OUTLINE = 2;
inline int esp_corner_pitch(int fontH) { return fontH + 2 * ESP_CORNER_OUTLINE; }

struct EspCornerY { int note, counter, pose, chests; };
inline EspCornerY esp_corner_lines(bool showNote, bool showCounter, bool showPose,
                                   int noteH, int counterH,
                                   bool showChests = false, int chestsH = 0) {
  EspCornerY c = { -1, -1, -1, -1 };
  int y = ESP_SECRET_EXIT_NOTE_MARGIN;
  if (showNote)    { c.note    = y; y += esp_corner_pitch(noteH); }
  if (showCounter) { c.counter = y; y += esp_corner_pitch(counterH); }
  if (showChests)  { c.chests  = y; y += esp_corner_pitch(chestsH); }
  if (showPose)    { c.pose    = y; }
  return c;
}

inline int esp_outline_offsets(int r, int dx[8], int dy[8]) {
  if (r <= 0 || !dx || !dy) return 0;
  int n = 0;
  for (int j = -1; j <= 1; j++)
    for (int i = -1; i <= 1; i++) {
      if (i == 0 && j == 0) continue;
      dx[n] = i * r; dy[n] = j * r; n++;
    }
  return n;
}

const int ESP_CORNER_SCALE_MIN     = 100;
const int ESP_CORNER_SCALE_MAX     = UI_SCALE_MAX;
const int ESP_CORNER_SCALE_DEFAULT = 150;
struct EspCornerText { int scale; int slot; };
inline EspCornerText esp_corner_text(int cornerscale) {
  EspCornerText t = { clamp_int(cornerscale, ESP_CORNER_SCALE_MIN, ESP_CORNER_SCALE_MAX),
                      CANVAS_SLOT_CORNER };
  return t;
}

// One outlined line: stamped at the 8 offsets, then the fill. The corner stack and the ESP labels draw through this;
// r 0 is one plain canvas_text.
inline void esp_draw_outlined_text(int x, int y, const char* s, unsigned fill,
                                   unsigned outline, int font, int r) {
  int dx[8], dy[8];
  const int n = esp_outline_offsets(r, dx, dy);
  for (int i = 0; i < n; i++) canvas_text(x + dx[i], y + dy[i], s, outline, font);
  canvas_text(x, y, s, fill, font);
}
inline void esp_draw_corner_line(int x, int y, const char* s, unsigned fill, int font) {
  esp_draw_outlined_text(x, y, s, fill, C_CORNER_OUTLINE, font, ESP_CORNER_OUTLINE);
}

// Gate: Secrets OR Switches (World Tour's habit is Secrets, HRP's is Switches), and the level must have a secret
// exit. Pure so the suite can pin all four corners.
inline bool esp_secret_exit_note_wanted(int espsecrets, int espswitches, bool levelHas) {
  return (espsecrets != 0 || espswitches != 0) && levelHas;
}

// Takes the already-resolved flag rather than reading g_cfg itself; the test host does not link canvas_text, so the
// suite pins the geometry and the text instead of the draw.
inline void esp_draw_secret_exit_note(bool show) {
  if (!show) return;
  esp_draw_corner_line(esp_secret_exit_note_x(), esp_secret_exit_note_y(),
                       ESP_SECRET_EXIT_NOTE, canvas_esp_secret_colour(), FONT_UI);
}

const int FREECAM_POSE_X = ESP_SECRET_EXIT_NOTE_MARGIN;
const int FREECAM_POSE_BUFSIZE = 96;
inline void freecam_pose_line(char* buf, int x, int y, int z, int ang,
                              int horiz) {
  sprintf_s(buf, FREECAM_POSE_BUFSIZE, "cam x=%d y=%d z=%d ang=%d horiz=%d", x,
            y, z, ang, horiz);
}

// The number run of the health label. A state label replaces the number outright; this port has no state source yet
// (dying/choking, World Tour's actor_state_label).
const int ESP_HEALTH_NUM_BUFSIZE = 32;
inline bool esp_health_number(char* numBuf, int hp, int maxhp, const char* stateLabel) {
  if (!stateLabel && hp <= 0) return false;
  if (stateLabel)     sprintf_s(numBuf, ESP_HEALTH_NUM_BUFSIZE, "%s", stateLabel);
  else if (maxhp > 0) sprintf_s(numBuf, ESP_HEALTH_NUM_BUFSIZE, "%d/%d", hp, maxhp);
  else                sprintf_s(numBuf, ESP_HEALTH_NUM_BUFSIZE, "%d", hp);
  return true;
}

// The name run. AA's own nameplate quotes `L%d NAME ^23%d` (level, name, health); this panel drew that shape until
// the level moved to its own row.
const int ESP_ACTOR_LABEL_BUFSIZE = 48;
inline bool esp_actor_label(char* buf, int bufSize, const char* name) {
  if (!buf || bufSize <= 0) return false;
  buf[0] = 0;
  if (!name) return false;
  _snprintf_s(buf, bufSize, _TRUNCATE, "%s", name);
  return buf[0] != 0;
}

// The level row sits below the name on its own line: "Level %d" rather than "L%d", since an abbreviation that read
// well next to a name is a fragment alone.
const int ESP_LEVEL_LABEL_BUFSIZE = 24;
inline bool esp_level_label(char* buf, int bufSize, int level) {
  if (!buf || bufSize <= 0) return false;
  _snprintf_s(buf, bufSize, _TRUNCATE, "Level %d", level);
  return buf[0] != 0;
}

// Name+health line y: unchanged without a level line, one small-font line higher with one, so the level line takes
// the name's old place and nothing overlaps.
inline int esp_actor_name_label_y(int boxY, bool showLevel) {
  int y = esp_name_label_y(boxY);
  if (showLevel) y = clamp_ll_to_int((long long)y - ESP_LABEL_GAP);
  return y;
}

// Where the name line used to sit, directly above the box: Level now owns that position whether or not Name shows.
inline int esp_level_label_y(int boxY) { return esp_name_label_y(boxY); }

// The secret marker's cross keeps that category distinct from the enemy/item box, which now differ only by colour.
const int ESP_SECRET_ARM = 5;
const int ESP_SECRET_T   = 2;

const char* const ESP_SECRET_LABEL = "Secret";

inline int esp_secret_bar_span(int arm = ESP_SECRET_ARM) { return arm * 2 + 1; }

inline int esp_secret_hbar_x(int cx, int arm = ESP_SECRET_ARM) { return clamp_ll_to_int((long long)cx - arm); }
inline int esp_secret_hbar_y(int cy, int t = ESP_SECRET_T) { return clamp_ll_to_int((long long)cy - t / 2); }
inline int esp_secret_vbar_x(int cx, int t = ESP_SECRET_T) { return clamp_ll_to_int((long long)cx - t / 2); }
inline int esp_secret_vbar_y(int cy, int arm = ESP_SECRET_ARM) { return clamp_ll_to_int((long long)cy - arm); }

// Label above, distance below, at the same two gaps the enemy marker uses, expressed from the arm so labels follow
// the cross.
inline int esp_secret_name_label_y(int cy, int arm = ESP_SECRET_ARM) {
  return clamp_ll_to_int((long long)cy - arm - ESP_LABEL_GAP);
}
inline int esp_secret_dist_label_y(int cy, int arm = ESP_SECRET_ARM) {
  return clamp_ll_to_int((long long)cy + arm + 1 + ESP_DIST_GAP);
}

// The keycard mark: a card rather than a cross, so a lock reads as a lock at a glance (World Tour's constants).
const int ESP_CARD_W     = 14;
const int ESP_CARD_H     = 10;
const int ESP_CARD_R     = 2;
const int ESP_CARD_T     = 1;
const int ESP_CARD_NOTCH = 3;

inline int esp_card_x(int cx) { return clamp_ll_to_int((long long)cx - ESP_CARD_W / 2); }
inline int esp_card_y(int cy) { return clamp_ll_to_int((long long)cy - ESP_CARD_H / 2); }

inline int esp_card_notch_x(int cx) { return clamp_ll_to_int((long long)cx - ESP_CARD_NOTCH / 2); }
inline int esp_card_notch_y(int cy) { return clamp_ll_to_int((long long)cy - ESP_CARD_NOTCH / 2); }

inline int esp_card_name_label_y(int cy) {
  return clamp_ll_to_int((long long)cy - ESP_CARD_H / 2 - ESP_LABEL_GAP);
}
inline int esp_card_dist_label_y(int cy) {
  return clamp_ll_to_int((long long)cy + ESP_CARD_H / 2 + ESP_DIST_GAP);
}

// High visibility, one switch (g_cfg.hivis). The overlay-visibility audit measured 11 of 14 marker colours under 3:1
// on a white wall; a black keyline under every stroke lifts the worst edge to 4.4:1.

// Half the side of a shot tracer's end mark (tracer.cpp); here so the strokes can scale it and the suite can see it.
const int EKM_TRACER_MARK = 2;

// Strokes scale with screen height against World Tour's 1080p design: ceil(base * H / 1080), floored at base.
const int ESP_REF_SCREEN_H = 1080;
inline int esp_stroke(int base, int screenH, bool hivis) {
  if (!hivis || base <= 0 || screenH <= ESP_REF_SCREEN_H) return base;
  const long long s = ((long long)base * screenH + ESP_REF_SCREEN_H - 1) / ESP_REF_SCREEN_H;
  return s > base ? clamp_ll_to_int(s) : base;
}

struct EspStrokes {
  int bracketT, secretT, secretArm, cardT, aimT, aimHalf, barW, tracerMark, lineW;
  int keyline;
};
inline EspStrokes esp_strokes(int screenH, bool hivis) {
  EspStrokes st;
  st.bracketT   = esp_stroke(ESP_BRACKET_T, screenH, hivis);
  st.secretT    = esp_stroke(ESP_SECRET_T, screenH, hivis);
  st.secretArm  = esp_stroke(ESP_SECRET_ARM, screenH, hivis);
  st.cardT      = esp_stroke(ESP_CARD_T, screenH, hivis);
  st.aimT       = esp_stroke(AIM_MARKER_T, screenH, hivis);
  st.aimHalf    = esp_stroke(AIM_MARKER_HALF, screenH, hivis);
  st.barW       = esp_stroke(ESP_BAR_W, screenH, hivis);
  st.tracerMark = esp_stroke(EKM_TRACER_MARK, screenH, hivis);
  st.lineW      = esp_stroke(1, screenH, hivis);
  st.keyline    = hivis ? 1 : 0;
  return st;
}

struct EspRect { int x, y, w, h; };
inline EspRect esp_keyline_rect(int x, int y, int w, int h, int k) {
  EspRect r = { clamp_ll_to_int((long long)x - k), clamp_ll_to_int((long long)y - k),
                clamp_ll_to_int((long long)w + 2LL * k), clamp_ll_to_int((long long)h + 2LL * k) };
  return r;
}

// The keyline takes its fill's alpha, so a fading tracer or damage number fades its edge rather than leaving a black
// ghost.
inline unsigned esp_keyline_colour(unsigned fill) {
  return canvas_with_alpha(C_KEYLINE, (int)(fill >> 24));
}

// Every keyline first, then every fill, so one bar's black never covers another's colour. k 0 is the fills alone.
inline void esp_keyed_rects(const EspRect* r, int n, unsigned colour, int k) {
  if (!r) return;
  if (k > 0) {
    const unsigned kc = esp_keyline_colour(colour);
    for (int i = 0; i < n; i++) {
      const EspRect g = esp_keyline_rect(r[i].x, r[i].y, r[i].w, r[i].h, k);
      canvas_rect(g.x, g.y, g.w, g.h, kc);
    }
  }
  for (int i = 0; i < n; i++) canvas_rect(r[i].x, r[i].y, r[i].w, r[i].h, colour);
}

// ESP label text: outlined at the corner's radius with the switch on, r 0 (one plain draw) off.
inline int esp_label_outline_radius(bool hivis) { return hivis ? ESP_CORNER_OUTLINE : 0; }
inline void esp_draw_label(bool hivis, int x, int y, const char* s, unsigned fill, int font) {
  esp_draw_outlined_text(x, y, s, fill, esp_keyline_colour(fill), font,
                         esp_label_outline_radius(hivis));
}

// Always on: a keycard pickup draws the card its lock draws (as red brackets it matched an enemy in colour and
// shape).
enum { ESP_SHAPE_BRACKETS = 0, ESP_SHAPE_CARD = 1 };
inline int esp_item_shape(unsigned cardColour) {
  return cardColour ? ESP_SHAPE_CARD : ESP_SHAPE_BRACKETS;
}

// Always on: explodables and breakables draw an x, keeping their yellow (the same + as a secret left only the label
// to tell them apart); a chest is + and x together, a star, which cannot pass for a figurine, secret, cash or
// explodable.
enum { ESP_CROSS_PLUS = 0, ESP_CROSS_SALTIRE = 1, ESP_CROSS_STAR = 2 };
inline int esp_point_cross(int kind) {
  if (kind == EK_MARKER_CHEST) return ESP_CROSS_STAR;
  return (kind == EK_MARKER_CRACK || kind == EK_MARKER_BREAKABLE) ? ESP_CROSS_SALTIRE
                                                                  : ESP_CROSS_PLUS;
}

struct EspSaltire { int x0, y0, x1, y1, x2, y2, x3, y3; };
inline EspSaltire esp_saltire(int cx, int cy, int arm) {
  const int l = clamp_ll_to_int((long long)cx - arm), r = clamp_ll_to_int((long long)cx + arm);
  const int t = clamp_ll_to_int((long long)cy - arm), b = clamp_ll_to_int((long long)cy + arm);
  EspSaltire s = { l, t, r, b, l, b, r, t };
  return s;
}
