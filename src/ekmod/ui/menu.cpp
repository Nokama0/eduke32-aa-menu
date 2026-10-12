// The Nuke Panel, ported from World Tour's menu.cpp: same panel, fewer rows (rows return with their features). config.h
// has a 0/1 `animations`, not menuanim's three values, and canvas.cpp's backdrop capture is a stub, so Off and Simple
// are the two styles. Interlocked: s_rowCommit has two writers.
#include <windows.h>
// ImGui's IO for ONE field, the mouse wheel the tab body scrolls on: menu_draw runs inside overlay.cpp's frame on the
// render thread that fills that IO, so the wheel needs no cross-clock scalar. Use, not an edit: the vendored copy stays
// stock.
#include "../imgui/imgui.h"
#include "menu.h"
#include "panel_layout.h"
#include "canvas.h"
#include "font.h"
#include "entry.h"
#include "anim.h"
#include "wallclock.h"
#include "tooltip.h"
#include "bind.h"
#include "input.h"
#include "../config.h"
#include "../log.h"
#include "../aim.h"
#include "../gamehooks.h"
#include "../aimsolve.h"
#include "../esp.h"
#include "../esp_layout.h"
#include "../cutaway.h"
#include "../input.h"
#include "../localinput.h"
#include "../weaponspeed.h"
#include "../coinmagnet.h"
#include "../pickuprange.h"
#include "../cvars.h"
#include "../autoswitch.h"
#include "../autosave.h"
#include "../camera.h"
#include "../shot.h"
#include "../freecam.h"
#include "../nocull.h"
#include "../fov.h"
#include "../gamecfg.h"
#include "../gamevars.h"
#include <stdio.h>
#include <string.h>

static int s_tab = TAB_VISUALS;

// Render thread only: every read and write is in menu_draw, the thread ImGui's IO fills, so no interlock is owed. Reset
// at menu_draw's open edge, not in menu_note_new_session (the window thread calls it too). Not in ekmod.ini: where
// somebody is looking is not a setting. s_contentH survives a close.
static int s_scroll[TAB_COUNT]   = { 0, 0, 0, 0, 0, 0 };
static int s_contentH[TAB_COUNT] = { 0, 0, 0, 0, 0, 0 };

// The frame's real logical cursor X, for the one read not behind a ui_hit: the caret drag in row_slider_at's open
// field, so a selection drag leaving the body still tracks the pointer. Written once in menu_draw before the cursor is
// parked.
static int s_frameMx = 0;

// One key capture for the whole panel (a second armed row would make the next press ambiguous); every transition is
// keycap_step's (bind.h). Forward-declared because menu_keycap_close() reaches it.
static void menu_note_new_session();

static KeyCapState s_keycap = { KEYCAP_IDLE, -1 };

bool menu_keycap_armed() { return s_keycap.phase == KEYCAP_ARMED; }

void menu_keycap_bind(int vk) {
  // A mouse button and a key are the same binding to config_bind_set, but not to keycap_step, which refuses mouse 1 on
  // the mouse arm (the click that armed the row).
  KeyCapEvent e;
  e.kind = bind_is_mouse(vk) ? KEYCAP_EV_MOUSE : KEYCAP_EV_KEY;
  e.row = -1;
  e.vk = vk;
  const KeyCapResult r = keycap_step(s_keycap, e);
  s_keycap = r.state;
  if (r.boundRow >= 0) config_bind_set(r.boundRow, r.boundVk);
}

void menu_keycap_cancel() {
  KeyCapEvent e;
  e.kind = KEYCAP_EV_ESCAPE;
  e.row = -1;
  e.vk = 0;
  s_keycap = keycap_step(s_keycap, e).state;
}

void menu_keycap_close() {
  KeyCapEvent e;
  e.kind = KEYCAP_EV_CLOSE;
  e.row = -1;
  e.vk = 0;
  s_keycap = keycap_step(s_keycap, e).state;
  menu_note_new_session();
}

// menu_gamecfg_load predates gamecfg.h and is a no-op stub: the real parser is gamecfg.cpp and row_bind calls
// gamecfg_load()/gamecfg_function_for() directly.
void menu_gamecfg_load(const char*) {}

// entry.h expects SDL_SCANCODE_A/C from input.h, which this port lacks; named here from SDL's table (matching
// ui/common.h's case 4/6) rather than editing the header. The Ctrl-chord comparison needs named constants, not
// vk_to_entry_scancode's value.
const int SDL_SCANCODE_A = 4;
const int SDL_SCANCODE_C = 6;

static EntryState s_entry = entry_begin();

// The left button went down in the field's well and is still down (caret put, then selection drag). Cleared on
// button-up and at menu_entry_begin_edit so a drag cannot carry into the next field.
static bool s_entryCaretDrag = false;

static UiRect s_entryFieldRect = { 0, 0, 0, 0 };

// Whether row_slider_at drew the open field this frame: a tab switch or a layout-dropped row never runs the per-row
// containment check, so this is the catch-all.
static bool s_entrySeenThisFrame = false;

static char s_entryClip[ENTRY_TEXT_CAP + 1] = { 0 };

bool menu_entry_open() { return s_entry.open != 0; }

const char* menu_entry_text() { return s_entry.text; }

void menu_entry_key(int sc) { menu_entry_key_mod(sc, false, false); }

void menu_entry_paste(const char* s) {
  if (!s_entry.open || !s) return;
  s_entry = entry_step(s_entry, ENTRY_EV_PASTE, 0, 0, EK_NONE, 0, 0, s).state;
}

const int GROUP_H = 22, GROUP_GAP = 6;
const int SUB_RULE_X = 3, SUB_INDENT = 21;


// The drag machine (idle until a press lands on the title band or grip); `s_dragBox` the last live box (no mouse
// position to re-derive from once the button is up); `s_dragLive` is read by row helpers so no row acts under a hand
// moving the panel.
static PanelDrag s_drag = { PDRAG_IDLE, 0, 0, { 0, 0, 0, 0 } };
static PanelBox  s_dragBox = { 0, 0, 0, 0 };
static bool      s_dragLive = false;

// A press already spent: the reset glyph overlaps the track's grab box and an open dropdown covers rows, so one press
// could do two things. A latch (the button stays down), cleared in menu_draw's preamble once up.
static bool      s_pressSpent = false;

// A left press no control took is a move (armed at the foot of menu_draw); this is how a control claims it. Not
// s_pressSpent, which spans the whole hold, so a dragging slider would disarm itself.
static bool      s_pressClaimed = false;

static PanelBox panel_stored_box() {
  PanelBox b = { g_cfg.panelx, g_cfg.panely, g_cfg.panelw, g_cfg.panelh };
  return b;
}

// The tallest a panel may be laid out or dragged to while ui_scale_max_for stays at or above the STORED scale (else a
// resize dropped the scale silently). Floored at PANEL_MIN_H; if the minimum does not fit, it is not forced smaller.
static int menu_panel_h_cap(int screenH) {
  const int stored = ui_scale_clamped(g_cfg.uiscale);
  const int cap = ui_panel_max_h_for(screenH, stored, PANEL_VINSET);
  return cap < PANEL_MIN_H ? PANEL_MIN_H : cap;
}

// The stored box with its height pre-clamped to menu_panel_h_cap, so a tall stored value draws at the cap instead of
// pulling the scale down. Drawing is not a write. Takes the DEVICE screen height, as menu_ceiling_height and
// ui_scale_max_for do (passing logical gave a wrong cap in both directions).
static PanelBox panel_stored_box_capped(int screenH) {
  PanelBox b = panel_stored_box();
  if (b.h > 0) {
    const int cap = menu_panel_h_cap(screenH);
    if (b.h > cap) b.h = cap;
  }
  return b;
}

// The scale the panel settles at on a screen this tall: the stored setting capped so the whole logical height fits. The
// ceiling follows the stored height (floored at PANEL_MIN_H, which panel_layout enforces) and menu_panel_h_cap, i.e.
// the height actually drawn, not the one still in the ini.
static int menu_ceiling_height(int screenH) {
  const int h = g_cfg.panelh > 0 ? g_cfg.panelh : PANEL_H;
  const int floored = h < PANEL_MIN_H ? PANEL_MIN_H : h;
  const int cap = menu_panel_h_cap(screenH);
  return floored < cap ? floored : cap;
}

int menu_applied_scale(int screenH) {
  const int stored = ui_scale_clamped(g_cfg.uiscale);
  const int ceiling = ui_scale_max_for(screenH, menu_ceiling_height(screenH), PANEL_VINSET);
  return stored < ceiling ? stored : ceiling;
}

// Takes the same device screenW/screenH menu_draw does (not menu.h's old logical contract, which would reintroduce the
// D21 bug) and converts via menu_applied_scale/ui_unscale_px, the one place that does.
PanelBox menu_panel_box(int screenW, int screenH) {
  const int pct = menu_applied_scale(screenH);
  const int logicalW = ui_unscale_px(screenW, pct);
  const int logicalH = ui_unscale_px(screenH, pct);
  const PanelBox laid = panel_layout(panel_stored_box_capped(screenH), logicalW, logicalH);
  return s_dragLive ? s_dragBox : laid;
}

UiRect menu_panel_rect(int screenW, int screenH) {
  const PanelBox b = menu_panel_box(screenW, screenH);
  return UiRect{ b.x, b.y, b.w, b.h };
}

static int s_scaleHeld = UI_SCALE_DEFAULT;

int menu_frame_scale(int screenH) {
  return ui_scale_effective(menu_applied_scale(screenH), s_scaleHeld, g_ui.down);
}

int  menu_active_tab() { return s_tab; }
void menu_set_tab(int tab) { if (tab >= 0 && tab < TAB_COUNT) s_tab = tab; }

static const char* TAB_NAMES[TAB_COUNT] = { "Visuals", "Combat", "Weapons", "Items", "View", "Settings" };
static const unsigned TAB_COLOURS[TAB_COUNT] = { C_BLUE, C_RED, C_HAZARD, C_BLUE, C_GREEN, C_SETTINGS };

static int anim_style() { return g_cfg.animations ? ANIM_SIMPLE : ANIM_OFF; }

// A cosmetic layer beside the logical layout: hit testing never reads it (ui_hit compares against the logical UiRect),
// so a click during a transition lands where the control is going. `knob`/`fill`/`hover` are indexed by row ordinal, so
// two tabs' Nth rows share a slot; a tab switch snaps every spring to its logical value on the next frame.
struct MenuPresentation {
  unsigned lastMs;
  AnimSpring open;
  int openTarget;
  int wasOpen;
  unsigned tabSwitchMs;
  int tabFrom;
  AnimSpring knob[64];
  AnimSpring fill[64];
  double hover[64];
  double resetHover[64];
  int rowsThisFrame;
};
static MenuPresentation s_pres;

static const int ANIM_ROW_SLOTS = 64;
static bool s_tabSwitchedThisFrame = false;
static double s_frameDt = 0.0;

// Hints as tooltips (rule: tooltip.h). `s_lastRow`/`s_lastRowKey` are the row just drawn; row_hint attaches a hint by
// adjacency in the draw, not a table that could drift. Key is `s_tab * 1000 + ordinal` (ordinal is stable only within a
// tab); -1 = no row.
static UiRect s_lastRow = { 0, 0, 0, 0 };
static int s_lastRowKey = -1;
static int s_hoverRowKey = -1;
// The tab body's visible rect, so row_dropdown/row_slider_at can tell whether their open list or field is still inside
// the scrolled body.
static UiRect s_bodyRect = { 0, 0, 0, 0 };
// A row that ran out of height draws nothing, and the panel has no scrollbar, so it is invisible otherwise: set by each
// guard, reported once a session at the foot of menu_draw (PANEL_MIN_H 740 once lost the last two Settings rows).
static int s_rowsDropped = 0;
static bool s_rowsDroppedLogged = false;
static char s_hintBuf[96];
static const char* s_hoverHint = nullptr;
static const char* s_tooltipText = nullptr;
static TooltipState s_tooltip = tooltip_begin();
static const int TOOLTIP_R = 4;
static const char* s_glyphHint = nullptr;

// What a session must not inherit: a drag left armed by a panel closed mid-move, a spent press, a tooltip on the row
// the cursor closed over. input.cpp calls this via menu_keycap_close() on close; menu_draw also takes the edge from the
// open state, which keeps it right under ANIM_OFF.
static void menu_note_new_session() {
  s_drag.phase = PDRAG_IDLE;
  s_dragLive = false;
  s_pressSpent = false;
  s_tooltip = tooltip_begin();
  s_tooltipText = nullptr;
  s_hoverHint = nullptr;
  s_glyphHint = nullptr;
  s_hoverRowKey = -1;
  s_lastRowKey = -1;
  // The scroll offsets are NOT reset here: this is reached from the WINDOW thread too (input.cpp's panel_set), which
  // would put s_scroll[] on both clocks. menu_draw resets them at the open edge, on the render thread alone.
}

const char* menu_tooltip_text() { return s_tooltipText; }

static void row_hint(const char* text) {
  if (s_lastRowKey < 0 || !text) return;
  if (!ui_hit(s_lastRow, g_ui.mx, g_ui.my)) return;
  s_hoverRowKey = s_lastRowKey;
  strncpy_s(s_hintBuf, sizeof(s_hintBuf), text, _TRUNCATE);
  s_hoverHint = s_hintBuf;
}

static unsigned s_nowOverride = 0;
void menu_set_now_ms(unsigned ms) { s_nowOverride = ms; }
static unsigned menu_now() { return s_nowOverride ? s_nowOverride : wallclock_ms(); }

bool menu_closing_animation_active() {
  if (anim_style() == ANIM_OFF || input_panel_open()) return false;
  if (menu_now() - s_pres.lastMs > ANIM_STALE_MS) { s_pres.open.x = 0.0; s_pres.open.v = 0.0; }
  return !anim_spring_settled(s_pres.open, 0.0, 0.01);
}

