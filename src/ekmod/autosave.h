#pragma once
#include <string.h>
#include <time.h>
#include "playerstate.h"

const int AUTOSAVE_MINS_MIN = 0, AUTOSAVE_MINS_MAX = 30;
const int AUTOSAVE_MINS_DEFAULT = AUTOSAVE_MINS_MIN;
const unsigned AUTOSAVE_MS_PER_MIN = 60000u;

inline int autosave_mins_clamped(int v) {
  if (v < AUTOSAVE_MINS_MIN) return AUTOSAVE_MINS_MIN;
  if (v > AUTOSAVE_MINS_MAX) return AUTOSAVE_MINS_MAX;
  return v;
}

// int flags, never bool: MSVC 19.50 /O2 loses a bool member of a struct returned by inline.
struct AutosaveState { unsigned lastMs; int armed; };

// A level build restarts the interval.
inline int autosave_step(AutosaveState* s, int intervalMins, unsigned nowMs,
                         int levelLoading, int inPlay) {
  if (!s) return 0;
  if (!s->armed || levelLoading) {
    s->armed = 1;
    s->lastMs = nowMs;
    return 0;
  }
  const int mins = autosave_mins_clamped(intervalMins);
  if (mins <= 0) { s->lastMs = nowMs; return 0; }
  if (!inPlay) return 0;
  if ((unsigned)(nowMs - s->lastMs) < (unsigned)mins * AUTOSAVE_MS_PER_MIN) return 0;
  s->lastMs = nowMs;
  return 1;
}

// The engine saves only in game mode and refuses a dead player (game.cpp:5040-5055).
inline int autosave_in_play(int gmOk, int gm, int healthOk, int health, int panelOpen) {
  if (!gmOk || !healthOk) return 0;
  if (!esp_level_loaded(gm, true)) return 0;
  if (health <= 0) return 0;
  if (panelOpen) return 0;
  return 1;
}

// g_lastautosave is a savebrief_t { char name[32]; char path[256]; uint8_t isExt; } (savegame.h:37).
const int EK_SAVEBRIEF_NAME_CAP = 32;
const int EK_SAVEBRIEF_PATH_OFF = EK_SAVEBRIEF_NAME_CAP;

// Only CON_SAVE fills the name; an empty one makes the save unreadable and G_DeleteOldSaves removes it.
inline int autosave_name_ok(const char* name) {
  if (!name || name[0] == 0) return 0;
  for (int i = 0; i < EK_SAVEBRIEF_NAME_CAP; i++)
    if (name[i] == 0) return 1;
  return 0;
}

// Matches gameexec.cpp:5344 byte for byte.
inline int autosave_name_from_tm(char* out, int cap, const struct tm* t) {
  if (!out || cap < EK_SAVEBRIEF_NAME_CAP) return 0;
  size_t n = 0;
  if (t) n = strftime(out, (size_t)EK_SAVEBRIEF_NAME_CAP, "%d %b %Y %I:%M%p", t);
  if (n == 0) memcpy(out, "Autosave", 9);
  out[EK_SAVEBRIEF_NAME_CAP - 1] = 0;
  return autosave_name_ok(out);
}

void autosave_frame(void);

bool autosave_ready(void);
