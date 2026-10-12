// Weapon speed, engine half (weaponspeed.h holds every decision): reads the player's weapon fields, asks the pure
// gates, and writes back only kickback_pic, weapon_pos and AA's own m4reload/uzireload counters.

#include <windows.h>
#include <string.h>
#include "weaponspeed.h"
#include "firestops.h"
#include "gamevars.h"
#include "gamearrays.h"
#include "config.h"
#include "playerstate.h"
#include "udstate.h"
#include "input.h"
#include "log.h"
#include "ui/wallclock.h"
#include "weapondata.h"
#include "camerascan.h"
#include "freecam.h"

// The local player is 0 (weapondata.cpp's argument): playerstate.cpp anchors on g_player[0].ps, the DukePlayer_t
// whose kickback_pic this file writes.
static const int EK_WS_LOCAL_PLAYER = 0;

static bool s_haveLast = false;
static unsigned s_lastMs = 0;

// Milliseconds owed toward the next tic, carried across frames; reset on every refusal so a paused stretch is not
// banked.
static double s_ticAcc = 0.0;

// One carry per row, persisting across tics: at 150 the extra is half a step a tic, and a per-tic reset would floor
// it to 0.
static double s_kickCarry = 0.0;
static double s_posCarry  = 0.0;
// The reload ladder's own carry: a firing fraction must not land a step in the ladder or back.
static double s_reloadCarry = 0.0;
static double s_ctrCarry = 0.0;
static int s_lastWeapon = -1;

// weapon_pos's movement state, independent of the debuglog-gated trace state: it decides the write, so it runs with
// logging off.
static WswitchTrack s_wswitchTrack = { 0, false, 0 };

// Everything the feature stops doing when refused; called on every refusal path, since a leftover fraction is not the
// next sequence's to spend.
static void weaponspeed_reset(void) {
  s_ticAcc = 0.0;
  s_kickCarry = 0.0;
  s_posCarry = 0.0;
  s_reloadCarry = 0.0;
  s_ctrCarry = 0.0;
}

static const char* weaponspeed_source_name(int src) {
  if (src == EK_STOPS_LIVE) return "live";
  if (src == EK_STOPS_REFUSE) return "refused";
  return "static";
}

static void weaponspeed_log_once(const char* what, int rate, int from, int to) {
  if (!g_cfg.debuglog) return;
  ek_log("weaponspeed: first %s write -- %d%%, %d to %d", what, rate, from, to);
}

static unsigned s_wposCount[21];
static bool s_wposSeen[21];

static bool s_haveWposPending = false;
static int s_wposPendingWrote = 0;

const int WPOS_TRACE_SEQ_CAP = 16;
static int s_seqLogCount = 0;
static int s_traceLastPos = 0;

static void weaponspeed_hist_text(char* out, int cap) {
  if (!out || cap <= 0) return;
  out[0] = 0;
  int used = 0;
  for (int i = 0; i < 21; i++) {
    if (!s_wposSeen[i]) continue;
    if (used >= cap - 1) break;
    const int n = _snprintf_s(out + used, (size_t)(cap - used), _TRUNCATE,
                              "%s%d:%u", used ? "," : "", i - 9, s_wposCount[i]);
    if (n < 0) break;
    used += n;
  }
}

static void weaponspeed_trace_wpos(int weaponPos, bool switchOk, int switchRate,
                                   bool ready, bool menuOpen, bool paused,
                                   bool panelOpen, int weapon) {
  if (!g_cfg.debuglog) return;

  if ((s_traceLastPos == 0 && weaponPos != 0) || weapon != s_lastWeapon)
    s_seqLogCount = 0;
  s_traceLastPos = weaponPos;

  const int idx = weaponPos + 9;
  if (idx >= 0 && idx < 21) {
    s_wposCount[idx]++;
    if (!s_wposSeen[idx]) {
      s_wposSeen[idx] = true;
      char hist[192];
      weaponspeed_hist_text(hist, sizeof(hist));
      ek_log("weaponspeed: wpos trace -- new value %d seen, histogram %s",
             weaponPos, hist);
    }
  } else {
    ek_log("weaponspeed: wpos trace -- value %d is OUTSIDE -9..11", weaponPos);
  }

  if (s_haveWposPending) {
    const char* const kind = wswitch_readback_kind(s_wposPendingWrote, weaponPos);
    const bool foreign = !strncmp(kind, "slam", 4);
    if (foreign) {
      static long disagreeCount = 0;
      disagreeCount++;
      if (s_seqLogCount < WPOS_TRACE_SEQ_CAP) {
        ek_log("weaponspeed: wpos trace -- wrote %d, read back %d (%s) "
               "DISAGREE #%ld", s_wposPendingWrote, weaponPos, kind, disagreeCount);
        s_seqLogCount++;
      }
    } else if (s_seqLogCount < WPOS_TRACE_SEQ_CAP) {
      ek_log("weaponspeed: wpos trace -- wrote %d, read back %d (%s)",
             s_wposPendingWrote, weaponPos, kind);
      s_seqLogCount++;
    }
    s_haveWposPending = false;
  }

  if (!switchOk && weaponPos != 0 && s_seqLogCount < WPOS_TRACE_SEQ_CAP) {
    const char* const reason = wswitch_gate_reason(ready, menuOpen, paused,
                                                    panelOpen, switchRate, weaponPos);
    ek_log("weaponspeed: wpos trace -- refused pos %d (%s)", weaponPos, reason);
    s_seqLogCount++;
  }
}

static void weaponspeed_trace_wpos_wrote(int next) {
  if (!g_cfg.debuglog) return;
  if (s_seqLogCount < WPOS_TRACE_SEQ_CAP) {
    ek_log("weaponspeed: wpos trace -- advance -> %d", next);
    s_seqLogCount++;
  }
  s_haveWposPending = true;
  s_wposPendingWrote = next;
}

static const int EK_PISTOL_WEAPON = 1;

// Trace (measurement only) of kickback_pic through a pistol reload. The range covers the firing window and AA's
// reload ladder past RELOAD (34 live); 64 matches FIRERATE_EVENTS_MAX. Never negative on a live read.
static unsigned s_kickCount[64];
static bool s_kickSeen[64];

const int KICK_TRACE_SEQ_CAP = 16;
static int s_kickSeqLogCount = 0;
static int s_kickLastKick = 0;

// Per-sequence, not per-process: s_kickSeen says a value was ever seen; these say what this reload did.
static bool s_kickSeqOpen = false;
static bool s_kickSeqSaw26 = false;
static int s_kickSeqSteps = 0;
// Max kickback is the filter: ordinary shots end at frame 7 or 8, and a raw "26 visited NO" count mixed them with
// genuine reload attempts (misread twice).
static int s_kickSeqMaxKick = 0;

static bool s_kick26Reported = false;
static int s_kick26ForWeapon = -1;

const int KICK_CLIP_TRACE_SEQ_CAP = 64;
static int s_kickClipLogCount = 0;
static int s_kickClipSeqOccurred = 0;
static bool s_kickSeqFFPerksLogged = false;

static KickTicAccumulator s_kickTicAcc;
static KickTicAccumulator s_accTicAcc;

// The same located clock camera.cpp reads (s_otclkAddr) with the same guarded-read shape: an interlocked sample of
// the address, then an SEH-guarded dereference, so an unmapped address logs once and gives up.
static int s_kickTicPrev = 0;
static bool s_kickTicHavePrev = false;
static bool s_kickTicReadFailed = false;
static bool s_kickTicFailSaid = false;

// Whether the clock has an address at all, apart from this frame's read: a locate that never happened is permanent
// (no cycle can be trusted), one faulting dereference is not.
static bool weaponspeed_tic_clock_located(void) {
  return InterlockedCompareExchange64(&s_otclkAddr, 0, 0) != 0;
}

static bool weaponspeed_read_tic(int* out) {
  const LONGLONG addr = InterlockedCompareExchange64(&s_otclkAddr, 0, 0);
  if (addr == 0) return false;
  __try {
    *out = *(const volatile int*)(size_t)addr;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Not this file's clock to clear: camera.cpp's step already clears s_otclkAddr on the same fault. This read only
    // stops counting for itself and says so once.
    return false;
  }
}

