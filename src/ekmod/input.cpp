// Take input while the panel is open and give it back when it closes. The WndProc is subclassed, not the SDL loop; a
// swallowed message never reaches SDL. SDL's relative mode warps the cursor to the centre every frame, so we accumulate
// WM_INPUT deltas into our own position. Window thread only; interlocked scalars cross to the render thread.

#include <windows.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "log.h"
#include "input.h"
#include "config.h"
#include "camera.h"
#include "campos.h"
#include "clipboard.h"
#include "ui/menu.h"
#include "ui/bind.h"
#include "ui/entry.h"
#include "gamecfg.h"
#include "console.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static WNDPROC s_origProc = NULL;
static HWND s_wnd = NULL;
static volatile LONG s_open = 0;

static volatile LONG s_mouseX = 0;
static volatile LONG s_mouseY = 0;

// Which mouse source is real now: SDL uses raw input in relative mode, WM_MOUSEMOVE otherwise; raw input wins WHILE IT
// IS ARRIVING. Aged by timestamp, not latched: the pause menu leaves relative mode and a latch froze the cursor at its
// seed. Not reset on open: the first message may be a warp's WM_MOUSEMOVE.
static volatile LONG s_sawRawInput = 0;
static volatile LONG s_lastRawMs = 0;

static const DWORD EK_RAW_LIVE_MS = 1000;

// WM_INPUT and WM_MOUSEMOVE counts while the panel is open (input_debug_take). Nothing drains them with the debug log
// off, so they roll at a ceiling instead of overflowing a signed LONG.
static volatile LONG s_rawCount = 0;
static volatile LONG s_moveCount = 0;
static const LONG EK_DEBUG_COUNT_MAX = 1000000;

static void bump_debug_count(volatile LONG* c) {
  if (InterlockedIncrement(c) > EK_DEBUG_COUNT_MAX) InterlockedExchange(c, 0);
}

static bool raw_input_is_live(void) {
  if (!InterlockedCompareExchange(&s_sawRawInput, 0, 0)) return false;
  const DWORD last = (DWORD)InterlockedCompareExchange(&s_lastRawMs, 0, 0);
  return (DWORD)(GetTickCount() - last) < EK_RAW_LIVE_MS;
}

// The panel key is the player's (Settings bind row); falls back to Insert for an unmappable value so the panel is never
// unreachable.
static int panel_key(void) {
  return bind_menu_key_effective(config_bind_get(0));
}

static int cam_key(void) { return config_bind_get(BIND_ROW_CAMERA); }

static int cam_copy_key(void)  { return config_bind_get(BIND_ROW_CAMCOPY); }
static int cam_paste_key(void) { return config_bind_get(BIND_ROW_CAMPASTE); }

static int cam_teleport_key(void) { return config_bind_get(BIND_ROW_TELEPORT); }

// Window thread only. Who owns the keyboard owes the game a release: release_held_keys reads PHYSICAL key state, and
// while the mod owns input the game is told nothing, so a second claim would forge releases for keys it never saw
// pressed (opening the panel with W held mid-flight did). freecam.h's ekm_input_owned_edge owns the rule.
static LONG s_modOwnsInput = 0;

static bool camera_owns_input(void);

static bool claim_input(bool nowOwned) {
  const LONG now = nowOwned ? 1 : 0;
  const bool need = ekm_input_owned_edge(s_modOwnsInput, now);
  s_modOwnsInput = now;
  return need;
}

// Is this a key the mod SWALLOWS, so the game never sees it pressed? The one place that answers it; release_held_keys
// must never forge a release for one. The camera key is ours only while the row is on and the draw hook installed. The
// flight keys are absent: they are swallowed only while camera_owns_input() holds, which the claim above covers.
static bool key_is_ours(int vk) {
  if (vk == panel_key()) return true;
  if (g_cfg.freecamon == 0 || !camera_available()) return false;
  // Copy and paste are swallowed for a WINDOW (camera_owns_input()), like the teleport and flight keys, so naming them
  // here would make release_held_keys skip a real press.
  const int key = cam_key();
  return key != 0 && vk == key && bind_key_is_live(key);
}

