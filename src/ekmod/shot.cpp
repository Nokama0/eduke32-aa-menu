// Screenshot mode, the DLL half. THIS FILE DECIDES NOTHING: every decision is a pure rule in shot.h; here we read the
// registry, decide through those and write on the two edges. Weapon, crosshair and level stats are ordinary OSD cvars
// reached by name (cvars.cpp); the status bar and scuba mask are call-site redirects below. Details: docs/re/shot.md.
#include <windows.h>

#include "shot.h"
#include "cvars.h"
#include "config.h"
#include "hook.h"
#include "imagescan.h"
#include "hitscan.h"
#include "gamevars.h"
#include "log.h"

// The player's own settings: the engine writes them to settings.cfg on exit AND on every save (our autosave too), so
// a missed restore leaves crosshair and weapon off in his config. hud_statusbarmode and ud.screen_size CANNOT be
// written (CVAR_FUNCPTR: a bare store leaves the view uncomputed). Order: SHOT_CVAR_CROSSHAIR, _WEAPON, _STATS.
static const char* const kShotCvars[SHOT_CVARS] = {
  "crosshair", "r_drawweapon", "hud_stats"
};

// ONE THREAD OWNS ALL OF THIS: shot_frame runs from camera_frame on the game thread and shot_active_now is read from
// the same path; nothing crosses to input.cpp's window thread.
static ShotHold s_hold = { 0, 0, { 0, 0, 0 } };
static bool s_active = false;

bool shot_active_now(void) { return s_active; }

bool shot_available(void) {
  // The level stats are a bonus; the weapon and the crosshair are the feature.
  return cvar_row_available(cvars_find(kShotCvars[SHOT_CVAR_CROSSHAIR])) &&
         cvar_row_available(cvars_find(kShotCvars[SHOT_CVAR_WEAPON]));
}

// Write one value per set bit. A refusal is cvar_set_int's to log and costs that one cvar, not the mode: half a
// restore is still better than none.
static void shot_write(int mask, const int* value) {
  for (int i = 0; i < SHOT_CVARS; i++)
    if (mask & (1 << i)) cvar_set_int(kShotCvars[i], value[i]);
}

// Clear the on-disk stash after a normal OFF-edge restore and after an install-time replay of either outcome; a stash
// never cleared is a bug.
static void shot_stash_clear(void) {
  g_cfg.shotstashheld = 0;
  g_cfg.shotstashmask = 0;
  g_cfg.shotstashcrosshair = 0;
  g_cfg.shotstashweapon = 0;
  g_cfg.shotstashstats = 0;
  g_cfgDirty = 1;
  config_write_changed();
}

static void shot_step(int edge) {
  int cur[SHOT_CVARS] = { 0, 0, 0 };
  int readMask = 0;
  for (int i = 0; i < SHOT_CVARS; i++)
    if (cvar_get_int(kShotCvars[i], &cur[i])) readMask |= (1 << i);

  int outMask = 0;
  int outVal[SHOT_CVARS] = { 0, 0, 0 };
  const int act = shot_hold_step(&s_hold, edge, readMask, cur, &outMask, outVal);
  if (act == SHOT_ACT_NONE) return;

  // THE STASH GOES TO DISK BEFORE THE CVARS ARE TOUCHED ON HIDE, AND AFTER ON RESTORE: there is no instant where the
  // live cvars read hidden and the on-disk recovery copy does not exist yet.
  if (act == SHOT_ACT_HIDE) {
    g_cfg.shotstashheld = 1;
    g_cfg.shotstashmask = outMask;
    g_cfg.shotstashcrosshair = s_hold.saved[SHOT_CVAR_CROSSHAIR];
    g_cfg.shotstashweapon    = s_hold.saved[SHOT_CVAR_WEAPON];
    g_cfg.shotstashstats     = s_hold.saved[SHOT_CVAR_STATS];
    g_cfgDirty = 1;
    config_write_changed();
  }

  shot_write(outMask, outVal);

  if (act == SHOT_ACT_RESTORE) shot_stash_clear();

  // Not behind g_cfg.debuglog: this writes the player's own settings and the log must prove the pair happened. The
  // two redirects are named on the same line: neither leaves a trace in settings.cfg.
  const char* const gone = act == SHOT_ACT_HIDE ? "not drawn from the next frame"
                                                : "drawn again from the next frame";
  const char* const barWord =
      shot_statusbar_available() ? gone : "left alone, the redirect is not in";
  const char* const maskWord =
      shot_scuba_available() ? gone : "left alone, the redirect is not in";
  ek_log(act == SHOT_ACT_HIDE
           ? "shot: on -- crosshair %d, r_drawweapon %d, hud_stats %d saved and hidden (mask 0x%X); status bar %s; scuba mask %s; AA HUD event redirects in: %d of 3"
           : "shot: off -- crosshair %d, r_drawweapon %d, hud_stats %d restored (mask 0x%X); status bar %s; scuba mask %s; AA HUD event redirects in: %d of 3",
         act == SHOT_ACT_HIDE ? cur[SHOT_CVAR_CROSSHAIR] : outVal[SHOT_CVAR_CROSSHAIR],
         act == SHOT_ACT_HIDE ? cur[SHOT_CVAR_WEAPON] : outVal[SHOT_CVAR_WEAPON],
         act == SHOT_ACT_HIDE ? cur[SHOT_CVAR_STATS] : outVal[SHOT_CVAR_STATS],
         outMask, barWord, maskWord, shot_hud_events_installed());
}

