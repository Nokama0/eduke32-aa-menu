#pragma once

// A picnum's first sighting as a one-bit-per-tile set; true once per in-range picnum. `bits` needs (cap + 7) / 8 bytes.
inline bool ekm_first_sight(unsigned char* bits, int cap, int picnum) {
  if (!bits || picnum < 0 || picnum >= cap) return false;
  const int byte = picnum >> 3;
  const unsigned char mask = (unsigned char)(1u << (picnum & 7));
  if (bits[byte] & mask) return false;
  bits[byte] = (unsigned char)(bits[byte] | mask);
  return true;
}
