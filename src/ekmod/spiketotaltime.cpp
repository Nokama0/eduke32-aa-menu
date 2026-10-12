// Throwaway spike (spiketotaltime.h), game thread only. The read-back comes FIRST in the frame: AA2's counter read back
// after its own write, so it only saw its own echo while AA overwrote it.

#include <windows.h>
#include "spiketotaltime.h"
#include "gamevars.h"
#include "weapondata.h"
#include "playerstate.h"
#include "config.h"
#include "log.h"

static const int EK_STT_LOCAL_PLAYER = 0;

struct EkSttSlot {
  bool held;
  int wrote;
  unsigned reasserts;
  bool everWritten;
  int baseline;
  bool said;
  int saidRead, saidTotal;
};
static EkSttSlot s_slot[EK_WD_MAX_WEAPONS];
static bool s_writeFailed[EK_WD_MAX_WEAPONS];

void spiketotaltime_frame(void) {
  if (!g_cfg.spiketotaltime) {
    spiketotaltime_restore();
    return;
  }

  int weapon = 0;
  if (!player_read_weapon(&weapon)) return;
  if (weapon < 0 || weapon >= EK_WD_MAX_WEAPONS) return;
  if (s_writeFailed[weapon]) return;

  int totalTime = 0;
  if (!gamevars_weapon_field(weapon, EK_WD_TOTALTIME, EK_STT_LOCAL_PLAYER, &totalTime))
    return;

  EkSttSlot& s = s_slot[weapon];
  const bool readbackMatched = !s.held || (totalTime == s.wrote);
  if (s.held && !readbackMatched) s.reasserts++;

  // The baseline is captured once and held: recomputing it every frame ratcheted `target` down (stt_next_baseline).
  s.baseline = stt_next_baseline(s.held, s.baseline, s.wrote, totalTime);
  s.held = false;

  if (s.baseline < 2) return;

  const int target = s.baseline - 1;
  // Ungated by debuglog on purpose (the module is a diagnostic), but only on change: ek_log opens and closes the file
  // per call (log.cpp), and 60+ file operations a second would distort the timing.
  if (!s.said || s.saidRead != totalTime || s.saidTotal != target) {
    s.said = true;
    s.saidRead = totalTime;
    s.saidTotal = target;
    ek_log("spiketotaltime: w%02d TotalTime=%d readback=%s baseline=%d target=%d "
           "reasserts=%u",
           weapon, totalTime,
           s.everWritten ? (readbackMatched ? "HELD" : "REVERTED") : "n/a",
           s.baseline, target, s.reasserts);
  }

  if (target == totalTime) {
    s.held = true;
    s.wrote = target;
    return;
  }

  if (!gamevars_weapon_field_set(weapon, EK_WD_TOTALTIME, EK_STT_LOCAL_PLAYER, target)) {
    s_writeFailed[weapon] = true;
    ek_log("spiketotaltime: w%02d WEAPON%d_TOTALTIME would not take a write; "
           "left exactly as the game set it and this is not logged again",
           weapon, weapon);
    return;
  }
  s.held = true;
  s.wrote = target;
  s.everWritten = true;
}

void spiketotaltime_restore(void) {
  // Keyed on `everWritten`, not the transient `held`, which skipped restoring the slot that had ratcheted to the floor.
  for (int w = 0; w < EK_WD_MAX_WEAPONS; w++) {
    EkSttSlot& s = s_slot[w];
    if (!s.everWritten) continue;
    s.held = false;
    s.everWritten = false;
    // Only if still ours (reloadspeed_restore's guard).
    int live = 0;
    if (!gamevars_weapon_field(w, EK_WD_TOTALTIME, EK_STT_LOCAL_PLAYER, &live)) continue;
    if (live != s.wrote) continue;
    gamevars_weapon_field_set(w, EK_WD_TOTALTIME, EK_STT_LOCAL_PLAYER, s.baseline);
  }
}
