// Copy of Megaton's panel_layout.h except PANEL_MIN_H.
#pragma once
#include "ui.h"
const int PANEL_W = 620, PANEL_H = 900, PANEL_INSET = 60, PANEL_VINSET = 30;
// PANEL_MIN_H = 790 (Megaton 740): Settings is the tallest tab and two key-clash notes can show at once.
// PANEL_MIN_W = 540: smallest multiple of 20 where row_slider_at's track clears the widest label.
const int PANEL_MIN_W = 540;
const int PANEL_MIN_H = 790;
const int PANEL_GRIP = 16;
struct PanelBox { int x, y, w, h; };
enum { PDRAG_IDLE = 0, PDRAG_MOVE = 1, PDRAG_RESIZE = 2 };
struct PanelDrag { int phase; int grabDx, grabDy; PanelBox start; };

inline PanelBox panel_default_box(int screenW, int screenH) {
  int w = PANEL_W, h = PANEL_H;
  if (h > screenH - 2 * PANEL_VINSET) h = screenH - 2 * PANEL_VINSET;
  if (w > screenW - 2 * PANEL_INSET) w = screenW - 2 * PANEL_INSET;
  PanelBox b = { PANEL_INSET, (screenH - h) / 2, w, h };
  return b;
}
inline int panel_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
// w or h <= 0 means unset: default box, keeping a stored non-negative x/y; then clamp on screen.
inline PanelBox panel_layout(PanelBox stored, int screenW, int screenH) {
  if (stored.w <= 0 || stored.h <= 0) {
    PanelBox d = panel_default_box(screenW, screenH);
    if (stored.x >= 0) d.x = stored.x;
    if (stored.y >= 0) d.y = stored.y;
    stored = d;
  }
  PanelBox b = stored;
  const int maxW = screenW, maxH = screenH;
  b.w = panel_clamp(b.w, PANEL_MIN_W < maxW ? PANEL_MIN_W : maxW, maxW);
  b.h = panel_clamp(b.h, PANEL_MIN_H < maxH ? PANEL_MIN_H : maxH, maxH);
  b.x = panel_clamp(b.x, 0, screenW - b.w);
  b.y = panel_clamp(b.y, 0, screenH - b.h);
  return b;
}
inline UiRect panel_grip_rect(PanelBox b) { UiRect r = { b.x + b.w - PANEL_GRIP, b.y + b.h - PANEL_GRIP, PANEL_GRIP, PANEL_GRIP }; return r; }
inline UiRect panel_title_rect(PanelBox b, int stripH, int statusH) { UiRect r = { b.x, b.y, b.w, 1 + stripH + statusH }; return r; }
inline PanelDrag panel_drag_step(PanelDrag d, bool pressed, bool down, bool titleHit, bool gripHit,
                                 int mx, int my, PanelBox current, int screenW, int screenH,
                                 PanelBox* box, bool* ended) {
  *ended = false; *box = current;
  if (d.phase == PDRAG_IDLE) {
    if (pressed && gripHit) { d.phase = PDRAG_RESIZE; d.start = current; d.grabDx = mx - (current.x + current.w); d.grabDy = my - (current.y + current.h); }
    else if (pressed && titleHit) { d.phase = PDRAG_MOVE; d.start = current; d.grabDx = mx - current.x; d.grabDy = my - current.y; }
    return d;
  }
  if (!down) { d.phase = PDRAG_IDLE; *ended = true; return d; }
  PanelBox b = d.start;
  if (d.phase == PDRAG_MOVE) { b.x = mx - d.grabDx; b.y = my - d.grabDy; }
  else {
    b.w = (mx - d.grabDx) - b.x; b.h = (my - d.grabDy) - b.y;
    // Floor of 1: w/h <= 0 reads as unset, so a resize past the minimum must not jump to the default box.
    if (b.w < 1) b.w = 1;
    if (b.h < 1) b.h = 1;
  }
  *box = panel_layout(b, screenW, screenH);
  return d;
}
