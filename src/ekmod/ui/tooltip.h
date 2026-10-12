// Copy of Megaton's tooltip.h; keep identical.
#pragma once
#include "ui.h"
#include "anim.h"

struct TooltipState { int rowKey; unsigned restSinceMs; bool shown; bool pressedHide; double alpha; };
// 150 ms: 400 made deliberate movement outrun the hint.
const unsigned TOOLTIP_DELAY_MS = 150;
const int TOOLTIP_PAD = 8, TOOLTIP_DX = 16, TOOLTIP_DY = 20;

inline TooltipState tooltip_begin() {
  TooltipState s; s.rowKey = -1; s.restSinceMs = 0; s.shown = false; s.pressedHide = false; s.alpha = 0.0;
  return s;
}

// noinline: MSVC 19.50 x86 /O2 folds literal-argument calls and loses a byte bool in the by-value return.
__declspec(noinline) inline TooltipState tooltip_step(TooltipState s, int hoveredRowKey, bool pressed, unsigned nowMs,
                                 int animStyle, double dtMs) {
  if (hoveredRowKey != s.rowKey) {
    s.rowKey = hoveredRowKey; s.restSinceMs = nowMs; s.shown = false; s.pressedHide = false; s.alpha = 0.0;
    return s;
  }
  if (pressed) { s.pressedHide = true; s.shown = false; s.alpha = 0.0; return s; }
  if (s.rowKey < 0 || s.pressedHide) { s.shown = false; s.alpha = 0.0; return s; }
  if (!s.shown && nowMs - s.restSinceMs >= TOOLTIP_DELAY_MS) { s.shown = true; s.alpha = 0.0; }
  if (s.shown) {
    if (animStyle == ANIM_OFF) s.alpha = 1.0;
    else {
      s.alpha += dtMs / (double)ANIM_HOVER_IN_MS;
      if (s.alpha > 1.0) s.alpha = 1.0;
    }
  }
  return s;
}

inline UiRect tooltip_box_place(int mx, int my, int textW, int textH, int screenW, int screenH) {
  UiRect b; b.w = textW + 2 * TOOLTIP_PAD; b.h = textH + 2 * TOOLTIP_PAD;
  b.x = mx + TOOLTIP_DX; b.y = my + TOOLTIP_DY;
  if (b.x + b.w > screenW) b.x = mx - TOOLTIP_DX - b.w;
  if (b.y + b.h > screenH) b.y = my - TOOLTIP_DY - b.h;
  if (b.x < 0) b.x = 0;
  if (b.y < 0) b.y = 0;
  return b;
}
