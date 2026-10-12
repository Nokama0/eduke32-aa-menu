// No recoil, live half (rule: norecoil.h): moves q16horiz to where AA's recovery would end and zeroes horizfall so that
// recovery never runs. Limit: at low fps a second tic in a batch can fire mid-kick.

#include <windows.h>
#include "norecoil.h"
#include "config.h"
#include "gamevars.h"
#include "playerstate.h"
#include "udstate.h"
#include "input.h"
#include "log.h"
#include "ui/wallclock.h"

static const int EK_NR_LOCAL_PLAYER = 0;

// Game thread only; a refused view write latches here.
static bool s_viewDead = false;
static unsigned s_kicks = 0, s_units = 0;
static unsigned s_logKicks = 0, s_lastLogMs = 0;

static void maybe_log(void) {
  if (!g_cfg.debuglog || s_kicks == s_logKicks) return;
  const unsigned now = wallclock_ms();
  if (now - s_lastLogMs < 1000) return;
  s_lastLogMs = now;
  s_logKicks = s_kicks;
  ek_log("norecoil: %u kick(s) collapsed, %u horiz unit(s) removed", s_kicks, s_units);
}

static void collapse_now(void) {
  int gm = 0;
  const bool gmOk = player_read_gm(&gm);
  if (!esp_level_loaded(gm, gmOk)) return;
  bool pauseOn = true;
  const bool paused = !ud_paused(&pauseOn) || pauseOn;
  if (paused || input_panel_open()) return;

  int horizfall = 0;
  const bool fallOk = gamevars_recoil(EK_GV_RV_HORIZFALL, EK_NR_LOCAL_PLAYER, &horizfall);
  if (!fallOk || horizfall <= 0) return;
  int saved = 0;
  const bool savedOk = gamevars_recoil(EK_GV_RV_SAVEDHORIZ, EK_NR_LOCAL_PLAYER, &saved);
  if (!savedOk) return;
  PlayerSnap snap;
  const bool snapOk = player_snap(&snap);
  if (!snapOk) return;
  int q16 = 0, removed = 0;
  if (!ekm_norecoil_collapse(snap.horiz, horizfall, saved, &q16, &removed)) return;

  // horizfall first: its write can be refused; if the view write then fails, horizfall is restored.
  if (!gamevars_recoil_set(EK_GV_RV_HORIZFALL, EK_NR_LOCAL_PLAYER, 0)) return;
  // Also write the old view: the tic copies the kicked q16horiz to oq16horiz (actors.cpp:1540) and the draw
  // interpolates.
  if (!player_write_view(snap.ang, q16, true)) {
    gamevars_recoil_set(EK_GV_RV_HORIZFALL, EK_NR_LOCAL_PLAYER, horizfall);
    s_viewDead = true;
    ek_log("norecoil: the q16horiz write was refused; horizfall restored to %d and "
           "no recoil stops for this session", horizfall);
    return;
  }
  s_kicks++;
  s_units += (unsigned)removed;
}

void norecoil_before_draw(void) {
  if (!g_cfg.norecoil || s_viewDead) return;
  collapse_now();
}

void norecoil_frame(void) {
  if (!g_cfg.norecoil || s_viewDead) return;
  maybe_log();
  collapse_now();
}