static unsigned pres_colour(unsigned c) {
  if (anim_style() == ANIM_OFF) return c;
  const int srcA = (int)((c >> 24) & 0xFFu);
  const int a = (int)(srcA * s_pres.open.x + 0.5);
  return canvas_with_alpha(c, a);
}

static unsigned alpha_scale(unsigned c, double a) {
  if (a < 0.0) a = 0.0;
  if (a > 1.0) a = 1.0;
  const int srcA = (int)((c >> 24) & 0xFFu);
  return canvas_with_alpha(c, (int)(srcA * a + 0.5));
}

static unsigned pres_content_colour(unsigned c) {
  const unsigned withOpen = pres_colour(c);
  if (anim_style() == ANIM_OFF) return withOpen;
  const double tabP = anim_ease_out(anim_progress(s_pres.tabSwitchMs, s_pres.lastMs, ANIM_TAB_MS));
  const int srcA = (int)((withOpen >> 24) & 0xFFu);
  const int a = (int)(srcA * tabP + 0.5);
  return canvas_with_alpha(withOpen, a);
}

static unsigned pres_hover_lighten(unsigned c, double hoverFrac) {
  if (anim_style() == ANIM_OFF) return c;
  return anim_blend_colour(c, C_BONE, hoverFrac);
}

// A target of the caller's choosing: lightening a hovered field's well, rim and text all toward C_BONE loses the text.
static unsigned pres_hover_toward(unsigned c, unsigned target, double hoverFrac) {
  if (anim_style() == ANIM_OFF) return c;
  return anim_blend_colour(c, target, hoverFrac);
}

static const int ROW_R = 4;
static const int SIMPLE_SLIDE_PX = 8;

// No row acts while the panel is dragged: a move or resize clamps (panel_layout), so the cursor can sit over a content
// row with the button held, which a slider reads as a drag. A filter on the button fields, as a row must still draw and
// hover-test.
static bool row_pressed() { return !s_dragLive && !s_pressSpent && g_ui.pressed; }
static bool row_down()    { return !s_dragLive && !s_pressSpent && g_ui.down; }

// Where a value lands from outside its row: one slot; the reset glyph, dropdown list and typed field write here, the
// owning row takes it next draw. Written from BOTH threads (render, and window via input.cpp's entry_route_key), so ONE
// LONG64 word, rowKey high / value low, -1 empty (row_commit_pack); a claim CASes the exact word it read.
static LONG64 s_rowCommit = row_commit_pack(-1, 0);

static void row_commit_publish(int rowKey, int value) {
  InterlockedExchange64(&s_rowCommit, row_commit_pack(rowKey, value));
}

// Claims the pending commit for `key` ONLY: several rows ask every frame. Reads once (`seen`), so the answer is
// self-consistent even when the claim fails.
static bool row_commit_take(int key, int* value) {
  const LONG64 seen = InterlockedCompareExchange64(&s_rowCommit, 0, 0);
  if (row_commit_key(seen) != key) return false;
  // Claims EXACTLY `seen`: if the word moved (a newer commit, either thread) the CAS fails and nothing is taken.
  if (InterlockedCompareExchange64(&s_rowCommit, row_commit_pack(-1, 0), seen) != seen) return false;
  if (value) *value = row_commit_value(seen);
  return true;
}

static void row_commit_drop() {
  InterlockedExchange64(&s_rowCommit, row_commit_pack(-1, 0));
}

bool menu_entry_pending(int* rowKey, int* value) {
  const LONG64 seen = InterlockedCompareExchange64(&s_rowCommit, 0, 0);
  const int key = row_commit_key(seen);
  if (key == -1) return false;
  if (rowKey) *rowKey = key;
  if (value)  *value  = row_commit_value(seen);
  return true;
}

void menu_entry_close() {
  // The field goes with the panel (else it eats keys with nothing on screen); reachable from the window thread.
  // `s_entry = entry_step(...)` on both threads is not a torn read; the hazard is a LOST UPDATE: the render thread's
  // caret-drag write can overwrite a window-thread Enter commit and resurrect the field. Narrow and accepted.
  s_entry = entry_begin();
  row_commit_drop();
}

// The one place a click away becomes a commit (a left press outside the field, a right press opening a field on another
// row); Escape is the only cancel.
static void entry_clickaway_commit() {
  if (!s_entry.open) return;
  const int row = s_entry.rowKey;
  const EntryResult r = entry_step(s_entry, ENTRY_EV_CLICKAWAY, 0, 0, EK_NONE, 0, 0, 0);
  s_entry = r.state;
  if (r.commit) row_commit_publish(row, r.value);
}

void menu_entry_begin_edit(int rowKey, int kind, int lo, int hi, const char* text) {
  menu_keycap_cancel();
  s_entryCaretDrag = false;
  s_entry = entry_step(s_entry, ENTRY_EV_OPEN, 0, rowKey, kind, lo, hi, text).state;
}

const char* menu_entry_key_mod(int sc, bool shift, bool ctrl) {
  if (!s_entry.open) return nullptr;
  const int row = s_entry.rowKey;

  int ev = ENTRY_EV_CHAR;
  int ch = 0;
  if (ctrl && sc == SDL_SCANCODE_A)      ev = ENTRY_EV_SELECT_ALL;
  else if (ctrl && sc == SDL_SCANCODE_C) ev = ENTRY_EV_COPY;
  else if (ctrl && sc == SDL_SCANCODE_X) ev = ENTRY_EV_CUT;
  else if (ctrl && sc == SDL_SCANCODE_V) return nullptr;
  else if (sc == SDL_SCANCODE_ESCAPE)    ev = ENTRY_EV_ESCAPE;
  else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER)
                                         ev = ENTRY_EV_ENTER;
  else if (sc == SDL_SCANCODE_BACKSPACE) ev = ENTRY_EV_BACKSPACE;
  else if (sc == SDL_SCANCODE_DELETE)    ev = ENTRY_EV_DELETE;
  else if (sc == SDL_SCANCODE_LEFT)  { ev = ENTRY_EV_LEFT;  ch = shift ? 1 : 0; }
  else if (sc == SDL_SCANCODE_RIGHT) { ev = ENTRY_EV_RIGHT; ch = shift ? 1 : 0; }
  else if (sc == SDL_SCANCODE_HOME)  { ev = ENTRY_EV_HOME;  ch = shift ? 1 : 0; }
  else if (sc == SDL_SCANCODE_END)   { ev = ENTRY_EV_END;   ch = shift ? 1 : 0; }
  else {
    ch = entry_char_of_scancode(sc);
    if (!ch) return nullptr;
  }

  const EntryResult r = entry_step(s_entry, ev, ch, 0, EK_NONE, 0, 0, 0);
  s_entry = r.state;
  if (r.commit) row_commit_publish(row, r.value);
  if (!r.hasClip) return nullptr;
  int n = 0;
  while (n < ENTRY_TEXT_CAP && r.clip[n]) { s_entryClip[n] = r.clip[n]; n++; }
  s_entryClip[n] = 0;
  return s_entryClip;
}

// The panel's whole vocabulary, carried complete; several widgets have no caller yet (dropdown, named and sub sliders),
// and a widget added back approximately later is a different panel.

// Shared body of both switch rows. With no room it draws nothing (ui_row has no floor). A disabled row is C_BONEFNT and
// inert; one that is ON keeps its knob right with a neutral C_RAISED fill: "on, but doing nothing" must differ from "on
// and live" and "off".
static int row_switch_at(UiRect& cursor, const char* label, int value, unsigned accent,
                         bool enabled, int indent, unsigned labelOn) {
  if (cursor.h < ROW_H) { s_lastRowKey = -1; s_rowsDropped = 1; return value; }
  const UiRect r = ui_row(cursor, ROW_H, ROW_GAP);
  const UiRect sw = { r.x + r.w - 42, r.y + 5, 42, 20 };
  const int idx = s_pres.rowsThisFrame++ % ANIM_ROW_SLOTS;
  s_lastRow = r; s_lastRowKey = s_tab * 1000 + idx;

  const bool hovered = enabled && ui_hit(sw, g_ui.mx, g_ui.my);
  if (anim_style() == ANIM_OFF) {
    s_pres.hover[idx] = hovered ? 1.0 : 0.0;
  } else {
    const double rate = hovered ? (1.0 / ANIM_HOVER_IN_MS) : (-1.0 / ANIM_HOVER_OUT_MS);
    double hv = s_pres.hover[idx] + rate * s_frameDt;
    if (hv < 0.0) hv = 0.0; else if (hv > 1.0) hv = 1.0;
    s_pres.hover[idx] = hv;
  }

  // The knob's drawn fraction springs toward the logical value, reset on the first row after a tab switch so nothing
  // slides in from another tab's row at this ordinal. ANIM_OFF computes `knobX` from `value` directly.
  int knobX;
  if (anim_style() == ANIM_OFF) {
    knobX = value ? sw.x + 23 : sw.x + 2;
  } else {
    const double targetX = value ? 1.0 : 0.0;
    if (s_tabSwitchedThisFrame) { s_pres.knob[idx].x = targetX; s_pres.knob[idx].v = 0.0; }
    else anim_spring_step(&s_pres.knob[idx], targetX, ANIM_SPRING_CRITICAL.stiffness,
                          ANIM_SPRING_CRITICAL.damping, s_frameDt);
    knobX = sw.x + 2 + (int)(21.0 * s_pres.knob[idx].x + 0.5);
  }

  if (indent > 0) canvas_rect(r.x + SUB_RULE_X, r.y, 1, r.h, pres_content_colour(C_RAISED));
  canvas_text(r.x + indent, r.y + 6, label,
              pres_content_colour(pres_hover_lighten(enabled ? labelOn : C_BONEFNT, s_pres.hover[idx])), FONT_UI);
  canvas_bevel(sw.x, sw.y, sw.w, sw.h,
               pres_content_colour(value ? (enabled ? accent : C_RAISED) : C_WELL),
               pres_content_colour(C_BEVELLO), pres_content_colour(C_RAISED));
  canvas_rect(knobX, sw.y + 2, 17, 16,
              pres_content_colour((anim_style() == ANIM_OFF ? value != 0 : s_pres.knob[idx].x >= 0.5)
                                   ? C_BONE : C_RAISED));
  if (ui_row_accepts_click(enabled, row_pressed(), ui_hit(sw, g_ui.mx, g_ui.my))) {
    s_pressClaimed = true;
    g_cfgDirty = 1;
    return !value;
  }
  return value;
}

static int row_switch(UiRect& cursor, const char* label, int value, unsigned accent, bool enabled) {
  return row_switch_at(cursor, label, value, accent, enabled, 0, C_BONE);
}

static int row_switch_sub(UiRect& cursor, const char* label, int value, unsigned accent,
                          bool enabled) {
  return row_switch_at(cursor, label, value, accent, enabled, SUB_INDENT, C_BONEDIM);
}

const int FIELD_PAD        = 8;
const int TRACK_GAP        = 8;
const int TRACK_X_OFFSET   = 172;
const int TRACK_MAX_W      = 120;
const int TRACK_MIN_W      = 40;

// 24 logical pixels are reserved between every numeric track and its readout on every frame: shortening only while
// hovered would move the knob from under the hand, and drawing over the track's end would stack a click target on a
// drag target.
const int RESET_W          = 20;
const int RESET_RESERVE    = 24;

// Measured, never multiplied: canvas_text accumulates the DEVICE advance while canvas_text_width unscales a sum once,
// so multiplying one advance per character drifts from the glyphs where that round trip is not the identity (~40% of a
// glyph over 11 characters at uiscale 132).
static int entry_prefix_w(const EntryState& s, int i) {
  if (i < 0) i = 0;
  if (i > s.len) i = s.len;
  char buf[ENTRY_TEXT_CAP + 1];
  int n = 0;
  while (n < i && n < ENTRY_TEXT_CAP && s.text[n]) { buf[n] = s.text[n]; n++; }
  buf[n] = '\0';
  return canvas_text_width(buf, FONT_MONO);
}
// The same measurement inverted for the mouse: nearest boundary, ties up; matches entry_index_at where the advance is
// uniform.
static int entry_index_measured(const EntryState& s, int textX, int px) {
  int i = 0, wPrev = 0;
  while (i < s.len) {
    const int wNext = entry_prefix_w(s, i + 1);
    if (px - textX < (wPrev + wNext + 1) / 2) break;
    wPrev = wNext;
    i++;
  }
  return i;
}