// Install-time replay, called once from overlay.cpp right after cvars_scan() (the earliest the registry can answer)
// and before camera_frame can reach another ON edge; config_load() has already read g_cfg.shotstash* from the ini.
void shot_replay_stash(void) {
  if (!g_cfg.shotstashheld) return;

  int cur[SHOT_CVARS] = { 0, 0, 0 };
  int readMask = 0;
  for (int i = 0; i < SHOT_CVARS; i++)
    if (cvar_get_int(kShotCvars[i], &cur[i])) readMask |= (1 << i);

  const int saved[SHOT_CVARS] = {
    g_cfg.shotstashcrosshair, g_cfg.shotstashweapon, g_cfg.shotstashstats
  };
  int outMask = 0;
  int outVal[SHOT_CVARS] = { 0, 0, 0 };
  const int act = shot_stash_replay(g_cfg.shotstashheld, g_cfg.shotstashmask, saved,
                                     readMask, cur, &outMask, outVal);
  if (act == SHOT_ACT_RESTORE) {
    shot_write(outMask, outVal);
    ek_log("shot: replayed a crash stash from a previous session -- "
           "crosshair %d, r_drawweapon %d, hud_stats %d put back (mask 0x%X)",
           outVal[SHOT_CVAR_CROSSHAIR], outVal[SHOT_CVAR_WEAPON], outVal[SHOT_CVAR_STATS], outMask);
  } else if (shot_stash_healed(g_cfg.shotstashmask & readMask, saved, cur)) {
    // NOT A FAILURE: a stash outlives a session that healed itself (shot_restore puts the cvars back at
    // DLL_PROCESS_DETACH and writes no file), so the ini still says held while every value is the player's.
    ek_log("shot: a stash from a previous session is already back in the live "
           "cvars -- crosshair %d, r_drawweapon %d, hud_stats %d, so nothing "
           "was replayed and the stash is dropped. This is what a session that "
           "restored itself on the way out leaves behind (stash mask 0x%X, "
           "readable 0x%X)",
           cur[SHOT_CVAR_CROSSHAIR], cur[SHOT_CVAR_WEAPON], cur[SHOT_CVAR_STATS],
           g_cfg.shotstashmask, readMask);
  } else {
    ek_log("shot: a stash from a previous session matches neither the hidden "
           "state nor its own saved values -- crosshair %d, r_drawweapon %d, "
           "hud_stats %d against saved %d/%d/%d. Something changed these since, "
           "so they are left exactly as they are and the stash is dropped "
           "without replay (stash mask 0x%X, readable 0x%X)",
           cur[SHOT_CVAR_CROSSHAIR], cur[SHOT_CVAR_WEAPON], cur[SHOT_CVAR_STATS],
           saved[SHOT_CVAR_CROSSHAIR], saved[SHOT_CVAR_WEAPON], saved[SHOT_CVAR_STATS],
           g_cfg.shotstashmask, readMask);
  }
  shot_stash_clear();
}

void shot_frame(bool cameraActive, bool menuOpen) {
  const bool now = shot_active(cameraActive, g_cfg.freecamshot != 0, menuOpen);
  const int edge = shot_edge(s_active, now);
  s_active = now;
  // Steady frames touch nothing: with the row off the cost is two int reads, a compare and a return.
  if (edge == SHOT_EDGE_NONE) return;
  shot_step(edge);
}

// DLL_PROCESS_DETACH is the belt, NOT the guarantee: on a clean quit the engine writes settings.cfg inside G_Shutdown
// before this runs, and Duke's menu cannot open while the camera flies, so a menu quit never reaches an OFF edge; the
// on-disk stash closes that. A plain store; it does NOT clear the stash (an ini write under the loader lock).
void shot_restore(void) {
  if (!s_hold.held) return;
  int cur[SHOT_CVARS] = { 0, 0, 0 };
  int outMask = 0;
  int outVal[SHOT_CVARS] = { 0, 0, 0 };
  if (shot_hold_step(&s_hold, SHOT_EDGE_OFF, 0, cur, &outMask, outVal) == SHOT_ACT_RESTORE) {
    shot_write(outMask, outVal);
    ek_log("shot: the mod is unloading with the hud still hidden -- "
           "crosshair %d, r_drawweapon %d, hud_stats %d put back (mask 0x%X)",
           outVal[SHOT_CVAR_CROSSHAIR], outVal[SHOT_CVAR_WEAPON],
           outVal[SHOT_CVAR_STATS], outMask);
  }
  s_active = false;
}

// THE STATUS BAR: ONE CALL SITE, REDIRECTED WHILE THE MODE IS ON. Not a cvar (see above) and not an entry hook (a
// call-site redirect rewrites four operand bytes of an existing call). Found with nothing hardcoded: "Game Paused"
// (screens.cpp:1301) -> its one lea -> G_DisplayRest's bracket -> the one callee loading "On", "Off" and "Auto".

typedef void (*DrawStatusBarFn)(int snum);

// sbar.cpp's three literals, searched WITH their NULs (sizeof, not strlen), so a longer string cannot match.
static const char EK_SBAR_ON[]     = "On";
static const char EK_SBAR_OFF[]    = "Off";
static const char EK_SBAR_AUTO[]   = "Auto";
static const char EK_SBAR_ANCHOR[] = "Game Paused";

// 40 distinct targets on the shipped build; the cap keeps a wrong scan off the end of a fixed array.
static const int EK_SBAR_MAX_TARGETS = 64;

// The address the redirected site used to reach, captured at install; the thunk forwards through this only.
static DrawStatusBarFn s_sbarOrig = NULL;
static EkCallPatch s_sbarPatch;
static bool s_saidNoSbarOrig = false;

bool shot_statusbar_available(void) { return s_sbarPatch.site != NULL; }

// THE THUNK: one int argument, in ecx. IT FORWARDS UNLESS THE MODE IS ON: this is the only call to G_DrawStatusBar,
// so a thunk that always returned would remove the bar for the session. Skipping is safe (its side effects are a
// dirty flag and lazily filled icon tables). s_active is written and read on the game thread, one frame apart.
static void EK_DrawStatusBarThunk(int snum) {
  DrawStatusBarFn const orig = s_sbarOrig;
  if (!orig) {
    // Unreachable: the address is captured before the patch and cleared after it comes out. Said once; no bar that
    // frame.
    if (!s_saidNoSbarOrig) {
      s_saidNoSbarOrig = true;
      ek_log("shot: the status bar thunk ran with no original to forward to, "
             "which nothing in this build can produce -- please report it. The "
             "bar is not drawn while this lasts");
    }
    return;
  }
  if (!shot_draws_statusbar(s_active)) return;
  orig(snum);
}

