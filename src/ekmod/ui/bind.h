// From Megaton's src/duke3dmod/bind.h. DIVERGED in the enum below only (two extra rows); otherwise identical, do not
// edit.
#pragma once
#include "common.h"

// Every binding is a WINDOWS VIRTUAL KEY, mouse buttons included, so one capture/label/save/lookup serves all rows.
// Mouse codes are the real Windows ones (0x01/02/04/05/06, outside vk_to_sdl_scancode's table). SDL numbers right 3 and
// middle 2 where Windows has 0x02 and 0x04: a cast swaps them, hence bind_from_sdl_mouse.
const int VK_BIND_LBUTTON = 0x01, VK_BIND_RBUTTON = 0x02;
const int VK_BIND_MBUTTON = 0x04, VK_BIND_XBUTTON1 = 0x05, VK_BIND_XBUTTON2 = 0x06;

// The rows, in config_bind_get/set's order (config.cpp). Append, never insert: config.cpp's switch is written against
// the index.
enum { BIND_ROW_MENU = 0, BIND_ROW_AIM, BIND_ROW_CAMERA, BIND_ROW_TELEPORT,
       BIND_ROW_CAMCOPY, BIND_ROW_CAMPASTE, BIND_ROW_COUNT };

// Whether a binding is a mouse button this panel accepts. Mouse 1 is not: it is the click that arms a capture.
inline bool bind_is_mouse(int vk) {
  return vk == VK_BIND_RBUTTON || vk == VK_BIND_MBUTTON
      || vk == VK_BIND_XBUTTON1 || vk == VK_BIND_XBUTTON2;
}

// The SDL button number a mouse binding means, or -1 (key, unbound, mouse 1, VK_CANCEL 0x03). Mouse 1 is -1 so
// input_bind_held refuses it independently.
inline int bind_mouse_sdl_button(int vk) {
  switch (vk) {
    case VK_BIND_MBUTTON:  return 2;
    case VK_BIND_RBUTTON:  return 3;
    case VK_BIND_XBUTTON1: return 4;
    case VK_BIND_XBUTTON2: return 5;
    default:               return -1;
  }
}

// The inverse: the binding an SDL button means, or 0 for a button this DLL does not track (dropped, not clamped).
inline int bind_from_sdl_mouse(int sdlButton) {
  switch (sdlButton) {
    case 2: return VK_BIND_MBUTTON;
    case 3: return VK_BIND_RBUTTON;
    case 4: return VK_BIND_XBUTTON1;
    case 5: return VK_BIND_XBUTTON2;
    default: return 0;
  }
}

// The binding an SDL SCANCODE means, or 0. Swept over vk_to_sdl_scancode rather than a second table, so a key added to
// common.h cannot go unnoticed. Once per captured key press.
inline int bind_from_sdl_scancode(int sc) {
  for (int vk = 0; vk <= 255; vk++) {
    int s = -1;
    if (vk_to_sdl_scancode(vk, &s) && s == sc) return vk;
  }
  return 0;
}

inline bool bind_matches_key(int vk, int sdlScancode) {
  if (vk == 0 || bind_is_mouse(vk)) return false;
  int sc = -1;
  return vk_to_sdl_scancode(vk, &sc) && sc == sdlScancode;
}

inline bool bind_matches_mouse(int vk, int sdlButton) {
  return bind_is_mouse(vk) && bind_mouse_sdl_button(vk) == sdlButton;
}

// The panel must stay reachable: a menukey with no bindable button or scancode falls back to Insert (config_load clamps
// menukey to 1..254, so menukey=0 lands on VK_BIND_LBUTTON). Every other binding is left unbound instead.
inline int bind_menu_key_effective(int vk) {
  int sc = -1;
  if (bind_is_mouse(vk) || vk_to_sdl_scancode(vk, &sc)) return vk;
  return 0x2D;
}