bool weaponspeed_engine_tic(int* out) {
  return out != NULL && weaponspeed_read_tic(out);
}

// A value slot that cannot be mistaken for a value (gamevars.cpp's rule): a failed read never prints 0.
static void kick_value_text(bool got, int v, char* out, size_t bytes) {
  if (got) _snprintf_s(out, bytes, _TRUNCATE, "%d", v);
  else     _snprintf_s(out, bytes, _TRUNCATE, "unread");
}

// The achieved readout's accounting runs with the Debug log off: the tic accountant used to sit behind debuglog and
// the pistol, so a readout built on it worked only while logging.

// Published for the render thread as interlocked scalars, never a struct or pointer. -1 on the span means nothing to
// show (no cycle closed yet, or the last was refused) and is the only such value.
static LONG s_achWeapon = -1;
static LONG s_achSpan = -1;
static LONG s_achStock = -1;
static LONG s_achSamples = 0;
static LONG s_achState = EK_ACH_MEASURING;

// Needs only to reach EK_RATE_MIN_SAMPLES; capped so a long session cannot walk a LONG toward its limit.
static const int EK_ACH_SAMPLES_MAX = 999;

static void weaponspeed_publish_achieved(int weapon, int stockSpan, int spanSum,
                                         int samples, int state, int lastSpan) {
  static int lastWeapon = -2;
  const bool weaponMoved = weapon != lastWeapon;
  // The -1-first, weapon-last order guarantees less than once claimed: a weapon change is not the one moment the pair
  // could be mixed.
  if (weaponMoved) InterlockedExchange(&s_achWeapon, -1);
  InterlockedExchange(&s_achSpan, (LONG)spanSum);
  InterlockedExchange(&s_achStock, (LONG)stockSpan);
  InterlockedExchange(&s_achSamples, (LONG)samples);
  InterlockedExchange(&s_achState, (LONG)state);
  if (weaponMoved) InterlockedExchange(&s_achWeapon, (LONG)weapon);
  lastWeapon = weapon;

  // Readout instrument: the published tuple, change-triggered, with the same ekm_rate_achieved_x100 call the menu
  // makes so log and row cannot disagree. Writes nothing new.
  if (g_cfg.debuglog) {
    static bool havePrevTuple = false;
    static EkAchTuple prevTuple = { 0, 0, 0, 0, 0, 0 };
    static unsigned long publishCount = 0;
    publishCount++;
    const int x100 = ekm_rate_achieved_x100(stockSpan, spanSum, samples);
    const EkAchTuple curTuple = { weapon, stockSpan, spanSum, samples, state, x100 };
    if (ekm_ach_tuple_changed(havePrevTuple, prevTuple, curTuple)) {
      havePrevTuple = true;
      prevTuple = curTuple;
      // Logs both the last cycle's span and the running sum, so one odd cycle stays visible although the row shows
      // the mean.
      ek_log("weaponspeed: readout publish #%lu -- weapon %d stock %d "
             "lastSpan %d sum %d samples %d state %s x100 %d",
             publishCount, weapon, stockSpan, lastSpan, spanSum, samples,
             ekm_ach_state_name(state), x100);
    }
  }
}

bool weaponspeed_achieved(int* outWeapon, int* outStockSpan, int* outMeasuredSpan,
                          int* outSamples, int* outState) {
  if (!outWeapon || !outStockSpan || !outMeasuredSpan || !outSamples || !outState)
    return false;
  // The weapon is read before and after the values that belong to it: if it moved under the read this frame says
  // nothing rather than naming the wrong gun.
  const LONG w0 = InterlockedCompareExchange(&s_achWeapon, 0, 0);
  const LONG span = InterlockedCompareExchange(&s_achSpan, 0, 0);
  const LONG stock = InterlockedCompareExchange(&s_achStock, 0, 0);
  const LONG samples = InterlockedCompareExchange(&s_achSamples, 0, 0);
  const LONG state = InterlockedCompareExchange(&s_achState, 0, 0);
  const LONG w1 = InterlockedCompareExchange(&s_achWeapon, 0, 0);
  if (w0 < 0 || w0 != w1) return false;
  *outWeapon = (int)w0;
  *outStockSpan = (int)stock;
  *outMeasuredSpan = (int)span;
  *outSamples = (int)samples;
  *outState = (int)state;
  return true;
}

// Game-thread state. The cycle bracket is its own (s_accLastKick), not the trace's s_kickLastKick, which only steps
// on pistol frames with the log on.
static int s_accLastKick = 0;
static bool s_accSeqOpen = false;
static int s_accSeqWeapon = -1;
static bool s_accSeqMixed = false;
// The cycle cannot be trusted if a frame straddled more than one tic, the clock could not be read, the rate moved
// under it, or it did not begin at frame 1: each makes the span short, and a short span overstates.
static bool s_accSeqSuspect = false;
// The open cycle's peak kickback_pic, raised every frame it stays open including through the reload ladder
// (ekm_cycle_closes ends the cycle only when the counter decreases).
static int s_accSeqMaxKick = 0;
static int s_accWeapon = -1;
static int s_accRate = 0;
static int s_accSamples = 0;
// The running sum of published spans (-1 = nothing published, which ekm_rate_achieved_x100's spanSum <= 0 guard
// refuses).
static int s_accSpanSum = -1;
static int s_accStock = -1;
// The most recent published cycle's own span, for the log line only; never read by the arithmetic.
static int s_accLastSpan = -1;