// Shared body of both slider rows. `reserveText` is the widest readout and sizes the track, never the live text (hit
// geometry must not depend on the value being dragged). `kind` (entry.h EK_*): EK_NONE for a name readout, which takes
// no right-click and no reset glyph. `def` is the glyph's only reader.
static int row_slider_at(UiRect& cursor, const char* label, int value, int lo, int hi,
                         const char* valueText, const char* reserveText,
                         bool enabled, int indent, unsigned labelOn,
                         int kind, int def) {
  if (cursor.h < ROW_H) { s_lastRowKey = -1; s_rowsDropped = 1; return value; }
  const UiRect r = ui_row(cursor, ROW_H, ROW_GAP);
  const int idx = s_pres.rowsThisFrame++ % ANIM_ROW_SLOTS;
  s_lastRow = r; s_lastRowKey = s_tab * 1000 + idx;
  const int key = s_lastRowKey;
  const int valueW = canvas_text_width(valueText, FONT_MONO);
  const int valueX = r.x + r.w - VALUE_MARGIN - valueW;
  const int reserveX = r.x + r.w - VALUE_MARGIN - canvas_text_width(reserveText, FONT_MONO);
  // The track's left edge is the fixed TRACK_X_OFFSET so tracks line up; a wider reserve slides it left so TRACK_MIN_W
  // stays a floor. A further RESET_RESERVE on rows with a number, for the glyph.
  const int resetRoom   = (kind != EK_NONE) ? RESET_RESERVE : 0;
  const int trackRight  = reserveX - TRACK_GAP - resetRoom;
  const int trackXFixed = r.x + r.w - TRACK_X_OFFSET;
  const int trackXFit   = trackRight - TRACK_MIN_W;
  const int trackX      = trackXFixed < trackXFit ? trackXFixed : trackXFit;
  const UiRect track = { trackX, r.y + 11,
                         clamp_int(trackRight - trackX, TRACK_MIN_W, TRACK_MAX_W),
                         8 };
  const UiRect grab = { track.x - 6, track.y - 8, track.w + 12, track.h + 16 };
  // The glyph is centred in its reserved room at a switch box's y. It draws only when enabled, with a number, cursor on
  // the row and no field open there (a reset button under the typing hand would be a trap).
  const UiRect glyph = { trackRight + (RESET_RESERVE - RESET_W) / 2, r.y + 5, RESET_W, RESET_W };
  const bool fieldHere = s_entry.open != 0 && s_entry.rowKey == key;
  const bool glyphLive = enabled && kind != EK_NONE && !fieldHere
                         && !s_dragLive && ui_hit(r, g_ui.mx, g_ui.my);
  const bool glyphHot  = glyphLive && ui_hit(glyph, g_ui.mx, g_ui.my);

  if (glyphHot && row_pressed()) {
    row_commit_publish(key, def);
    s_pressSpent = true;
    s_pressClaimed = true;
  }

  // A committed value lands on its row clamped to the bounds it holds THIS frame (the UI scale ceiling moves with the
  // panel's height). `committed` gates the dirty flag: no change is not a write.
  bool committed = false;
  int landedRaw = 0;
  if (row_commit_take(key, &landedRaw)) {
    const int landed = clamp_int(landedRaw, lo, hi);
    committed = landed != value;
    value = landed;
  }

  if (kind != EK_NONE && enabled && !s_dragLive && g_ui.rpressed
      && ui_hit(r, g_ui.mx, g_ui.my)) {
    char prefill[ENTRY_TEXT_CAP + 1];
    entry_format(prefill, sizeof prefill, value, kind);
    entry_clickaway_commit();
    menu_entry_begin_edit(key, kind, lo, hi, prefill);
  }

  const bool fieldOpen = s_entry.open != 0 && s_entry.rowKey == key;

  if (glyphHot) { s_hoverRowKey = key; s_glyphHint = MENU_RESET_HINT; }

  const int kx = ui_slider_x(track, value, lo, hi);

  const bool hovered = enabled && ui_hit(grab, g_ui.mx, g_ui.my);
  if (anim_style() == ANIM_OFF) {
    s_pres.hover[idx] = hovered ? 1.0 : 0.0;
  } else {
    const double rate = hovered ? (1.0 / ANIM_HOVER_IN_MS) : (-1.0 / ANIM_HOVER_OUT_MS);
    double hv = s_pres.hover[idx] + rate * s_frameDt;
    if (hv < 0.0) hv = 0.0; else if (hv > 1.0) hv = 1.0;
    s_pres.hover[idx] = hv;
  }
  if (anim_style() == ANIM_OFF) {
    s_pres.resetHover[idx] = glyphHot ? 1.0 : 0.0;
  } else {
    const double rate = glyphHot ? (1.0 / ANIM_HOVER_IN_MS) : (-1.0 / ANIM_HOVER_OUT_MS);
    double rv = s_pres.resetHover[idx] + rate * s_frameDt;
    if (rv < 0.0) rv = 0.0; else if (rv > 1.0) rv = 1.0;
    s_pres.resetHover[idx] = rv;
  }

  int drawnKx;
  if (anim_style() == ANIM_OFF) {
    drawnKx = kx;
  } else {
    const double targetFrac = track.w > 0 ? (double)(kx - track.x) / (double)track.w : 0.0;
    if (s_tabSwitchedThisFrame) { s_pres.fill[idx].x = targetFrac; s_pres.fill[idx].v = 0.0; }
    else anim_spring_step(&s_pres.fill[idx], targetFrac, ANIM_SPRING_CRITICAL.stiffness,
                          ANIM_SPRING_CRITICAL.damping, s_frameDt);
    drawnKx = track.x + (int)(s_pres.fill[idx].x * track.w + 0.5);
    if (drawnKx < track.x) drawnKx = track.x;
    if (drawnKx > track.x + track.w) drawnKx = track.x + track.w;
  }

  if (indent > 0) canvas_rect(r.x + SUB_RULE_X, r.y, 1, r.h, pres_content_colour(C_RAISED));
  canvas_text(r.x + indent, r.y + 6, label,
              pres_content_colour(pres_hover_lighten(enabled ? labelOn : C_BONEFNT, s_pres.hover[idx])), FONT_UI);
  canvas_bevel(track.x, track.y, track.w, track.h, pres_content_colour(C_WELL),
               pres_content_colour(C_BEVELLO), pres_content_colour(C_RAISED));
  canvas_rect(track.x, track.y, drawnKx - track.x, track.h, pres_content_colour(C_HAZARDDIM));
  canvas_rect(drawnKx - 4, track.y - 4, 9, 16, pres_content_colour(enabled ? C_HAZARD : C_BONEFNT));
  if (fieldOpen) {
    // The typed field: rounded well, 1px rim in C_HAZARD ("acting right now"), sized for the widest text it can hold
    // ("-2147483648"), never what it holds: a well that grew while typed into would move under the hand.
    const int fieldW = canvas_text_width("-2147483648", FONT_MONO) + 2 * FIELD_PAD;
    const UiRect field = { r.x + r.w - fieldW, r.y + 5, fieldW, 20 };
    s_entryFieldRect = field;
    s_entrySeenThisFrame = true;
    if (!ui_rect_inside(field, s_bodyRect)) entry_clickaway_commit();
    canvas_round_rect(field.x, field.y, field.w, field.h, ROW_R,
                      pres_content_colour(C_WELL));
    canvas_round_rect_outline(field.x, field.y, field.w, field.h, ROW_R, 1,
                              pres_content_colour(C_HAZARD));
    const Glyph& capA  = font_glyphs[FONT_MONO]['A' - 0x20];
    const Glyph& descY = font_glyphs[FONT_MONO]['y' - 0x20];
    const int inkTop = capA.by;
    const int inkH   = (descY.by + descY.h) - inkTop;
    const int textY  = field.y + (field.h - inkH) / 2 - inkTop;
    const int textX  = field.x + FIELD_PAD;
    // Every x inside the well comes from entry_prefix_w. A press puts the caret; holding drags the selection's far end
    // from the press as anchor. Not through row_pressed()/row_down(): s_pressSpent, set by the press in this well,
    // filters those, so the field's drag uses the raw button and its own latch.
    if (g_ui.pressed && ui_hit(field, g_ui.mx, g_ui.my)) {
      s_entry = entry_step(s_entry, ENTRY_EV_SET_CARET,
                           entry_index_measured(s_entry, textX, g_ui.mx),
                           0, EK_NONE, 0, 0, 0).state;
      s_entryCaretDrag = true;
    } else if (s_entryCaretDrag && g_ui.down) {
      // s_frameMx, not g_ui.mx: the one mouse read not behind a ui_hit would see the viewport park applied while the
      // cursor is outside the body, and a parked x measures as index 0 (the caret snaps to the first character).
      // entry_index_measured clamps to [0, len].
      s_entry = entry_step(s_entry, ENTRY_EV_SET_CARET,
                           entry_index_measured(s_entry, textX, s_frameMx),
                           1, EK_NONE, 0, 0, 0).state;
    }
    const int selA = entry_sel_lo(s_entry), selB = entry_sel_hi(s_entry);
    if (selB > selA) {
      const int selAx = entry_prefix_w(s_entry, selA);
      const int selBx = entry_prefix_w(s_entry, selB);
      canvas_rect(textX + selAx, field.y + 4, selBx - selAx,
                  field.h - 8, pres_content_colour(C_HAZARDDIM));
    }
    canvas_text(textX, textY, s_entry.text, pres_content_colour(C_BONE), FONT_MONO);
    const bool caretOn = (anim_style() == ANIM_OFF) || ((menu_now() / 500u) % 2u == 0u);
    if (caretOn)
      canvas_rect(textX + entry_prefix_w(s_entry, s_entry.caret) + 1, field.y + 4,
                  1, field.h - 8, pres_content_colour(C_BONE));
  } else {
  canvas_text(valueX, r.y + 7, valueText, pres_content_colour(C_BONE), FONT_MONO);

  // The reset button: a 20x20 well like a hotkey field's, hovered by its own fraction. The icon is a circular arrow:
  // eight points joined by seven segments at radii 6 and 5 (2px stroke; canvas_line is the only curve), ring open at no
  // RING6[7]-RING6[0] segment, arrowhead a filled 3x3 at RING6[0] plus a short stroke.
  if (glyphLive) {
    const double rf = s_pres.resetHover[idx];
    canvas_round_rect(glyph.x, glyph.y, glyph.w, glyph.h, ROW_R,
                      pres_content_colour(pres_hover_toward(C_WELL, C_RAISED, rf)));
    canvas_round_rect_outline(glyph.x, glyph.y, glyph.w, glyph.h, ROW_R, 1,
                              pres_content_colour(pres_hover_toward(C_RAISED, C_BONEDIM, rf)));
    static const int RING6[8][2] = {
      { 16, 10 }, { 15, 6 }, { 11, 4 }, { 7, 5 }, { 4, 8 },
      { 4, 12 }, { 7, 15 }, { 11, 16 }
    };
    static const int RING5[8][2] = {
      { 15, 10 }, { 14, 7 }, { 11, 5 }, { 7, 6 }, { 5, 8 },
      { 5, 12 }, { 7, 14 }, { 11, 15 }
    };
    static const int BARB[2] = { 13, 13 };
    const int ix = glyph.x - 1, iy = glyph.y;
    const unsigned gc = pres_content_colour(glyphHot ? C_BONE : C_BONEDIM);
    for (int i = 0; i < 7; i++) {
      canvas_line(ix + RING6[i][0], iy + RING6[i][1],
                  ix + RING6[i + 1][0], iy + RING6[i + 1][1], gc);
      canvas_line(ix + RING5[i][0], iy + RING5[i][1],
                  ix + RING5[i + 1][0], iy + RING5[i + 1][1], gc);
    }
    canvas_rect(ix + RING6[0][0] - 1, iy + RING6[0][1] - 1, 3, 3, gc);
    canvas_line(ix + RING6[0][0], iy + RING6[0][1], ix + BARB[0], iy + BARB[1], gc);
  }
  }

  if (ui_row_accepts_click(enabled, row_down(), ui_hit(grab, g_ui.mx, g_ui.my))) {
    // Claimed every frame of the drag: a slider reads the held button, the move rule only the press edge. Not
    // s_pressSpent, which row_down() reads and would stop its own drag.
    s_pressClaimed = true;
    const int v = ui_slider_value(track, g_ui.mx, lo, hi);
    if (v != value) { g_cfgDirty = 1; return v; }
  }
  if (committed) g_cfgDirty = 1;
  return value;
}

static int row_slider(UiRect& cursor, const char* label, int value, int lo, int hi,
                      const char* suffix, bool enabled, int def) {
  char buf[32], reserve[32];
  sprintf_s(buf, "%d%s", value, suffix);
  sprintf_s(reserve, "%d%s", hi, suffix);
  return row_slider_at(cursor, label, value, lo, hi, buf, reserve, enabled, 0, C_BONE,
                       EK_INT, def);
}

static int row_slider_sub(UiRect& cursor, const char* label, int value, int lo, int hi,
                          const char* suffix, bool enabled, int def) {
  char buf[32], reserve[32];
  sprintf_s(buf, "%d%s", value, suffix);
  sprintf_s(reserve, "%d%s", hi, suffix);
  return row_slider_at(cursor, label, value, lo, hi, buf, reserve, enabled,
                       SUB_INDENT, C_BONEDIM, EK_INT, def);
}

// Readout in METRES for the Aim range row (row_slider's literal suffix cannot divide). A sibling caller, not a wider
// row_slider_at contract. Reserve is `hi` through the same formatting, since digit count only grows with distance.
static int row_slider_metres(UiRect& cursor, const char* label, int value, int lo, int hi,
                             bool enabled, int def) {
  char buf[32], reserve[32];
  sprintf_s(buf, "%d m", ekm_metres(value));
  sprintf_s(reserve, "%d m", ekm_metres(hi));
  return row_slider_at(cursor, label, value, lo, hi, buf, reserve, enabled, 0, C_BONE,
                       EK_METRES, def);
}

static int row_slider_named(UiRect& cursor, const char* label, int value, int lo, int hi,
                            const char* (*labelFn)(int), bool enabled) {
  const char* reserve = labelFn(lo);
  int reserveW = canvas_text_width(reserve, FONT_MONO);
  for (int v = lo + 1; v <= hi; v++) {
    const char* s = labelFn(v);
    const int w = canvas_text_width(s, FONT_MONO);
    if (w > reserveW) { reserve = s; reserveW = w; }
  }
  return row_slider_at(cursor, label, value, lo, hi, labelFn(value), reserve,
                       enabled, 0, C_BONE, EK_NONE, 0);
}

static int row_slider_named_sub(UiRect& cursor, const char* label, int value, int lo, int hi,
                                const char* (*labelFn)(int), bool enabled) {
  const char* reserve = labelFn(lo);
  int reserveW = canvas_text_width(reserve, FONT_MONO);
  for (int v = lo + 1; v <= hi; v++) {
    const char* s = labelFn(v);
    const int w = canvas_text_width(s, FONT_MONO);
    if (w > reserveW) { reserve = s; reserveW = w; }
  }
  return row_slider_at(cursor, label, value, lo, hi, labelFn(value), reserve,
                       enabled, SUB_INDENT, C_BONEDIM, EK_NONE, 0);
}

