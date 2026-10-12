#pragma once

#include "gamestate.h"

const int EK_TRIG_PIC_TOUCHPLATE      = 3;
const int EK_TRIG_PIC_ACTIVATORLOCKED = 4;
const int EK_TRIG_PIC_MASTERSWITCH    = 8;
const int EK_TRIG_STAT_ACTIVATOR      = 8;

enum { EK_TRIG_TOUCHPLATE = 0, EK_TRIG_SWITCH = 1 };

// A free sprite slot keeps its old picnum and lotag (engine.cpp:9380, :8830), so only statnum says not in the world.
inline int ekm_trig_sprite_live(int statnum) {
  return statnum >= 0 && statnum < EK_MAXSTATUS;
}

// G_OperateActivators (sector.cpp:1117) and G_OperateMasterSwitches (:1179) both end at the responder's sector, so one
// lotag join serves both.
enum { EK_TRIG_RESP_NONE = -1, EK_TRIG_RESP_ACTIVATOR = 0,
       EK_TRIG_RESP_MASTERSWITCH = 1 };

inline int ekm_trig_responder_kind(int statnum, int picnum) {
  if (statnum == EK_TRIG_STAT_ACTIVATOR) return EK_TRIG_RESP_ACTIVATOR;
  if (statnum == EK_STAT_STANDABLE && picnum == EK_TRIG_PIC_MASTERSWITCH)
    return EK_TRIG_RESP_MASTERSWITCH;
  return EK_TRIG_RESP_NONE;
}

// Only an activator can be locked (sector.cpp:1126).
inline int ekm_trig_responder_locked(int responderKind, int picnum) {
  if (responderKind != EK_TRIG_RESP_ACTIVATOR) return 0;
  return picnum == EK_TRIG_PIC_ACTIVATORLOCKED ? 1 : 0;
}

// SECRET may be unreachable on real maps: lotag 32767 masks to 16383, no ST_* case (sector.cpp:596). UNLOCKS wins.
enum { EK_TRIG_SECRET = 0, EK_TRIG_OPENS = 1, EK_TRIG_UNLOCKS = 2 };

const int EK_TRIG_MAX_CHAINS = 32;

struct EkTrigChain { int triggerIndex, triggerKind, effect, sectnum, tx, ty, tz, cx, cy; };

inline int ekm_trig_effect(int operatedIsSecret, int operatedAdjoinsSecret, int activatorLocked) {
  if (activatorLocked) return EK_TRIG_UNLOCKS;
  if (operatedIsSecret) return EK_TRIG_SECRET;
  return EK_TRIG_OPENS;
}

inline const char* ekm_trig_label(int effect) {
  if (effect == EK_TRIG_SECRET) return "Secret";
  if (effect == EK_TRIG_UNLOCKS) return "Locked";
  return "Opens secret";
}

inline int ekm_trig_reaches_secret(int operatedIsSecret, int operatedAdjoinsSecret) {
  return operatedIsSecret || operatedAdjoinsSecret;
}

inline int ekm_trig_chain_fits(int staged, int max) {
  return staged < max;
}

// 1 Hz sampler only: entering a secret zeroes its lotag (sector.cpp:3293).
bool secrettrig_scan(void);

// Acquire once, then pass gen: chains are repacked each scan (secretvol.h).
int secrettrig_generation(void);

int secrettrig_chains(int gen);
const EkTrigChain* secrettrig_chain(int gen, int i);

// refused: EK_TRIG_MAX_CHAINS. dropped: scratch capacity. Two limits, two counters.
int secrettrig_seen(int gen);
int secrettrig_refused(int gen);
int secrettrig_dropped(int gen);