static const unsigned kRawButtonDown = 0x0001u | 0x0004u | 0x0010u | 0x0040u | 0x0100u;
static const unsigned kRawButtonUp   = 0x0002u | 0x0008u | 0x0020u | 0x0080u | 0x0200u;

bool input_panel_open(void) { return InterlockedCompareExchange(&s_open, 0, 0) != 0; }

void input_mouse_pos(float* x, float* y) {
  if (x) *x = (float)InterlockedCompareExchange(&s_mouseX, 0, 0);
  if (y) *y = (float)InterlockedCompareExchange(&s_mouseY, 0, 0);
}

void input_debug_take(int* rawSeen, int* rawLive, int* rawCount, int* moveCount) {
  if (rawSeen)   *rawSeen   = InterlockedCompareExchange(&s_sawRawInput, 0, 0) ? 1 : 0;
  if (rawLive)   *rawLive   = raw_input_is_live() ? 1 : 0;
  if (rawCount)  *rawCount  = (int)InterlockedExchange(&s_rawCount, 0);
  if (moveCount) *moveCount = (int)InterlockedExchange(&s_moveCount, 0);
}

static void clamp_cursor_to_client(void) {
  RECT rc = {0, 0, 0, 0};
  if (!s_wnd || !GetClientRect(s_wnd, &rc)) return;
  LONG x = InterlockedCompareExchange(&s_mouseX, 0, 0);
  LONG y = InterlockedCompareExchange(&s_mouseY, 0, 0);
  if (x < 0) x = 0; else if (x > rc.right) x = rc.right;
  if (y < 0) y = 0; else if (y > rc.bottom) y = rc.bottom;
  InterlockedExchange(&s_mouseX, x);
  InterlockedExchange(&s_mouseY, y);
}

// Tell the game every held key went up, once, when the panel opens: the WM_KEYUP would be swallowed, so SDL never
// learns it and Duke keeps walking or firing. Released once at the transition rather than letting WM_KEYUPs through
// (unpaired releases). Window thread: GetKeyboardState reports the calling thread's queue.
static void release_held_keys(void) {
  if (!s_origProc || !s_wnd) return;
  BYTE state[256];
  if (!GetKeyboardState(state)) return;

  int keys = 0;

  // Mouse buttons go through SendInput, not the window proc: SDL reads buttons from raw input in relative mode, so a
  // forged WM_LBUTTONUP never reaches it; SendInput arrives as genuine raw input and the WM_INPUT handler forwards
  // releases.
  INPUT inj[5];
  int buttons = 0;
  static const struct { int vk; DWORD flag; DWORD data; } kBtn[] = {
    { VK_LBUTTON,  MOUSEEVENTF_LEFTUP,   0 },
    { VK_RBUTTON,  MOUSEEVENTF_RIGHTUP,  0 },
    { VK_MBUTTON,  MOUSEEVENTF_MIDDLEUP, 0 },
    { VK_XBUTTON1, MOUSEEVENTF_XUP,      XBUTTON1 },
    { VK_XBUTTON2, MOUSEEVENTF_XUP,      XBUTTON2 },
  };
  for (int i = 0; i < (int)(sizeof(kBtn) / sizeof(kBtn[0])); i++) {
    if (!(state[kBtn[i].vk] & 0x80)) continue;
    ZeroMemory(&inj[buttons], sizeof(INPUT));
    inj[buttons].type = INPUT_MOUSE;
    inj[buttons].mi.dwFlags = kBtn[i].flag;
    inj[buttons].mi.mouseData = kBtn[i].data;
    buttons++;
  }
  if (buttons) {
    const UINT sent = SendInput((UINT)buttons, inj, sizeof(INPUT));
    if (sent != (UINT)buttons)
      ek_log("input: SendInput released %u of %d button(s) (%lu)", sent, buttons,
             GetLastError());
  }

  for (int vk = 0; vk < 256; vk++) {
    if (!(state[vk] & 0x80)) continue;
    if (key_is_ours(vk)) continue;
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
        vk == VK_XBUTTON1 || vk == VK_XBUTTON2) continue;

    // A real release: scancode in bits 16-23, bit 30 (was down) and bit 31 (up) set, or SDL may not read it as genuine.
    const UINT sc = MapVirtualKeyA((UINT)vk, 0);
    const LPARAM lp = (LPARAM)((sc << 16) | 0x1u) | (LPARAM)0xC0000000;
    CallWindowProcW(s_origProc, s_wnd, WM_KEYUP, (WPARAM)vk, lp);
    keys++;
  }

  if (keys || buttons)
    ek_log("input: released %d key(s) and %d mouse button(s) on open", keys, buttons);
}

