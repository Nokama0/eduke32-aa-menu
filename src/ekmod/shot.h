#pragma once

// Screenshot mode: weapon, crosshair and level stats off together while the free camera flies, so a comparison shot has
// only the world. Ported from World Tour's shot.h, rules only: EDuke32 registers crosshair, hud_stats and r_drawweapon
// as OSD cvars (osdcmds.cpp:1596, :1665, :1713), so no hook is needed.

// Camera flying, row on, no menu owning the screen. The menu term is Duke's own menu (World Tour reads
// player_struct+844 bit 0), not this panel: opening our panel mid-flight stays hidden. Duke's menu cannot open
// mid-flight, so the ini stash replayed at install is the protection against an unclean exit.
inline bool shot_active(bool cameraActive, bool shotSwitch, bool menuOpen) {
  return cameraActive && shotSwitch && !menuOpen;
}

// The one signal shot_active's menu argument may carry: Duke's menu bit from gm (EK_MODE_MENU), never the panel's.
inline bool shot_menu_term(bool gmMenuOpen) { return gmMenuOpen; }

enum ShotEdge { SHOT_EDGE_NONE = 0, SHOT_EDGE_ON = 1, SHOT_EDGE_OFF = 2 };

inline int shot_edge(bool wasActive, bool nowActive) {
  if (wasActive == nowActive) return SHOT_EDGE_NONE;
  return nowActive ? SHOT_EDGE_ON : SHOT_EDGE_OFF;
}

// The three cvars the hold covers. Append, never insert or reorder: the indices name config fields and bit positions of
// the on-disk mask, checked only against THIS build's readable set.
const int SHOT_CVAR_CROSSHAIR = 0;
const int SHOT_CVAR_WEAPON    = 1;
const int SHOT_CVAR_STATS     = 2;
const int SHOT_CVARS          = 3;

// `mask` is which of the three the registry could read on the ON edge; only those are hidden and given back. `held` is
// int: MSVC 19.50 /O2 loses a bool member of a struct an inline returns by value.
struct ShotHold {
  int held;
  int mask;
  int saved[SHOT_CVARS];
};

enum ShotAction { SHOT_ACT_NONE = 0, SHOT_ACT_HIDE = 1, SHOT_ACT_RESTORE = 2 };

// Values are read on the ON edge ONLY and written back on the OFF edge ONLY: a repeated ON edge would save our own
// zeros as the player's, and `!h->held` refuses it. HIDE writes 0, each cvar's vmin and off state.
inline int shot_hold_step(ShotHold* h, int edge, int readMask, const int* current,
                          int* outMask, int* outValue) {
  if (!h || !current || !outMask || !outValue) return SHOT_ACT_NONE;
  *outMask = 0;
  for (int i = 0; i < SHOT_CVARS; i++) outValue[i] = 0;

  if (edge == SHOT_EDGE_ON && !h->held) {
    h->held = 1;
    h->mask = readMask;
    for (int i = 0; i < SHOT_CVARS; i++)
      h->saved[i] = (readMask & (1 << i)) ? current[i] : 0;
    *outMask = readMask;
    return SHOT_ACT_HIDE;
  }
  if (edge == SHOT_EDGE_OFF && h->held) {
    h->held = 0;
    *outMask = h->mask;
    for (int i = 0; i < SHOT_CVARS; i++) outValue[i] = h->saved[i];
    h->mask = 0;
    return SHOT_ACT_RESTORE;
  }
  return SHOT_ACT_NONE;
}

// Crash stash: install-time replay of what an ON edge saved. Trustworthy = every checked bit still reads 0 (a player
// who zeroed all three leaves the same reading; the stash is spent either way). Any drift refuses the WHOLE stash.
inline bool shot_stash_valid(int mask, const int* current) {
  if (mask == 0) return false;
  for (int i = 0; i < SHOT_CVARS; i++)
    if ((mask & (1 << i)) && current[i] != 0) return false;
  return true;
}

// A stash on disk is not proof of failure: shot_restore heals at DLL_PROCESS_DETACH without writing a file. Every bit
// reading its SAVED value is that fingerprint; a tie goes to the crash.
inline bool shot_stash_healed(int mask, const int* saved, const int* current) {
  if (mask == 0 || !saved || !current) return false;
  for (int i = 0; i < SHOT_CVARS; i++)
    if ((mask & (1 << i)) && current[i] != saved[i]) return false;
  return true;
}