// Whether a binding can be acted on: the ONE predicate the panel, input.cpp's swallows and a hand-edited ini agree
// with, so a row cannot show Unbound while eating a key.
inline bool bind_key_is_live(int vk) {
  int sc = -1;
  return bind_is_mouse(vk) || vk_to_sdl_scancode(vk, &sc);
}

inline bool bind_hold_is_active(int vk, bool keyDown) {
  return bind_key_is_live(vk) && keyDown;
}

// ek_WndProc's swallow test; windowOwned is read by the caller (the toggle's own row, camera_available() or
// camera_owns_input()).
inline bool bind_swallows(int pressedVk, int boundVk, bool windowOwned) {
  return windowOwned && bind_key_is_live(boundVk) && pressedVk == boundVk;
}

// While EDuke32's console takes typing every hotkey of ours yields (it reads SDL_KEYDOWN). Not while the panel is open:
// it eats every other key, so a yielded panel key could never close it.
inline bool hotkey_yields_to_console(bool consoleCapturing, bool panelOpen) {
  return consoleCapturing && !panelOpen;
}

inline bool hotkey_swallows(int pressedVk, int boundVk, bool windowOwned,
                            bool consoleCapturing, bool panelOpen) {
  return !hotkey_yields_to_console(consoleCapturing, panelOpen) &&
         bind_swallows(pressedVk, boundVk, windowOwned);
}

// "Unbound" is the honest answer for a key vk_to_sdl_scancode rejects (a modifier from a hand-edited ini): "F6" over a
// dead key would be worse. Pinned by a sweep of every byte.
inline const char* freecam_key_label(int vk) {
  switch (vk) {
    case 0x2D: return "Insert";
    case 0x2E: return "Delete";
    case 0x24: return "Home";
    case 0x23: return "End";
    case 0x21: return "Page up";
    case 0x22: return "Page down";
    case 0x09: return "Tab";
    case 0xC0: return "Grave";
    case 0x54: return "T";
    case 0x56: return "V";
    case 0x70: return "F1";
    case 0x71: return "F2";
    case 0x72: return "F3";
    case 0x73: return "F4";
    case 0x74: return "F5";
    case 0x75: return "F6";
    case 0x76: return "F7";
    case 0x77: return "F8";
    case 0x78: return "F9";
    case 0x79: return "F10";
    case 0x7A: return "F11";
    case 0x7B: return "F12";
    case 0x41: return "A";
    case 0x42: return "B";
    case 0x43: return "C";
    case 0x44: return "D";
    case 0x45: return "E";
    case 0x46: return "F";
    case 0x47: return "G";
    case 0x48: return "H";
    case 0x49: return "I";
    case 0x4A: return "J";
    case 0x4B: return "K";
    case 0x4C: return "L";
    case 0x4D: return "M";
    case 0x4E: return "N";
    case 0x4F: return "O";
    case 0x50: return "P";
    case 0x51: return "Q";
    case 0x52: return "R";
    case 0x53: return "S";
    case 0x55: return "U";
    case 0x57: return "W";
    case 0x58: return "X";
    case 0x59: return "Y";
    case 0x5A: return "Z";
    case 0x30: return "0";
    case 0x31: return "1";
    case 0x32: return "2";
    case 0x33: return "3";
    case 0x34: return "4";
    case 0x35: return "5";
    case 0x36: return "6";
    case 0x37: return "7";
    case 0x38: return "8";
    case 0x39: return "9";
    case 0x0D: return "Enter";
    case 0x08: return "Backspace";
    case 0x20: return "Space";
    case 0xBD: return "-";
    case 0xBB: return "=";
    case 0xDB: return "[";
    case 0xDD: return "]";
    case 0xDC: return "\\";
    case 0xBA: return ";";
    case 0xDE: return "'";
    case 0xBC: return ",";
    case 0xBE: return ".";
    case 0xBF: return "/";
    case 0x13: return "Pause";
    case 0x25: return "Left";
    case 0x27: return "Right";
    case 0x26: return "Up";
    case 0x28: return "Down";
    case 0x6F: return "Num /";
    case 0x6A: return "Num *";
    case 0x6D: return "Num -";
    case 0x6B: return "Num +";
    case 0x60: return "Num 0";
    case 0x61: return "Num 1";
    case 0x62: return "Num 2";
    case 0x63: return "Num 3";
    case 0x64: return "Num 4";
    case 0x65: return "Num 5";
    case 0x66: return "Num 6";
    case 0x67: return "Num 7";
    case 0x68: return "Num 8";
    case 0x69: return "Num 9";
    case 0x6E: return "Num .";
    default:   return "Unbound";
  }
}