// Do the bytes at `rva` hold `lit` and its NUL, inside a section the loader maps? Bounds come first: a test that
// walks off a section end is the fault every scan here avoids.
static bool sbar_literal_at(const unsigned char* base, unsigned rva,
                            const char* lit, unsigned n) {
  unsigned b = 0, e = 0;
  if (!ek_image_data_extent(base, rva, &b, &e)) return false;
  if (rva < b || rva >= e || e - rva < n) return false;
  for (unsigned i = 0; i < n; i++)
    if (base[rva + i] != (unsigned char)lit[i]) return false;
  return true;
}

// G_DrawStatusBar's fingerprint: does [b,e) load all three literals' addresses? The walk is positional, not decoded,
// so it can only OVER-match, the safe direction for a test whose authority is that exactly one function passes.
static bool sbar_shape_of(const unsigned char* base, unsigned b, unsigned e) {
  bool on = false, off = false, autoLit = false;
  if (e <= b + 7) return false;
  for (unsigned q = b; q + 7 <= e; q++) {
    const unsigned char rex = base[q];
    if ((rex != 0x48 && rex != 0x4C) || base[q + 1] != 0x8D) continue;
    if ((base[q + 2] & 0xC7) != 0x05) continue;
    const int disp = *(const int*)(base + q + 3);
    const unsigned t = (unsigned)((int)q + 7 + disp);
    if (!on && sbar_literal_at(base, t, EK_SBAR_ON, (unsigned)sizeof(EK_SBAR_ON)))
      on = true;
    if (!off && sbar_literal_at(base, t, EK_SBAR_OFF, (unsigned)sizeof(EK_SBAR_OFF)))
      off = true;
    if (!autoLit &&
        sbar_literal_at(base, t, EK_SBAR_AUTO, (unsigned)sizeof(EK_SBAR_AUTO)))
      autoLit = true;
    if (shot_sbar_shape(on, off, autoLit)) return true;
  }
  return shot_sbar_shape(on, off, autoLit);
}

// Every count a refusal line may need. `sites` counts only 0xE8 positions whose target is a real function entry, so
// another instruction's operand byte is not read as an opcode.
struct SbarTargets {
  int sites; int targets; int shaped; unsigned rva; int capped; int faulted;
};

