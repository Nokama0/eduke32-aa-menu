#pragma once

// Spike: does a written WEAPON<n>_TOTALTIME survive AA's own writes? The cycle runs until weaponFrame >= TotalTime,
// so TotalTime-1 moves it one tic. Gated on g_cfg.spiketotaltime only.

// Capture AA's value once; re-capture only when the read-back no longer matches what we last wrote (AA chose a new
// number). `lastWrote` is unread when `haveBaseline` is false.
inline int stt_next_baseline(bool haveBaseline, int baseline, int lastWrote, int liveRead) {
  if (haveBaseline && liveRead == lastWrote) return baseline;
  return liveRead;
}

void spiketotaltime_frame(void);

// Off means put it back, not stop writing.
void spiketotaltime_restore(void);
