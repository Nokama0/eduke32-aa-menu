#pragma once

// One engine cvar, cl_weaponswitch: 1 if new, 2 if empty, 4 by wchoice. Bits 1 and 4 form one three-state option.

const int EK_WSWITCH_NEW     = 1;
const int EK_WSWITCH_EMPTY   = 2;
const int EK_WSWITCH_WCHOICE = 4;
const int EK_WSWITCH_MASK    = EK_WSWITCH_NEW | EK_WSWITCH_EMPTY | EK_WSWITCH_WCHOICE;

// Clamped, not masked, to match the OSD's vmin/vmax clamp (8 becomes 7).
inline int wswitch_mask_clamped(int v) {
  if (v < 0) return 0;
  if (v > EK_WSWITCH_MASK) return EK_WSWITCH_MASK;
  return v;
}

inline int wswitch_bit_on(int mask, int bit) {
  return (wswitch_mask_clamped(mask) & bit) ? 1 : 0;
}

inline int wswitch_with_bit(int mask, int bit, int on) {
  const int m = wswitch_mask_clamped(mask);
  const int b = bit & EK_WSWITCH_MASK;
  return on ? (m | b) : (m & ~b);
}

// Writes only 0, 1 and 1|4, values the engine menu can produce.
const int EK_WSWITCH_PICKUP_MASK = EK_WSWITCH_NEW | EK_WSWITCH_WCHOICE;
const int EK_PICKUP_NEVER = 0, EK_PICKUP_ALWAYS = 1, EK_PICKUP_IF_BETTER = 2;
const int EK_PICKUP_STATES = 3;

inline int wswitch_pickup_state(int mask) {
  const int m = wswitch_mask_clamped(mask);
  if (!(m & EK_WSWITCH_NEW)) return EK_PICKUP_NEVER;
  return (m & EK_WSWITCH_WCHOICE) ? EK_PICKUP_IF_BETTER : EK_PICKUP_ALWAYS;
}

inline int wswitch_with_pickup(int mask, int state) {
  int m = wswitch_mask_clamped(mask) & ~EK_WSWITCH_PICKUP_MASK;
  if (state == EK_PICKUP_ALWAYS)    m |= EK_WSWITCH_NEW;
  if (state == EK_PICKUP_IF_BETTER) m |= EK_WSWITCH_PICKUP_MASK;
  return m;
}

// "Only if better" uses wchoice, a per-player list this port does not expose.
inline const char* wswitch_pickup_name(int state) {
  switch (state) {
    case EK_PICKUP_ALWAYS:    return "Always";
    case EK_PICKUP_IF_BETTER: return "Only if better";
    default:                  return "Never";
  }
}
