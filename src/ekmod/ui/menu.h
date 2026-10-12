// Started as a copy of Megaton's menu.h and has diverged: the tab set, menu_live_count's arguments, the entry field,
// the keycap, panel_layout and the scroll rules are this port's own. Megaton is still worth reading for a row's
// SHAPE.
#pragma once
#include "ui.h"
#include "common.h"
#include "input.h"
#include "tbot.h"
#include "cutaway.h"
#include "freecam.h"
#include "panel_layout.h"

// TAB_SETTINGS is not a cheat tab: nothing on it reaches menu_live_count.
enum { TAB_VISUALS = 0, TAB_COMBAT = 1, TAB_WEAPONS = 2, TAB_ITEMS = 3, TAB_VIEW = 4,
       TAB_SETTINGS = 5, TAB_COUNT = 6 };

UiRect menu_panel_rect(int screenW, int screenH);
// The box the panel is drawn at: panel_layout's answer for the four stored ini keys, or the live box a drag/resize is
// producing. Both DEVICE pixels (the raw screen size), not logical: the height cap needs the device screen; the
// logical conversion happens once, inside menu_panel_box.
PanelBox menu_panel_box(int screenW, int screenH);
int    menu_active_tab();
void   menu_set_tab(int tab);
void   menu_draw(int screenW, int screenH);
int    menu_applied_scale(int screenH);
int    menu_frame_scale(int screenH);

// How many cheat categories are live (hazard strip, "N cheats live"). Counts PARENT switches only, never a
// sub-setting that does nothing until its parent is on, else a fresh install would report cheats live. A row that
// walks its own data (espcash, breakables) is a parent. New parents join as trailing defaulted arguments.
inline int menu_live_count(int fovon, int espenemies, int espitems, int espsecrets,
                           int espexplodables, int espswitches,
                           int aimon, int silenton, int freecamon,
                           int accpistol, int accchaingun, int accshotgun, int accexpander,
                           int dmgnumbers, int secrettriggers, int tboton,
                           int espbreakables = 0, int espfriendlies = 0,
                           int accincinerator = 0, int espcash = 0,
                           int espfigurines = 0, int figurinecounter = 0,
                           int espchests = 0) {
  return (fovon       ? 1 : 0) + (espenemies  ? 1 : 0)
       + (espitems    ? 1 : 0) + (espsecrets  ? 1 : 0)
       + (espexplodables ? 1 : 0) + (espswitches ? 1 : 0)
       + (aimon       ? 1 : 0) + (silenton    ? 1 : 0)
       + (freecamon   ? 1 : 0)
       + (accpistol   ? 1 : 0) + (accchaingun ? 1 : 0)
       + (accshotgun  ? 1 : 0) + (accexpander ? 1 : 0)
       + (dmgnumbers  ? 1 : 0) + (secrettriggers ? 1 : 0)
       + (tboton      ? 1 : 0) + (espbreakables  ? 1 : 0)
       + (espfriendlies ? 1 : 0) + (accincinerator ? 1 : 0)
       + (espcash      ? 1 : 0)
       + (espfigurines ? 1 : 0) + (figurinecounter ? 1 : 0)
       + (espchests    ? 1 : 0);
}

// Menu log lines: single words the log can be grepped for, logged as the last line of open_menu()/close_menu().
// Named here since input.cpp can't link into test_host.
const char* const MENU_LOG_OPEN  = "menu: open";
const char* const MENU_LOG_CLOSE = "menu: close";

const char* const MENU_KEYCAP_PROMPT = "Press a key";
const char* const MENU_GAMECFG_NOTE  = "Also bound in the game";
const char* const MENU_KEYCAP_HOVER  = "Click to set";

const char* const MENU_RESET_HINT = "Reset to default";

// Stale stub: not called from anywhere; the real parser is gamecfg.h/.cpp. Kept only because menu.cpp still defines
// it.
void menu_gamecfg_load(const char* path);

bool menu_keycap_armed();

void menu_keycap_bind(int vk);

void menu_keycap_cancel();

// Disarms because the panel is closing; a capture left armed across a close would eat the next key pressed in-game.
void menu_keycap_close();

// One numeric entry field for the whole panel (only one row can be typed into at a time). Rules in entry.h.

bool menu_entry_open();

void menu_entry_key(int sc);

// The function input.cpp's poll hook calls (menu_entry_key is its shift=ctrl=false case). Returns the text to put on
// the clipboard for Ctrl+C/X, else nullptr; the syscall is input.cpp's. Ctrl+V goes through menu_entry_paste.
const char* menu_entry_key_mod(int sc, bool shift, bool ctrl);

// Puts `s` into the open field over the selection through the typed-character rule: a pasted "FOV: 90" delivers "90".
void menu_entry_paste(const char* s);

void menu_entry_begin_edit(int rowKey, int kind, int lo, int hi, const char* text);

// Closes the field because the panel is closing: input.cpp's routing sits above its menu-open check, so an open field
// would keep eating keys.
void menu_entry_close();

const char* menu_entry_text();

bool menu_entry_pending(int* rowKey, int* value);

void menu_set_now_ms(unsigned ms);

// True while the panel is closing under animation: logical state gone but the open spring not settled, so overlay.cpp
// keeps drawing and it fades out. Always false under ANIM_OFF.
bool menu_closing_animation_active();

const char* menu_tooltip_text();

const int RAIL_W = 116;
const int RAIL_LABEL_X = 12;
const int MENU_RAIL_ENTRY_H = 46;