// One frame's accounting. The one tic read this file makes: *outTicAdvanced feeds the trace's own accumulator.
static void weaponspeed_account_kick(int kickback, int weapon, int rate,
                                     int achState, bool* outTicAdvanced) {
  // The tic read, once a frame, continuous across cycles (freecam_tic_advanced's "no previous reading is not a tic"
  // covers skipped frames). A failed read stops counting this frame and says so once with the log on.
  int curTic = 0;
  const bool ticOk = weaponspeed_read_tic(&curTic);
  if (!ticOk) {
    s_kickTicReadFailed = true;
    if (g_cfg.debuglog && !s_kickTicFailSaid) {
      s_kickTicFailSaid = true;
      ek_log("weaponspeed: kick trace -- tic clock unreadable; per-value tic "
             "counts stop here rather than print a made-up number");
    }
  }
  // The raw delta, a different question from "did it move": freecam_tic_advanced counts a two-tic gap once, but a tic
  // the span never counted makes the span short, and short overstates.
  const bool deltaSuspect = !ticOk ||
      ekm_tic_delta_suspect(s_kickTicPrev, curTic, s_kickTicHavePrev);
  const bool ticAdvanced = ticOk &&
      freecam_tic_advanced(s_kickTicPrev, curTic, s_kickTicHavePrev);
  if (ticOk) { s_kickTicPrev = curTic; s_kickTicHavePrev = true; }

  // Samples belong to one weapon at one rate: a span measured on the pistol, or at 200% before the slider moved, does
  // not describe what is held now.
  if (weapon != s_accWeapon || rate != s_accRate) {
    s_accWeapon = weapon;
    s_accRate = rate;
    s_accSamples = 0;
    s_accSpanSum = -1;
    s_accStock = -1;
    s_accLastSpan = -1;
    s_accSeqSuspect = true;
  }

  // The cycle closes on a decrease: a held automatic wraps TotalTime-1 -> 1 in one tic and never shows 0, so
  // ekm_cycle_closes covers both ends with one test (a tap still closes exactly one cycle).
  const bool closes = s_accSeqOpen && ekm_cycle_closes(s_accLastKick, kickback);
  if (s_accSeqOpen && deltaSuspect) s_accSeqSuspect = true;

  if (closes) {
    s_accSeqOpen = false;
    // stockSpan is derived and read live (AA rewrites TotalTime per character and upgrade). TotalTime - 1 was
    // verified at 100% (TotalTime 9 gave span 8 on 23 of 25 cycles).
    int totalTime = 0, holdDelay = 0, flags = 0;
    const bool ttOk = gamevars_weapon_field(weapon, EK_WD_TOTALTIME,
                                            EK_WS_LOCAL_PLAYER, &totalTime);
    const bool hdOk = gamevars_weapon_field(weapon, EK_WD_HOLDDELAY,
                                            EK_WS_LOCAL_PLAYER, &holdDelay);
    const bool flOk = gamevars_weapon_field(weapon, EK_WD_FLAGS,
                                            EK_WS_LOCAL_PLAYER, &flags);
    const int stock = (ttOk && hdOk && flOk)
        ? ekm_stock_cycle_span(totalTime, holdDelay, flags) : -1;

    // Readout instrument: the three reads behind `stock`, change-triggered. FLAGS is the one field nothing had proved
    // yet, so this line is the test for it.
    if (g_cfg.debuglog) {
      static bool haveFieldsPrev = false;
      static EkStockFieldRead prevFields = { false, false, false, 0 };
      static unsigned long fieldsSeen = 0;
      fieldsSeen++;
      const EkStockFieldRead curFields = { ttOk, hdOk, flOk, stock };
      if (ekm_stock_fields_changed(haveFieldsPrev, prevFields, curFields)) {
        haveFieldsPrev = true;
        prevFields = curFields;
        ek_log("weaponspeed: readout stock fields #%lu -- ttOk=%s tt=%d "
               "hdOk=%s hd=%d flOk=%s fl=%d stock=%d",
               fieldsSeen, ttOk ? "yes" : "no", totalTime, hdOk ? "yes" : "no",
               holdDelay, flOk ? "yes" : "no", flags, stock);
      }
    }

    // Why this cycle did or did not leave a clean measurement, on a change of reason, beside a cumulative
    // closed/published/ refused count (a count and an enumeration of the same thing must agree).
    const bool isReload = ekm_cycle_is_reload(s_accSeqMaxKick, totalTime);
    const bool publishableForLog = ekm_cycle_publishable(
        s_accSeqSuspect, s_accSeqMixed, s_accLastKick, stock);
    const int decision = ekm_cycle_close_decision(isReload, publishableForLog);

    if (g_cfg.debuglog) {
      static bool haveReasonPrev = false;
      static int prevReason = -1;
      static unsigned long closedCount = 0, publishedCount = 0, refusedCount = 0,
                           reloadCount = 0;
      closedCount++;
      // Reload is its own bucket, not refused: folding reloads into refused would pass the mismatch check while
      // hiding the number it exists to show.
      if (isReload) reloadCount++;
      else if (publishableForLog) publishedCount++;
      else refusedCount++;
      const int reason = ekm_cycle_close_reason(s_accSeqSuspect, s_accSeqMixed,
                                                s_accLastKick, stock,
                                                s_accSeqMaxKick, totalTime);
      if (!haveReasonPrev || reason != prevReason) {
        haveReasonPrev = true;
        prevReason = reason;
        ek_log("weaponspeed: readout cycle close -- reason %s, lastKick %d, "
               "stock %d, closed %lu published %lu refused %lu reloads %lu%s",
               ekm_cycle_close_reason_name(reason), s_accLastKick, stock,
               closedCount, publishedCount, refusedCount, reloadCount,
               (publishedCount + refusedCount + reloadCount != closedCount)
                   ? " (MISMATCH -- instrument suspect, CLAUDE.md)"
                   : "");
      }
    }

    if (decision == EK_CLOSE_PUBLISH) {
      // The mean, not the last cycle: ekm_rate_accum_publish folds this span into the running sum, resetting on a
      // stock change (AA rewrote TotalTime) or fresh start and halving before adding at the sample cap.
      s_accLastSpan = (int)s_accTicAcc.span();
      ekm_rate_accum_publish(&s_accSpanSum, &s_accSamples, &s_accStock,
                             s_accLastSpan, stock, EK_ACH_SAMPLES_MAX);
    }
    // EK_CLOSE_IGNORE (reload, or any refusal) neither publishes nor withdraws: withdrawing on refusal fell back to
    // "Fire a few shots" after every magazine (the chaingun's 1 -> 0 wrap stub).
  }

  // Opens only at a real beginning: the counter leaving rest, or the wrap this frame closed. A cycle joined halfway
  // is short by definition.
  if (!s_accSeqOpen && kickback != 0 && (s_accLastKick == 0 || closes)) {
    s_accTicAcc = KickTicAccumulator();
    s_accSeqOpen = true;
    s_accSeqWeapon = weapon;
    s_accSeqMixed = false;
    s_accSeqSuspect = deltaSuspect || kickback != 1;
    s_accSeqMaxKick = kickback;
  }
  if (s_accSeqOpen && weapon != s_accSeqWeapon) s_accSeqMixed = true;
  // Raised every frame the cycle stays open, including through the reload ladder: the only place the ladder's peak is
  // seen.
  if (s_accSeqOpen && kickback > s_accSeqMaxKick) s_accSeqMaxKick = kickback;

  s_accTicAcc.sample(kickback, ticAdvanced, 0, 0);
  s_accLastKick = kickback;

  if (outTicAdvanced) *outTicAdvanced = ticAdvanced;
  weaponspeed_publish_achieved(weapon, s_accStock, s_accSpanSum, s_accSamples,
                               achState, s_accLastSpan);
}

// Primary line: THISCLIP, ammo_amount[pistol], kickback_pic, reloading and burstfired on every kickback change while
// a sequence runs (own cap: a per-frame cap could never reach AA's refill window).
static void weaponspeed_trace_kick_clip(int kickback, int prevKick, unsigned prevTics) {
  s_kickClipSeqOccurred++;
  if (s_kickClipLogCount >= KICK_CLIP_TRACE_SEQ_CAP) return;
  int clip = 0, ammo = 0, reloading = 0, burst = 0;
  const bool clipOk = gamevars_thisclip(EK_WS_LOCAL_PLAYER, &clip);
  const bool ammoOk = player_read_ammo(EK_PISTOL_WEAPON, &ammo);
  const bool reloadOk = player_read_reloading(&reloading);
  const bool burstOk = gamevars_burstfired(EK_WS_LOCAL_PLAYER, &burst);
  char clipText[16], ammoText[16], reloadText[16], burstText[16];
  kick_value_text(clipOk, clip, clipText, sizeof(clipText));
  kick_value_text(ammoOk, ammo, ammoText, sizeof(ammoText));
  kick_value_text(reloadOk, reloading, reloadText, sizeof(reloadText));
  kick_value_text(burstOk, burst, burstText, sizeof(burstText));
  ek_log("weaponspeed: kick trace -- clip THISCLIP=%s ammo=%s kickback=%d "
         "reloading=%s burstfired=%s prevKick=%d prevTics=%u", clipText,
         ammoText, kickback, reloadText, burstText, prevKick, prevTics);
  s_kickClipLogCount++;
}

static void weaponspeed_kick_hist_text(char* out, int cap) {
  if (!out || cap <= 0) return;
  out[0] = 0;
  int used = 0;
  for (int i = 0; i < 64; i++) {
    if (!s_kickSeen[i]) continue;
    if (used >= cap - 1) break;
    const int n = _snprintf_s(out + used, (size_t)(cap - used), _TRUNCATE,
                              "%s%d:%u", used ? "," : "", i, s_kickCount[i]);
    if (n < 0) break;
    used += n;
  }
}

