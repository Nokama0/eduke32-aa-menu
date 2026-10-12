#pragma once

// A fov cvar write lands next frame: EDuke32 recomputes it from ud.fov (game.cpp:890).

const int EK_FOV_MIN = 60, EK_FOV_MAX = 140, EK_FOV_DEFAULT = 90;

inline int ekm_fov_clamped(int v) {
  if (v < EK_FOV_MIN) return EK_FOV_MIN;
  if (v > EK_FOV_MAX) return EK_FOV_MAX;
  return v;
}

enum { EK_FOV_ACT_NONE = 0, EK_FOV_ACT_SET = 1, EK_FOV_ACT_RESTORE = 2 };

// int, not bool: MSVC 19.5x /O2 loses a bool member returned by value.
struct EkFovHold { int held; int saved; int forced; };

// Same edge machine as shot_hold_step: read the player's value on ON, restore on OFF.
inline int ekm_fov_step(EkFovHold* h, int edge, int live, int want, int* out) {
  if (!h || !out) return EK_FOV_ACT_NONE;
  *out = 0;
  if (edge > 0 && !h->held) {
    h->held = 1;
    h->saved = live;
    h->forced = ekm_fov_clamped(want);
    *out = h->forced;
    return EK_FOV_ACT_SET;
  }
  if (edge < 0 && h->held) {
    h->held = 0;
    *out = h->saved;
    h->saved = 0;
    h->forced = 0;
    return EK_FOV_ACT_RESTORE;
  }
  return EK_FOV_ACT_NONE;
}

inline int ekm_fov_retarget(EkFovHold* h, int want, int* out) {
  if (!h || !out) return 0;
  if (!h->held) return 0;
  const int v = ekm_fov_clamped(want);
  if (v == h->forced) return 0;
  h->forced = v;
  *out = v;
  return 1;
}

// Trustworthy = live still equals `forced`; a player who set exactly that value is overwritten.
inline int ekm_fov_stash_valid(int held, int forced, int live) {
  if (!held) return 0;
  if (forced < EK_FOV_MIN || forced > EK_FOV_MAX) return 0;
  return live == forced ? 1 : 0;
}

// After a crash settings.cfg still holds the player's fov, so a stash on disk reads back as `saved`; a tie goes to
// REPLAY.
enum { EK_FOV_STASH_NONE = 0, EK_FOV_STASH_REPLAY = 1, EK_FOV_STASH_BACK = 2 };
inline int ekm_fov_stash_answer(int held, int forced, int saved, int live) {
  if (!held) return EK_FOV_STASH_NONE;
  if (ekm_fov_stash_valid(held, forced, live)) return EK_FOV_STASH_REPLAY;
  if (live == saved) return EK_FOV_STASH_BACK;
  return EK_FOV_STASH_NONE;
}

void fov_frame(void);

bool fov_available(void);

void fov_replay_stash(void);

int fov_live(int* out);

void fov_hold_state(int* held, int* saved, int* forced);