inline const char* bind_label(int vk) {
  switch (vk) {
    case VK_BIND_RBUTTON:  return "Mouse 2";
    case VK_BIND_MBUTTON:  return "Mouse 3";
    case VK_BIND_XBUTTON1: return "Mouse 4";
    case VK_BIND_XBUTTON2: return "Mouse 5";
    default:               return freecam_key_label(vk);
  }
}

const char* const BIND_LABEL_WIDEST = "Page down";

// A bind field's text by precedence: ARMED wins over HOVERED (a capture keeps saying so wherever the cursor drifts),
// then the key's name. The strings are parameters: menu.h owns the copy.
inline const char* bind_field_text(bool armed, bool hovered, int vk,
                                   const char* prompt, const char* hoverText) {
  if (armed) return prompt;
  if (hovered) return hoverText;
  return bind_label(vk);
}

enum { KEYCAP_IDLE = 0, KEYCAP_ARMED = 1 };
enum { KEYCAP_EV_NONE = 0, KEYCAP_EV_CLICK, KEYCAP_EV_KEY,
       KEYCAP_EV_MOUSE, KEYCAP_EV_ESCAPE, KEYCAP_EV_CLOSE };
struct KeyCapState  { int phase; int row; };
struct KeyCapEvent  { int kind; int row; int vk; };
// `consumed` is about the event, not the state: a bind and a cancel are consumed so the key does not also reach the
// toggle, the game or the panel's close.
struct KeyCapResult { KeyCapState state; int boundRow; int boundVk; bool consumed; };

inline KeyCapResult keycap_result(KeyCapState s, int boundRow, int boundVk, bool consumed) {
  KeyCapResult r;
  r.state = s; r.boundRow = boundRow; r.boundVk = boundVk; r.consumed = consumed;
  return r;
}

inline KeyCapResult keycap_step(KeyCapState s, KeyCapEvent e) {
  const KeyCapState idle = { KEYCAP_IDLE, -1 };
  switch (e.kind) {
    case KEYCAP_EV_CLICK: {
      if (e.row < 0 || e.row >= BIND_ROW_COUNT) return keycap_result(s, -1, 0, false);
      const KeyCapState armed = { KEYCAP_ARMED, e.row };
      return keycap_result(armed, -1, 0, false);
    }
    case KEYCAP_EV_KEY:
    case KEYCAP_EV_MOUSE:
      if (s.phase != KEYCAP_ARMED) return keycap_result(s, -1, 0, false);
      // 0 would silently unbind the row; mouse 1 armed it. Both leave the capture armed.
      if (e.vk == 0 || e.vk == VK_BIND_LBUTTON) return keycap_result(s, -1, 0, false);
      return keycap_result(idle, s.row, e.vk, true);
    case KEYCAP_EV_ESCAPE:
      // Cancels, keeps the old binding. Consumed while armed (or the press would also close the panel); not while idle.
      if (s.phase != KEYCAP_ARMED) return keycap_result(s, -1, 0, false);
      return keycap_result(idle, -1, 0, true);
    case KEYCAP_EV_CLOSE:
      return keycap_result(idle, -1, 0, false);
    case KEYCAP_EV_NONE:
    default:
      return keycap_result(s, -1, 0, false);
  }
}