// Pistol only, once a frame, before this frame's write: the histogram sees every observed value including refused
// frames.
static void weaponspeed_trace_kick(int kickback, int weapon, const FireRateWeapon* table,
                                   bool ticAdvanced) {
  if (!g_cfg.debuglog) return;
  if (weapon != EK_PISTOL_WEAPON) return;

  // A new reload sequence starts when kickback leaves rest.
  if (s_kickLastKick == 0 && kickback != 0) {
    s_kickSeqLogCount = 0;
    s_kickSeqSaw26 = false;
    s_kickSeqSteps = 0;
    s_kickSeqMaxKick = 0;
    s_kickSeqOpen = true;
    s_kickClipLogCount = 0;
    s_kickClipSeqOccurred = 0;
    s_kickSeqFFPerksLogged = false;
    s_kickTicAcc = KickTicAccumulator();   // fresh per-sequence tic account
  }

  // One sample a frame feeding the pure accountant; `finished` is true when kickback just changed, and
  // doneKick/doneTics are that ended value's final tic count for the clip line.
  int doneKick = 0;
  unsigned doneTics = 0;
  const bool kickTicFinished =
      s_kickTicAcc.sample(kickback, ticAdvanced, &doneKick, &doneTics);

  // The filter number, updated every frame the sequence is open so it reflects what happened, not what the cap let
  // through.
  if (kickback > s_kickSeqMaxKick) s_kickSeqMaxKick = kickback;

  // fastfire and perks decide whether the refill window is 25..26 or exactly 26 (AAPLAYER.CON); once per sequence
  // suffices.
  if (kickback != 0 && !s_kickSeqFFPerksLogged) {
    s_kickSeqFFPerksLogged = true;
    int fastfire = 0, perks = 0;
    const bool ffOk = gamevars_fastfire(EK_WS_LOCAL_PLAYER, &fastfire);
    const bool perksOk = gamevars_perks(EK_WS_LOCAL_PLAYER, &perks);
    char ffText[16], perksText[16];
    kick_value_text(ffOk, fastfire, ffText, sizeof(ffText));
    kick_value_text(perksOk, perks, perksText, sizeof(perksText));
    ek_log("weaponspeed: kick trace -- sequence fastfire=%s perks=%s (widens "
           "the refill window to 25..26 when fastfire>0 or perks&2048)",
           ffText, perksText);
  }

  // Clip line keyed on kickback change, not per frame; s_kickLastKick is still the previous frame's value.
  if (kickback != 0 && kickback != s_kickLastKick)
    weaponspeed_trace_kick_clip(kickback, kickTicFinished ? doneKick : 0,
                                 kickTicFinished ? doneTics : 0u);

  // One line when a sequence ends makes each reload readable alone: the "new value seen" lines fire once per value
  // per process, so a later reload that behaved differently printed nothing.
  if (s_kickSeqOpen && s_kickLastKick != 0 && kickback == 0) {
    s_kickSeqOpen = false;
    // Max kickback is the filter: nearly every sequence ends at frame 7 or 8 (an ordinary shot), so a raw "26
    // visited" count is meaningless without it.
    ek_log("weaponspeed: kick trace -- sequence ENDED at rest: 26 visited %s, "
           "max kickback %d, %d advance(s) logged%s, %d of %d clip line(s) "
           "logged%s",
           s_kickSeqSaw26 ? "YES" : "NO", s_kickSeqMaxKick, s_kickSeqSteps,
           s_kickSeqSteps >= KICK_TRACE_SEQ_CAP
               ? " (CAP REACHED -- later advances in this sequence were NOT"
                 " reported, so this count is a floor)"
               : "",
           s_kickClipLogCount, s_kickClipSeqOccurred,
           s_kickClipSeqOccurred > s_kickClipLogCount
               ? " (CAP REACHED -- later kickback changes in this sequence"
                 " were NOT reported)"
               : "");

    // Was frame TotalTime-1 held for a whole tic on this sequence? TotalTime is read live (the row changes per
    // character). span and sumAll agree by KickTicAccumulator's construction; disagreeing here would mean this wiring
    // is wrong.
    int kickTotalTime = 0;
    const bool kickTtOk = gamevars_weapon_field(
        EK_PISTOL_WEAPON, EK_WD_TOTALTIME, EK_WS_LOCAL_PLAYER, &kickTotalTime);
    const int kickTrigger = kickTtOk ? kickTotalTime - 1 : -1;
    char kickTriggerText[80];
    if (kickTtOk && kickTrigger >= 0 && kickTrigger < KickTicAccumulator::MAXV) {
      _snprintf_s(kickTriggerText, sizeof(kickTriggerText), _TRUNCATE,
                  "frame %d seen=%s held=%u tic(s)", kickTrigger,
                  s_kickTicAcc.valueSeen(kickTrigger) ? "yes" : "no",
                  s_kickTicAcc.valueTics(kickTrigger));
    } else {
      _snprintf_s(kickTriggerText, sizeof(kickTriggerText), _TRUNCATE,
                  "TotalTime unread");
    }
    const unsigned kickSpan = s_kickTicAcc.span();
    const unsigned kickSum = s_kickTicAcc.sumAll();
    // "held 0 tic(s)" means two opposite things unless the clock says which: a genuine sub-tic dwell, or a tic clock
    // that never read.
    ek_log("weaponspeed: kick trace -- tic accounting: TotalTime-1 %s, "
           "span %u tic(s), sum %u tic(s)%s%s",
           kickTriggerText, kickSpan, kickSum,
           s_kickTicReadFailed
               ? " -- CLOCK UNAVAILABLE this session, so every tic count above"
                 " is meaningless and a 0 says nothing about the dwell"
               : "",
           kickSum != kickSpan
               ? " (MISMATCH -- instrument suspect, CLAUDE.md)"
               : "");
  }
  if (kickback == 26) s_kickSeqSaw26 = true;
  s_kickLastKick = kickback;

  if (kickback >= 0 && kickback < 64) {
    s_kickCount[kickback]++;
    if (!s_kickSeen[kickback]) {
      s_kickSeen[kickback] = true;
      // 1024, not 192: the formatter fills low indices first and _TRUNCATE drops the tail, so a reload visiting
      // 30-odd values would lose 26 and 33, the two this trace exists to find.
      char hist[1024];
      weaponspeed_kick_hist_text(hist, sizeof(hist));
      ek_log("weaponspeed: kick trace -- new value %d seen, histogram %s",
             kickback, hist);
    }
  } else {
    ek_log("weaponspeed: kick trace -- value %d is OUTSIDE 0..63", kickback);
  }

  // The stop set, once per weapon: whether 26 is one of this weapon's applied stops (live derivation or static
  // fallback).
  if (table && (!s_kick26Reported || s_kick26ForWeapon != weapon)) {
    s_kick26Reported = true;
    s_kick26ForWeapon = weapon;
    ek_log("weaponspeed: kick trace -- weapon %d applied stop set contains 26: %s",
           weapon, firerate_stop_contains(*table, 26) ? "yes" : "no");
  }
}

// Records this frame's fire-rate write so the report shows every from->to the advance produced for the pistol. Called
// only when the write landed.
static void weaponspeed_trace_kick_wrote(int from, int to, int weapon) {
  if (!g_cfg.debuglog) return;
  if (weapon != EK_PISTOL_WEAPON) return;
  s_kickSeqSteps++;
  if (s_kickSeqLogCount < KICK_TRACE_SEQ_CAP) {
    ek_log("weaponspeed: kick trace -- advance %d -> %d", from, to);
    s_kickSeqLogCount++;
  }
}

// AA's counter reloads: which counter AA's own gates would run for the held weapon (ekm_reload_counter_kind); every
// failed read answers NONE.
static int weaponspeed_counter_kind(int weapon, bool execRefused) {
  if (weapon != EK_RCTR_WEAPON || execRefused) return EK_RCTR_NONE;
  int pchar = -1;
  const bool pcharOk = gamevars_pchar(EK_WS_LOCAL_PLAYER, &pchar);
  if (!pcharOk) return EK_RCTR_NONE;
  return ekm_reload_counter_kind(weapon, execRefused, pchar);
}

static int weaponspeed_counter_var(int kind) {
  if (kind == EK_RCTR_M4) return EK_GV_RC_M4RELOAD;
  if (kind == EK_RCTR_UZI) return EK_GV_RC_UZIRELOAD;
  return -1;
}
static int weaponspeed_counter_clip_var(int kind) {
  if (kind == EK_RCTR_M4) return EK_GV_RC_M4CLIP;
  if (kind == EK_RCTR_UZI) return EK_GV_RC_UZICLIP;
  return -1;
}

// One line per counter reload, debuglog only: the counter, the value first seen, tics to the end, our writes and the
// clip before and after, so a flight shows whether AA's refill ran.
static bool s_ctrTraceOpen = false;
static int s_ctrTraceKind = EK_RCTR_NONE, s_ctrTraceStart = 0, s_ctrTraceWrites = 0;
static int s_ctrTraceTic0 = 0, s_ctrTraceClip0 = 0;
static bool s_ctrTraceTicOk = false, s_ctrTraceClipOk = false;