static void panel_set(bool open) {
  InterlockedExchange(&s_open, open ? 1 : 0);
  if (open) {
    gamecfg_load();

    // Before anything else: from here every key message is swallowed. Only on the FIRST claim: opening the panel
    // mid-flight is the camera handing over the keyboard, whose held keys were already released, so a release here
    // would forge one.
    if (claim_input(true)) {
      release_held_keys();
    }

    RECT rc = {0, 0, 0, 0};
    if (s_wnd && GetClientRect(s_wnd, &rc)) {
      InterlockedExchange(&s_mouseX, rc.right / 2);
      InterlockedExchange(&s_mouseY, rc.bottom / 2);
    }
  }
  // Written on close, not every change: a dragged row writes many times a second.
  if (!open) {
    // The camera may still have the keyboard: closing the panel mid-flight hands it back to the camera, not the game.
    claim_input(camera_owns_input());
    menu_keycap_close();
    // The field goes with the panel: left open it would eat keys with nothing on screen and a pending s_rowCommit would
    // land on a row the player no longer sees.
    menu_entry_close();
    config_write_changed();
  }

  ek_log("input: panel %s", open ? "opened" : "closed");
}

static bool is_input_message(UINT m) {
  switch (m) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_UNICHAR:
    case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_INPUT:
      return true;
    default:
      return false;
  }
}

static bool raw_mouse(LPARAM lParam, RAWMOUSE* out) {
  UINT size = 0;
  if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size,
                      sizeof(RAWINPUTHEADER)) != 0) return false;
  if (size == 0 || size > sizeof(RAWINPUT) * 4) return false;

  BYTE buf[sizeof(RAWINPUT) * 4];
  if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &size,
                      sizeof(RAWINPUTHEADER)) != size) return false;

  const RAWINPUT* ri = (const RAWINPUT*)buf;
  if (ri->header.dwType != RIM_TYPEMOUSE) return false;
  *out = ri->data.mouse;
  return true;
}

static unsigned accumulate_raw(LPARAM lParam) {
  RAWMOUSE m;
  if (!raw_mouse(lParam, &m)) return 0;
  InterlockedExchange(&s_sawRawInput, 1);
  InterlockedExchange(&s_lastRawMs, (LONG)GetTickCount());

  const unsigned btn = m.usButtonFlags;

  if (m.usFlags & MOUSE_MOVE_ABSOLUTE) return btn;

  InterlockedExchangeAdd(&s_mouseX, m.lLastX);
  InterlockedExchangeAdd(&s_mouseY, m.lLastY);
  clamp_cursor_to_client();
  return btn;
}


// Who owns a key is one chain with no overlap: panel, then camera, then game, each of the first two swallowing the same
// set (is_input_message). No key is ever both; no second WndProc, hook or thread. Scalars only across the clocks: one
// bitmask for the eight keys (read at one instant), two summed motion counters and a toggle-edge count.
enum {
  FCK_FWD  = 1 << 0, FCK_BACK = 1 << 1, FCK_LEFT = 1 << 2, FCK_RIGHT = 1 << 3,
  FCK_UP   = 1 << 4, FCK_DOWN = 1 << 5, FCK_FAST = 1 << 6, FCK_SLOW  = 1 << 7,
};

// Fixed keys, not eight more bind rows. VK_SHIFT/CONTROL/MENU rather than L/R variants: those are what Windows puts in
// wParam; Alt arrives as WM_SYSKEYDOWN.
static const struct { int vk; LONG bit; } kCamKeys[] = {
  { 'W', FCK_FWD },       { 'S', FCK_BACK },        { 'A', FCK_LEFT },
  { 'D', FCK_RIGHT },     { VK_SPACE, FCK_UP },     { VK_CONTROL, FCK_DOWN },
  { VK_SHIFT, FCK_FAST }, { VK_MENU, FCK_SLOW },
};