const int STRIP_H = 15, STATUS_H = 30, ROW_H = 30, ROW_GAP = 4, PAD = 14;

const int VALUE_MARGIN = 10;

// The tab body scrolls. The panel once ran out of room and silently dropped rows (no scrollbar, no ellipsis); neither
// sibling port has scrolling, so these rules are this port's own.

const int MENU_SCROLL_STEP = 3 * (ROW_H + ROW_GAP);

// The height a tab's content is laid out into, far taller than any panel: the offset's limit is contentH - viewH, so
// a budget equal to the visible height could never scroll.
const int MENU_CONTENT_BUDGET = 4096;

const int MENU_SCROLLBAR_W = 5;
const int MENU_SCROLLBAR_MIN_THUMB = 20;

// Where menu_draw parks the logical cursor while content draws with the real cursor outside the body: the clip rect
// stops a scrolled-out row being DRAWN, not CLICKED (ui_hit tests the logical rect). It must be below anything a row
// can reach (PAD - MENU_CONTENT_BUDGET at worst).
const int MENU_MOUSE_NOWHERE = -1000000;

inline int menu_scroll_max(int contentH, int viewH) {
  const int over = contentH - viewH;
  return over > 0 ? over : 0;
}

inline int menu_scroll_clamp(int offset, int contentH, int viewH) {
  return clamp_int(offset, 0, menu_scroll_max(contentH, viewH));
}

// Where a wheel notch leaves the offset: ImGui reports the wheel POSITIVE when pushed away, which walks UP the tab,
// so the notch is subtracted. Widened before rounding so a pathological float cannot wrap the int.
inline int menu_scroll_after_wheel(int offset, double wheel, int contentH, int viewH) {
  const double moved = (double)offset - wheel * (double)MENU_SCROLL_STEP;
  return menu_scroll_clamp(clamp_double_to_int(moved >= 0.0 ? moved + 0.5 : moved - 0.5),
                           contentH, viewH);
}

inline int menu_scroll_thumb_h(int trackH, int contentH, int viewH) {
  if (trackH <= 0 || contentH <= 0 || viewH <= 0) return 0;
  if (viewH >= contentH) return trackH;
  long long h = ((long long)trackH * (long long)viewH) / (long long)contentH;
  if (h < MENU_SCROLLBAR_MIN_THUMB) h = MENU_SCROLLBAR_MIN_THUMB;
  if (h > trackH) h = trackH;
  return (int)h;
}

// The thumb's top derives from the TRAVEL (trackH - thumbH) against the maximum offset, not the content fraction:
// once the length is floored they disagree, and the flush bottom at max offset reads as the last row.
inline int menu_scroll_thumb_y(int trackH, int thumbH, int offset,
                               int contentH, int viewH) {
  const int maxOff = menu_scroll_max(contentH, viewH);
  const int travel = trackH - thumbH;
  if (maxOff <= 0 || travel <= 0) return 0;
  const int off = clamp_int(offset, 0, maxOff);
  return (int)(((long long)travel * (long long)off + maxOff / 2) / (long long)maxOff);
}

inline bool menu_scrollbar_visible(int contentH, int viewH) {
  return menu_scroll_max(contentH, viewH) > 0;
}

// Whether `inner` is inside (or flush with) `outer`: closes an open dropdown or typed field once its row leaves the
// scrolled body; touching an edge counts as visible (the clip rect is inclusive).
inline bool ui_rect_inside(const UiRect& inner, const UiRect& outer) {
  return inner.x >= outer.x && inner.y >= outer.y &&
         inner.x + inner.w <= outer.x + outer.w &&
         inner.y + inner.h <= outer.y + outer.h;
}

struct MenuDdState { int open; int rowKey; };

// The per-row dropdown decision, pure so it can be pinned. `*seen` is computed from the RETURNED state, so the frame
// that opens a dropdown is not mistaken for an unowned one and closed by the catch-all in menu_draw.
inline MenuDdState menu_dd_row_step(MenuDdState s, int key, bool hot,
                                     bool pressed, bool boxInside, bool* seen) {
  const bool mine = s.open != 0 && s.rowKey == key;
  MenuDdState out = s;
  if (hot && pressed) {
    if (mine) { out.open = 0; out.rowKey = -1; }
    else { out.open = 1; out.rowKey = key; }
  } else if (mine && !boxInside) {
    out.open = 0; out.rowKey = -1;
  }
  if (seen) *seen = out.open != 0 && out.rowKey == key;
  return out;
}

// Whether a Combat-tab sub-row is enabled: its parent switch's config value is nonzero (aimon, silenton).
inline bool combat_sub_enabled(int parentValue) { return parentValue != 0; }

// Whether the TARGETING group is enabled: its rows are shared by both aim modes (aim_acquire_should_run), so either
// parent enables them.
inline bool combat_shared_enabled(int aimon, int silenton) {
  return aimon != 0 || silenton != 0;
}

// "Pick target" shows a NAME, not a switch: any nonzero value reads as "closest".
inline const char* aim_target_label(int aimtarget) {
  return aimtarget ? "Closest" : "Crosshair";
}

inline const char* aim_point_label(int aimpoint) {
  return aimpoint ? "Head" : "Centre";
}

inline const char* tbot_when_label(int when) {
  return tbot_when_clamped(when) == TBOT_WHEN_AIMBOT ? "Aimbot locked" : "Crosshair on enemy";
}

