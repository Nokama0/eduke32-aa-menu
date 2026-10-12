#pragma once

// Last two DISTINCT states held: the engine alternates viewingrange/yxaspect between two values, so a one-slot
// "changed" test always passes. Log only a state matching neither slot, then evict the older. T needs ==.
template<typename T>
struct EkSeen2 {
  bool haveA, haveB;
  T a, b;
};

template<typename T>
bool ekm_seen2_admit(EkSeen2<T>& slots, const T& state) {
  if (slots.haveA && slots.a == state) return false;
  if (slots.haveB && slots.b == state) return false;
  slots.a = slots.b; slots.haveA = slots.haveB;
  slots.b = state;   slots.haveB = true;
  return true;
}