static void weaponspeed_trace_counter(int kind, bool ctrOk, int counter) {
  if (!g_cfg.debuglog) { s_ctrTraceOpen = false; return; }
  const bool running = kind != EK_RCTR_NONE && ctrOk && counter != 0;
  if (s_ctrTraceOpen && running && kind == s_ctrTraceKind) return;
  if (s_ctrTraceOpen) {
    int tic1 = 0, clip1 = 0;
    const bool ticOk = weaponspeed_read_tic(&tic1) && s_ctrTraceTicOk;
    const bool clipOk = gamevars_reload_counter(
        weaponspeed_counter_clip_var(s_ctrTraceKind), EK_WS_LOCAL_PLAYER, &clip1);
    char ticText[16], clip0Text[16], clip1Text[16];
    kick_value_text(ticOk, (tic1 - s_ctrTraceTic0) / EK_OTOTALCLOCK_PER_TIC,
                    ticText, sizeof(ticText));
    kick_value_text(s_ctrTraceClipOk, s_ctrTraceClip0, clip0Text, sizeof(clip0Text));
    kick_value_text(clipOk, clip1, clip1Text, sizeof(clip1Text));
    const bool zeroed = kind == s_ctrTraceKind && ctrOk && counter == 0;
    ek_log("weaponspeed: counter reload -- %s first seen at %d, %s, %s tic(s) "
           "after that, %d write(s), clip %s -> %s",
           s_ctrTraceKind == EK_RCTR_M4 ? "m4reload" : "uzireload",
           s_ctrTraceStart, zeroed ? "AA reset it to 0" : "no longer tracked",
           ticText, s_ctrTraceWrites, clip0Text, clip1Text);
    s_ctrTraceOpen = false;
  }
  if (!running) return;
  s_ctrTraceOpen = true;
  s_ctrTraceKind = kind;
  s_ctrTraceStart = counter;
  s_ctrTraceWrites = 0;
  s_ctrTraceTicOk = weaponspeed_read_tic(&s_ctrTraceTic0);
  s_ctrTraceClipOk = gamevars_reload_counter(weaponspeed_counter_clip_var(kind),
                                             EK_WS_LOCAL_PLAYER, &s_ctrTraceClip0);
}

static bool s_s3Open = false;
static int s_s3SeqKind = EK_RCTR_NONE;   // fixed at open: the counter this sequence tracks
static int s_s3Lines = 0, s_s3Suppressed = 0;
static int s_s3CtrWrites = 0, s_s3RegionWrites = 0;
static bool s_s3TicOk0 = false;
static int s_s3Tic0 = 0;
static bool s_s3HaveExtrema = false;
static int s_s3KickMin = 0, s_s3KickMax = 0, s_s3M4Min = 0, s_s3M4Max = 0,
           s_s3UziMin = 0, s_s3UziMax = 0;
static bool s_s3ClipOk0 = false;
static int s_s3Clip0 = 0;
static int s_s3LastKick = -1, s_s3LastM4 = -1, s_s3LastUzi = -1,
           s_s3LastKind = -2, s_s3LastWpos = -999;
static bool s_s3LastCounterOn = false;

static const char* weaponspeed_s3_kind_name(int kind) {
  if (kind == EK_RCTR_M4) return "m4";
  if (kind == EK_RCTR_UZI) return "uzi";
  return "none";
}

