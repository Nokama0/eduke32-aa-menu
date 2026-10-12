// A copy of Megaton's src/duke3dmod/canvas.h; the palette above the ESP entries is identical to it, so a player of both
// ports reads the same colours. C_SETTINGS, C_AIM, C_PLAYER, C_TELEPORT_MARK, the tracers, the glass style and the
// combination volume are this port's own.
#pragma once
const unsigned C_PANEL   = 0xF014171A;
const unsigned C_RECESS  = 0xF01C2127;
const unsigned C_RAISED  = 0xFF333C46;
const unsigned C_BEVELHI = 0xFF48535F;
const unsigned C_BEVELLO = 0xFF0A0C0E;
const unsigned C_HAZARD  = 0xFFF2B01E;
const unsigned C_RED     = 0xFFD8402F;
const unsigned C_BLUE    = 0xFF3C9BD6;
const unsigned C_GREEN   = 0xFF6FBF5B;
const unsigned C_BONE    = 0xFFE8E4DA;
const unsigned C_BONEDIM = 0xFF9AA1A8;
const unsigned C_BONEFNT = 0xFF666E77;
const unsigned C_WELL      = 0xFF0E1114;
const unsigned C_HAZARDDIM = 0xFF8A6512;
const unsigned C_AIM       = 0xFFC061E8;  // aim marker: violet
const unsigned C_PLAYER    = 0xFF2FE0D0;  // free camera body: cyan
const unsigned C_SETTINGS  = 0xFFB8A9E0;  // Settings rail accent: lavender

// Friendlies take hue 325, the one unoccupied region. Not blue: every pickup's brackets are C_BLUE (esp_items.h:124).
const unsigned C_FRIENDLY  = 0xFFE85FA8;

// Figurines: hue 250, between C_BLUE and C_AIM. Not C_GREEN: the Cash row's cross, the same shape, uses it.
const unsigned C_FIGURINE = 0xFF735CE6;

// Top-left corner text: outlined in opaque black since some AA maps are mostly white walls. The counter is C_FIGURINE
// lightened to #B9AEFF (C_FIGURINE is ~14% luminance); the pose readout is plain grey.
const unsigned C_FIGURINE_TEXT  = 0xFFB9AEFF;
const unsigned C_CORNER_POSE    = 0xFFC8C8C8;
const unsigned C_CORNER_OUTLINE = 0xFF000000;
const unsigned C_KEYLINE = C_CORNER_OUTLINE;

// Combination tint, a VOLUME around the lock's model, pushed toward full saturation for dark walls; alpha 0x76 (0x40
// read too faint).
const unsigned C_TINT_BOX_FLIP  = 0x7660FF60;
const unsigned C_TINT_BOX_LEAVE = 0x76FF5050;

// Secret volume outline: C_HAZARD's hue at a lower alpha than the tint, since 2-3 room panels can overlap on one
// sightline.
const unsigned C_SECRET_VOLUME = 0x40F2B01E;

// Teleport landing marker: C_PLAYER's cyan at C_SECRET_VOLUME's alpha, so it reads as "you land here", not a secret
// room.
const unsigned C_TELEPORT_MARK = 0x402FE0D0;

// Tracers must differ from each other and every marker hue: predicted ray achromatic near white (exact), actual impact
// rose at about 340.
const unsigned C_TRACE_PRED = 0xFFF2F4FF;
const unsigned C_TRACE_REAL = 0xFFFF3E8C;

// Glass style (menuanim == 2 only): the only translucent-surface colours in this palette.
const unsigned C_GLASS_TINT = 0x99141C26;  // slab ground over the blurred backdrop; alpha = how much scene shows through
const unsigned C_GLASS_RIM  = 0xB0A8C8E0;  // one-pixel rim; brighter than C_BEVELHI as Glass has no bevel
const unsigned C_GLASS_TOP  = 0x40D8E8F8;  // top-edge sheen gradient
const unsigned C_GLASS_GLOW = 0x66A8D8FF;  // additive bloom (open sweep, switch flare, hover glow)

// True when canvas_hazard's stripe geometry is well formed: a non-positive period never advances the stripe loop
// (glBegin left open) and a non-positive w/h gives a degenerate scissor box.
inline bool canvas_hazard_params_ok(int w, int h, int period) {
  return w > 0 && h > 0 && period > 0;
}

// ESP marker colour by category: red enemy, blue pickup; colour alone separates them now both share a box shape.
inline unsigned canvas_esp_colour(bool enemy) { return enemy ? C_RED : C_BLUE; }

inline unsigned canvas_esp_secret_colour() { return C_HAZARD; }

inline unsigned canvas_notice_colour() { return C_HAZARD; }

inline unsigned canvas_aim_colour() { return C_AIM; }

inline unsigned canvas_player_colour() { return C_PLAYER; }

// Two functions, not one with a flag: the predicted line is exact, the actual one a best-effort pairing.
inline unsigned canvas_tracer_predicted_colour() { return C_TRACE_PRED; }
inline unsigned canvas_tracer_actual_colour() { return C_TRACE_REAL; }

inline unsigned canvas_esp_explodable_colour() { return C_HAZARD; }

inline unsigned canvas_esp_switch_colour() { return C_BLUE; }

// Cash container marker: green, above all not C_HAZARD: this row marks only the breakables that pay, while
// Explodables/Breakables draw the same cross in hazard yellow.
inline unsigned canvas_esp_cash_colour() { return C_GREEN; }

// Figurine marker: the counter's lavender (C_FIGURINE_TEXT).
inline unsigned canvas_esp_figurine_colour() { return C_FIGURINE_TEXT; }

