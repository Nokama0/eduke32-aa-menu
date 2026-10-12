#pragma once
// Not a verbatim carry: Megaton's common.h is engine internals with its addresses. Only the UI's symbols live here.

inline int clamp_int(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Saturate rather than wrap.
inline int clamp_ll_to_int(long long v) {
  if (v >  2147483647LL)       return  2147483647;
  if (v < -2147483647LL - 1LL) return -2147483647 - 1;
  return (int)v;
}

// Out-of-range or NaN casts are UB and this runs per vertex; `!(v > MIN)` also catches NaN.
inline int clamp_double_to_int(double v) {
  if (!(v > -2147483648.0)) return -2147483647 - 1;
  if (v > 2147483647.0)     return  2147483647;
  return (int)v;
}

// VK to SDL scancode, as in the sibling ports; a key missing here is one bind_from_sdl_scancode refuses. Absent on
// purpose: ESCAPE, modifiers, lock and system keys, numpad ENTER (reports VK_RETURN).
inline bool vk_to_sdl_scancode(int vk, int* scancodeOut) {
  switch (vk) {
    case 0x2D: *scancodeOut = 73; return true;
    case 0x2E: *scancodeOut = 76; return true;
    case 0x24: *scancodeOut = 74; return true;
    case 0x23: *scancodeOut = 77; return true;
    case 0x21: *scancodeOut = 75; return true;
    case 0x22: *scancodeOut = 78; return true;
    case 0x09: *scancodeOut = 43; return true;
    case 0xC0: *scancodeOut = 53; return true;
    // V is the toggle default (F6 collided with Quick_Save).
    case 0x54: *scancodeOut = 23; return true;
    case 0x56: *scancodeOut = 25; return true;
    case 0x70: *scancodeOut = 58; return true;
    case 0x71: *scancodeOut = 59; return true;
    case 0x72: *scancodeOut = 60; return true;
    case 0x73: *scancodeOut = 61; return true;
    case 0x74: *scancodeOut = 62; return true;
    case 0x75: *scancodeOut = 63; return true;
    case 0x76: *scancodeOut = 64; return true;
    case 0x77: *scancodeOut = 65; return true;
    case 0x78: *scancodeOut = 66; return true;
    case 0x79: *scancodeOut = 67; return true;
    case 0x7A: *scancodeOut = 68; return true;
    case 0x7B: *scancodeOut = 69; return true;
    case 0x41: *scancodeOut =  4; return true;
    case 0x42: *scancodeOut =  5; return true;
    case 0x43: *scancodeOut =  6; return true;
    case 0x44: *scancodeOut =  7; return true;
    case 0x45: *scancodeOut =  8; return true;
    case 0x46: *scancodeOut =  9; return true;
    case 0x47: *scancodeOut = 10; return true;
    case 0x48: *scancodeOut = 11; return true;
    case 0x49: *scancodeOut = 12; return true;
    case 0x4A: *scancodeOut = 13; return true;
    case 0x4B: *scancodeOut = 14; return true;
    case 0x4C: *scancodeOut = 15; return true;
    case 0x4D: *scancodeOut = 16; return true;
    case 0x4E: *scancodeOut = 17; return true;
    case 0x4F: *scancodeOut = 18; return true;
    case 0x50: *scancodeOut = 19; return true;
    case 0x51: *scancodeOut = 20; return true;
    case 0x52: *scancodeOut = 21; return true;
    case 0x53: *scancodeOut = 22; return true;
    case 0x55: *scancodeOut = 24; return true;
    case 0x57: *scancodeOut = 26; return true;
    case 0x58: *scancodeOut = 27; return true;
    case 0x59: *scancodeOut = 28; return true;
    case 0x5A: *scancodeOut = 29; return true;
    case 0x31: *scancodeOut = 30; return true;
    case 0x32: *scancodeOut = 31; return true;
    case 0x33: *scancodeOut = 32; return true;
    case 0x34: *scancodeOut = 33; return true;
    case 0x35: *scancodeOut = 34; return true;
    case 0x36: *scancodeOut = 35; return true;
    case 0x37: *scancodeOut = 36; return true;
    case 0x38: *scancodeOut = 37; return true;
    case 0x39: *scancodeOut = 38; return true;
    case 0x30: *scancodeOut = 39; return true;
    case 0x0D: *scancodeOut = 40; return true;
    case 0x08: *scancodeOut = 42; return true;
    case 0x20: *scancodeOut = 44; return true;
    // Physical-key US-layout OEM codes.
    case 0xBD: *scancodeOut = 45; return true;
    case 0xBB: *scancodeOut = 46; return true;
    case 0xDB: *scancodeOut = 47; return true;
    case 0xDD: *scancodeOut = 48; return true;
    case 0xDC: *scancodeOut = 49; return true;
    case 0xBA: *scancodeOut = 51; return true;
    case 0xDE: *scancodeOut = 52; return true;
    case 0xBC: *scancodeOut = 54; return true;
    case 0xBE: *scancodeOut = 55; return true;
    case 0xBF: *scancodeOut = 56; return true;
    case 0x13: *scancodeOut = 72; return true;
    case 0x27: *scancodeOut = 79; return true;
    case 0x25: *scancodeOut = 80; return true;
    case 0x28: *scancodeOut = 81; return true;
    case 0x26: *scancodeOut = 82; return true;
    case 0x6F: *scancodeOut = 84; return true;
    case 0x6A: *scancodeOut = 85; return true;
    case 0x6D: *scancodeOut = 86; return true;
    case 0x6B: *scancodeOut = 87; return true;
    case 0x61: *scancodeOut = 89; return true;
    case 0x62: *scancodeOut = 90; return true;
    case 0x63: *scancodeOut = 91; return true;
    case 0x64: *scancodeOut = 92; return true;
    case 0x65: *scancodeOut = 93; return true;
    case 0x66: *scancodeOut = 94; return true;
    case 0x67: *scancodeOut = 95; return true;
    case 0x68: *scancodeOut = 96; return true;
    case 0x69: *scancodeOut = 97; return true;
    case 0x60: *scancodeOut = 98; return true;
    case 0x6E: *scancodeOut = 99; return true;
    default: return false;
  }
}

// Adds ESCAPE back for entry.h (cancels an open field). Literal 0x1B keeps this windows.h-free.
inline bool vk_to_entry_scancode(int vk, int* scancodeOut) {
  if (vk == 0x1B) { if (scancodeOut) *scancodeOut = 41; return true; }
  return vk_to_sdl_scancode(vk, scancodeOut);
}

// Packs key and value into one 64-bit word so an interlocked publish/claim cannot separate them (menu.cpp s_rowCommit);
// -1 key = empty. Widen and shift via unsigned: sign extension would smear 1s.
inline long long row_commit_pack(int rowKey, int value) {
  return (long long)(((unsigned long long)(unsigned int)rowKey << 32) |
                     (unsigned long long)(unsigned int)value);
}
inline int row_commit_key(long long w) {
  return (int)(unsigned int)((unsigned long long)w >> 32);
}
inline int row_commit_value(long long w) {
  return (int)(unsigned int)(w & 0xFFFFFFFFLL);
}