static void weaponspeed_trace_slot3(int weapon, int kickback, int weaponPos,
                                    bool fireOk, bool counterOn,
                                    int ctrFrom, int ctrTo,
                                    int regionFrom, int regionTo) {
  if (!g_cfg.debuglog) { s_s3Open = false; return; }
  if (!s_s3Open && weapon != EK_RCTR_WEAPON) return;

  // Own reads: pchar, the upgrade bits, gotshock and execRefused are recomputed here because weaponspeed_frame's
  // locals are filled only when kickRate > 100, the gate this trace must see past.
  int pchar = -1;
  const bool pcharOk = gamevars_pchar(EK_WS_LOCAL_PLAYER, &pchar);
  int shelly3 = 0, mando3 = 0, wes3 = 0;
  const bool shellyOk = gamearrays_upgrade(EK_GA_UP_SHELLY, EK_RCTR_WEAPON, &shelly3);
  const bool mandoOk  = gamearrays_upgrade(EK_GA_UP_MANDO,  EK_RCTR_WEAPON, &mando3);
  const bool wesOk    = gamearrays_upgrade(EK_GA_UP_WES,    EK_RCTR_WEAPON, &wes3);
  int gotshock = 0;
  const bool gotshockOk = gamevars_gotshock(EK_WS_LOCAL_PLAYER, &gotshock);
  const bool execRefused = ekm_executioner_refuses(
      weapon, gamevars_gotshock_present(), gotshockOk, gotshock);
  const int kind = weaponspeed_counter_kind(weapon, execRefused);
  int m4reload = 0, uzireload = 0, m4clip = 0, uziclip = 0;
  const bool m4Ok  = gamevars_reload_counter(EK_GV_RC_M4RELOAD,  EK_WS_LOCAL_PLAYER, &m4reload);
  const bool uziOk = gamevars_reload_counter(EK_GV_RC_UZIRELOAD, EK_WS_LOCAL_PLAYER, &uzireload);
  const bool m4ClipOk  = gamevars_reload_counter(EK_GV_RC_M4CLIP,  EK_WS_LOCAL_PLAYER, &m4clip);
  const bool uziClipOk = gamevars_reload_counter(EK_GV_RC_UZICLIP, EK_WS_LOCAL_PLAYER, &uziclip);
  int totalTime = 0;
  const bool ttOk = gamevars_weapon_field(EK_RCTR_WEAPON, EK_WD_TOTALTIME,
                                          EK_WS_LOCAL_PLAYER, &totalTime);
  int ammo = 0;
  const bool ammoOk = player_read_ammo(EK_RCTR_WEAPON, &ammo);
  const bool ctrOk = kind == EK_RCTR_M4 ? m4Ok : (kind == EK_RCTR_UZI ? uziOk : true);
  const int region = firerate_kick_region(kickback, ttOk, totalTime);
  const bool ladderActive = ttOk && region == EK_KICK_RELOAD;

  const bool active = weapon == EK_RCTR_WEAPON &&
                      (ladderActive || m4reload != 0 || uzireload != 0);

  if (!s_s3Open && active) {
    s_s3Open = true;
    s_s3SeqKind = kind;
    s_s3Lines = 0;
    s_s3Suppressed = 0;
    s_s3CtrWrites = 0;
    s_s3RegionWrites = 0;
    s_s3TicOk0 = weaponspeed_read_tic(&s_s3Tic0);
    s_s3HaveExtrema = true;
    s_s3KickMin = s_s3KickMax = kickback;
    s_s3M4Min = s_s3M4Max = m4reload;
    s_s3UziMin = s_s3UziMax = uzireload;
    const int clipVar = kind == EK_RCTR_M4 ? EK_GV_RC_M4CLIP
                        : kind == EK_RCTR_UZI ? EK_GV_RC_UZICLIP : -1;
    s_s3ClipOk0 = clipVar >= 0 &&
        gamevars_reload_counter(clipVar, EK_WS_LOCAL_PLAYER, &s_s3Clip0);
    s_s3LastKick = -1; s_s3LastM4 = -1; s_s3LastUzi = -1;
    s_s3LastKind = -2; s_s3LastWpos = -999; s_s3LastCounterOn = false;
    char pcharText[16], shellyText[16], mandoText[16], wesText[16], gsText[16];
    kick_value_text(pcharOk, pchar, pcharText, sizeof(pcharText));
    kick_value_text(shellyOk, shelly3, shellyText, sizeof(shellyText));
    kick_value_text(mandoOk, mando3, mandoText, sizeof(mandoText));
    kick_value_text(wesOk, wes3, wesText, sizeof(wesText));
    kick_value_text(gotshockOk, gotshock, gsText, sizeof(gsText));
    ek_log("weaponspeed: slot3 trace -- OPEN pchar=%s shelly3=%s mando3=%s "
           "wes3=%s gotshock=%s execRefused=%s kind=%s",
           pcharText, shellyText, mandoText, wesText, gsText,
           execRefused ? "Y" : "N", weaponspeed_s3_kind_name(kind));
  }

  if (s_s3Open) {
    if (regionFrom >= 0) s_s3RegionWrites++;
    if (ctrFrom >= 0) s_s3CtrWrites++;
    if (s_s3HaveExtrema) {
      if (kickback < s_s3KickMin) s_s3KickMin = kickback;
      if (kickback > s_s3KickMax) s_s3KickMax = kickback;
      if (m4reload < s_s3M4Min) s_s3M4Min = m4reload;
      if (m4reload > s_s3M4Max) s_s3M4Max = m4reload;
      if (uzireload < s_s3UziMin) s_s3UziMin = uzireload;
      if (uzireload > s_s3UziMax) s_s3UziMax = uzireload;
    }

    const bool changed = kickback != s_s3LastKick || m4reload != s_s3LastM4 ||
                         uzireload != s_s3LastUzi || kind != s_s3LastKind ||
                         counterOn != s_s3LastCounterOn || weaponPos != s_s3LastWpos;
    if (changed) {
      if (s_s3Lines < 64) {
        char m4Text[16], uziText[16], m4ClipText[16], uziClipText[16],
             ttText[16], ammoText[16], ctrFromText[16], ctrToText[16],
             regFromText[16], regToText[16];
        kick_value_text(m4Ok, m4reload, m4Text, sizeof(m4Text));
        kick_value_text(uziOk, uzireload, uziText, sizeof(uziText));
        kick_value_text(m4ClipOk, m4clip, m4ClipText, sizeof(m4ClipText));
        kick_value_text(uziClipOk, uziclip, uziClipText, sizeof(uziClipText));
        kick_value_text(ttOk, totalTime, ttText, sizeof(ttText));
        kick_value_text(ammoOk, ammo, ammoText, sizeof(ammoText));
        kick_value_text(ctrFrom >= 0, ctrFrom, ctrFromText, sizeof(ctrFromText));
        kick_value_text(ctrTo >= 0, ctrTo, ctrToText, sizeof(ctrToText));
        kick_value_text(regionFrom >= 0, regionFrom, regFromText, sizeof(regFromText));
        kick_value_text(regionTo >= 0, regionTo, regToText, sizeof(regToText));
        ek_log("weaponspeed: slot3 trace -- kickback=%d pos=%d m4reload=%s "
               "uzireload=%s m4clip=%s uziclip=%s TotalTime=%s ammo=%s "
               "kind=%s ctrOk=%s counterOn=%s fireOk=%s region=%s "
               "write ctr %s->%s region %s->%s",
               kickback, weaponPos, m4Text, uziText, m4ClipText, uziClipText,
               ttText, ammoText, weaponspeed_s3_kind_name(kind),
               ctrOk ? "Y" : "N", counterOn ? "Y" : "N", fireOk ? "Y" : "N",
               region == EK_KICK_RELOAD ? "RELOAD"
                   : region == EK_KICK_FIRE ? "FIRE" : "UNREAD",
               ctrFromText, ctrToText, regFromText, regToText);
        s_s3Lines++;
      } else {
        s_s3Suppressed++;
      }
      s_s3LastKick = kickback; s_s3LastM4 = m4reload; s_s3LastUzi = uzireload;
      s_s3LastKind = kind; s_s3LastWpos = weaponPos; s_s3LastCounterOn = counterOn;
    }
  }

  const bool closes = s_s3Open && (weapon != EK_RCTR_WEAPON || !active);
  if (closes) {
    s_s3Open = false;
    int tic1 = 0;
    const bool ticOk = weaponspeed_read_tic(&tic1) && s_s3TicOk0;
    char ticText[16], clip0Text[16], clip1Text[16];
    kick_value_text(ticOk, (tic1 - s_s3Tic0) / EK_OTOTALCLOCK_PER_TIC,
                    ticText, sizeof(ticText));
    const int clipVar = s_s3SeqKind == EK_RCTR_M4 ? EK_GV_RC_M4CLIP
                        : s_s3SeqKind == EK_RCTR_UZI ? EK_GV_RC_UZICLIP : -1;
    int clip1 = 0;
    const bool clip1Ok = clipVar >= 0 &&
        gamevars_reload_counter(clipVar, EK_WS_LOCAL_PLAYER, &clip1);
    kick_value_text(s_s3ClipOk0, s_s3Clip0, clip0Text, sizeof(clip0Text));
    kick_value_text(clip1Ok, clip1, clip1Text, sizeof(clip1Text));
    ek_log("weaponspeed: slot3 trace -- CLOSE kind=%s %s tic(s) open, "
           "%d counter write(s), %d region write(s), kickback %d..%d, "
           "m4reload %d..%d, uzireload %d..%d, clip %s -> %s, %d line(s) "
           "suppressed",
           weaponspeed_s3_kind_name(s_s3SeqKind), ticText, s_s3CtrWrites,
           s_s3RegionWrites, s_s3KickMin, s_s3KickMax, s_s3M4Min, s_s3M4Max,
           s_s3UziMin, s_s3UziMax, clip0Text, clip1Text, s_s3Suppressed);
  }
}