// A dropdown: a value with named stops reads as a list, not a slider; mouse only (the panel has no arrow-key
// navigation). The open list cannot be drawn by the row: rows draw in order into an immediate-mode canvas with no
// z-order, so every later row would paint over it. It is deferred to menu_dropdown_draw, called where the tooltip is.
struct MenuDropdown {
  int  open;            // int, not bool: MSVC /O2 loses a bool member of a struct returned by inline.
  int  rowKey;
  UiRect box;
  int  count;
  const char* (*labelFn)(int);
};
static MenuDropdown s_dd = { 0, -1, { 0, 0, 0, 0 }, 0, 0 };

static bool s_ddSeenThisFrame = false;

static const int DD_CHEVRON_W = 14;
static const int DD_PAD       = 6;

static int row_dropdown(UiRect& cursor, const char* label, int value, int count,
                        const char* (*labelFn)(int), bool enabled) {
  if (cursor.h < ROW_H) { s_lastRowKey = -1; s_rowsDropped = 1; return value; }
  const UiRect r = ui_row(cursor, ROW_H, ROW_GAP);
  const int idx = s_pres.rowsThisFrame++ % ANIM_ROW_SLOTS;
  s_lastRow = r; s_lastRowKey = s_tab * 1000 + idx;
  const int key = s_lastRowKey;

  int nameW = 0;
  for (int v = 0; v < count; v++) {
    const int w = canvas_text_width(labelFn(v), FONT_MONO);
    if (w > nameW) nameW = w;
  }
  const int boxW = nameW + DD_PAD * 2 + DD_CHEVRON_W;
  const UiRect box = { r.x + r.w - VALUE_MARGIN - boxW, r.y + 4, boxW, ROW_H - 8 };

  const bool hot = enabled && ui_hit(box, g_ui.mx, g_ui.my);
  const bool pressed = row_pressed();
  if (hot && pressed) s_pressSpent = true;
  // menu_dd_row_step (menu.h) computes `seen` from the RETURNED state: the old inline form marked it from the state
  // before open() flipped 0->1, so the opening frame never marked itself seen and the catch-all undid the open the same
  // frame.
  const bool wasMine = s_dd.open != 0 && s_dd.rowKey == key;
  bool seen = false;
  const MenuDdState newSt = menu_dd_row_step(MenuDdState{ s_dd.open, s_dd.rowKey },
                                              key, hot, pressed,
                                              ui_rect_inside(box, s_bodyRect), &seen);
  s_dd.open = newSt.open; s_dd.rowKey = newSt.rowKey;
  if (seen) s_ddSeenThisFrame = true;
  const bool mine = newSt.open != 0 && newSt.rowKey == key;
  if (mine && !wasMine) { s_dd.box = box; s_dd.count = count; s_dd.labelFn = labelFn; }
  else if (mine) { s_dd.box = box; }

  const unsigned fill = enabled ? (hot || mine ? C_RAISED : C_WELL) : C_WELL;
  canvas_bevel(box.x, box.y, box.w, box.h, pres_content_colour(fill),
               pres_content_colour(C_BEVELHI), pres_content_colour(C_BEVELLO));
  canvas_text(r.x, r.y + 8, label, pres_colour(enabled ? C_BONE : C_BONEFNT), FONT_UI);
  canvas_text(box.x + DD_PAD, box.y + 4, labelFn(clamp_int(value, 0, count - 1)),
              pres_colour(enabled ? C_BONE : C_BONEFNT), FONT_MONO);
  {
    const int cx = box.x + box.w - DD_CHEVRON_W / 2 - DD_PAD / 2;
    const int cy = box.y + box.h / 2;
    const unsigned cc = pres_colour(enabled ? C_BONEDIM : C_BONEFNT);
    const int dy = mine ? -3 : 3;
    canvas_line(cx - 4, cy - dy / 2, cx, cy + dy, cc);
    canvas_line(cx, cy + dy, cx + 4, cy - dy / 2, cc);
  }

  int pickedRaw = 0;
  if (row_commit_take(key, &pickedRaw)) {
    const int v = clamp_int(pickedRaw, 0, count - 1);
    if (v != value) g_cfgDirty = 1;
    return v;
  }
  return value;
}

static void menu_dropdown_draw() {
  if (!s_dd.open || !s_dd.labelFn || s_dd.count <= 0) return;
  const int itemH = ROW_H - 6;
  const UiRect list = { s_dd.box.x, s_dd.box.y + s_dd.box.h + 2,
                        s_dd.box.w, itemH * s_dd.count + 4 };
  canvas_bevel(list.x, list.y, list.w, list.h, pres_content_colour(C_PANEL),
               pres_content_colour(C_BEVELHI), pres_content_colour(C_BEVELLO));
  bool onAnItem = false;
  for (int i = 0; i < s_dd.count; i++) {
    const UiRect it = { list.x + 2, list.y + 2 + i * itemH, list.w - 4, itemH };
    const bool hot = ui_hit(it, g_ui.mx, g_ui.my);
    if (hot) {
      onAnItem = true;
      canvas_rect(it.x, it.y, it.w, it.h, pres_content_colour(C_RAISED));
    }
    canvas_text(it.x + DD_PAD - 2, it.y + 3, s_dd.labelFn(i),
                pres_colour(hot ? C_BONE : C_BONEDIM), FONT_MONO);
    if (hot && row_pressed()) {
      s_pressSpent = true;
      row_commit_publish(s_dd.rowKey, i);
      s_dd.open = 0; s_dd.rowKey = -1;
      return;
    }
  }
  if (!onAnItem && row_pressed() && !ui_hit(s_dd.box, g_ui.mx, g_ui.my)) {
    s_pressSpent = true;
    s_dd.open = 0; s_dd.rowKey = -1;
  }
}

static void draw_group(UiRect& cursor, const char* heading) {
  if (cursor.h < GROUP_H) { s_rowsDropped = 1; return; }
  const UiRect r = ui_row(cursor, GROUP_H, GROUP_GAP);
  canvas_text(r.x, r.y + 5, heading, pres_content_colour(C_BONEFNT), FONT_SMALL);
}

static void draw_placeholder(UiRect& cursor, const char* text) {
  if (cursor.h < ROW_H) { s_rowsDropped = 1; return; }
  const UiRect r = ui_row(cursor, ROW_H, ROW_GAP);
  canvas_text(r.x, r.y + 6, text, pres_content_colour(C_BONEFNT), FONT_SMALL);
}

// A binding row: a rimmed well invites a click where bare text would not. The click only ARMS; the next key or button,
// handed to menu_keycap_bind by input.cpp, binds. It lands anywhere on the row. The Aimbot button is the one sub-row,
// greyed unless the aimbot and its Hold key row are on.
static void row_bind_at(UiRect& cursor, const char* label, int row, bool enabled,
                        int indent, unsigned labelOn) {
  if (cursor.h < ROW_H) { s_lastRowKey = -1; s_rowsDropped = 1; return; }
  const UiRect r = ui_row(cursor, ROW_H, ROW_GAP);
  const int idx = s_pres.rowsThisFrame++ % ANIM_ROW_SLOTS;
  s_lastRow = r; s_lastRowKey = s_tab * 1000 + idx;
  // Sized for the widest thing it can show (prompt, invitation, widest bind_label), never what it shows, as
  // row_slider_at's `reserve`: a well changing width on arm or hover would move under the hand.
  int fieldW = canvas_text_width(MENU_KEYCAP_PROMPT, FONT_MONO);
  {
    const int hoverW = canvas_text_width(MENU_KEYCAP_HOVER, FONT_MONO);
    const int nameW  = canvas_text_width(BIND_LABEL_WIDEST, FONT_MONO);
    if (hoverW > fieldW) fieldW = hoverW;
    if (nameW  > fieldW) fieldW = nameW;
  }
  fieldW += 2 * FIELD_PAD;
  const UiRect field = { r.x + r.w - fieldW, r.y + 5, fieldW, 20 };

  const bool hovered = enabled && ui_hit(field, g_ui.mx, g_ui.my);
  if (anim_style() == ANIM_OFF) {
    s_pres.hover[idx] = hovered ? 1.0 : 0.0;
  } else {
    const double rate = hovered ? (1.0 / ANIM_HOVER_IN_MS) : (-1.0 / ANIM_HOVER_OUT_MS);
    double hv = s_pres.hover[idx] + rate * s_frameDt;
    if (hv < 0.0) hv = 0.0; else if (hv > 1.0) hv = 1.0;
    s_pres.hover[idx] = hv;
  }

  if (indent > 0) canvas_rect(r.x + SUB_RULE_X, r.y, 1, r.h, pres_content_colour(C_RAISED));
  canvas_text(r.x + indent, r.y + 6, label,
              pres_content_colour(enabled ? labelOn : C_BONEFNT), FONT_UI);
  const bool armed = s_keycap.phase == KEYCAP_ARMED && s_keycap.row == row;
  const int vk = config_bind_get(row);
  // Hover lifts the well, not the text (fill C_WELL -> C_RAISED, rim C_RAISED -> C_BONEDIM, text stays C_BONE), else a
  // hovered field would draw its name in its own well's colour. Rim is hazard yellow in capture ("acting right now").
  canvas_round_rect(field.x, field.y, field.w, field.h, ROW_R,
                    pres_content_colour(pres_hover_toward(C_WELL, C_RAISED, s_pres.hover[idx])));
  canvas_round_rect_outline(field.x, field.y, field.w, field.h, ROW_R, 1,
                            pres_content_colour(armed ? C_HAZARD
                                                      : pres_hover_toward(C_RAISED, C_BONEDIM, s_pres.hover[idx])));
  const char* const value = bind_field_text(armed, hovered, vk,
                                            MENU_KEYCAP_PROMPT, MENU_KEYCAP_HOVER);
  const int valueW = canvas_text_width(value, FONT_MONO);
  const Glyph& capA  = font_glyphs[FONT_MONO]['A' - 0x20];
  const Glyph& descY = font_glyphs[FONT_MONO]['y' - 0x20];
  const int inkTop = capA.by;
  const int inkH   = (descY.by + descY.h) - inkTop;
  const int textY  = field.y + (field.h - inkH) / 2 - inkTop;
  canvas_text(field.x + (field.w - valueW) / 2, textY, value,
              pres_content_colour(armed ? C_HAZARD : (enabled ? C_BONE : C_BONEFNT)), FONT_MONO);
  if (ui_row_accepts_click(enabled, row_pressed(), ui_hit(r, g_ui.mx, g_ui.my))) {
    KeyCapEvent e; e.kind = KEYCAP_EV_CLICK; e.row = row; e.vk = 0;
    s_keycap = keycap_step(s_keycap, e).state;
    s_pressClaimed = true;
  }
  // The game's own claim on this key: one line, not a block or rebind, so it is found while choosing rather than in a
  // firefight. Absent whenever settings.cfg could not be read: a warning that cannot be computed is never guessed.
  const char* taken = gamecfg_function_for(vk);
  if (taken) draw_placeholder(cursor, MENU_GAMECFG_NOTE);
}

static void row_bind(UiRect& cursor, const char* label, int row) {
  row_bind_at(cursor, label, row, true, 0, C_BONE);
}

static void row_bind_sub(UiRect& cursor, const char* label, int row, bool enabled) {
  row_bind_at(cursor, label, row, enabled, SUB_INDENT, C_BONEDIM);
}