// Only the intersection of readable bits and the stash's mask is checked or written; the caller clears the stash either
// way.
inline int shot_stash_replay(int held, int stashMask, const int* saved,
                              int readMask, const int* current,
                              int* outMask, int* outValue) {
  *outMask = 0;
  for (int i = 0; i < SHOT_CVARS; i++) outValue[i] = 0;
  if (!held) return SHOT_ACT_NONE;
  const int checkMask = stashMask & readMask;
  if (!shot_stash_valid(checkMask, current)) return SHOT_ACT_NONE;
  *outMask = checkMask;
  for (int i = 0; i < SHOT_CVARS; i++)
    if (checkMask & (1 << i)) outValue[i] = saved[i];
  return SHOT_ACT_RESTORE;
}

// Status bar: no cvar covers it (hud_statusbarmode's handler calls G_UpdateScreenArea(), cvar_set_int does not), so the
// lever is its one `call rel32`, rewritten by hook.cpp's callsite_install to a thunk of ours.

// G_DrawStatusBar's fingerprint is the combination of literals "On", "Off", "Auto" (none unique alone) that sbar.cpp's
// inventory legend loads; no other function does. Two of three is no match.
inline bool shot_sbar_shape(bool on, bool off, bool autoLit) {
  return on && off && autoLit;
}

inline bool shot_sbar_target_ok(int shaped, int faulted) {
  return shaped == 1 && faulted == 0;
}

// Exactly one call site: screens.cpp calls G_DrawStatusBar twice (:1154, :1160 under SPLITSCREEN_MOD_HACKS).
inline bool shot_sbar_locate_ok(int sites, int faulted) {
  return sites == 1 && faulted == 0;
}

inline bool shot_draws_statusbar(bool active) { return !active; }

// Scuba mask: P_DisplayScuba (player.cpp:2156) has no `call` in the image: it is inlined into G_DisplayRest (8554
// RUNTIME_FUNCTION extents walked). So its three G_DrawTileScaled calls (:2178-2181) are named by argument values.

// player.cpp:1863 and the x coordinates and bit masks of :2178-2181. The right half's `mov ecx, 320-43` occurs exactly
// ONCE in eduke32.exe: the strongest anchor.
const int SHOT_DRAWEAP_CENTER  = 262144;
const int SHOT_SCUBA_X_SEAM    = 44;
const int SHOT_SCUBA_X_LEFT    = 43;
const int SHOT_SCUBA_X_RIGHT   = 320 - 43;
const int SHOT_SCUBA_BITS      = 2 + 16 + SHOT_DRAWEAP_CENTER;
const int SHOT_SCUBA_BITS_FLIP = 2 + 4 + 16 + SHOT_DRAWEAP_CENTER;
const int SHOT_SCUBA_SITES     = 3;

const int SHOT_EVENT_DISPLAYSCUBA = 112;

inline bool shot_scuba_anchor_ok(int count, int faulted) {
  return count == 1 && faulted == 0;
}

// The three sites must agree on one callee nothing else in the bracket calls; a fourth call site refuses.
inline bool shot_scuba_locate_ok(int anchored, int targets, int sites, int faulted) {
  return anchored == SHOT_SCUBA_SITES && targets == 1 &&
         sites == SHOT_SCUBA_SITES && faulted == 0;
}

// Exact pairs: left half and GL seam carry 2+16+DRAWEAP_CENTER, the right half adds the flip bit. A belt: a wrong
// callee then fails safe by forwarding.
inline bool shot_scuba_tile(int drawX, int drawShade, int drawBits) {
  if (drawShade != 0) return false;
  if (drawX == SHOT_SCUBA_X_LEFT || drawX == SHOT_SCUBA_X_SEAM)
    return drawBits == SHOT_SCUBA_BITS;
  if (drawX == SHOT_SCUBA_X_RIGHT)
    return drawBits == SHOT_SCUBA_BITS_FLIP;
  return false;
}

inline bool shot_draws_scuba(bool active, int drawX, int drawShade, int drawBits) {
  return !(active && shot_scuba_tile(drawX, drawShade, drawBits));
}

// All three or none: the count is raised only after the third callsite_install succeeds and dropped before the first
// callsite_remove, so a half-installed redirect forwards.
inline bool shot_scuba_installed(int patched) {
  return patched == SHOT_SCUBA_SITES;
}

