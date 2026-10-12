// Verbatim from Megaton's src/duke3dmod/ui.cpp; kept identical on purpose -- do not edit here.
#include "ui.h"
#include "common.h"

bool ui_hit(const UiRect& r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

UiRect ui_row(UiRect& cursor, int height, int gap) {
  UiRect out = { cursor.x, cursor.y, cursor.w, height };
  cursor.y += height + gap;
  cursor.h -= height + gap;
  return out;
}

int ui_slider_value(const UiRect& track, int mx, int lo, int hi) {
  if (track.w <= 0) return lo;
  const int dx = clamp_int(mx - track.x, 0, track.w);
  // Round to nearest so the midpoint of the track lands on the midpoint value.
  return lo + (dx * (hi - lo) + track.w / 2) / track.w;
}

int ui_slider_x(const UiRect& track, int value, int lo, int hi) {
  if (hi == lo) return track.x;
  const int v = clamp_int(value, lo, hi);
  return track.x + ((v - lo) * track.w + (hi - lo) / 2) / (hi - lo);
}

bool ui_press_edge(bool down, bool wasDown) {
  return down && !wasDown;
}

bool ui_key_is_initial_press(bool repeat) {
  return !repeat;
}

int ui_saved_toggle_value(bool panelOwnsInput, int openValue, int savedValue) {
  return panelOwnsInput ? openValue : savedValue;
}

bool ui_bytes_any_nonzero(const unsigned char* bytes, int count) {
  for (int i = 0; i < count; i++)
    if (bytes[i] != 0) return true;
  return false;
}

bool ui_row_accepts_click(bool enabled, bool buttonState, bool hit) {
  return enabled && buttonState && hit;
}