void menu_draw(int screenW, int screenH) {
  s_pres.rowsThisFrame = 0;
  s_rowsDropped = 0;
  s_hoverRowKey = -1;
  s_hoverHint = nullptr;
  s_lastRowKey = -1;
  s_glyphHint = nullptr;
  if (!g_ui.down) s_pressSpent = false;
  s_pressClaimed = false;
  {
    const unsigned now = menu_now();
    // Measured before lastMs moves: a gap this long means no frame drew the panel, so springs and latches are from a
    // session that ended. Under ANIM_OFF no closing frame draws, so the open edge alone would not see every close.
    const bool stale = (now - s_pres.lastMs) > ANIM_STALE_MS;
    s_frameDt = anim_dt_clamped(s_pres.lastMs, now);
    s_pres.lastMs = now;
    // The logical state; the open spring chases it, so menu_closing_animation_active() can ask whether the panel
    // finished fading.
    s_pres.openTarget = input_panel_open() ? 1 : 0;
    if (s_pres.openTarget && (!s_pres.wasOpen || stale)) {
      menu_note_new_session();
      // Every tab to its top, here and not in menu_note_new_session (also called from the window thread), keeping
      // s_scroll[] single-clocked. A scroll position belongs to its session.
      for (int i = 0; i < TAB_COUNT; i++) s_scroll[i] = 0;
      if (stale) { s_pres.open.x = 0.0; s_pres.open.v = 0.0; }
    }
    s_pres.wasOpen = s_pres.openTarget;
    if (anim_style() == ANIM_OFF) {
      s_pres.open.x = (double)s_pres.openTarget;
      s_pres.open.v = 0.0;
    } else {
      anim_spring_step(&s_pres.open, (double)s_pres.openTarget,
                       ANIM_SPRING_CRITICAL.stiffness, ANIM_SPRING_CRITICAL.damping, s_frameDt);
    }
  }
  // Below this line is logical pixels; the factor must be back to UI_SCALE_DEFAULT before returning, as the ESP draws
  // through the same canvas earlier in the frame. `pct` is `settled` held at its pre-drag value while the button is
  // down, so a layout moving with the value cannot slide a row from under the dragging hand.
  const int settled = menu_applied_scale(screenH);
  const int pct = menu_frame_scale(screenH);
  if (!g_ui.down) s_scaleHeld = settled;
  canvas_set_scale(pct);
  const int deviceMx = g_ui.mx, deviceMy = g_ui.my;
  g_ui.mx = ui_unscale_px(deviceMx, pct);
  g_ui.my = ui_unscale_px(deviceMy, pct);
  const int logicalW = ui_unscale_px(screenW, pct);
  const int logicalH = ui_unscale_px(screenH, pct);

  // The click away, once, before any row: a press of either button outside the open field's well finishes it (the press
  // may land on a row drawn earlier or nothing). A right press opening a field elsewhere still works (g_ui.rpressed is
  // read raw). An open field claims the left press: inside the caret, outside the close.
  if (s_entry.open && (g_ui.pressed || g_ui.rpressed) && !s_dragLive) {
    if (g_ui.pressed) { s_pressClaimed = true; s_pressSpent = true; }
    if (!ui_hit(s_entryFieldRect, g_ui.mx, g_ui.my)) entry_clickaway_commit();
  }

  // The box drawn this frame: the ini keys through panel_layout, overridden by the live box during a move or resize;
  // stepped before the first primitive. The title band is tested here so the grip wins over a move (panel_drag_step
  // asks about the grip first).
  const PanelBox laid = panel_layout(panel_stored_box_capped(screenH), logicalW, logicalH);
  PanelBox box = laid;
  bool gripHit = false;
  {
    const bool titleHit = ui_hit(panel_title_rect(laid, STRIP_H, STATUS_H), g_ui.mx, g_ui.my);
    // Tested against the box the grip is drawn at (s_dragBox in a live drag), not `laid`.
    gripHit = ui_hit(panel_grip_rect(s_dragLive ? s_dragBox : laid), g_ui.mx, g_ui.my);
    bool ended = false;
    s_drag = panel_drag_step(s_drag, g_ui.pressed, g_ui.down, titleHit, gripHit,
                             g_ui.mx, g_ui.my, laid, logicalW, logicalH, &box, &ended);
    // panel_drag_step/panel_layout clamp height only to [PANEL_MIN_H, screenH], so a resize could pass the height that
    // keeps the scale ceiling above the stored scale; applied after the step (panel_layout.h stays Megaton's). Takes
    // the DEVICE height: logicalH starved the resize at high scales and let the panel draw off screen at low ones.
    const int panelHCap = menu_panel_h_cap(screenH);
    if (box.h > panelHCap) box.h = panelHCap;
    s_dragLive = s_drag.phase != PDRAG_IDLE;
    if (ended) {
      // The box the frame before drew: with the button gone there is no mouse to re-derive a moved box from.
      box = s_dragBox;
      if (box.x != laid.x || box.y != laid.y || box.w != laid.w || box.h != laid.h) {
        g_cfg.panelx = box.x; g_cfg.panely = box.y;
        g_cfg.panelw = box.w; g_cfg.panelh = box.h;
        g_cfgDirty = 1;
      }
    } else if (s_dragLive) {
      s_dragBox = box;
    }
  }
  const UiRect p = { box.x, box.y, box.w, box.h };

  // Counts parent switches only (sub-rows never count, menu_live_count in menu.h). The free camera counts though it is
  // a spectator tool: seeing anywhere in the map is an advantage. fovon counts: a view wider than the game's menu
  // offers is one. dmgnumbers is its own parent row; secrettriggers is a sub-row and passes 0.
  const int liveCount = menu_live_count(g_cfg.fovon, g_cfg.esp,
                                        g_cfg.espitems, g_cfg.espsecrets, g_cfg.espexplodables, g_cfg.espswitches,
                                        g_cfg.aimon, g_cfg.silenton, g_cfg.freecamon,
                                        g_cfg.accpistol, g_cfg.accchaingun,
                                        g_cfg.accshotgun, g_cfg.accexpander,
                                        g_cfg.dmgnumbers, 0, g_cfg.tboton, g_cfg.espbreakables,
                                        g_cfg.espfriendlies, g_cfg.accincinerator,
                                        g_cfg.espcash, g_cfg.espfigurines,
                                        g_cfg.figurinecounter, g_cfg.espchests);
  const unsigned strip = liveCount ? C_RED : C_HAZARD;

  // The whole panel moves as one unit by one canvas-wide transform (canvas.h) applied to device coordinates, while `p`
  // and `cursor` stay put so every ui_hit still tests the logical layout. Set in every style (identity under Off);
  // canvas_begin clears it each frame.
  if (anim_style() == ANIM_SIMPLE) {
    const int slideY = (int)((1.0 - s_pres.open.x) * (double)SIMPLE_SLIDE_PX + 0.5);
    canvas_set_transform(0, -slideY, 1.0, 0, 0);
  } else {
    canvas_set_transform(0, 0, 1.0, 0, 0);
  }

  canvas_bevel(p.x, p.y, p.w, p.h,
               pres_colour(C_PANEL), pres_colour(C_BEVELHI), pres_colour(C_BEVELLO));
  canvas_hazard(p.x + 1, p.y + 1, p.w - 2, STRIP_H, pres_colour(strip), pres_colour(C_PANEL), 9);

  const int sy = p.y + 1 + STRIP_H;
  canvas_rect(p.x + 1, sy, p.w - 2, STATUS_H, pres_colour(C_RECESS));
  canvas_rect(p.x + PAD, sy + 11, 9, 9, pres_colour(strip));
  char status[32];
  if (liveCount) sprintf_s(status, "%d cheat%s live", liveCount, liveCount == 1 ? "" : "s");
  else           sprintf_s(status, "Idle");
  canvas_text(p.x + PAD + 18, sy + 7, status, pres_colour(C_BONEDIM), FONT_SMALL);
  // The panel key BY NAME (World Tour draws "Ins closes"): it is rebindable. Through bind_menu_key_effective, the same
  // fallback input.cpp applies, so it names the key that really closes the panel. Right-aligned, as a wider name would
  // overrun.
  char closeText[32];
  sprintf_s(closeText, "%s closes",
            bind_label(bind_menu_key_effective(config_bind_get(BIND_ROW_MENU))));
  canvas_text(p.x + p.w - PAD - canvas_text_width(closeText, FONT_SMALL), sy + 7,
              closeText, pres_colour(C_BONEFNT), FONT_SMALL);

  const int bodyY = sy + STATUS_H;
  const int bodyH = p.y + p.h - bodyY - 1;

  canvas_rect(p.x + 1, bodyY, RAIL_W, bodyH, pres_colour(C_WELL));
  // The rail's click is recorded here, applied at the foot: switching inside the loop would leave a commit owned by a
  // row nothing draws (menu_set_tab runs after the tab's rows had their chance to consume it).
  int railTab = -1;
  for (int i = 0; i < TAB_COUNT; i++) {
    const UiRect t = { p.x + 1, bodyY + i * MENU_RAIL_ENTRY_H, RAIL_W, MENU_RAIL_ENTRY_H };
    if (i == s_tab) {
      canvas_rect(t.x, t.y, t.w, t.h, pres_colour(C_PANEL));
      canvas_rect(t.x, t.y, 3, t.h, pres_colour(TAB_COLOURS[i]));
    }
    const int nameY = t.y + (t.h - font_height[FONT_UI]) / 2;
    canvas_text(t.x + RAIL_LABEL_X, nameY, TAB_NAMES[i],
                pres_colour(i == s_tab ? C_BONE : C_BONEDIM), FONT_UI);
    if (g_ui.pressed && ui_hit(t, g_ui.mx, g_ui.my)) {
      railTab = i;
      s_pressClaimed = true;
    }
  }

  // A switch snaps every row's spring on its first content frame and starts the ANIM_TAB_MS crossfade; the rail click's
  // switch is applied at the foot, so this is the frame after the click.
  s_tabSwitchedThisFrame = (s_tab != s_pres.tabFrom);
  if (s_tabSwitchedThisFrame) s_pres.tabSwitchMs = s_pres.lastMs;
  s_pres.tabFrom = s_tab;

  // The tab body scrolls by one line: the offset comes off the cursor's STARTING y, and every rect comes from ui_row
  // and every hit from ui_hit against it, so draw and click follow together. The budget is MENU_CONTENT_BUDGET, not the
  // visible height, or the panel could never scroll (menu.h).
  const UiRect bodyRect = { p.x + 1 + RAIL_W, bodyY, p.w - RAIL_W - 2, bodyH };
  // Published for the dropdown/field containment checks, with the two "did its row run" flags reset beside it.
  s_bodyRect = bodyRect;
  s_ddSeenThisFrame = false;
  s_entrySeenThisFrame = false;
  const int viewH = bodyH - 2 * PAD;
  // The wheel from ImGui's IO on the thread that fills it, read every frame the panel draws: the panel swallows
  // WM_MOUSEWHEEL whole while open (input.cpp's is_input_message), so there is no other claimant.
  s_scroll[s_tab] = menu_scroll_after_wheel(s_scroll[s_tab], (double)ImGui::GetIO().MouseWheel,
                                            s_contentH[s_tab], viewH);
  const int scroll = s_scroll[s_tab];

  UiRect cursor = { p.x + 1 + RAIL_W + PAD, bodyY + PAD - scroll, p.w - RAIL_W - 2 * PAD - 2, MENU_CONTENT_BUDGET };
  const int contentTop = cursor.y;
  canvas_push_clip(bodyRect.x, bodyRect.y, bodyRect.w, bodyRect.h);
  // Nor CLICKED there, which the clip cannot stop (ui_hit tests the logical rect): the cursor is parked where no rect
  // contains it for the content block, gating every row helper at once. A viewport gate, not the scroll offset. The one
  // read not behind a ui_hit (the open field's caret drag) takes s_frameMx.
  const int bodyMx = g_ui.mx, bodyMy = g_ui.my;
  s_frameMx = g_ui.mx;
  if (!ui_hit(bodyRect, g_ui.mx, g_ui.my)) {
    g_ui.mx = MENU_MOUSE_NOWHERE; g_ui.my = MENU_MOUSE_NOWHERE;
  }
  switch (s_tab) {
    case TAB_VISUALS: {
      // Each row writes straight into g_cfg, so a toggle takes effect on the next ESP frame with the panel open. No
      // "Enemies" heading: it would repeat its first row in that row's accent (likewise Friendlies, Aimbot, Triggerbot,
      // Silent aim).
      g_cfg.esp = row_switch(cursor, "Enemies", g_cfg.esp, C_RED, true);
      const bool enemiesOn = g_cfg.esp != 0;
      g_cfg.espboxes     = row_switch_sub(cursor, "Bounding boxes",  g_cfg.espboxes,     C_RED, enemiesOn);
      g_cfg.esphealthbar = row_switch_sub(cursor, "Health bars",     g_cfg.esphealthbar, C_RED, enemiesOn);
      g_cfg.espname      = row_switch_sub(cursor, "Name and health", g_cfg.espname,      C_RED, enemiesOn);
      g_cfg.espdistance  = row_switch_sub(cursor, "Distance",        g_cfg.espdistance,  C_RED, enemiesOn);
      // AA rolls a hostile's level and scales its health off it, which explains two of one enemy having different
      // maxima. Drawn before the name as AA's nameplate does; shows nothing where the game has no such variable.
      g_cfg.esplevel     = row_switch_sub(cursor, "Level",           g_cfg.esplevel,     C_RED, enemiesOn);
      row_hint("Needs the game's own monster level variable. Stock Duke has none.");

      // AA's allies and civilians, their own group, not a sub-row: the collector puts them on a second list
      // (targets.cpp), so this gates a different array. Magenta (C_FRIENDLY) is the brackets and rows; the health bar
      // keeps canvas_health_colour.
      g_cfg.espfriendlies = row_switch(cursor, "Friendlies", g_cfg.espfriendlies,
                                       C_FRIENDLY, true);
      row_hint("Needs the game's own team variable. Stock Duke has none.");
      const bool friendliesOn = g_cfg.espfriendlies != 0;
      g_cfg.espfriendlyboxes     = row_switch_sub(cursor, "Bounding boxes",  g_cfg.espfriendlyboxes,     C_FRIENDLY, friendliesOn);
      g_cfg.espfriendlyhealthbar = row_switch_sub(cursor, "Health bars",     g_cfg.espfriendlyhealthbar, C_FRIENDLY, friendliesOn);
      g_cfg.espfriendlyname      = row_switch_sub(cursor, "Name and health", g_cfg.espfriendlyname,      C_FRIENDLY, friendliesOn);
      g_cfg.espfriendlydistance  = row_switch_sub(cursor, "Distance",        g_cfg.espfriendlydistance,  C_FRIENDLY, friendliesOn);
      g_cfg.espfriendlylevel     = row_switch_sub(cursor, "Level",           g_cfg.espfriendlylevel,     C_FRIENDLY, friendliesOn);

      // WORLD is Megaton's heading (menu.cpp:1790), never carried over; without it Items, Secrets, Explodables,
      // Breakables and Switches read as more rows of the category above.
      draw_group(cursor, "WORLD");
      // Its own parent: a pickup mark is a separate category and walk; nesting under Enemies would tie it to a switch
      // it does not need. It shares the Range group, which applies to every category.
      g_cfg.espitems = row_switch(cursor, "Items and pickups", g_cfg.espitems,
                                  C_BLUE, true);
      g_cfg.espitemshidefull = row_switch_sub(cursor, "Hide what you can't take",
                                              g_cfg.espitemshidefull, C_BLUE,
                                              g_cfg.espitems != 0);
      row_hint("Hides items AA would refuse right now (full health, full ammo, "
               "a weapon you own and are full on, held keycard, a weapon your "
               "character can never take...). Upgrades, dual pistols, "
               "Swinelord's gun, BMFG and Executioner are always shown.");

      // A third parent beside Items: figurines are cut OUT of the Items walk (esp.cpp refuses every MINI* picnum), so
      // this is the only place they draw, and it marks them even when AA's cstat 32768 hides them pending approach:
      // finding hidden ones is the point.
      g_cfg.espfigurines = row_switch(cursor, "Figurines", g_cfg.espfigurines,
                                      C_FIGURINE, true);
      row_hint("Marks unfound figurines, including ones hidden until you're close.");

      g_cfg.figurinecounter = row_switch(cursor, "Figurine counter",
                                        g_cfg.figurinecounter, C_FIGURINE, true);
      row_hint("Top-left corner. Hidden on a level with none left to find.");

      g_cfg.espchests = row_switch(cursor, "EDF chests & bomb parts", g_cfg.espchests,
                                   C_CHEST, true);
      row_hint("Marks unopened ones by weapon. Count top-left in httkb.");

      g_cfg.espsecrets = row_switch(cursor, "Secret areas", g_cfg.espsecrets,
                                    C_HAZARD, true);
      // A sub-row gated on its parent, as "Combination tint" is on Switches: the panels are pushed inside the secret
      // pass, which only runs while that row is on. The parent's accent: the panels are the cross's yellow
      // (C_SECRET_VOLUME).
      g_cfg.secretvolume = row_switch_sub(cursor, "Secret volume", g_cfg.secretvolume,
                                          C_HAZARD, g_cfg.espsecrets != 0);
      // Beside it, a second sub-row of Secret areas, gated the same way (the chains are drawn inside the secret pass).
      // The hint admits the limit: an activator's hitag gates on its sector being open (sector.cpp:1133) and a CON
      // script may veto the operation (:1121), neither knowable at scan time, so the line shows map structure.
      g_cfg.secrettriggers = row_switch_sub(cursor, "Secret triggers", g_cfg.secrettriggers,
                                            C_HAZARD, g_cfg.espsecrets != 0);
      row_hint("Map structure only. A hitag or CON script may still block.");

      // A third sub-row of Secret areas, gated the same way (the route is drawn inside the secret pass). The hint is
      // load-bearing: the search sees geometry only, so it routes through a keycard door, past a locked activator and
      // down a drop that cannot be climbed back; its failure mode is a confident wrong answer.
      g_cfg.secretpath = row_switch_sub(cursor, "Secret path", g_cfg.secretpath,
                                        C_HAZARD, g_cfg.espsecrets != 0);
      row_hint("Geometry only. May route through locks and one way drops.");

      g_cfg.espexplodables = row_switch(cursor, "Explodables", g_cfg.espexplodables,
                                        C_HAZARD, true);
      g_cfg.espbreakables  = row_switch(cursor, "Breakables", g_cfg.espbreakables,
                                        C_HAZARD, true);

      // A third parent beside those two, not a sub-row: Breakables marks WALLS from the cached wall scan and cannot see
      // a sprite container, while this walks sprites and asks AA's per-actor `droptile`. Green, not hazard yellow: it
      // marks a subset of the breakables with the same cross, so sharing their colour would hide the distinction.
      g_cfg.espcash = row_switch(cursor, "Cash containers", g_cfg.espcash,
                                 C_GREEN, true);
      row_hint("Marks only breakables that drop money. No amount shown.");

      // Its own parent: drawn from the damage detour's ring, depending on no ESP category. Hazard yellow for a hit, the
      // enemy colour for a kill.
      g_cfg.dmgnumbers = row_switch(cursor, "Damage numbers", g_cfg.dmgnumbers,
                                    C_HAZARD, true);
      row_hint("Your own hits only. Needs the damage hook to have taken.");

      g_cfg.espswitches = row_switch(cursor, "Switches and locks", g_cfg.espswitches,
                                     C_BLUE, true);
      // A sub-row gated on its parent, inert not merely greyed: the tint is collected inside the switch walk, which
      // only runs while that row is on. The parent's accent (same category); the tint's own colours are the verdict's.
      g_cfg.esptint = row_switch_sub(cursor, "Combination tint", g_cfg.esptint,
                                     C_BLUE, g_cfg.espswitches != 0);

      draw_group(cursor, "RANGE");
      // Always live: a range, not a feature. Metres for the label only (BUILD_UNITS_PER_METRE); the stored value stays
      // in Build units. Default is the MAXIMUM on purpose: a cull that shipped active reads as a broken projection.
      g_cfg.espdrawdist = row_slider_metres(cursor, "Draw distance", g_cfg.espdrawdist,
                                            ESP_DRAWDIST_MIN, ESP_DRAWDIST_MAX,
                                            true, ESP_DRAWDIST_DEFAULT);
      row_hint("Markers stop at this range.");
      g_cfg.espnolimit = row_switch_sub(cursor, "Unlimited range", g_cfg.espnolimit,
                                        C_GREEN, true);
      row_hint("Ignores the distance. Slow on a large map.");

      break;
    }
    case TAB_COMBAT: {
      draw_group(cursor, "TARGETING");
      // Targeting shapes silent aim too: aim_acquire is the one acquisition both use, so cone, range, pick target, aim
      // point and lead all apply to a silent shot. Mirrors aim_frame's `wantAim || wantSilent`; gating on the aimbot
      // alone locked silent-aim-only players out.
      const bool tgtOn = aim_available() &&
                         (g_cfg.aimon || (g_cfg.silenton && gamehooks_shoot_ready()));
      g_cfg.aimtarget = row_slider_named(cursor, "Pick target", g_cfg.aimtarget,
                                         0, 1, aim_target_label, tgtOn);
      row_hint("Crosshair picks what you are facing.");
      g_cfg.aimpoint = row_slider_named(cursor, "Aim at", g_cfg.aimpoint,
                                        0, 1, aim_point_label, tgtOn);
      row_hint("Head aims at AA's headshot zone where the enemy has one.");
      g_cfg.aimcone = row_slider(cursor, "Cone", g_cfg.aimcone, 1, 90, " deg", tgtOn, 30);
      row_hint("How far off centre a target may be.");
      g_cfg.aimrange = row_slider_metres(cursor, "Aim range", g_cfg.aimrange,
                                         512, 32768, tgtOn, 32768);
      g_cfg.aimlead = row_switch(cursor, "Lead moving targets", g_cfg.aimlead, C_RED, tgtOn);
      g_cfg.aimleadscale = row_slider_sub(cursor, "Lead amount", g_cfg.aimleadscale,
                                          0, 100, "%", tgtOn && g_cfg.aimlead, 70);
      g_cfg.aimdebug = row_switch(cursor, "Aim calibration marker", g_cfg.aimdebug,
                                  C_RED, tgtOn);
      row_hint("Shows the raw aim point and the led point.");

      const bool armed = aim_available();
      // The stored value is passed whether or not the row is live: a disabled row returns what it was given, and a 0
      // handed in while the scan still ran overwrote the player's setting just for drawing the tab. Only `enabled` is
      // gated.
      g_cfg.aimon = row_switch(cursor, armed ? "Aimbot" : "Aimbot (Unavailable)",
                               g_cfg.aimon, C_RED, armed);
      // Stands down rather than fight AA's own Aimbot Goggles, which write the same fields every tic while they hold a
      // lock.
      row_hint(armed ? "Turns your view onto the nearest visible enemy. "
                       "Stands down under AA's Aimbot Goggles."
                     : "Needs the engine scan to finish. See ekmod.log.");
      const bool aimOn = armed && g_cfg.aimon;
      g_cfg.aimhold = row_switch_sub(cursor, "Hold key", g_cfg.aimhold, C_RED, aimOn);
      row_hint("Only while the Aimbot button is held down.");
      row_bind_sub(cursor, "Aimbot button", BIND_ROW_AIM, aimOn && g_cfg.aimhold);
      row_hint("Held to aim when Hold key is on.");
      g_cfg.aimsmooth = row_slider_sub(cursor, "Smoothing", g_cfg.aimsmooth,
                                       0, 100, "%", aimOn, 0);

      // The triggerbot: switch, then what must be true and how long to wait; one hint for the group, on the switch. Two
      // things must be true, failing independently: acquisition needs the player struct and cansee, the write needs
      // localInput located. One suffix for both; the log says which half is missing.
      const bool tb = aim_available() && localinput_ready();
      g_cfg.tboton = row_switch(cursor, tb ? "Triggerbot" : "Triggerbot (Unavailable)",
                                g_cfg.tboton, C_RED, tb);
      row_hint(tb ? "Fires by itself while the condition below holds"
                  : "Needs the engine scan to finish. See ekmod.log.");
      const bool tbOn = tb && g_cfg.tboton;
      g_cfg.tbotwhen = row_slider_named_sub(cursor, "Fire when", g_cfg.tbotwhen,
                                            TBOT_WHEN_CROSSHAIR, TBOT_WHEN_AIMBOT,
                                            tbot_when_label, tbOn);
      row_hint("Crosshair mode needs you pointing at a visible enemy.");
      g_cfg.tbotdelay = row_slider_sub(cursor, "Delay", g_cfg.tbotdelay,
                                       TBOT_DELAY_MS_MIN, TBOT_DELAY_MS_MAX, " ms",
                                       tbOn, TBOT_DELAY_MS_DEFAULT);
      row_hint("How long the condition must hold before it fires.");

      const bool sa = gamehooks_shoot_ready() && aim_available();
      g_cfg.silenton = row_switch(cursor, sa ? "Silent aim" : "Silent aim (Unavailable)",
                                  g_cfg.silenton, C_RED, sa);
      row_hint("Shots go to the target without moving your view.");
      g_cfg.silentmaxangle = row_slider_sub(cursor, "Max redirect", g_cfg.silentmaxangle,
                                            1, 180, " deg", sa && g_cfg.silenton, 45);
      row_hint("Shots further off than this are left alone.");
      break;
    }
    // VIEW below stays empty on purpose: a heading over no row would promise something the DLL cannot do; the rail
    // keeps every entry, as the rail IS the menu.
    case TAB_WEAPONS: {
      draw_group(cursor, "PERFECT ACCURACY");
      const bool sp = gamehooks_spread_ready();
      g_cfg.accpistol   = row_switch(cursor, sp ? "Pistol" : "Pistol (Unavailable)",
                                     g_cfg.accpistol, C_HAZARD, sp);
      row_hint("Removes this weapon's own spread.");
      g_cfg.accchaingun = row_switch(cursor, sp ? "Chaingun" : "Chaingun (Unavailable)",
                                     g_cfg.accchaingun, C_HAZARD, sp);
      g_cfg.accshotgun  = row_switch(cursor, sp ? "Shotgun" : "Shotgun (Unavailable)",
                                     g_cfg.accshotgun, C_HAZARD, sp);
      row_hint("Pellets land in one hole.");
      g_cfg.accexpander = row_switch(cursor, sp ? "Expander" : "Expander (Unavailable)",
                                     g_cfg.accexpander, C_HAZARD, sp);
      g_cfg.accincinerator = row_switch(cursor, sp ? "Incinerator" : "Incinerator (Unavailable)",
                                        g_cfg.accincinerator, C_HAZARD, sp);
      const bool nr = gamevars_recoil_present();
      g_cfg.norecoil = row_switch(cursor, nr ? "No recoil" : "No recoil (Unavailable)",
                                  g_cfg.norecoil, C_HAZARD, nr);
      row_hint("Your aim no longer kicks up when you fire.");

      // Handling covers reload and switching too. Both rows are always live: no hook or scan of ours, the write goes
      // through playerstate.cpp which answers NULL rather than faulting. The value is put back through
      // firerate_rate_clamped so the drag settles on the header's 50-step stops.
      draw_group(cursor, "HANDLING");
      {
        // The prediction on the row itself, since the hint is hover-only and hides mid-drag. FONT_MONO is 11px a glyph,
        // so `400%/2.0x` is 99px against the 90px that keeps the track in the tab's column: the reserve slides THIS
        // row's track 9px left, no other's.
        char fireVal[32];
        int rowWeapon = 0, rowX100 = EK_RATE_ACHIEVED_NONE;
        const bool rowPred = g_cfg.firerate > 100 &&
            weaponspeed_predicted(g_cfg.firerate, &rowWeapon, &rowX100);
        if (!rowPred)
          _snprintf_s(fireVal, sizeof(fireVal), _TRUNCATE, "%d%%", g_cfg.firerate);
        else if (rowX100 == EK_RATE_ACHIEVED_NONE)
          _snprintf_s(fireVal, sizeof(fireVal), _TRUNCATE, "%d%%/none", g_cfg.firerate);
        else
          _snprintf_s(fireVal, sizeof(fireVal), _TRUNCATE, "%d%%/%d.%dx", g_cfg.firerate,
                      rowX100 / 100, (rowX100 / 10) % 10);
        g_cfg.firerate = firerate_rate_clamped(
            row_slider_at(cursor, "Fire rate", g_cfg.firerate, FIRERATE_MIN,
                          FIRERATE_MAX, fireVal, "400%/2.0x", true, 0, C_BONE,
                          EK_INT, FIRERATE_DEFAULT));
      }
      // What the held weapon actually gets, in the hint: the percentage cannot be made true (AA has spent the
      // headroom), so the label stays the SETTING and the hint says what is measured per weapon, a dash rather than a
      // guess, only above 100. The figure comes first so a truncation at s_hintBuf's 96 costs the generic sentence.
      {
        static const char* const kFireWhat = "Compresses every frame that is not a shot.";
        char fireHint[96];
        int achWeapon = 0, achStock = 0, achSpan = 0, achSamples = 0;
        int achState = EK_ACH_MEASURING;
        const bool achOk = g_cfg.firerate > 100 &&
            weaponspeed_achieved(&achWeapon, &achStock, &achSpan, &achSamples,
                                 &achState);
        const int achX100 = (achOk && achState == EK_ACH_MEASURING)
            ? ekm_rate_achieved_x100(achStock, achSpan, achSamples)
            : EK_RATE_ACHIEVED_NONE;
        const char* const achName = achOk ? ekm_weapon_slot_name(achWeapon) : 0;
        if (achOk && achState == EK_ACH_NO_SPEEDUP) {
          _snprintf_s(fireHint, sizeof(fireHint), _TRUNCATE,
                      "No speed-up on this weapon. %s", kFireWhat);
        } else if (achOk && achState == EK_ACH_NO_CLOCK) {
          // Without the engine's tic clock nothing can be measured (every cycle is suspect, none publishes): "fire a
          // few shots" would ask for something that cannot help. Fits the buffer whole (34 + 42 of 95).
          _snprintf_s(fireHint, sizeof(fireHint), _TRUNCATE,
                      "The game clock could not be found. %s", kFireWhat);
        } else if (achOk && achState == EK_ACH_NOT_KNOWN_YET) {
          _snprintf_s(fireHint, sizeof(fireHint), _TRUNCATE,
                      "Not known yet for this weapon. %s", kFireWhat);
        } else if (achX100 != EK_RATE_ACHIEVED_NONE) {
          if (achName)
            _snprintf_s(fireHint, sizeof(fireHint), _TRUNCATE,
                        "%s: %d.%dx measured. %s", achName, achX100 / 100,
                        (achX100 / 10) % 10, kFireWhat);
          else
            _snprintf_s(fireHint, sizeof(fireHint), _TRUNCATE,
                        "Weapon %d: %d.%dx measured. %s", achWeapon,
                        achX100 / 100, (achX100 / 10) % 10, kFireWhat);
        } else if (achOk && achSpan >= 0 && achStock <= 0) {
          _snprintf_s(fireHint, sizeof(fireHint), _TRUNCATE,
                      "This weapon's stock timing could not be read. %s",
                      kFireWhat);
        } else if (g_cfg.firerate > 100) {
          _snprintf_s(fireHint, sizeof(fireHint), _TRUNCATE,
                      "Fire a few shots to measure. %s", kFireWhat);
        } else {
          _snprintf_s(fireHint, sizeof(fireHint), _TRUNCATE, "%s", kFireWhat);
        }
        // The prediction leads the hint: what the held weapon will get at this setting, before it fires. Not on the
        // row, whose ~90px readout would push the track out of the column. The tooltip is one line, so truncation falls
        // on the generic sentence.
        char predText[48];
        predText[0] = 0;
        int predWeapon = 0, predX100 = EK_RATE_ACHIEVED_NONE;
        const bool predOk = g_cfg.firerate > 100 &&
            weaponspeed_predicted(g_cfg.firerate, &predWeapon, &predX100);
        if (predOk) {
          char predNum[16];
          const char* predName = ekm_weapon_slot_name(predWeapon);
          if (!predName) {
            _snprintf_s(predNum, sizeof(predNum), _TRUNCATE, "Weapon %d", predWeapon);
            predName = predNum;
          }
          if (predX100 == EK_RATE_ACHIEVED_NONE)
            _snprintf_s(predText, sizeof(predText), _TRUNCATE, "%d%% -> %s no speed-up. ",
                        g_cfg.firerate, predName);
          else
            _snprintf_s(predText, sizeof(predText), _TRUNCATE, "%d%% -> %s %d.%dx. ",
                        g_cfg.firerate, predName, predX100 / 100, (predX100 / 10) % 10);
        }
        char hintLine[160];
        _snprintf_s(hintLine, sizeof(hintLine), _TRUNCATE, "%s%s", predText, fireHint);
        row_hint(hintLine);
      }
      // Reload speed has its own row so Fire rate no longer shortens reloads. Value text is the Fire rate row's
      // predicted over the held weapon's ladder; `?` for a CON reload no frame bounds. Range 100-1000 plus Max
      // (weaponspeed.h).
      {
        char reloadVal[32];
        int rWeapon = 0, rX100 = EK_RATE_ACHIEVED_NONE;
        const int rRate = reloadrate_clamped(g_cfg.reloadrate);
        const bool rPred = rRate > 100 &&
            weaponspeed_reload_predicted(rRate, &rWeapon, &rX100);
        reloadrate_value_text(rRate, rPred, rX100, reloadVal, (int)sizeof(reloadVal));
        g_cfg.reloadrate = reloadrate_from_slider(
            row_slider_at(cursor, "Reload speed", reloadrate_to_slider(rRate), FIRERATE_MIN,
                          RELOADRATE_SLIDER_MAX, reloadVal, "1000%/10.0x", true, 0, C_BONE,
                          EK_INT, FIRERATE_DEFAULT));
      }
      row_hint("Faster reloads, up to Max; every frame the game acts on still plays.");
      g_cfg.switchspeed = firerate_rate_clamped(
          row_slider(cursor, "Weapon switch speed", g_cfg.switchspeed,
                     FIRERATE_MIN, FIRERATE_MAX, "%", true, FIRERATE_DEFAULT));
      row_hint("Faster lowering and raising when you switch");

      // Automatic weapon switching is the engine's own setting (cl_weaponswitch, autoswitch.h): no hook, no
      // Unavailable, no state to drift from the console. Two rows over one int, the first three-state: bits 1 and 4 are
      // one setting (4 alone reads "never switch on pickup"), bit 2 stands alone. Each row reads-modifies-writes.
      draw_group(cursor, "AUTOMATIC SWITCHING");
      {
        const EkCvar* const ws = cvars_find("cl_weaponswitch");
        const bool haveWs = cvar_row_available(ws);
        int mask = 0;
        if (haveWs) cvar_get_int("cl_weaponswitch", &mask);

        const int wasPickup = wswitch_pickup_state(mask);
        const int isPickup = row_dropdown(cursor, haveWs ? "Switch on pickup"
                                                         : "Switch on pickup (Unavailable)",
                                          wasPickup, EK_PICKUP_STATES,
                                          wswitch_pickup_name, haveWs);
        if (haveWs && isPickup != wasPickup)
          cvar_set_int("cl_weaponswitch", wswitch_with_pickup(mask, isPickup), &mask);
        row_hint("Better is by the game's own weapon order.");

        const int wasEmpty = wswitch_bit_on(mask, EK_WSWITCH_EMPTY);
        const int isEmpty = row_switch(cursor, haveWs ? "Switch when empty"
                                                      : "Switch when empty (Unavailable)",
                                       wasEmpty, C_HAZARD, haveWs);
        if (haveWs && isEmpty != wasEmpty)
          cvar_set_int("cl_weaponswitch", wswitch_with_bit(mask, EK_WSWITCH_EMPTY, isEmpty), &mask);
        row_hint("Falls back to another weapon when this one runs dry.");
      }
      break;
    }
    case TAB_ITEMS: {
      draw_group(cursor, "PICKUPS");
      g_cfg.coinmagnet = row_dropdown(cursor, "Coin magnet",
                                      coinmagnet_clamp(g_cfg.coinmagnet), EK_CM_COUNT,
                                      coinmagnet_name, true);
      row_hint("Up to the whole map. Far coins wait, then pay when they would have arrived.");
      g_cfg.coinspeed = row_dropdown(cursor, "Coin speed",
                                     coinspeed_clamp(g_cfg.coinspeed), EK_CS_COUNT,
                                     coinspeed_name, true);
      row_hint("How fast coins come to you; alone it speeds AA's pull. Instant pays next tic.");
      g_cfg.coinheal = row_switch(cursor, "Health coins", g_cfg.coinheal ? 1 : 0, C_GREEN,
                                  coinmagnet_clamp(g_cfg.coinmagnet) != EK_CM_OFF);
      row_hint("Health coins too; without AA's Coin Magnet perk, only from the sector you stand in.");
      {
        const bool prOk = pickuprange_ready();
        g_cfg.pickuprange = row_dropdown(cursor, prOk ? "Pickup range" : "Pickup range (Unavailable)",
                                         pickuprange_clamp(g_cfg.pickuprange), EK_PR_COUNT,
                                         pickuprange_name, prOk);
        row_hint(prOk ? "Items you can see are picked up from this far, where they lie."
                      : pickuprange_status());
      }
      break;
    }
    case TAB_VIEW: {
      // The field of view is the engine's own setting: World Tour intercepted the viewing range because that executable
      // has none; EDuke32 keeps it in the registered cvar `fov` (fov.h). Own Unavailable: `fov` must be reachable and
      // writable through the registry, and it is the one cvar a name-first registry search already lost on this port.
      draw_group(cursor, "FIELD OF VIEW");
      const bool fovOk = fov_available();
      g_cfg.fovon = row_switch(cursor, fovOk ? "Custom field of view"
                                             : "Custom field of view (Unavailable)",
                               g_cfg.fovon, C_GREEN, fovOk);
      row_hint("Writes the game's own fov. Off gives your value back.");
      g_cfg.fov = row_slider_sub(cursor, "Field of view", g_cfg.fov,
                                 EK_FOV_MIN, EK_FOV_MAX, "",
                                 fovOk && g_cfg.fovon != 0, EK_FOV_DEFAULT);
      row_hint("Wider than the game's own menu allows. 90 is stock.");

      // The free camera never moves the player: the pose is substituted around one drawrooms call and taken straight
      // back out (camera.h). Unavailable until the detour is in AND a frame has proved the renderer is Polymer: a
      // switch that cannot do anything must say so.
      draw_group(cursor, "FREE CAMERA");
      const bool camOk = camera_available();
      g_cfg.freecamon = row_switch(cursor, camOk ? "Free camera"
                                                 : "Free camera (Unavailable)",
                                   g_cfg.freecamon, C_GREEN, camOk);
      row_hint("Fly a camera away from Duke. He stays where he is.");
      g_cfg.freecamspeed =
          row_slider_sub(cursor, "Flight speed", g_cfg.freecamspeed,
                         FREECAM_SPEED_MIN, FREECAM_SPEED_MAX, "%",
                         camOk && g_cfg.freecamon, FREECAM_SPEED_DEFAULT);
      row_hint("100% matches a running player.");
      // The whole map has its own Unavailable: the sweep needs four more located functions than the camera. Nocull is
      // the other half of the row: the Polymer sweep is REND_POLYMER-only, nocull runs where that cannot, so the row
      // reads Unavailable only when neither can act.
      const bool sweepOk = camera_wholemap_available() || !nocull_row_unavailable();
      g_cfg.freecamwholemap =
          row_switch_sub(cursor, sweepOk ? "Draw the whole map"
                                         : "Draw the whole map (Unavailable)",
                         g_cfg.freecamwholemap, C_GREEN,
                         sweepOk && g_cfg.freecamon);
      // The hint says what this build adds: geometry always; actors when the sprite pass located its arrays; the sky
      // when the call-site redirect went in (camera.cpp). Under Polymost the provider is nocull: actors yes
      // (polymost_scansector adds the sector's sprites, polymost.cpp:6593), sky no.
      const bool skyOk = camera_sky_available();
      const bool actorsOk = camera_sprites_available();
      if (nocull_located() && nocull_available()) {
        row_hint("Adds the skipped sectors and their actors. No sky out there.");
      } else {
        row_hint(skyOk ? (actorsOk
                              ? "Adds the sky and the skipped sectors, actors and all."
                              : "Adds the sky and the skipped sectors. No actors in them.")
                       : (actorsOk
                              ? "Adds the skipped sectors and their actors."
                              : "Adds the skipped sectors. No actors in them."));
      }
      // The cutaway's sky has its own Unavailable: a hook on polymost_domost that can refuse where the camera flies.
      // The hint says what each state gives, since the naming is counter-intuitive ("sky off" is the cleaner picture)
      // and read mid-flight.
      const bool cutSkyOk = !cutaway_row_unavailable();
      const bool wholeMapOn = g_cfg.freecamwholemap != 0;
      g_cfg.freecamcutawaysky =
          row_switch_sub(cursor, cutSkyOk ? "Cutaway sky"
                                          : "Cutaway sky (Unavailable)",
                         g_cfg.freecamcutawaysky, C_GREEN,
                         cutSkyOk && wholeMapOn && g_cfg.freecamon);
      row_hint("Off: black sky, no black slivers. On: some sky, some slivers.");
      // Retired rows (code, config keys, counters and pins untouched, inert defaults; a return is one row each): TROR
      // support, sweep budget, order and passes, Ceilings outside the map (parked); Cutaway parts, Unclamp wall paint,
      // Refuse tall walls, Drop masked walls, Freecam subst and ud.
      const bool fogOk = camera_fog_available();
      g_cfg.freecamfog =
          row_switch_sub(cursor, fogOk ? "Remove fog"
                                       : "Remove fog (Unavailable)",
                         g_cfg.freecamfog, C_GREEN, fogOk && g_cfg.freecamon);
      row_hint("Every sector keeps its own light, however far away.");
      const bool fbOk = camera_fullbright_available();
      g_cfg.freecamfullbright =
          row_switch_sub(cursor, fbOk ? "Fullbright"
                                      : "Fullbright (Unavailable)",
                         g_cfg.freecamfullbright, C_GREEN,
                         fbOk && g_cfg.freecamon);
      row_hint("Every surface at full brightness while the camera flies.");
      const bool tportOk = camera_transport_available();
      g_cfg.freecamtransport =
          row_switch_sub(cursor, tportOk ? "Follow teleporters"
                                         : "Follow teleporters (Unavailable)",
                         g_cfg.freecamtransport, C_GREEN,
                         tportOk && g_cfg.freecamon);
      row_hint("The camera is carried across teleporters and water.");
      g_cfg.freecamshowplayer =
          row_switch_sub(cursor, "Show player", g_cfg.freecamshowplayer,
                         C_GREEN, g_cfg.freecamon);
      row_hint("Marks your own body, so you can find it again.");
      g_cfg.freecamdebug =
          row_switch_sub(cursor, "Debug readout", g_cfg.freecamdebug, C_GREEN,
                         g_cfg.freecamon);
      row_hint("Shows the camera's position and angle while flying.");
      g_cfg.freecamteleportmarker = row_switch_sub(cursor, "Teleport marker",
                                                   g_cfg.freecamteleportmarker,
                                                   C_GREEN, g_cfg.freecamon != 0);
      row_hint("Where the teleport key would put you. Bind it first.");
      // Screenshot mode has its own Unavailable: the weapon and crosshair cvars must be reachable AND writable, which a
      // build can refuse. Level stats, the status bar and the scuba mask are bonuses, not terms (each rides a call-site
      // redirect that could refuse; the row stays, the log says which went).
      const bool shotOk = shot_available();
      g_cfg.freecamshot =
          row_switch_sub(cursor, shotOk ? "Screenshot mode"
                                        : "Screenshot mode (Unavailable)",
                         g_cfg.freecamshot, C_GREEN,
                         shotOk && g_cfg.freecamon);
      row_hint("Hides the gun, crosshair, stats, bar and scuba mask.");
      row_bind(cursor, "Camera key", BIND_ROW_CAMERA);
      row_hint("Press to fly. W A S D, space, ctrl, shift and alt fly it.");
      row_bind(cursor, "Teleport key", BIND_ROW_TELEPORT);
      row_hint("Press while flying to put Duke where you are looking.");
      row_bind(cursor, "Copy view key", BIND_ROW_CAMCOPY);
      row_hint("Copies where the camera is looking to the clipboard.");
      row_bind(cursor, "Paste view key", BIND_ROW_CAMPASTE);
      row_hint("Flies the camera to a viewpoint on the clipboard.");
      break;
    }
    case TAB_SETTINGS: {
      draw_group(cursor, "HOTKEYS");
      // The panel's own key: a hand-edited ini can leave an unmappable one; input.cpp falls back to Insert while the
      // row reports what is stored. The teleport and camera keys have their rows on the View tab, beside what they
      // belong to.
      row_bind(cursor, "Panel key", BIND_ROW_MENU);
      row_hint("The key that opens and closes this panel.");

      // Autosave: the first three rows store nothing: cl_autosave, cl_autosavedeletion, cl_maxautosaves are registered
      // cvars (osdcmds.cpp:1601), read live and written in place, so ekmod.ini holds no second answer. The fourth is
      // ours: EDuke32 autosaves at checkpoints, never on a clock (autosave.h).
      draw_group(cursor, "AUTOSAVE");
      {
        int autos = 0;
        const bool haveAuto = cvar_get_int("cl_autosave", &autos);
        const int nAuto = row_switch(cursor, haveAuto ? "Checkpoint autosave"
                                                      : "Checkpoint autosave (Unavailable)",
                                     autos, C_GREEN, haveAuto);
        if (haveAuto && nAuto != autos) cvar_set_int("cl_autosave", nAuto, &autos);
        row_hint("The game's own save when you reach a checkpoint.");

        int del = 0;
        const bool haveDel = cvar_get_int("cl_autosavedeletion", &del);
        const int nDel = row_switch(cursor, haveDel ? "Delete old autosaves"
                                                    : "Delete old autosaves (Unavailable)",
                                    del, C_GREEN, haveDel);
        if (haveDel && nDel != del) cvar_set_int("cl_autosavedeletion", nDel, &del);
        row_hint("Keeps the newest few and removes the rest.");

        // Gated on the row above, as the game's menu is (menus.cpp:2731). The track's ends are the record's own
        // vmin/vmax, so the row cannot offer a refused value. No reset glyph or typed field (EK_NONE): osdcvardata_t
        // has bounds but not the registered default, and the panel will not guess one.
        const EkCvar* const mx = cvars_find("cl_maxautosaves");
        const bool haveMax = cvar_row_available(mx);
        int keep = 0;
        if (haveMax) cvar_get_int("cl_maxautosaves", &keep);
        const int keepLo = haveMax ? mx->vmin : 0;
        const int keepHi = haveMax ? mx->vmax : 1;
        char keepText[24], keepReserve[24];
        sprintf_s(keepText, "%d", keep);
        sprintf_s(keepReserve, "%d", keepHi);
        const int nKeep = row_slider_at(cursor, haveMax ? "Autosaves kept"
                                                        : "Autosaves kept (Unavailable)",
                                        keep, keepLo, keepHi, keepText, keepReserve,
                                        haveMax && del != 0, SUB_INDENT, C_BONEDIM,
                                        EK_NONE, keep);
        if (haveMax && nKeep != keep) cvar_set_int("cl_maxautosaves", nKeep, &keep);
        row_hint("How many are kept before the oldest one goes.");

        const bool ts = autosave_ready();
        g_cfg.autosavemins = row_slider(cursor, ts ? "Autosave timer"
                                                   : "Autosave timer (Unavailable)",
                                        g_cfg.autosavemins,
                                        AUTOSAVE_MINS_MIN, AUTOSAVE_MINS_MAX, " min",
                                        ts, AUTOSAVE_MINS_DEFAULT);
        row_hint(ts ? "0 is off. Each one is a new autosave."
                    : "The game's autosave was not found. See ekmod.log.");
      }

      draw_group(cursor, "STARTUP");
      g_cfg.skipintro = row_switch(cursor, "Skip intro", g_cfg.skipintro, C_GREEN, true);
      row_hint("Takes effect the next time the game starts.");

      draw_group(cursor, "PANEL");
      {
        // The range is what this screen can apply; knob and readout show the applied scale. The reserve is the literal
        // "200%". Drawing is not a write: a stored value above the ceiling is kept, so a taller screen gives the player
        // their number back.
        const int ceiling = ui_scale_max_for(screenH, menu_ceiling_height(screenH), PANEL_VINSET);
        char scaleText[16];
        sprintf_s(scaleText, "%d%%", settled);
        const int v = row_slider_at(cursor, "UI scale", settled, UI_SCALE_MIN, ceiling,
                                    scaleText, "200%", true, 0, C_BONE,
                                    EK_INT, UI_SCALE_DEFAULT);
        if (v != settled) g_cfg.uiscale = v;
      }
      g_cfg.cornerscale = row_slider(cursor, "Corner text size",
                                     clamp_int(g_cfg.cornerscale, ESP_CORNER_SCALE_MIN,
                                               ESP_CORNER_SCALE_MAX),
                                     ESP_CORNER_SCALE_MIN, ESP_CORNER_SCALE_MAX, "%", true,
                                     ESP_CORNER_SCALE_DEFAULT);
      row_hint("Top-left text. Follows the panel's size while the panel is open.");
      g_cfg.hivis = row_switch(cursor, "High-visibility overlays", g_cfg.hivis, C_GREEN, true);
      row_hint("Black edges, thicker lines and outlined text on markers.");
      g_cfg.animations = row_switch(cursor, "Panel animations", g_cfg.animations, C_GREEN, true);
      row_hint("Fades and slides in the panel");

      draw_group(cursor, "DIAGNOSTICS");
      // Always live: how you find out what the rest did. Install-time lines are written regardless (a mod that fails to
      // load must say so); this adds the per-feature commentary.
      g_cfg.debuglog = row_switch(cursor, "Debug log", g_cfg.debuglog, C_GREEN, true);
      row_hint("Per-feature detail in ekmod.log. Off by default.");
      g_cfg.tracers = row_switch(cursor, "Shot tracers", g_cfg.tracers, C_GREEN, true);
      row_hint("Draws each shot: our predicted ray and the real impact.");
      g_cfg.perfzones = row_switch(cursor, "Frame timings", g_cfg.perfzones,
                                   C_GREEN, true);
      row_hint("Where each millisecond goes. Also written to the log.");
      g_cfg.perftable = row_switch_sub(cursor, "Show the table",
                                       g_cfg.perftable, C_GREEN,
                                       g_cfg.perfzones != 0);
      row_hint("The table costs more than the timings it reports.");
      g_cfg.perfdetail2 = row_switch_sub(cursor, "Per sector and wall",
                                         g_cfg.perfdetail2, C_GREEN,
                                         g_cfg.perfzones != 0);
      row_hint("Times each engine call. Use while hunting, not always.");
      g_cfg.secvolnodepth = row_switch(cursor, "Secret volumes ignore depth (diagnostic)",
                                       g_cfg.secvolnodepth, C_GREEN, true);
      row_hint("Draws secret panels through walls. Resets each launch.");
      break;
    }
  }

  canvas_pop_clip();
  g_ui.mx = bodyMx; g_ui.my = bodyMy;

  // The row-local containment checks only ran if the owning row drew: a tab change or a layout-dropped row (cursor.h <
  // ROW_H) skips them, so catch both here. No value change either way.
  if (s_dd.open && !s_ddSeenThisFrame) { s_dd.open = 0; s_dd.rowKey = -1; }
  if (s_entry.open && !s_entrySeenThisFrame) entry_clickaway_commit();

  // How far the cursor advanced, measured after the fact (a pre-pass would be a second layout to keep in agreement),
  // including the last ROW_GAP. The clamp is a frame behind on purpose: on a tab's first frame s_contentH is 0 and a
  // wheel notch clamps to 0. The offset is re-clamped here so a shrunk tab pulls it back.
  const int contentH = cursor.y - contentTop;
  s_contentH[s_tab] = contentH;
  s_scroll[s_tab] = menu_scroll_clamp(scroll, contentH, viewH);

  // The scrollbar: drawn only when scrollable, outside the clip as furniture, in the gutter rows already leave. C_WELL
  // is the track (this panel's sunken steel). Non-interactive by decision: a drag needs its own capture state.
  if (menu_scrollbar_visible(contentH, viewH)) {
    const int barX = p.x + p.w - 1 - 4 - MENU_SCROLLBAR_W;
    const int barY = bodyY + PAD;
    const int thumbH = menu_scroll_thumb_h(viewH, contentH, viewH);
    const int thumbY = menu_scroll_thumb_y(viewH, thumbH, scroll, contentH, viewH);
    canvas_rect(barX, barY, MENU_SCROLLBAR_W, viewH, pres_content_colour(C_WELL));
    canvas_rect(barX, barY + thumbY, MENU_SCROLLBAR_W, thumbH, pres_content_colour(C_RAISED));
  }

  // A tab that ran out of LAYOUT BUDGET says so once a session, before the tab changes. A row past the bottom is now
  // off-screen with a bar pointing at it; what scrolling cannot rescue is content past MENU_CONTENT_BUDGET, never laid
  // out (PANEL_MIN_H 740 cost two Settings rows unnoticed). The line reports what was laid out, never "exceeds".
  if (s_rowsDropped && !s_rowsDroppedLogged) {
    s_rowsDroppedLogged = true;
    ek_log("menu: tab %d exhausted the %d px layout budget (laid out %d px) at panel %dx%d -- rows not drawn",
           s_tab, MENU_CONTENT_BUDGET, contentH, box.w, box.h);
  }

  if (railTab >= 0) menu_set_tab(railTab);

  // The resize grip: three diagonals in the lower-right triangle of panel_grip_rect, after the content so no row covers
  // the corner, before the tooltip. C_BONEFNT at rest (furniture), C_BONEDIM while hovered.
  {
    const UiRect g = panel_grip_rect(box);
    const unsigned gc = pres_colour(gripHit ? C_BONEDIM : C_BONEFNT);
    for (int i = 1; i <= 3; i++) {
      const int off = i * 4;
      canvas_line(g.x + off, g.y + g.h - 1, g.x + g.w - 1, g.y + off, gc);
    }
  }

  // After the content, before the tooltip: no z-order, so a list at its own row would be painted over by every row
  // below.
  menu_dropdown_draw();

  // The tooltip, stepped once per frame here (the rule is about the whole panel: one row can be rested on).
  // `s_hoverRowKey` is -1 off any row, tooltip_step's "left" state. Last, on top of everything; either button puts it
  // away.
  s_tooltip = tooltip_step(s_tooltip, s_hoverRowKey, g_ui.pressed || g_ui.rpressed,
                           s_pres.lastMs, anim_style(), s_frameDt);
  const char* const hint = s_glyphHint ? s_glyphHint : s_hoverHint;
  s_tooltipText = nullptr;
  if (s_tooltip.shown && s_tooltip.alpha > 0.0 && hint && s_pres.openTarget) {
    const UiRect b = tooltip_box_place(g_ui.mx, g_ui.my,
                                       canvas_text_width(hint, FONT_SMALL),
                                       font_height[FONT_SMALL], logicalW, logicalH);
    canvas_round_rect(b.x, b.y, b.w, b.h, TOOLTIP_R,
                      alpha_scale(C_WELL, s_tooltip.alpha));
    canvas_round_rect_outline(b.x, b.y, b.w, b.h, TOOLTIP_R, 1,
                              alpha_scale(C_RAISED, s_tooltip.alpha));
    canvas_text(b.x + TOOLTIP_PAD, b.y + TOOLTIP_PAD, hint,
                alpha_scale(C_BONE, s_tooltip.alpha), FONT_SMALL);
    s_tooltipText = hint;
  }

  // A press no control took moves the panel: a left press inside that nothing claimed is a grab, continued by
  // panel_drag_step from the next frame; the last thing the frame decides. Claimed: every control, the grip and title
  // band (`!s_dragLive` makes the grip win). `g_ui.down` and `openTarget` keep a stray click from leaving a drag armed.
  if (g_ui.pressed && g_ui.down && !s_pressClaimed && !s_dragLive &&
      s_pres.openTarget && ui_hit(p, g_ui.mx, g_ui.my)) {
    s_drag.phase = PDRAG_MOVE;
    s_drag.start = box;
    s_drag.grabDx = g_ui.mx - box.x;
    s_drag.grabDy = g_ui.my - box.y;
    s_dragBox = box;
    s_dragLive = true;
  }

  g_ui.mx = deviceMx;
  g_ui.my = deviceMy;
  canvas_set_scale(UI_SCALE_DEFAULT);
}