void weaponspeed_frame(void) {
  const unsigned now = wallclock_ms();
  const int elapsedMs = s_haveLast ? (int)(now - s_lastMs) : 0;
  s_haveLast = true;
  s_lastMs = now;

  const int fireRate   = firerate_rate_clamped(g_cfg.firerate);
  const int switchRate = firerate_rate_clamped(g_cfg.switchspeed);
  // kickback_pic has two rows (fire, reload); the write is open when either is raised and each frame's region picks
  // one.
  const int reloadRate = reloadrate_clamped(g_cfg.reloadrate);   // 100-1000 or Max
  const int kickRate   = firerate_kick_gate_rate(fireRate, reloadRate);

  // The cheap refusal first, which is what "features default off" means: with every row at 100 nothing is read and no
  // byte is written (three int reads).
  if (kickRate <= 100 && switchRate <= 100) { weaponspeed_reset(); return; }

  // A failed read is a refusal: a feature that cannot tell whether the game's menu is open must assume it is.
  int gm = 0;
  const bool gmOk = player_read_gm(&gm);
  const bool menuOpen = !gmOk || (gm & EK_MODE_MENU) != 0;

  // The pause is its own term, gm cannot stand in: Pause leaves gm at MODE_GAME while this hook runs every frame, so
  // the wall clock would keep owing tics and spend them on unpause. An unreadable &ud reads as paused.
  bool pauseOn = true;
  const bool paused = !ud_paused(&pauseOn) || pauseOn;

  const bool panelOpen = input_panel_open();

  int weapon = 0, kickback = 0, weaponPos = 0;
  const bool readOk = player_read_weapon_anim(&weapon, &kickback, &weaponPos);
  int s3CtrFrom = -1, s3CtrTo = -1, s3RegionFrom = -1, s3RegionTo = -1;
  if (!readOk) {
    weaponspeed_trace_slot3(weapon, kickback, weaponPos, false, false,
                            s3CtrFrom, s3CtrTo, s3RegionFrom, s3RegionTo);
    weaponspeed_reset();
    return;
  }

  FireRateWeapon live = { -1, 0, { 0 } };
  EkReloadStops liveReload;
  const int stopSrc = firestops_source(weapon, &live, &liveReload);
  // Own statement because the readout needs the same answer: a NULL static row is weapons 5, 8 and 10 (refused
  // permanently), whereas a refused source with a row may simply be unpublished yet.
  const FireRateWeapon* const stat = firerate_weapon(weapon);
  const FireRateWeapon* const table = ekm_firestops_choose(stopSrc, stat, &live);

  // The Executioner shares slot 3 with the chaingun and cannot be accelerated.
  bool execHaveVar = false, execReadOk = false;
  int gotshock = 0;
  if (weapon == EK_EXECUTIONER_WEAPON && kickRate > 100) {
    execHaveVar = gamevars_gotshock_present();
    execReadOk = gamevars_gotshock(EK_WS_LOCAL_PLAYER, &gotshock);
  }
  const bool execRefused =
      ekm_executioner_refuses(weapon, execHaveVar, execReadOk, gotshock);

  // Once, saying which of the two cases it is: the unreadable case must be distinguishable from the ordinary one, and
  // no speed-up beats a skipped shot.
  if (execRefused && g_cfg.debuglog) {
    static bool said = false;
    if (!said) {
      said = true;
      ek_log("weaponspeed: slot 3's fire rate is REFUSED -- %s. The chaingun "
             "that shares this slot is unaffected; the Executioner's own CON "
             "frames sit inside the window the engine's data leaves free, so "
             "advancing it would skip a frame AA's script waits on while the "
             "ammo still drains",
             execReadOk ? "AA's gotshock says the Executioner is on top"
                        : "this CON declares gotshock and it could not be "
                          "read, so the two cannot be told apart");
    }
  }

  // One gate for both kick rows, so the reload row is refused by exactly the code that refuses fire rate
  // (Executioner, 5/8/10, an unpublished set).
  const bool fireOk = firerate_gate(readOk, menuOpen, paused, panelOpen,
                                    kickRate, kickback, table != 0)
                      && !execRefused;

  // The movement term, via weaponspeed.h's pure state machine (s_lastWeapon is still the previous frame's value,
  // which lets wswitch_track_step see a weapon change).
  const bool wswitchMoved = wswitch_track_step(&s_wswitchTrack, weapon, s_lastWeapon, weaponPos);

  // The base gate without the movement term, so the early return can tell "nothing to do" from "something to do, but
  // this frame's movement read says wait".
  const bool switchGateBase = wswitch_gate(readOk, menuOpen, paused, panelOpen,
                                           switchRate, weaponPos);
  const bool switchOk = switchGateBase && wswitchMoved;

  weaponspeed_trace_wpos(weaponPos, switchOk, switchRate, readOk, menuOpen,
                        paused, panelOpen, weapon);

  // The achieved readout's accounting, conditioned on the fire-rate row itself, not the log.
  bool kickTicAdvanced = false;
  // Which of four things the row has to say: `table == 0` used to cover a weapon whose set is merely unpublished
  // (about a second at startup), which is not a property of the weapon.
  const bool clockLocated = weaponspeed_tic_clock_located();
  const int achState = ekm_ach_state(stat != 0, stopSrc == EK_STOPS_REFUSE,
                                     execRefused, clockLocated);
  if (fireRate > 100 || g_cfg.debuglog)
    weaponspeed_account_kick(kickback, weapon, fireRate, achState,
                             &kickTicAdvanced);

  weaponspeed_trace_kick(kickback, weapon, table, kickTicAdvanced);

  // AA's counter reloads: the M4's and Uzi's hold kickback_pic at 0, so fireOk is false throughout and only the
  // counter moves. Read only with the chaingun up and Reload speed raised; written only at rest (weapon_pos 0, AA's
  // condition).
  int ctrKind = EK_RCTR_NONE, counter = 0;
  bool ctrOk = false;
  if (reloadRate > 100 && weapon == EK_RCTR_WEAPON) {
    ctrKind = weaponspeed_counter_kind(weapon, execRefused);
    const int var = weaponspeed_counter_var(ctrKind);
    if (var >= 0) ctrOk = gamevars_reload_counter(var, EK_WS_LOCAL_PLAYER, &counter);
  }
  const int ctrRefill = ekm_reload_counter_refill(ctrKind);
  const bool counterOn = ctrOk && counter > 0 && counter <= ctrRefill &&
                         weaponPos == 0 && !menuOpen && !paused && !panelOpen;
  weaponspeed_trace_counter(ctrKind, ctrOk, counter);

  // Use switchGateBase here, not switchOk: a parked or slammed counter that is otherwise eligible is something to do,
  // gated only by this frame's movement read.
  const bool nothingToDo = !fireOk && !switchGateBase && !counterOn;
  if (nothingToDo) {
    weaponspeed_trace_slot3(weapon, kickback, weaponPos, fireOk, counterOn,
                            s3CtrFrom, s3CtrTo, s3RegionFrom, s3RegionTo);
    weaponspeed_reset();
    // The refused reason, at most once a second with the log on: "the rate is 100" and "the panel is open" are
    // answers a player asking why nothing happened must tell apart.
    if (g_cfg.debuglog) {
      static unsigned lastMs = 0;
      static bool said = false;
      if (!said || now - lastMs >= 1000) {
        said = true; lastMs = now;
        // The stop source is part of the reason: "notable" covers no row (5, 8, 10), a derivation that declined, and
        // a set caught mid-publish.
        ek_log("weaponspeed: idle -- fire %s, stops %s, weapon %d, "
               "kickback %d, pos %d",
               execRefused ? "executioner"
                           : firerate_gate_reason(readOk, menuOpen, paused,
                                                  panelOpen, kickRate,
                                                  kickback, table != 0),
               weaponspeed_source_name(stopSrc), weapon, kickback, weaponPos);
      }
    }
    return;
  }

  s_kickCarry = firerate_carry_for_weapon(s_kickCarry, s_lastWeapon, weapon);
  s_reloadCarry = firerate_carry_for_weapon(s_reloadCarry, s_lastWeapon, weapon);
  s_posCarry  = firerate_carry_for_weapon(s_posCarry,  s_lastWeapon, weapon);
  s_ctrCarry  = firerate_carry_for_weapon(s_ctrCarry,  s_lastWeapon, weapon);
  s_lastWeapon = weapon;

  const int tics = firerate_tics_due(&s_ticAcc, elapsedMs);
  if (tics <= 0) {
    weaponspeed_trace_slot3(weapon, kickback, weaponPos, fireOk, counterOn,
                            s3CtrFrom, s3CtrTo, s3RegionFrom, s3RegionTo);
    return;
  }

  // The whole batch in one write per counter: advancing tic by tic and by the sum saturate at the same clamp, and one
  // write is one chance to fault.
  if (fireOk) {
    // Which row this frame runs at: the live TotalTime, read each frame because AA rewrites it per character and
    // upgrade.
    int totalTime = 0;
    const bool ttOk = gamevars_weapon_field(weapon, EK_WD_TOTALTIME,
                                            EK_WS_LOCAL_PLAYER, &totalTime);
    const int region = firerate_kick_region(kickback, ttOk, totalTime);
    if (region == EK_KICK_UNREAD && g_cfg.debuglog) {
      static bool said = false;
      if (!said) {
        said = true;
        ek_log("weaponspeed: weapon %d's TotalTime did not read at kickback %d "
               "-- firing and reloading cannot be told apart, so the slower of "
               "Fire rate and Reload speed applies", weapon, kickback);
      }
    }
    int steps = 0;
    for (int i = 0; i < tics; i++)
      steps += firerate_region_steps(region, fireRate, reloadRate, &s_kickCarry,
                                     &s_reloadCarry);
    // Which set: in the reload region, the essential-only set for the character it was derived for, read live beside
    // TotalTime; otherwise the full set.
    int pchar = -1;
    const bool pcharOk = region == EK_KICK_RELOAD && stopSrc == EK_STOPS_LIVE &&
                         liveReload.code == EK_RSET_OK &&
                         gamevars_pchar(EK_WS_LOCAL_PLAYER, &pchar);
    const FireRateWeapon* const adv = ekm_firestops_for_region(
        region, table, stopSrc == EK_STOPS_LIVE ? &liveReload : 0, totalTime, pcharOk, pchar);
    const int next = firerate_advance(*adv, kickback, steps);
    if (next != kickback && player_write_kickback(next)) {
      if (region == EK_KICK_RELOAD) {
        static bool said = false;
        if (!said) { said = true; weaponspeed_log_once("reload speed (9999% is Max)", reloadRate, kickback, next); }
      } else {
        static bool said = false;
        if (!said) { said = true; weaponspeed_log_once("fire rate", fireRate, kickback, next); }
      }
      weaponspeed_trace_kick_wrote(kickback, next, weapon);
      if (weapon == EK_RCTR_WEAPON && region == EK_KICK_RELOAD) {
        s3RegionFrom = kickback; s3RegionTo = next;
      }
    }
  } else {
    s_kickCarry = 0.0;
    s_reloadCarry = 0.0;
  }

  if (switchOk) {
    int steps = 0;
    for (int i = 0; i < tics; i++) steps += firerate_steps(switchRate, &s_posCarry);
    const int next = wswitch_advance(weaponPos, steps);
    if (next != weaponPos && player_write_weapon_pos(next)) {
      static bool said = false;
      if (!said) { said = true; weaponspeed_log_once("switch speed", switchRate, weaponPos, next); }
      weaponspeed_trace_wpos_wrote(next);
      wswitch_track_wrote(&s_wswitchTrack, next);
    }
  } else {
    s_posCarry = 0.0;
  }

  // The counter write cannot race AA: this hook runs on the thread that runs the tics, between them.
  if (counterOn) {
    const int next = reloadcounter_advance(counter, ctrRefill, reloadRate, tics,
                                           &s_ctrCarry);
    if (next != counter &&
        gamevars_reload_counter_set(weaponspeed_counter_var(ctrKind),
                                    EK_WS_LOCAL_PLAYER, next)) {
      static bool said = false;
      if (!said) { said = true; weaponspeed_log_once("reload counter (9999% is Max)", reloadRate, counter, next); }
      if (s_ctrTraceOpen) s_ctrTraceWrites++;
      s3CtrFrom = counter; s3CtrTo = next;
    }
  } else {
    s_ctrCarry = 0.0;
  }

  weaponspeed_trace_slot3(weapon, kickback, weaponPos, fireOk, counterOn,
                          s3CtrFrom, s3CtrTo, s3RegionFrom, s3RegionTo);
}

