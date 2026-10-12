// The fov row, DLL half; rules are pure in fov.h. No hook: `fov` is a plain CVAR_INT over ud.fov (osdcmds.cpp:1650),
// recomputed each frame (game.cpp:890), so this writes through the cvar registry like the console.
#include <windows.h>

#include "fov.h"
#include "cvars.h"
#include "config.h"
#include "log.h"

static const char kFovCvar[] = "fov";

static EkFovHold s_hold = { 0, 0, 0 };
static int s_prevOn = 0;
static int s_gaveUp = 0;

static void fov_give_up(const char* why) {
  if (s_gaveUp) return;
  s_gaveUp = 1;
  ek_log("fov: %s -- the row is giving up for this session rather than "
         "retrying every frame", why);
}

static bool fov_write(int v, int* applied) {
  if (cvar_set_int(kFovCvar, v, applied)) return true;
  fov_give_up("the fov cvar will not take a write");
  return false;
}

// Stash hits disk on the two edges only (input.cpp:360: drag-rate rewrites risk holding the file in a crash).
static void fov_stash_store(void) {
  g_cfg.fovstashheld = s_hold.held;
  g_cfg.fovstashsaved = s_hold.saved;
  g_cfg.fovstashforced = s_hold.forced;
  g_cfgDirty = 1;
}

static void fov_stash_clear(void) {
  g_cfg.fovstashheld = 0;
  g_cfg.fovstashsaved = 0;
  g_cfg.fovstashforced = 0;
  g_cfgDirty = 1;
}

bool fov_available(void) {
  return cvar_row_available(cvars_find(kFovCvar));
}

int fov_live(int* out) {
  if (!out) return 0;
  return cvar_get_int(kFovCvar, out) ? 1 : 0;
}

void fov_hold_state(int* held, int* saved, int* forced) {
  if (held) *held = s_hold.held;
  if (saved) *saved = s_hold.saved;
  if (forced) *forced = s_hold.forced;
}

void fov_frame(void) {
  const int on = g_cfg.fovon != 0;
  const int edge = (on == s_prevOn) ? 0 : (on ? 1 : -1);
  s_prevOn = on;
  if (!edge && !s_hold.held) return;
  if (s_gaveUp) {
    // The off edge clears the hold even if nothing can be written, or diagnostics say "held" forever.
    if (edge < 0 && s_hold.held) {
      s_hold.held = 0;
      s_hold.saved = 0;
      s_hold.forced = 0;
    }
    return;
  }

  int live = 0;
  if (!cvar_get_int(kFovCvar, &live)) {
    fov_give_up("the fov cvar will not read");
    return;
  }

  int want = 0;
  const int act = ekm_fov_step(&s_hold, edge, live, g_cfg.fov, &want);

  if (act == EK_FOV_ACT_SET) {
    fov_stash_store();
    config_write_changed();
    int applied = 0;
    if (!fov_write(want, &applied)) return;
    ek_log("fov: on -- the player's %d saved, %d forced (applied %d)",
           s_hold.saved, want, applied);
    return;
  }

  if (act == EK_FOV_ACT_RESTORE) {
    int applied = 0;
    if (!fov_write(want, &applied)) return;
    fov_stash_clear();
    config_write_changed();
    ek_log("fov: off -- the player's %d put back (applied %d)", want, applied);
    return;
  }

  // Slider move while held re-forces without touching `saved`; a console change is not fought.
  if (!ekm_fov_retarget(&s_hold, g_cfg.fov, &want)) return;
  fov_stash_store();
  int applied = 0;
  if (!fov_write(want, &applied)) return;
  ek_log("fov: slider -- %d forced (applied %d), the player's %d still held",
         want, applied, s_hold.saved);
}

void fov_replay_stash(void) {
  if (!g_cfg.fovstashheld) return;

  int live = 0;
  if (!cvar_get_int(kFovCvar, &live)) {
    fov_give_up("the fov cvar will not read at install, so a stash from a "
                "previous session cannot be judged");
    return;
  }

  const int saved = g_cfg.fovstashsaved;
  const int forced = g_cfg.fovstashforced;
  const int answer = ekm_fov_stash_answer(g_cfg.fovstashheld, forced, saved, live);
  if (answer == EK_FOV_STASH_REPLAY) {
    int applied = 0;
    if (fov_write(saved, &applied))
      ek_log("fov: replayed a crash stash from a previous session -- the live "
             "fov still read the %d we forced, so the player's %d is back "
             "(applied %d)", forced, saved, applied);
  } else if (answer == EK_FOV_STASH_BACK) {
    // Not a failure: after a crash settings.cfg still holds the player's fov (CONFIG_WriteSetup never ran), already
    // live.
    ek_log("fov: a stash from a previous session is already back in the live "
           "fov -- it saved %d and fov reads %d, so nothing was replayed and "
           "the stash is dropped. This is what a crash that never wrote "
           "settings.cfg leaves behind (it had forced %d)",
           saved, live, forced);
  } else {
    // Live value matches neither forced nor saved (hand change or other build): left alone.
    ek_log("fov: a stash from a previous session is not trustworthy -- it "
           "forced %d and saved %d, and fov now reads %d. Nothing was written "
           "and the stash is dropped", forced, saved, live);
  }
  fov_stash_clear();
  config_write_changed();
}