// AA draws its weapon, crosshair and HUD in CON events that fire before every cvar gate (EVENT_DISPLAYWEAPON
// player.cpp:2300, DISPLAYCROSSHAIR screens.cpp:1221, DISPLAYREST :1296); the mode skips those calls.

const int SHOT_EVENT_DISPLAYWEAPON    = 39;
const int SHOT_EVENT_DISPLAYCROSSHAIR = 59;
const int SHOT_EVENT_DISPLAYREST      = 60;

// A skipped event's return: WEAPON must be nonzero or the engine draws Duke's stock weapon; CROSSHAIR -1 is past
// MAXTILES.
inline int shot_event_forced_return(int event) {
  return (event == SHOT_EVENT_DISPLAYWEAPON || event == SHOT_EVENT_DISPLAYCROSSHAIR)
             ? -1 : 0;
}

// Bombshell's brutality kill writes `weapon_pos 10` in AA's weapon event while brutalcount is 1..3, so the weapon runs
// then. A failed read never skips.
inline bool shot_weapon_guard_clear(bool brutalRead, int brutalcount) {
  return brutalRead && !(brutalcount >= 1 && brutalcount <= 3);
}

const int SHOT_MODAL_CRAWLCLOCK = 0;
const int SHOT_MODAL_VENDOR     = 1;
const int SHOT_MODAL_VRMENU     = 2;
const int SHOT_MODAL_STARTMODE  = 3;
const int SHOT_MODAL_BONUS      = 4;
const int SHOT_MODAL_EPSELECT   = 5;
const int SHOT_MODAL_MATCHMENU  = 6;
const int SHOT_MODAL_VRSCREEN   = 7;
const int SHOT_MODAL_THISCAM    = 8;
const int SHOT_MODALS           = 9;
const int SHOT_MODAL_READ_ALL   = (1 << SHOT_MODALS) - 1;

// Each at the CON's own test, widened where AA widens it (crawl, vendor_screen, startmode -1/-2); exact where AA is
// (epselect only -1, `ifn thiscam -1`).
inline bool shot_modal_open(int which, int value) {
  switch (which) {
    case SHOT_MODAL_CRAWLCLOCK: return value > 1;
    case SHOT_MODAL_VENDOR:     return value != 0;
    case SHOT_MODAL_VRMENU:     return value != 0;
    case SHOT_MODAL_STARTMODE:  return value < 0;
    case SHOT_MODAL_BONUS:      return value != 0;
    case SHOT_MODAL_EPSELECT:   return value == -1;
    case SHOT_MODAL_MATCHMENU:  return value != 0;
    case SHOT_MODAL_VRSCREEN:   return value != 0;
    case SHOT_MODAL_THISCAM:    return value != -1;
    default:                    return false;
  }
}

inline bool shot_rest_guard_clear(int readMask, const int* value) {
  if (!value || readMask != SHOT_MODAL_READ_ALL) return false;
  for (int i = 0; i < SHOT_MODALS; i++)
    if (shot_modal_open(i, value[i])) return false;
  return true;
}

inline bool shot_event_skips(bool active, int siteEvent, int event, bool guardClear) {
  return active && event == siteEvent && guardClear;
}

inline bool shot_event_site_ok(int anchors, int faulted) {
  return anchors == 1 && faulted == 0;
}

// WEAPON and REST go through VM_OnEvent's three-argument form (gameexec.h:61); CROSSHAIR through VM_OnEventWithReturn
// (:66).
inline bool shot_event_callee_ok(int event, unsigned callee, unsigned vmEvent3) {
  if (callee == 0 || vmEvent3 == 0) return false;
  if (event == SHOT_EVENT_DISPLAYCROSSHAIR) return callee != vmEvent3;
  return callee == vmEvent3;
}

inline bool shot_hides_overlays(bool active) { return active; }

void shot_frame(bool cameraActive, bool menuOpen);

bool shot_active_now(void);

void shot_restore(void);

// Call-site redirects, paired; installed once from overlay.cpp's per-frame path (inside SwapBuffers, hook.cpp's write
// window), removed at DLL_PROCESS_DETACH before shot_restore. The locates are independent.
void shot_install(void);
void shot_remove(void);

bool shot_statusbar_available(void);

bool shot_scuba_available(void);

int shot_hud_events_installed(void);

// Install-time: replays ekmod.ini's stash after cvars_scan and always clears it.
void shot_replay_stash(void);

bool shot_available(void);