static volatile LONG s_camKeys = 0;
static volatile LONG s_camDx = 0;
static volatile LONG s_camDy = 0;
static volatile LONG s_camToggle = 0;

static volatile LONG s_camTeleport = 0;

static LONG s_camSeenActive = 0;

static bool camera_owns_input(void) {
  return camera_active() && !input_panel_open();
}

// The handover runs here because only this thread can: release_held_keys reads the CALLING thread's queue. Nothing is
// owed on the way down: keys pressed while the camera flew were swallowed; only a key held from before it started is
// stuck.
static void cam_sync_active(void) {
  const LONG now = camera_owns_input() ? 1 : 0;
  if (now == s_camSeenActive) return;
  s_camSeenActive = now;

  InterlockedExchange(&s_camKeys, 0);
  InterlockedExchange(&s_camDx, 0);
  InterlockedExchange(&s_camDy, 0);
  // The teleport edge is ownership-scoped: a press banked as the camera lands must not fire an unrequested teleport on
  // a later flight. s_camToggle is NOT cleared: it is taken whether or not the camera owns input.
  InterlockedExchange(&s_camTeleport, 0);
  // The panel may still have the keyboard on the way down or up: the claim, not this flag, decides whether a release is
  // owed.
  if (claim_input(now ? true : input_panel_open())) {
    release_held_keys();
  }
  ek_log("input: the free camera %s the keyboard and the mouse",
         now ? "has taken" : "has given back");
}

static void cam_note_key(WPARAM w, bool down) {
  for (int i = 0; i < (int)(sizeof(kCamKeys) / sizeof(kCamKeys[0])); i++) {
    if ((int)w != kCamKeys[i].vk) continue;
    if (down) InterlockedOr(&s_camKeys, kCamKeys[i].bit);
    else      InterlockedAnd(&s_camKeys, ~kCamKeys[i].bit);
    return;
  }
}

// WM_MOUSEMOVE is deliberately not a fallback here: in play SDL warps the mouse to the centre every frame, so a delta
// from it is the warp, not the player's hand.
static unsigned cam_note_raw(LPARAM lParam) {
  RAWMOUSE m;
  if (!raw_mouse(lParam, &m)) return 0;
  InterlockedExchange(&s_sawRawInput, 1);
  InterlockedExchange(&s_lastRawMs, (LONG)GetTickCount());
  const unsigned btn = m.usButtonFlags;
  if (m.usFlags & MOUSE_MOVE_ABSOLUTE) return btn;
  InterlockedExchangeAdd(&s_camDx, m.lLastX);
  InterlockedExchangeAdd(&s_camDy, m.lLastY);
  return btn;
}

// Both clipboard keys run on the window thread (the clipboard is the OS's). The copy takes a seqlock snapshot and the
// paste leaves a latch for the game thread. campos.h owns the format (Megaton's debug-readout line), so a viewpoint
// pastes into the other two ports.

static void chord_copy_view(void) {
  FreecamPose p;
  if (!camera_viewpoint_read(&p)) {
    ek_log("input: no viewpoint has been published yet, so nothing was copied");
    return;
  }
  char line[96];
  if (!campos_format(p, line, (int)sizeof line)) {
    ek_log("input: the viewpoint did not fit its own line; nothing copied");
    return;
  }
  const bool ok = clipboard_write_text(line);
  ek_log("input: viewpoint %s the clipboard: %s",
         ok ? "copied to" : "NOT copied to", line);
}

static void chord_paste_view(void) {
  char text[1200];
  FreecamPose want;
  if (!clipboard_read_text(text, (int)sizeof text) ||
      !campos_accept(text, &want)) {
    ek_log("input: the clipboard holds no plausible "
           "\"cam x= y= z= ang= pitch=\" line; the camera is left where it is");
    return;
  }
  camera_request_pose(want);
  ek_log("input: viewpoint taken from the clipboard, (%d,%d,%d) ang %d horiz %d; "
         "the next frame flies there", want.x, want.y, want.z, want.ang, want.horiz);
}