static void find_sbar_target(const unsigned char* base, unsigned begin,
                             unsigned end, SbarTargets* out) {
  out->sites = 0; out->targets = 0; out->shaped = 0;
  out->rva = 0; out->capped = 0; out->faulted = 0;
  if (end <= begin + 5) { out->faulted = 1; return; }

  unsigned seen[EK_SBAR_MAX_TARGETS];
  int nseen = 0;
  __try {
    for (unsigned q = begin; q + 5 <= end; q++) {
      if (base[q] != 0xE8) continue;
      const int disp = *(const int*)(base + q + 1);
      const unsigned t = ekm_us_rel32_target(q, disp);
      unsigned b = 0, e = 0;
      if (!ek_image_extent_containing(base, t, &b, &e) || b != t) continue;
      out->sites++;
      int dup = 0;
      for (int k = 0; k < nseen; k++) if (seen[k] == t) { dup = 1; break; }
      if (dup) continue;
      if (nseen < EK_SBAR_MAX_TARGETS) seen[nseen++] = t;
      else out->capped = 1;
      out->targets++;
      if (!sbar_shape_of(base, b, e)) continue;
      out->shaped++;
      if (out->rva == 0) out->rva = t;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// Every `call rel32` in [begin,end) reaching one identified target, counted in a pass of its own: the walk above
// SKIPS a target it has already seen. Shared by both redirects.
struct EkSiteCount { int count; unsigned rva; int faulted; };

static void find_sites_reaching(const unsigned char* base, unsigned begin,
                                unsigned end, unsigned target, EkSiteCount* out) {
  out->count = 0; out->rva = 0; out->faulted = 0;
  if (target == 0 || end <= begin + 5) { out->faulted = 1; return; }
  __try {
    for (unsigned q = begin; q + 5 <= end; q++) {
      if (base[q] != 0xE8) continue;
      const int disp = *(const int*)(base + q + 1);
      if (ekm_us_rel32_target(q, disp) != target) continue;
      if (out->count++ == 0) out->rva = q;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// G_DisplayRest's own extent: the anchor string, the one rip-relative lea reaching it, and the RUNTIME_FUNCTION
// containing it. `what` names the feature lost if it refuses.
static bool locate_displayrest(const unsigned char* base, const char* what,
                               unsigned* beginOut, unsigned* endOut) {
  *beginOut = 0; *endOut = 0;

  EkStrHit s;
  ek_image_find_string(base, EK_SBAR_ANCHOR, sizeof(EK_SBAR_ANCHOR), &s);
  if (s.count != 1 || s.faulted) {
    ek_log("shot: %s -- the \"%s\" anchor occurs %d time(s) "
           "in the image's read-only data%s, so G_DisplayRest cannot be "
           "bracketed (r10669-ec5824db8 has exactly 1)", what, EK_SBAR_ANCHOR,
           s.count,
           s.faulted ? " and a section faulted mid-scan, so that count is partial"
                     : "");
    return false;
  }

  EkRefHit r;
  ek_image_find_lea_refs(base, s.rva, &r);
  if (r.count != 1 || r.faulted) {
    ek_log("shot: %s -- %d instruction(s) load the address "
           "of \"%s\" at rva 0x%07X (%d position(s) held the displacement before "
           "the lea shape test)%s; exactly one is what names G_DisplayRest",
           what, r.count, EK_SBAR_ANCHOR, s.rva, r.raw,
           r.faulted ? " and a section faulted mid-scan" : "");
    return false;
  }

  if (!ek_image_extent_containing(base, r.rva, beginOut, endOut)) {
    ek_log("shot: %s -- no RUNTIME_FUNCTION covers the lea "
           "at rva 0x%07X, so there is no bracketed caller to search", what,
           r.rva);
    return false;
  }
  return true;
}

// The target and the site, or false having already logged which count refused.
static bool locate_sbar(const unsigned char* base, unsigned begin, unsigned end,
                        unsigned* targetOut, unsigned* siteOut) {
  *targetOut = 0; *siteOut = 0;

  SbarTargets t;
  find_sbar_target(base, begin, end, &t);
  if (!shot_sbar_target_ok(t.shaped, t.faulted)) {
    ek_log("shot: the status bar stays up -- G_DrawStatusBar not identified. %d "
           "`call rel32` site(s) inside 0x%07X..0x%07X reach %d distinct function "
           "entry(ies), %d of which load all three of \"On\", \"Off\" and "
           "\"Auto\"%s%s (r10669-ec5824db8 has 1 of 40)",
           t.sites, begin, end, t.targets, t.shaped,
           t.capped ? " (target list capped)" : "",
           t.faulted ? " (the body faulted mid-scan, so those counts are partial)"
                     : "");
    return false;
  }

  EkSiteCount site;
  find_sites_reaching(base, begin, end, t.rva, &site);
  if (!shot_sbar_locate_ok(site.count, site.faulted)) {
    ek_log("shot: the status bar stays up -- G_DrawStatusBar at rva 0x%07X is "
           "reached from %d call site(s) inside 0x%07X..0x%07X%s; refusing to "
           "patch any of them. Two would mean SPLITSCREEN_MOD_HACKS is compiled "
           "in and screens.cpp:1160 draws a second bar this would not hide "
           "(r10669-ec5824db8 has exactly 1)", t.rva, site.count, begin, end,
           site.faulted ? " (the body faulted mid-scan, so that count is partial)"
                        : "");
    return false;
  }

  ek_log("shot: G_DrawStatusBar at rva 0x%07X -- the only one of %d distinct "
         "target(s) of the %d `call rel32` site(s) inside 0x%07X..0x%07X that "
         "loads \"On\", \"Off\" and \"Auto\"; the ONE site reaching it is at rva "
         "0x%07X, and the \"%s\" anchor is what named that bracket",
         t.rva, t.targets, t.sites, begin, end, site.rva, EK_SBAR_ANCHOR);
  *targetOut = t.rva;
  *siteOut = site.rva;
  return true;
}

// THE SCUBA MASK: THREE CALL SITES, ALL OR NONE. P_DisplayScuba is inlined into G_DisplayRest, so its three
// G_DrawTileScaled draws (player.cpp:2178-2181) are redirected, each found by its source x coordinate (`mov ecx,
// 320-43`). All three must reach the SAME entry, reached from exactly three sites; the event guard corroborates.

// From the `mov ecx, imm32` to the call is 18 to 21 bytes on the shipped build; 32 leaves room for reordered setup.
static const unsigned EK_SCUBA_ARG_WINDOW = 32;

typedef void (*DrawTileScaledFn)(int drawX, int drawY, int tileNum, int drawShade,
                                 int drawBits, int drawPal, int uniqueID);

// One anchored draw: how many positions held the immediate, how many entry calls followed the first, and that call.
struct ScubaDraw {
  int anchors; int calls; unsigned anchorRva; unsigned site; unsigned target;
  int faulted;
};

static void find_scuba_draw(const unsigned char* base, unsigned begin,
                            unsigned end, unsigned imm, ScubaDraw* out) {
  out->anchors = 0; out->calls = 0; out->anchorRva = 0;
  out->site = 0; out->target = 0; out->faulted = 0;
  if (end <= begin + (unsigned)EKM_CALL_LEN) { out->faulted = 1; return; }
  __try {
    for (unsigned q = begin; q + 5 <= end; q++) {
      if (base[q] != 0xB9) continue;
      if (*(const unsigned*)(base + q + 1) != imm) continue;
      out->anchors++;
      // A second position makes the anchor ambiguous and the check refuses on that count alone.
      if (out->anchors > 1) continue;
      out->anchorRva = q;
      const unsigned stop =
          (end - q < EK_SCUBA_ARG_WINDOW) ? end : q + EK_SCUBA_ARG_WINDOW;
      for (unsigned c = q + 5; c + 5 <= stop; c++) {
        if (base[c] != 0xE8) continue;
        const int disp = *(const int*)(base + c + 1);
        const unsigned t = ekm_us_rel32_target(c, disp);
        unsigned b = 0, e = 0;
        if (!ek_image_extent_containing(base, t, &b, &e) || b != t) continue;
        out->calls++;
        if (out->calls == 1) { out->site = c; out->target = t; }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

static bool locate_scuba(const unsigned char* base, unsigned begin, unsigned end,
                         unsigned* targetOut, unsigned* sitesOut) {
  *targetOut = 0;
  for (int i = 0; i < SHOT_SCUBA_SITES; i++) sitesOut[i] = 0;

  ScubaDraw ev;
  find_scuba_draw(base, begin, end, (unsigned)SHOT_EVENT_DISPLAYSCUBA, &ev);
  if (!shot_scuba_anchor_ok(ev.anchors, ev.faulted)) {
    ek_log("shot: the scuba mask stays on -- %d position(s) inside "
           "0x%07X..0x%07X load EVENT_DISPLAYSCUBA (%d) into ecx%s, so "
           "P_DisplayScuba's own body is not identified inside G_DisplayRest "
           "(r10669-ec5824db8 has exactly 1)", ev.anchors, begin, end,
           SHOT_EVENT_DISPLAYSCUBA,
           ev.faulted ? " and the body faulted mid-scan, so that count is partial"
                      : "");
    return false;
  }

  const unsigned kx[SHOT_SCUBA_SITES] = {
    (unsigned)SHOT_SCUBA_X_LEFT, (unsigned)SHOT_SCUBA_X_RIGHT,
    (unsigned)SHOT_SCUBA_X_SEAM
  };
  ScubaDraw d[SHOT_SCUBA_SITES];
  int anchored = 0, targets = 0, faulted = 0;
  unsigned target = 0;
  for (int i = 0; i < SHOT_SCUBA_SITES; i++) {
    find_scuba_draw(base, begin, end, kx[i], &d[i]);
    if (d[i].faulted) faulted = 1;
    if (!shot_scuba_anchor_ok(d[i].anchors, d[i].faulted)) continue;
    if (!shot_scuba_anchor_ok(d[i].calls, d[i].faulted)) continue;
    anchored++;
    sitesOut[i] = d[i].site;
    if (target == 0) { target = d[i].target; targets = 1; }
    else if (target != d[i].target) targets++;
  }

  EkSiteCount sc;
  find_sites_reaching(base, begin, end, target, &sc);
  if (sc.faulted) faulted = 1;

  if (!shot_scuba_locate_ok(anchored, targets, sc.count, faulted)) {
    ek_log("shot: the scuba mask stays on -- P_DisplayScuba's three draws not "
           "named. Inside 0x%07X..0x%07X, x=%d found %d position(s)/%d call(s), "
           "x=%d found %d/%d, x=%d found %d/%d; %d of 3 anchored, reaching %d "
           "distinct callee(s), and %d site(s) in the bracket reach 0x%07X%s "
           "(r10669-ec5824db8 has 3, 1 and 3)",
           begin, end,
           SHOT_SCUBA_X_LEFT, d[0].anchors, d[0].calls,
           SHOT_SCUBA_X_RIGHT, d[1].anchors, d[1].calls,
           SHOT_SCUBA_X_SEAM, d[2].anchors, d[2].calls,
           anchored, targets, sc.count, target,
           faulted ? " (a scan faulted mid-walk, so those counts are partial)"
                   : "");
    return false;
  }

  ek_log("shot: P_DisplayScuba is INLINED into G_DisplayRest and its three "
         "G_DrawTileScaled draws are at rva 0x%07X (x %d), 0x%07X (x %d) and "
         "0x%07X (x %d); all three reach 0x%07X, which %d site(s) inside "
         "0x%07X..0x%07X reach in total, and the EVENT_DISPLAYSCUBA guard at "
         "rva 0x%07X corroborates",
         sitesOut[0], SHOT_SCUBA_X_LEFT, sitesOut[1], SHOT_SCUBA_X_RIGHT,
         sitesOut[2], SHOT_SCUBA_X_SEAM, target, sc.count, begin, end,
         ev.anchorRva);
  *targetOut = target;
  return true;
}

// The address the three redirected sites used to reach, and their patches. The count is the ONE authority on whether
// the redirect is in: the thunk forwards unless it is three, so a half-installed state draws the mask as the engine
// would.
static DrawTileScaledFn s_scubaOrig = NULL;
static EkCallPatch s_scubaPatch[SHOT_SCUBA_SITES];
static int s_scubaPatched = 0;
static bool s_saidNoScubaOrig = false;
static bool s_saidScubaStranger = false;

bool shot_scuba_available(void) { return shot_scuba_installed(s_scubaPatched); }

// THE THUNK: seven int arguments (four in ecx/edx/r8d/r9d, the rest at [rsp+0x20..0x30]); all three sites write the
// seventh slot and keep no value in a volatile register across the call. Same thread as the flag's writer.
static void EK_DrawTileScaledThunk(int drawX, int drawY, int tileNum,
                                   int drawShade, int drawBits, int drawPal,
                                   int uniqueID) {
  DrawTileScaledFn const orig = s_scubaOrig;
  if (!orig) {
    // Unreachable: the address goes in before the first patch and is cleared after every patch is out. Said once.
    if (!s_saidNoScubaOrig) {
      s_saidNoScubaOrig = true;
      ek_log("shot: the scuba thunk ran with no original to forward to, which "
             "nothing in this build can produce -- please report it. The mask "
             "is not drawn while this lasts");
    }
    return;
  }
  if (shot_scuba_installed(s_scubaPatched)) {
    if (!shot_draws_scuba(s_active, drawX, drawShade, drawBits)) return;
    // A call through one of our sites that is NOT one of the mask's draws would mean the locate named the wrong
    // callee. It forwards; said once.
    if (s_active && !s_saidScubaStranger) {
      s_saidScubaStranger = true;
      ek_log("shot: a redirected scuba draw arrived with x %d, shade %d, bits "
             "0x%X, which is not one of P_DisplayScuba's three -- forwarding it "
             "and leaving the mask alone. Please report it", drawX, drawShade,
             drawBits);
    }
  }
  orig(drawX, drawY, tileNum, drawShade, drawBits, drawPal, uniqueID);
}

// ALL THREE OR NONE, AND THE UNWIND IS THE POINT: s_scubaPatched stays 0 through the loop, so a patched site is
// already harmless (its thunk forwards) until the unwind takes it out.
static void install_scuba(const unsigned char* base, unsigned begin,
                          unsigned end) {
  unsigned target = 0;
  unsigned sites[SHOT_SCUBA_SITES];
  if (!locate_scuba(base, begin, end, &target, sites)) return;

  s_scubaOrig = (DrawTileScaledFn)(const void*)(base + target);
  int in = 0;
  for (int i = 0; i < SHOT_SCUBA_SITES; i++) {
    if (!callsite_install((void*)(base + sites[i]), (void*)(base + target),
                          (void*)&EK_DrawTileScaledThunk, &s_scubaPatch[i]))
      break;
    in++;
  }

  if (in == SHOT_SCUBA_SITES) {
    s_scubaPatched = SHOT_SCUBA_SITES;
    ek_log("shot: the scuba mask redirect is IN -- all %d call(s), at rva "
           "0x%07X, 0x%07X and 0x%07X, now reach our thunk, which forwards to "
           "0x%07X on every frame except while screenshot mode is active",
           SHOT_SCUBA_SITES, sites[0], sites[1], sites[2], target);
    return;
  }

  int out = 0;
  for (int i = 0; i < in; i++) if (callsite_remove(&s_scubaPatch[i])) out++;
  if (out == in) s_scubaOrig = NULL;
  ek_log("shot: the scuba mask's redirect was NOT installed -- %d of %d site(s) "
         "took the patch and %d of those were put back; the mask stays on in a "
         "shot. The weapon, the crosshair, the stats and the status bar are "
         "unaffected", in, SHOT_SCUBA_SITES, out);
}

// AA'S CON-DRAWN HUD: THREE EVENT CALLS, ONE REDIRECT EACH. AA draws its weapon, crosshair and HUD in CON events that
// fire BEFORE every cvar gate above, so the `mov ecx, EVENT_*` + `call` that fires them is redirected (WEAPON 39,
// REST 60, CROSSHAIR 59). The callees are VM_ExecuteEvent's 3-arg and WithValue forms (the latter also reads r9d).

typedef int (*VmEvent3Fn)(int eventId, int spriteNum, int playerNum);
typedef int (*VmEventValueFn)(int eventId, int spriteNum, int playerNum, int dflt);

// REST's call is 51 bytes after its `mov ecx`, the longest of the three.
static const unsigned EK_EVENT_ARG_WINDOW = 64;

enum { EK_HUD_WEAPON = 0, EK_HUD_CROSSHAIR = 1, EK_HUD_REST = 2, EK_HUD_SITES = 3 };
static const char* const kHudName[EK_HUD_SITES] =
    { "EVENT_DISPLAYWEAPON", "EVENT_DISPLAYCROSSHAIR", "EVENT_DISPLAYREST" };
static const int kHudEvent[EK_HUD_SITES] =
    { SHOT_EVENT_DISPLAYWEAPON, SHOT_EVENT_DISPLAYCROSSHAIR, SHOT_EVENT_DISPLAYREST };

// Same thread as s_active. s_hudIn goes to 1 only after the patch is in and to 0 before removal, so a thunk outside
// that window forwards.
static const void* s_hudOrig[EK_HUD_SITES] = { NULL, NULL, NULL };
static EkCallPatch s_hudPatch[EK_HUD_SITES];
static int s_hudIn[EK_HUD_SITES] = { 0, 0, 0 };
static bool s_hudSaidNoOrig[EK_HUD_SITES] = { false, false, false };
static bool s_hudSaidStranger[EK_HUD_SITES] = { false, false, false };
static bool s_hudSaidHeld[EK_HUD_SITES] = { false, false, false };

int shot_hud_events_installed(void) {
  return s_hudIn[EK_HUD_WEAPON] + s_hudIn[EK_HUD_CROSSHAIR] + s_hudIn[EK_HUD_REST];
}

// The FIRST call after the `mov ecx` that reaches a function entry: ecx is volatile, so no later call can receive it.
static bool first_entry_call(const unsigned char* base, unsigned q, unsigned end,
                             unsigned* site, unsigned* callee) {
  const unsigned stop = (end - q < EK_EVENT_ARG_WINDOW) ? end : q + EK_EVENT_ARG_WINDOW;
  for (unsigned c = q + 5; c + 5 <= stop; c++) {
    if (base[c] != 0xE8) continue;
    const int disp = *(const int*)(base + c + 1);
    const unsigned t = ekm_us_rel32_target(c, disp);
    unsigned b = 0, e = 0;
    if (!ek_image_extent_containing(base, t, &b, &e) || b != t) continue;
    *site = c;
    *callee = t;
    return true;
  }
  return false;
}

// `raw` is every `mov ecx, imm` seen; `anchors` those that count (all in a bracket, only those reaching
// VM_ExecuteEvent image-wide).
struct HudSite {
  int raw; int anchors; unsigned anchorRva; unsigned site; unsigned callee;
  int faulted;
};

static void hud_site_clear(HudSite* out) {
  out->raw = 0; out->anchors = 0; out->anchorRva = 0;
  out->site = 0; out->callee = 0; out->faulted = 0;
}

static void find_event_in_bracket(const unsigned char* base, unsigned begin,
                                  unsigned end, unsigned imm, HudSite* out) {
  hud_site_clear(out);
  if (end <= begin + (unsigned)EKM_CALL_LEN) { out->faulted = 1; return; }
  __try {
    for (unsigned q = begin; q + 5 <= end; q++) {
      if (base[q] != 0xB9 || *(const unsigned*)(base + q + 1) != imm) continue;
      out->raw++;
      if (++out->anchors > 1) continue;
      out->anchorRva = q;
      first_entry_call(base, q, end, &out->site, &out->callee);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// IMAGE-WIDE, because P_DisplayWeapon is its own function and a later build may inline it; only one of the
// `mov ecx, 39`s may reach VM_ExecuteEvent.
static void find_weapon_event(const unsigned char* base, unsigned vmEvent3,
                              HudSite* out) {
  hud_site_clear(out);
  if (vmEvent3 == 0) { out->faulted = 1; return; }
  EkSpan spans[16];
  int n = 0;
  if (!ek_image_sections(base, true, spans, 16, &n)) { out->faulted = 1; return; }
  for (int i = 0; i < n; i++) {
    if (spans[i].size < 5) continue;
    const unsigned sb = ek_image_rva(base, spans[i].base);
    const unsigned se = sb + (unsigned)spans[i].size;
    __try {
      for (unsigned q = sb; q + 5 <= se; q++) {
        if (base[q] != 0xB9) continue;
        if (*(const unsigned*)(base + q + 1) != (unsigned)SHOT_EVENT_DISPLAYWEAPON) continue;
        out->raw++;
        unsigned s = 0, t = 0;
        if (!first_entry_call(base, q, se, &s, &t) || t != vmEvent3) continue;
        if (++out->anchors > 1) continue;
        out->anchorRva = q; out->site = s; out->callee = t;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      out->faulted = 1;
    }
  }
}

// Once per site: the mode was on and the call still forwarded. A stranger event id means the locate named the wrong
// call.
static void hud_note_forward(int k, int eventId, bool guardClear) {
  if (eventId != kHudEvent[k]) {
    if (s_hudSaidStranger[k]) return;
    s_hudSaidStranger[k] = true;
    ek_log("shot: the %s redirect received event %d, not %d -- forwarding it "
           "untouched. Please report it", kHudName[k], eventId, kHudEvent[k]);
    return;
  }
  if (guardClear || s_hudSaidHeld[k]) return;
  s_hudSaidHeld[k] = true;
  ek_log("shot: AA's %s ran during screenshot mode because its guard held "
         "(said once per session)", kHudName[k]);
}

// brutalcount 1..3, or a failed read, runs the weapon event (shot.h says why).
static bool hud_weapon_guard(int playerNum) {
  int brutal = 0;
  const bool got = gamevars_shot_guard(EK_GV_SG_BRUTALCOUNT, playerNum, &brutal);
  const bool clear = shot_weapon_guard_clear(got, brutal);
  if (!clear && !s_hudSaidHeld[EK_HUD_WEAPON])
    ek_log("shot: weapon guard -- brutalcount %s %d", got ? "reads" : "UNREAD", brutal);
  return clear;
}

static const int kModalVar[SHOT_MODALS] = {
  EK_GV_SG_CRAWLCLOCK, EK_GV_SG_VENDOR, EK_GV_SG_VRMENU, EK_GV_SG_STARTMODE,
  EK_GV_SG_BONUS, EK_GV_SG_EPSELECT, EK_GV_SG_MATCHMENU, EK_GV_SG_VRSCREEN,
  EK_GV_SG_THISCAM
};

static bool hud_rest_guard(int playerNum) {
  int v[SHOT_MODALS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  int mask = 0;
  for (int i = 0; i < SHOT_MODALS; i++) {
    int x = 0;
    const bool got = gamevars_shot_guard(kModalVar[i], playerNum, &x);
    if (got) { v[i] = x; mask |= (1 << i); }
  }
  const bool clear = shot_rest_guard_clear(mask, v);
  if (!clear && !s_hudSaidHeld[EK_HUD_REST])
    ek_log("shot: rest guard -- read mask 0x%X; crawlclock %d, vendor_screen %d, "
           "vrmenu %d, startmode %d, bonus_screen %d, epselect %d, matchmenu %d, "
           "vrscreen_time %d, thiscam %d", mask,
           v[SHOT_MODAL_CRAWLCLOCK], v[SHOT_MODAL_VENDOR], v[SHOT_MODAL_VRMENU],
           v[SHOT_MODAL_STARTMODE], v[SHOT_MODAL_BONUS], v[SHOT_MODAL_EPSELECT],
           v[SHOT_MODAL_MATCHMENU], v[SHOT_MODAL_VRSCREEN], v[SHOT_MODAL_THISCAM]);
  return clear;
}

// Unreachable: the address goes in before the patch and is cleared after it comes out.
static void hud_said_no_orig(int k) {
  if (s_hudSaidNoOrig[k]) return;
  s_hudSaidNoOrig[k] = true;
  ek_log("shot: the %s thunk ran with no original to forward to, which nothing "
         "in this build can produce -- please report it", kHudName[k]);
}

static int EK_DisplayWeaponThunk(int eventId, int spriteNum, int playerNum) {
  VmEvent3Fn const orig = (VmEvent3Fn)s_hudOrig[EK_HUD_WEAPON];
  if (!orig) { hud_said_no_orig(EK_HUD_WEAPON); return 0; }
  if (s_hudIn[EK_HUD_WEAPON] && s_active) {
    const bool clear = eventId == SHOT_EVENT_DISPLAYWEAPON && hud_weapon_guard(playerNum);
    if (shot_event_skips(true, SHOT_EVENT_DISPLAYWEAPON, eventId, clear))
      return shot_event_forced_return(eventId);
    hud_note_forward(EK_HUD_WEAPON, eventId, clear);
  }
  return orig(eventId, spriteNum, playerNum);
}

static int EK_DisplayCrosshairThunk(int eventId, int spriteNum, int playerNum,
                                    int dflt) {
  VmEventValueFn const orig = (VmEventValueFn)s_hudOrig[EK_HUD_CROSSHAIR];
  if (!orig) { hud_said_no_orig(EK_HUD_CROSSHAIR); return dflt; }
  if (s_hudIn[EK_HUD_CROSSHAIR] && s_active) {
    if (shot_event_skips(true, SHOT_EVENT_DISPLAYCROSSHAIR, eventId, true))
      return shot_event_forced_return(eventId);
    hud_note_forward(EK_HUD_CROSSHAIR, eventId, true);
  }
  return orig(eventId, spriteNum, playerNum, dflt);
}

static int EK_DisplayRestThunk(int eventId, int spriteNum, int playerNum) {
  VmEvent3Fn const orig = (VmEvent3Fn)s_hudOrig[EK_HUD_REST];
  if (!orig) { hud_said_no_orig(EK_HUD_REST); return 0; }
  if (s_hudIn[EK_HUD_REST] && s_active) {
    const bool clear = eventId == SHOT_EVENT_DISPLAYREST && hud_rest_guard(playerNum);
    if (shot_event_skips(true, SHOT_EVENT_DISPLAYREST, eventId, clear))
      return shot_event_forced_return(eventId);
    hud_note_forward(EK_HUD_REST, eventId, clear);
  }
  return orig(eventId, spriteNum, playerNum);
}

// One site, all or nothing: a refusal installs nothing and says which count refused.
static void install_hud_site(const unsigned char* base, int k, const HudSite& s,
                             unsigned vmEvent3, void* thunk, const char* where) {
  if (!shot_event_site_ok(s.anchors, s.faulted) || s.site == 0) {
    ek_log("shot: AA's %s stays on in a shot -- %d `mov ecx, %d` position(s) "
           "%s counted (%d seen)%s%s; exactly one, followed by a call to a "
           "function entry, is what names the site", kHudName[k], s.anchors,
           kHudEvent[k], where, s.raw,
           (s.anchors == 1 && s.site == 0) ? ", and no entry call follows it" : "",
           s.faulted ? ", and the scan faulted or had no callee to match" : "");
    return;
  }
  if (!shot_event_callee_ok(kHudEvent[k], s.callee, vmEvent3)) {
    ek_log("shot: AA's %s stays on in a shot -- its call at rva 0x%07X reaches "
           "0x%07X, and VM_ExecuteEvent (named by EVENT_DISPLAYSCUBA's guard) is "
           "0x%07X; the weapon and rest calls must reach it, the crosshair's must "
           "not", kHudName[k], s.site, s.callee, vmEvent3);
    return;
  }
  s_hudOrig[k] = (const void*)(base + s.callee);
  if (!callsite_install((void*)(base + s.site), (void*)(base + s.callee), thunk,
                        &s_hudPatch[k])) {
    s_hudOrig[k] = NULL;
    ek_log("shot: AA's %s call at rva 0x%07X was NOT redirected (hook.cpp said "
           "why); it stays on in a shot", kHudName[k], s.site);
    return;
  }
  s_hudIn[k] = 1;
  ek_log("shot: AA's %s redirect is IN -- `mov ecx, %d` at rva 0x%07X, the call "
         "at 0x%07X now reaches our thunk, which forwards to 0x%07X except while "
         "screenshot mode is active and its guard is clear", kHudName[k],
         kHudEvent[k], s.anchorRva, s.site, s.callee);
}

static void install_hud_events(const unsigned char* base, unsigned begin,
                               unsigned end) {
  // VM_ExecuteEvent's three-argument form, named by P_DisplayScuba's inlined guard (player.cpp:2161).
  HudSite ev;
  find_event_in_bracket(base, begin, end, (unsigned)SHOT_EVENT_DISPLAYSCUBA, &ev);
  unsigned vm3 = 0;
  if (shot_event_site_ok(ev.anchors, ev.faulted) && ev.site != 0) vm3 = ev.callee;
  else
    ek_log("shot: VM_ExecuteEvent not named -- %d EVENT_DISPLAYSCUBA guard(s) in "
           "0x%07X..0x%07X%s; AA's weapon and rest events stay on in a shot",
           ev.anchors, begin, end, ev.faulted ? " (faulted)" : "");

  HudSite rest, xhair, weapon;
  find_event_in_bracket(base, begin, end, (unsigned)SHOT_EVENT_DISPLAYREST, &rest);
  install_hud_site(base, EK_HUD_REST, rest, vm3, (void*)&EK_DisplayRestThunk,
                   "inside G_DisplayRest");
  find_event_in_bracket(base, begin, end, (unsigned)SHOT_EVENT_DISPLAYCROSSHAIR, &xhair);
  install_hud_site(base, EK_HUD_CROSSHAIR, xhair, vm3,
                   (void*)&EK_DisplayCrosshairThunk, "inside G_DisplayRest");
  find_weapon_event(base, vm3, &weapon);
  install_hud_site(base, EK_HUD_WEAPON, weapon, vm3, (void*)&EK_DisplayWeaponThunk,
                   "image-wide reaching VM_ExecuteEvent");
}

// ONCE PER SESSION, FROM overlay.cpp's PER-FRAME PATH ONLY: the game thread inside SwapBuffers is hook.cpp's write
// window, so there is no instruction stream to tear. UNCONDITIONAL, not gated on the row: the thunk costs a flag
// read.
static void install_sbar(const unsigned char* base, unsigned begin,
                         unsigned end) {
  unsigned target = 0, site = 0;
  if (!locate_sbar(base, begin, end, &target, &site)) return;

  // THE ADDRESS GOES IN FIRST: the patch is live the instant its four bytes land, so the thunk must know where to
  // forward.
  s_sbarOrig = (DrawStatusBarFn)(const void*)(base + target);
  if (!callsite_install((void*)(base + site), (void*)(base + target),
                        (void*)&EK_DrawStatusBarThunk, &s_sbarPatch)) {
    s_sbarOrig = NULL;
    ek_log("shot: the status bar's call site at rva 0x%07X was NOT redirected; "
           "the game keeps its own call and the bar stays up in a shot. The "
           "weapon, the crosshair and the stats are unaffected", site);
    return;
  }

  ek_log("shot: the status bar redirect is IN -- the call at rva 0x%07X now "
         "reaches our thunk, which forwards to 0x%07X on every frame except "
         "while screenshot mode is active", site, target);
}

void shot_install(void) {
  static bool tried = false;
  if (tried) return;
  tried = true;

  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  if (!base) {
    ek_log("shot: neither redirect goes in -- GetModuleHandleA(NULL) returned "
           "nothing, so the host image cannot be searched (%lu)", GetLastError());
    return;
  }

  // ONE BRACKET, EACH REDIRECT MAY REFUSE ALONE: the status bar, scuba mask and two HUD events are inside
  // G_DisplayRest, so the anchor is walked once; a build identifying only one still gets that one.
  unsigned begin = 0, end = 0;
  if (!locate_displayrest(base, "neither redirect goes in", &begin, &end)) return;

  install_sbar(base, begin, end);
  install_scuba(base, begin, end);
  install_hud_events(base, begin, end);
}

void shot_remove(void) {
  // AA's HUD events: every flag to 0 first, so a thunk left reachable by a failed removal forwards.
  for (int k = 0; k < EK_HUD_SITES; k++) s_hudIn[k] = 0;
  for (int k = 0; k < EK_HUD_SITES; k++) {
    if (!s_hudPatch[k].site) continue;
    if (callsite_remove(&s_hudPatch[k])) s_hudOrig[k] = NULL;
    else
      ek_log("shot: AA's %s call would not take its original bytes back; the "
             "thunk stays reachable and forwards every call", kHudName[k]);
  }

  // THE COUNT GOES TO ZERO BEFORE THE FIRST REMOVAL, so a thunk still reachable from a site that would not take its
  // bytes back sees a count that is not three and forwards.
  s_scubaPatched = 0;
  int in = 0, out = 0;
  for (int i = 0; i < SHOT_SCUBA_SITES; i++) {
    if (!s_scubaPatch[i].site) continue;
    in++;
    if (callsite_remove(&s_scubaPatch[i])) out++;
  }
  // FORWARDING OUTLIVES A FAILED RESTORE: hook.cpp keeps the patch and stub page when it cannot write the operand
  // back, so the thunk still needs somewhere to go; clearing the address would make a failed removal fatal.
  if (in > 0 && out == in) s_scubaOrig = NULL;
  if (in != out)
    ek_log("shot: %d of %d scuba site(s) would not take their original bytes "
           "back, so the thunk stays reachable -- it forwards every call from "
           "here, which draws the mask exactly as the engine would", in - out,
           in);

  if (!s_sbarPatch.site) return;
  if (callsite_remove(&s_sbarPatch)) s_sbarOrig = NULL;
}
