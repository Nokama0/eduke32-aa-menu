// Verbatim from Megaton's ui.h.
#pragma once
#include "common.h"

struct UiRect { int x, y, w, h; };

struct UiInput {
  int  mx, my;
  bool down;
  bool pressed;
  // Right-click on a slider row opens the numeric entry (entry.h).
  bool rdown;
  bool rpressed;
};

bool   ui_hit(const UiRect& r, int x, int y);
UiRect ui_row(UiRect& cursor, int height, int gap);
int    ui_slider_value(const UiRect& track, int mx, int lo, int hi);
int    ui_slider_x(const UiRect& track, int value, int lo, int hi);

bool   ui_press_edge(bool down, bool wasDown);

// False for an SDL auto-repeat; one-shot key actions (panel toggle, Escape) must gate on this.
bool   ui_key_is_initial_press(bool repeat);

// "Save real state, force it while the panel owns input, restore on close" toggle (SDL relative mouse, cursor
// visibility). Never invents a default: the caller supplies a value it observed.
int    ui_saved_toggle_value(bool panelOwnsInput, int openValue, int savedValue);

bool   ui_bytes_any_nonzero(const unsigned char* bytes, int count);

// A disabled row is inert, not dimmed: a click on a greyed sub-row would change a setting the player was told is
// unavailable.
bool   ui_row_accepts_click(bool enabled, bool buttonState, bool hit);

// The panel is laid out in logical pixels and the canvas multiplies on the way out. Floor 75: at 50 a switch is 10px.
const int UI_SCALE_MIN     = 75;
const int UI_SCALE_MAX     = 200;
const int UI_SCALE_DEFAULT = 100;

inline int ui_scale_clamped(int pct) { return clamp_int(pct, UI_SCALE_MIN, UI_SCALE_MAX); }

inline int ekm_scale_needs_atlas(int pct) {
  return ui_scale_clamped(pct) != UI_SCALE_DEFAULT ? 1 : 0;
}

inline int ekm_text_uses_scaled_atlas(int pct, bool magnify) {
  return (!magnify && ekm_scale_needs_atlas(pct)) ? 1 : 0;
}

// Scaled-text slots: one reserved rect per scale that can share a frame (panel, ESP corner), so neither magnifies.
const int CANVAS_SLOT_PANEL  = 0;
const int CANVAS_SLOT_CORNER = 1;
const int CANVAS_SLOT_COUNT  = 2;

inline int ekm_text_slot_rung(int slot) { return slot == CANVAS_SLOT_CORNER ? 2 : 3; }

inline int ekm_text_slot(int pct, bool magnify, int slot, bool reserved, int failedPct) {
  if (!ekm_text_uses_scaled_atlas(pct, magnify)) return -1;
  if (slot < 0 || slot >= CANVAS_SLOT_COUNT || !reserved) return -1;
  if (ui_scale_clamped(pct) == failedPct) return -1;
  return slot;
}

inline int ekm_slot_needs_build(int pct, int heldPct) {
  return (ekm_scale_needs_atlas(pct) && ui_scale_clamped(pct) != heldPct) ? 1 : 0;
}

// Round half away from zero: 1px bevels vanish at 75% if truncated. Widened first: project_world saturates.
inline int ui_scale_px(int px, int pct) {
  const long long p = (long long)ui_scale_clamped(pct);
  const long long v = (long long)px * p;
  return clamp_ll_to_int(v >= 0 ? (v + 50) / 100 : (v - 50) / 100);
}

inline int ui_unscale_px(int px, int pct) {
  const long long p = (long long)ui_scale_clamped(pct);
  const long long v = (long long)px * 100;
  return clamp_ll_to_int(v >= 0 ? (v + p / 2) / p : (v - p / 2) / p);
}

// Largest scale at which the panel's logical height fits the screen (else rows, once the scale row itself, clip away).
// Closed form as menu_draw calls it per frame: pct <= (100*screenH + 49) / total, pinned by the suite; snapped down to
// a multiple of five before the clamp so both ends stay reachable.
inline int ui_scale_max_for(int screenH, int panelLogicalH, int insetLogical) {
  const long long total = (long long)panelLogicalH + 2LL * (long long)insetLogical;
  if (total <= 0) return UI_SCALE_MAX;
  long long pct = ((long long)screenH * 100 + 49) / total;
  pct -= pct % 5;
  return clamp_int(clamp_ll_to_int(pct), UI_SCALE_MIN, UI_SCALE_MAX);
}

// Frozen while the left button is held: row_slider_at has no drag capture, so a layout that moves with the value slides
// the row from under the cursor and the value slams between the ends. Resizes on release.
inline int ui_scale_effective(int stored, int applied, bool dragging) {
  return dragging ? applied : stored;
}

// Tallest logical panel keeping the ceiling at or above `pct`, so a resize drag cannot drop the stored scale (clamped
// in menu.cpp). Bisection: the ceiling is non-increasing in height. pct <= UI_SCALE_MIN: INT_MAX.
inline int ui_panel_max_h_for(int screenH, int pct, int insetLogical) {
  const int pctC = ui_scale_clamped(pct);
  if (pctC <= UI_SCALE_MIN) return 0x7fffffff;
  long long lo = 0;
  long long hi = (long long)screenH * 100 + 100000;
  if (ui_scale_max_for(screenH, clamp_ll_to_int(lo), insetLogical) < pctC) return 0;
  while (lo < hi) {
    const long long mid = lo + (hi - lo + 1) / 2;
    if (ui_scale_max_for(screenH, clamp_ll_to_int(mid), insetLogical) >= pctC) lo = mid;
    else hi = mid - 1;
  }
  return clamp_ll_to_int(lo);
}
