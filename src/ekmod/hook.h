
#pragma once
#include <stddef.h>

inline int ekm_byte_at(const unsigned char* code, int avail, int i) {
  return (i >= 0 && i < avail) ? (int)code[i] : -1;
}

// Whole instructions covering >= 5 bytes, or 0 on anything unknown (only GCC prologues).
inline int ekm_prologue_len(const unsigned char* code, int avail) {
  if (code == NULL || avail <= 0) return 0;
  int n = 0;
  while (n < 5) {
    const int b0 = ekm_byte_at(code, avail, n);
    const int b1 = ekm_byte_at(code, avail, n + 1);
    const int b2 = ekm_byte_at(code, avail, n + 2);
    int step;
    if (b0 >= 0x50 && b0 <= 0x57)                     step = 1;
    else if (b0 == 0x41 && b1 >= 0x50 && b1 <= 0x57)  step = 2;
    else if (b0 == 0x48 && b1 == 0x83 && b2 == 0xEC)  step = 4;
    else if (b0 == 0x48 && b1 == 0x81 && b2 == 0xEC)  step = 7;
    else if (b0 == 0x48 && b1 == 0x89 && b2 == 0xE5)  step = 3;
    else if (b0 == 0x48 && b1 == 0x8B && b2 == 0xEC)  step = 3;
    else return 0;
    if (n + step > avail) return 0;
    n += step;
  }
  return n;
}

// rel32 is measured from the END of the 5-byte jump (rip's value); nothing downstream catches an off-by-five.
inline bool ekm_rel32(unsigned long long from, unsigned long long to, int* out) {
  if (out == NULL) return false;
  const unsigned long long end = from + 5ull;
  if (to >= end) {
    const unsigned long long up = to - end;
    if (up > 0x7FFFFFFFull) return false;
    *out = (int)up;
  } else {
    const unsigned long long down = end - to;
    if (down > 0x80000000ull) return false;
    *out = (int)(-(long long)down);
  }
  return true;
}

const int EKM_ABSJMP_LEN = 14;

const int EKM_MAX_PROLOGUE = 16;

// Displacement zero: rip already points at the address written after the 6-byte instruction.
inline int ekm_build_absjmp(unsigned char* out, int cap, unsigned long long to) {
  if (out == NULL || cap < EKM_ABSJMP_LEN) return 0;
  out[0] = 0xFF;
  out[1] = 0x25;
  out[2] = 0x00; out[3] = 0x00; out[4] = 0x00; out[5] = 0x00;
  for (int i = 0; i < 8; i++) out[6 + i] = (unsigned char)((to >> (i * 8)) & 0xFF);
  return EKM_ABSJMP_LEN;
}

// Trampoline: the clobbered bytes, then an absolute jump back. Returns total length, or 0.
inline int ekm_build_trampoline(unsigned char* out, int cap,
                                const unsigned char* saved, int len,
                                unsigned long long resume) {
  if (out == NULL || saved == NULL) return 0;
  if (len < 5 || len > EKM_MAX_PROLOGUE) return 0;
  if (cap < len) return 0;
  // absjmp's cap check is the one authority on the tail fitting.
  const int t = ekm_build_absjmp(out + len, cap - len, resume);
  if (!t) return 0;
  for (int i = 0; i < len; i++) out[i] = saved[i];
  return len + t;
}

// Patch: E9 rel32 then NOP padding, so no half-instruction tail is left as a jump landing place.
inline int ekm_build_patch(unsigned char* out, int cap, int len, int rel) {
  if (out == NULL) return 0;
  if (len < 5 || len > EKM_MAX_PROLOGUE || cap < len) return 0;
  out[0] = 0xE9;
  const unsigned int u = (unsigned int)rel;
  for (int i = 0; i < 4; i++) out[1 + i] = (unsigned char)((u >> (i * 8)) & 0xFF);
  for (int i = 5; i < len; i++) out[i] = 0x90;
  return len;
}

// Call-site patch writes only the 4 operand bytes, after verifying they are the claimed call.
const int EKM_CALL_LEN = 5;
const int EKM_CALL_OPERAND = 4;

inline bool ekm_call_rel32_targets(const unsigned char* code, int avail,
                                   unsigned long long site,
                                   unsigned long long target) {
  if (code == NULL || avail < EKM_CALL_LEN) return false;
  if (code[0] != 0xE8) return false;
  unsigned int u = 0;
  for (int i = 0; i < EKM_CALL_OPERAND; i++)
    u |= (unsigned int)code[1 + i] << (i * 8);
  const long long disp = (long long)(int)u;
  return (unsigned long long)((long long)site + (long long)EKM_CALL_LEN + disp)
         == target;
}

inline int ekm_build_call_operand(unsigned char* out, int cap,
                                  unsigned long long site,
                                  unsigned long long to) {
  if (out == NULL || cap < EKM_CALL_OPERAND) return 0;
  int rel = 0;
  if (!ekm_rel32(site, to, &rel)) return 0;
  const unsigned int u = (unsigned int)rel;
  for (int i = 0; i < EKM_CALL_OPERAND; i++)
    out[i] = (unsigned char)((u >> (i * 8)) & 0xFF);
  return EKM_CALL_OPERAND;
}

// `trampoline` is also the base of the near allocation holding trampoline and relay stub.
struct EkHook { void* target; void* trampoline; unsigned char original[16]; int len; };

bool hook_install(void* target, void* detour, EkHook* out);
bool hook_remove(EkHook* h);
void* hook_trampoline(const EkHook* h);

// `site` points at the 0xE8; `block` is the near page holding the absolute jump.
struct EkCallPatch { void* site; void* block; unsigned char original[4]; int len; };

// `expected` is not a hint: install refuses unless the bytes at `site` decode as a call going there.
bool callsite_install(void* site, void* expected, void* detour, EkCallPatch* out);
bool callsite_remove(EkCallPatch* p);