// The prediction: one thread (overlay_on_swap calls weaponspeed_frame then menu_draw, the only caller of
// weaponspeed_predicted), so no interlocking. The cache exists because a replay is up to 64 cycles.
static bool s_predHave = false;
static FireRateWeapon s_predSet;
static int s_predRate = 0, s_predEnd = 0, s_predX100 = 100;
static bool s_predRestarts = false;

// The set's content is its identity: a republish, a new fastfire variant or a weapon change that leaves the stops
// alone cannot change the answer.
static bool weaponspeed_sets_equal(const FireRateWeapon& a, const FireRateWeapon& b) {
  if (a.weapon != b.weapon || a.nEvents != b.nEvents) return false;
  for (int i = 0; i < a.nEvents && i < FIRERATE_EVENTS_MAX; i++)
    if (a.events[i] != b.events[i] || a.con[i] != b.con[i]) return false;
  return true;
}

static bool weaponspeed_pred_exec_refused(int weapon) {
  bool execHaveVar = false, execReadOk = false;
  int gotshock = 0;
  if (weapon == EK_EXECUTIONER_WEAPON) {
    execHaveVar = gamevars_gotshock_present();
    execReadOk = gamevars_gotshock(EK_WS_LOCAL_PLAYER, &gotshock);
  }
  return ekm_executioner_refuses(weapon, execHaveVar, execReadOk, gotshock);
}

// The same choice weaponspeed_frame makes, so a prediction describes the advance that will actually run. Null when
// the weapon is refused or has no stops.
static const FireRateWeapon* weaponspeed_pred_table(int weapon, FireRateWeapon* live,
                                                    EkReloadStops* liveReload = 0) {
  const int stopSrc = firestops_source(weapon, live, liveReload);
  const FireRateWeapon* const table =
      ekm_firestops_choose(stopSrc, firerate_weapon(weapon), live);
  if (!table || weaponspeed_pred_exec_refused(weapon) || table->nEvents <= 0) return 0;
  return table;
}

bool weaponspeed_predicted(int rate, int* outWeapon, int* outX100) {
  if (!outWeapon || !outX100) return false;
  const int fireRate = firerate_rate_clamped(rate);
  if (fireRate <= 100) return false;
  int weapon = 0, kickback = 0, weaponPos = 0;
  const bool readOk = player_read_weapon_anim(&weapon, &kickback, &weaponPos);
  if (!readOk) return false;

  FireRateWeapon live = { -1, 0, { 0 } };
  const FireRateWeapon* const table = weaponspeed_pred_table(weapon, &live);
  if (!table) {
    *outWeapon = weapon;
    *outX100 = EK_RATE_ACHIEVED_NONE;
    return true;
  }

  int totalTime = 0, holdDelay = 0, flags = 0;
  const bool ttOk = gamevars_weapon_field(weapon, EK_WD_TOTALTIME,
                                          EK_WS_LOCAL_PLAYER, &totalTime);
  const bool hdOk = gamevars_weapon_field(weapon, EK_WD_HOLDDELAY,
                                          EK_WS_LOCAL_PLAYER, &holdDelay);
  const bool flOk = gamevars_weapon_field(weapon, EK_WD_FLAGS,
                                          EK_WS_LOCAL_PLAYER, &flags);
  if (!ttOk || !hdOk || !flOk) return false;
  const int cycleEnd = ekm_stock_cycle_span(totalTime, holdDelay, flags);
  if (cycleEnd <= 0) return false;
  const bool restarts = ekm_cycle_carry_restarts(flags);

  if (!s_predHave || s_predRate != fireRate || s_predEnd != cycleEnd ||
      s_predRestarts != restarts || !weaponspeed_sets_equal(s_predSet, *table)) {
    s_predSet = *table;
    s_predRate = fireRate;
    s_predEnd = cycleEnd;
    s_predRestarts = restarts;
    s_predX100 = firerate_predict_x100(*table, fireRate, cycleEnd, restarts);
    s_predHave = true;
  }
  *outWeapon = weapon;
  *outX100 = s_predX100;
  return true;
}

static bool s_rpredHave = false;
static FireRateWeapon s_rpredSet;
static int s_rpredRate = 0, s_rpredTt = 0, s_rpredRl = 0, s_rpredX100 = 100;

bool weaponspeed_reload_predicted(int rate, int* outWeapon, int* outX100) {
  if (!outWeapon || !outX100) return false;
  const int reloadRate = reloadrate_clamped(rate);
  if (reloadRate <= 100) return false;
  int weapon = 0, kickback = 0, weaponPos = 0;
  const bool readOk = player_read_weapon_anim(&weapon, &kickback, &weaponPos);
  if (!readOk) return false;

  // An M4/Uzi reload is AA's counter, not the ladder: the row shows the counter's speed-up whenever AA's gates would
  // run it.
  const int ctrKind = weaponspeed_counter_kind(weapon, weaponspeed_pred_exec_refused(weapon));
  if (ctrKind != EK_RCTR_NONE) {
    *outWeapon = weapon;
    *outX100 = reloadcounter_predict_x100(ekm_reload_counter_refill(ctrKind), reloadRate);
    return true;
  }

  FireRateWeapon live = { -1, 0, { 0 } };
  EkReloadStops liveReload;
  const FireRateWeapon* const full = weaponspeed_pred_table(weapon, &live, &liveReload);
  if (!full) {
    *outWeapon = weapon;
    *outX100 = EK_RATE_ACHIEVED_NONE;
    return true;
  }

  int totalTime = 0, reload = 0;
  const bool ttOk = gamevars_weapon_field(weapon, EK_WD_TOTALTIME,
                                          EK_WS_LOCAL_PLAYER, &totalTime);
  const bool rlOk = gamevars_weapon_field(weapon, EK_WD_RELOAD,
                                          EK_WS_LOCAL_PLAYER, &reload);
  if (!ttOk || !rlOk || totalTime < 1) return false;

  int pchar = -1;
  const bool pcharOk = liveReload.code == EK_RSET_OK &&
                       gamevars_pchar(EK_WS_LOCAL_PLAYER, &pchar);
  const FireRateWeapon* const table = ekm_firestops_for_region(
      EK_KICK_RELOAD, full, &liveReload, totalTime, pcharOk, pchar);

  if (!s_rpredHave || s_rpredRate != reloadRate || s_rpredTt != totalTime ||
      s_rpredRl != reload || !weaponspeed_sets_equal(s_rpredSet, *table)) {
    s_rpredSet = *table;
    s_rpredRate = reloadRate;
    s_rpredTt = totalTime;
    s_rpredRl = reload;
    s_rpredX100 = firerate_predict_reload_x100(*table, reloadRate, totalTime, reload);
    s_rpredHave = true;
  }
  *outWeapon = weapon;
  *outX100 = s_rpredX100;
  return true;
}