// The typed numeric field's keys, on the window thread: entry.h is pure, so Ctrl+V's clipboard read and a Ctrl+C/X
// write happen here. VK maps to an SDL scancode through vk_to_entry_scancode; entry_char_of_scancode already refuses
// what the field would not accept.
static void entry_route_key(WPARAM w) {
  int sc = 0;
  if (!vk_to_entry_scancode((int)w, &sc)) return;
  const bool shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
  const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  if (ctrl && (int)w == 'V') {
    char text[ENTRY_PASTE_CAP + 8];
    if (clipboard_read_text(text, (int)sizeof text)) menu_entry_paste(text);
    return;
  }
  const char* clip = menu_entry_key_mod(sc, shift, ctrl);
  if (clip) clipboard_write_text(clip);
}

int input_freecam_toggle_take(void) {
  return (int)InterlockedExchange(&s_camToggle, 0);
}

int input_teleport_take(void) {
  return (int)InterlockedExchange(&s_camTeleport, 0);
}

bool input_freecam_state(FreecamInput* out) {
  if (!out) return false;

  const int dx = (int)InterlockedExchange(&s_camDx, 0);
  const int dy = (int)InterlockedExchange(&s_camDy, 0);
  const LONG keys = InterlockedCompareExchange(&s_camKeys, 0, 0);
  const bool owns = camera_owns_input();

  out->dx    = owns ? dx : 0;
  out->dy    = owns ? dy : 0;
  out->fwd   = owns && (keys & FCK_FWD)   != 0;
  out->back  = owns && (keys & FCK_BACK)  != 0;
  out->left  = owns && (keys & FCK_LEFT)  != 0;
  out->right = owns && (keys & FCK_RIGHT) != 0;
  out->up    = owns && (keys & FCK_UP)    != 0;
  out->down  = owns && (keys & FCK_DOWN)  != 0;
  out->fast  = owns && (keys & FCK_FAST)  != 0;
  out->slow  = owns && (keys & FCK_SLOW)  != 0;
  return owns;
}