// EDF chests and bomb parts: pale mint #B4FFA0; the star shape separates it from cash's +.
const unsigned C_CHEST = 0xFFB4FFA0;
inline unsigned canvas_esp_chest_colour() { return C_CHEST; }

inline unsigned canvas_health_colour(int hp, int maxhp) {
  if (maxhp <= 0 || hp <= 0) return C_RED;
  const long long pct = (long long)hp * 100 / maxhp;
  if (pct > 50) return C_GREEN;
  if (pct > 25) return C_HAZARD;
  return C_RED;
}

inline int canvas_bar_fill(int total, int hp, int maxhp) {
  if (total <= 0 || maxhp <= 0 || hp <= 0) return 0;
  if (hp >= maxhp) return total;
  return (int)((long long)total * hp / maxhp);
}

// Alpha for tap i of a blur summing to exactly aTint. Cumulative rounding: a flat aTint/taps share goes negative on the
// last tap (aTint=5, taps=9), wrapping to a wrong colour byte.
inline int canvas_tap_alpha(int aTint, int taps, int i) {
  if (taps <= 0 || i < 0 || i >= taps) return 0;
  if (aTint < 0) aTint = 0;
  if (aTint > 255) aTint = 255;
  const int upto = (int)((double)aTint * (double)(i + 1) / (double)taps + 0.5);
  const int prev = (int)((double)aTint * (double)i / (double)taps + 0.5);
  return upto - prev;
}

inline unsigned canvas_with_alpha(unsigned rgba, int alpha) {
  if (alpha < 0) alpha = 0;
  if (alpha > 255) alpha = 255;
  return (rgba & 0x00FFFFFFu) | ((unsigned)alpha << 24);
}

// Panel open motion moves every pixel drawn while the LOGICAL layout for hit testing stays put: d = ((p - c) * scale +
// c) + offset after the UI scale. Identity is exact and the default. Reset by canvas_begin.
struct CanvasTransform { int dx, dy, cx, cy; double scale; };
void canvas_set_transform(int dx, int dy, double scale, int cx, int cy);

#ifdef NO_GL
// Test-build-only recording of what was drawn.
const int CANVAS_RECORD_MAX = 512;
enum { CANVAS_OP_RECT = 0, CANVAS_OP_BEVEL, CANVAS_OP_HAZARD,
       CANVAS_OP_LINE, CANVAS_OP_TEXT,
       CANVAS_OP_ROUNDRECT, CANVAS_OP_GRADIENT, CANVAS_OP_GLOW,
       CANVAS_OP_CAPTURE, CANVAS_OP_BACKDROP,
       CANVAS_OP_ROUNDOUTLINE };
struct CanvasOp {
  int kind;
  int x, y, w, h;   // for TEXT, w is the rendered width and h is 0
  unsigned colour;  // for BEVEL, the FILL colour
  int font;
  char text[64];
};
void canvas_record_begin();
void canvas_record_end();
int  canvas_record_count();
bool canvas_record_overflowed(); // true if ops were dropped: never ignore it
const CanvasOp* canvas_record_op(int i);
const CanvasOp* canvas_record_find_text(const char* s);
const CanvasOp* canvas_record_find_kind(int kind, unsigned colour);
CanvasTransform canvas_record_transform();
#endif

// Coordinates below are LOGICAL; the canvas scales them on the way to the screen. menu_draw restores UI_SCALE_DEFAULT
// before returning, since the ESP draws earlier through the same primitives. canvas_text_width stays LOGICAL.
void canvas_set_scale(int pct);
int  canvas_scale();
void canvas_set_magnify_text(bool on);
bool canvas_magnify_text();
// Which scaled-text slot text samples: the ESP corner uses its own so its scale never evicts the panel's.
void canvas_set_text_slot(int slot);
int  canvas_text_slot();

void canvas_begin(int w, int h);
void canvas_end();
void canvas_rect(int x, int y, int w, int h, unsigned rgba);
void canvas_bevel(int x, int y, int w, int h, unsigned fill, unsigned hi, unsigned lo);
void canvas_line(int x0, int y0, int x1, int y1, unsigned rgba);
void canvas_line_w(int x0, int y0, int x1, int y1, unsigned rgba, int w);
int canvas_frame_height();
void canvas_hazard(int x, int y, int w, int h, unsigned stripe, unsigned ground, int period);
int canvas_text(int x, int y, const char* s, unsigned rgba, int font);
int canvas_text_width(const char* s, int font);

void canvas_round_rect(int x, int y, int w, int h, int r, unsigned rgba);
// Rounded rectangle outline, a ring `thickness` logical pixels wide. Its own primitive: a filled round rect under the
// slab only reads as an edge when opaque.
void canvas_round_rect_outline(int x, int y, int w, int h, int r, int thickness,
                               unsigned rgba);
// Clip rect until the matching pop, LOGICAL coordinates. Pushed without intersecting; pop in the same frame.
void canvas_push_clip(int x, int y, int w, int h);
void canvas_pop_clip();

void canvas_gradient_rect(int x, int y, int w, int h, unsigned top, unsigned bottom);
void canvas_glow_rect(int x, int y, int w, int h, unsigned rgba);
bool canvas_backdrop_capture(int x, int y, int w, int h);
void canvas_backdrop_draw(int x, int y, int w, int h, int r, unsigned tint,
                          double wobbleAmp, double wobblePhase, int blurTaps, int blurRadius);
void canvas_backdrop_release();

// Atlas texel a flat fill samples, for callers queueing their own triangles; selects the frame's atlas. False: draw
// nothing.
bool canvas_white_texel(float* u, float* v);
