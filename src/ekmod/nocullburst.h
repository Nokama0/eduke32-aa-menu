// Burst log for the alternating base-level plane (train, mode 2): a once-a-second average cannot show an
// every-other-frame pattern.
#pragma once

// Murmur3 fmix32: the fingerprint must be sensitive to WHICH sectors were swept, not just how many.
inline unsigned ekm_nocull_fp_mix(unsigned id) {
  unsigned x = id + 0x9E3779B9u;
  x ^= x >> 16;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  x *= 0xC2B2AE35u;
  x ^= x >> 16;
  return x;
}

// Sum and XOR of the mixed ids: order-independent, and an empty sweep is (0, 0).
inline void ekm_nocull_fp_add(unsigned id, unsigned* sum, unsigned* xorv) {
  const unsigned m = ekm_nocull_fp_mix(id);
  *sum += m;
  *xorv ^= m;
}

// Idle until due, then EK_NOCULL_BURST_SIZE consecutive passes before the next burst; `firstEver` starts the first at
// once.
const unsigned EK_NOCULL_BURST_INTERVAL_MS = 5000u;
const int EK_NOCULL_BURST_SIZE = 16;

inline bool ekm_nocull_burst_due(unsigned nowMs, unsigned lastStartMs,
                                 bool firstEver) {
  if (firstEver) return true;
  return (nowMs - lastStartMs) >= EK_NOCULL_BURST_INTERVAL_MS;
}

inline bool ekm_nocull_burst_room(int filled) {
  return filled < EK_NOCULL_BURST_SIZE;
}

inline bool ekm_nocull_burst_should_flush(int filledAfterAppend) {
  return filledAfterAppend >= EK_NOCULL_BURST_SIZE;
}

// A swept pass's kind, shared with perf.h's cut.drawalls split: g_nodraw 1 is prepare whatever else holds.
enum {
  EK_NOCULL_KIND_PREPARE = 0,
  EK_NOCULL_KIND_MAIN,
  EK_NOCULL_KIND_EXTRA,
  EK_NOCULL_KIND_OTHER
};

inline int ekm_nocull_pass_kind(int nodraw, bool extraPass) {
  if (nodraw == 1) return EK_NOCULL_KIND_PREPARE;
  return extraPass ? EK_NOCULL_KIND_EXTRA : EK_NOCULL_KIND_MAIN;
}

inline const char* ekm_nocull_pass_kind_name(int kind) {
  switch (kind) {
    case EK_NOCULL_KIND_PREPARE: return "prepare";
    case EK_NOCULL_KIND_MAIN:    return "main";
    case EK_NOCULL_KIND_EXTRA:   return "extra";
    default:                     return "other";
  }
}