static LRESULT CALLBACK ek_WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  // First, before any message is decoded: the camera may have started or stopped on the game thread since the last
  // message, and the handover must happen before the wrong owner acts on this one.
  cam_sync_active();

  // The toggle, de-repeated: bit 30 of lParam is the previous key state. A bind row waiting for a key gets it first, so
  // rebinding the panel key can use the CURRENT one without closing the panel.
  if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && menu_keycap_armed()) {
    // Escape is the player's cancel, not a key to bind: committing 0x1B would leave a row showing Unbound that still
    // eats the key.
    if ((l & (1 << 30)) == 0) {
      if ((int)w == 0x1B) menu_keycap_cancel();
      else menu_keycap_bind((int)w);
    }
    return 0;
  }

  // The typed field takes the keyboard whole while open, above the toggle and every hotkey: EVERY keydown is swallowed,
  // so the panel key does not close the panel with a field open; Escape cancels and a click away commits. Not
  // de-repeated: a held digit keeps typing, capped by entry.h.
  if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && input_panel_open() && menu_entry_open()) {
    entry_route_key(w);
    return 0;
  }

  // EDuke32's console reads SDL_KEYDOWN, so while it takes typing none of the five hotkeys below may swallow one;
  // hotkey_swallows (ui/bind.h) holds the term. The two blocks above keep precedence.
  const bool keyDown = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
  const bool consoleTyping = keyDown && console_capturing();

  if (keyDown &&
      hotkey_swallows((int)w, panel_key(), true, consoleTyping, input_panel_open())) {
    if ((l & (1 << 30)) == 0) panel_set(!input_panel_open());
    return 0;
  }

  // The free camera's toggle, de-repeated and swallowed like the panel key, recorded only while the row is on so
  // nothing is banked after it is switched off; taken whether or not the panel is open. camera_available() as well as
  // the row: a refused locate would otherwise eat this key from a persisted freecamon=1.
  if (keyDown &&
      hotkey_swallows((int)w, cam_key(), g_cfg.freecamon != 0 && camera_available(),
                      consoleTyping, input_panel_open())) {
    if ((l & (1 << 30)) == 0) InterlockedIncrement(&s_camToggle);
    return 0;
  }

  // The two clipboard keys sit above the camera_owns_input block, which would eat them. They are ours only while the
  // camera OWNS the keyboard, not whenever the row is on; de-repeated so a held key copies once.
  if (keyDown) {
    const int copyKey = cam_copy_key();
    const int pasteKey = cam_paste_key();
    if (hotkey_swallows((int)w, copyKey, camera_owns_input(), consoleTyping,
                        input_panel_open())) {
      if ((l & (1 << 30)) == 0) chord_copy_view();
      return 0;
    }
    if (hotkey_swallows((int)w, pasteKey, camera_owns_input(), consoleTyping,
                        input_panel_open())) {
      if ((l & (1 << 30)) == 0) chord_paste_view();
      return 0;
    }
  }

  // The teleport key, above the flight block for the same reason, de-repeated, ours only while the camera owns the
  // keyboard, so absent from key_is_ours.
  if (keyDown) {
    const int tpKey = cam_teleport_key();
    if (hotkey_swallows((int)w, tpKey, camera_owns_input(), consoleTyping,
                        input_panel_open())) {
      if ((l & (1 << 30)) == 0) InterlockedIncrement(&s_camTeleport);
      return 0;
    }
  }

  // The camera owns everything while it flies: the panel's swallowed message set, decoded into the camera's held state
  // instead of ImGui.
  if (camera_owns_input()) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) { cam_note_key(w, true);  return 0; }
    if (msg == WM_KEYUP   || msg == WM_SYSKEYUP)   { cam_note_key(w, false); return 0; }
    if (msg == WM_INPUT) {
      // Motion is swallowed, button releases are not: cam_sync_active SendInput'ed a release for every held button and
      // those arrive as raw packets; swallowing them would leave SDL believing the trigger is down for the whole
      // flight.
      const unsigned btn = cam_note_raw(l);
      if ((btn & kRawButtonUp) && !(btn & kRawButtonDown))
        return CallWindowProcW(s_origProc, h, msg, w, l);
      return 0;
    }
    if (is_input_message(msg)) return 0;
  }

  if (input_panel_open()) {
    if (msg == WM_INPUT)          bump_debug_count(&s_rawCount);
    else if (msg == WM_MOUSEMOVE) bump_debug_count(&s_moveCount);

    if (msg == WM_INPUT) {
      const unsigned btn = accumulate_raw(l);

      // Motion is swallowed, button releases are not: SDL takes mouse buttons from raw input in relative mode, so
      // swallowing every WM_INPUT meant a release never arrived and the gun fired forever. Releases only: presses would
      // fire the gun on every panel click.
      if ((btn & kRawButtonUp) && !(btn & kRawButtonDown))
        return CallWindowProcW(s_origProc, h, msg, w, l);

      return 0;
    }

    if (msg == WM_MOUSEMOVE && !raw_input_is_live()) {
      // No raw input is arriving, so the cursor is not warped and its real position is meaningful (the game's own
      // menus, windowed play, the panel over a pause menu).
      InterlockedExchange(&s_mouseX, (short)LOWORD(l));
      InterlockedExchange(&s_mouseY, (short)HIWORD(l));
    }

    ImGui_ImplWin32_WndProcHandler(h, msg, w, l);

    if (is_input_message(msg)) return 0;
  }

  return CallWindowProcW(s_origProc, h, msg, w, l);
}

bool input_install(HWND wnd) {
  if (!wnd) return false;
  if (s_origProc) return true;
  s_wnd = wnd;

  s_origProc = (WNDPROC)SetWindowLongPtrW(wnd, GWLP_WNDPROC, (LONG_PTR)ek_WndProc);
  if (!s_origProc) {
    ek_log("input: SetWindowLongPtrW failed (%lu) -- panel cannot take input", GetLastError());
    return false;
  }
  ek_log("input: WndProc subclassed on hwnd=%p, original=%p, panel key 0x%02X",
         (void*)wnd, (void*)s_origProc, panel_key());
  return true;
}

void input_remove(void) {
  if (s_origProc && s_wnd) {
    SetWindowLongPtrW(s_wnd, GWLP_WNDPROC, (LONG_PTR)s_origProc);
    s_origProc = NULL;
  }
}
