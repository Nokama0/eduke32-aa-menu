#pragma once
#include "worldquad.h"

// On a TROR map each storey draws with GL_ALWAYS and a depth clear, and the depth-only hole plane stamps a nearer depth
// over the farther storey, so quads flushed after the last renderDrawMasks failed LEQUAL. Each storey is flushed at the
// next renderDrawRoomsQ16 entry instead.

enum { EK_SF_CALL_NONE = 0, EK_SF_CALL_FAKE = 1, EK_SF_CALL_REAL = 2,
       EK_SF_CALL_BASE = 3 };

// g_nodraw set: discovery pass (engine.cpp:1072). Forced sector: a storey (:1160). Else the base level (:1203).
inline int ekm_sf_classify(bool inYax, int nodraw, int sect, int maxSectors) {
  if (!inYax || nodraw < 0 || maxSectors <= 0) return EK_SF_CALL_NONE;
  if (nodraw != 0) return EK_SF_CALL_FAKE;
  return sect >= maxSectors ? EK_SF_CALL_REAL : EK_SF_CALL_BASE;
}

// yax_globallev = YAX_MAXDRAWS + (2cf-1)(lev+1) (engine.cpp:1141), lev up to YAX_MAXDRAWS-1 (:1094-1098): 0..16 without
// 8, the base (:1199).
const int EK_SF_YAX_MAXDRAWS = 8;
inline bool ekm_sf_level_ok(int lev) {
  return lev >= 0 && lev <= 2 * EK_SF_YAX_MAXDRAWS && lev != EK_SF_YAX_MAXDRAWS;
}

const int EK_SF_FALLBACK_KEY = 0x10000;
inline int ekm_sf_level_key(bool levRead, int lev, int ordinal) {
  if (levRead && ekm_sf_level_ok(lev)) return lev;
  return EK_SF_FALLBACK_KEY + (ordinal > 0 ? ordinal : 0);
}
inline bool ekm_sf_key_is_fallback(int key) { return key >= EK_SF_FALLBACK_KEY; }

// Flush only when a DIFFERENT storey starts: the nmp redraw of one bunch (:1157-1175) repaints with GL_ALWAYS and would
// erase an earlier flush.
inline bool ekm_sf_flush_due(bool haveLast, int lastKey, int key, int pending) {
  return haveLast && key != lastKey && pending > 0;
}

struct EkSfLevelState { int haveLast; int lastKey; };
inline bool ekm_sf_enter_real(EkSfLevelState* s, int key, int pending) {
  if (!s) return false;
  const bool due = ekm_sf_flush_due(s->haveLast != 0, s->lastKey, key, pending);
  s->haveLast = 1;
  s->lastKey = key;
  return due;
}

inline int ekm_sf_fold_pass(const unsigned char* got, int gotBytes,
                            const short* cand, int ncand,
                            unsigned char* pending, int pendBytes,
                            int* repeats) {
  if (!got || !cand || !pending) return 0;
  int added = 0;
  for (int i = 0; i < ncand; i++) {
    const int s = cand[i];
    if (!ekm_wq_bit_test(got, gotBytes, s)) continue;
    if (ekm_wq_bit_test(pending, pendBytes, s)) {
      if (repeats) (*repeats)++;
      continue;
    }
    ekm_wq_bit_set(pending, pendBytes, s);
    if (ekm_wq_bit_test(pending, pendBytes, s)) added++;
  }
  return added;
}

struct EkSfSite { int count; unsigned at; unsigned target; unsigned slot; };

// Site A's compare sits 0x29 before the g_nodraw store, site B's store 0x43 before the base call.
const unsigned EK_SF_WINDOW = 0x60u;
const unsigned EK_SF_LOAD_GAP = 0x20u;

// mov r64, [rip+d32], the mingw .refptr load; returns the register or -1.
inline int ekm_sf_refptr_load(const unsigned char* b, unsigned p, unsigned e,
                              unsigned* slot) {
  if (!b || p + 7 > e) return -1;
  if (b[p] < 0x48 || b[p] > 0x4F || b[p + 1] != 0x8B) return -1;
  const unsigned char m = b[p + 2];
  if ((m & 0xC7) != 0x05) return -1;
  *slot = (unsigned)((int)(p + 7) + *(const int*)(b + p + 3));
  return (int)(((m >> 3) & 7) | (((b[p] >> 2) & 1) << 3));
}

inline bool ekm_sf_rip_form(const unsigned char* b, unsigned p, unsigned e,
                            bool cmp, int imm, unsigned* target) {
  const unsigned len = cmp ? 7u : 10u;
  if (!b || p + len > e) return false;
  if (b[p] != (cmp ? 0x83 : 0xC7) || b[p + 1] != (cmp ? 0x3D : 0x05)) return false;
  const int v = cmp ? (int)(signed char)b[p + 6] : *(const int*)(b + p + 6);
  if (v != imm) return false;
  *target = (unsigned)((int)(p + len) + *(const int*)(b + p + 2));
  return true;
}

inline int ekm_sf_reg_form(const unsigned char* b, unsigned p, unsigned e,
                           bool cmp, int imm, unsigned* len) {
  if (!b) return -1;
  unsigned at = p;
  int rexb = 0;
  if (at < e && b[at] == 0x41) { rexb = 1; at++; }
  const unsigned body = cmp ? 3u : 6u;
  if (at + body > e || b[at] != (cmp ? 0x83 : 0xC7)) return -1;
  const unsigned char m = b[at + 1];
  if ((m >> 6) != 0 || ((m >> 3) & 7) != (cmp ? 7 : 0)) return -1;
  if ((m & 7) == 4 || (m & 7) == 5) return -1;
  const int v = cmp ? (int)(signed char)b[at + 2] : *(const int*)(b + at + 2);
  if (v != imm) return -1;
  *len = (at + body) - p;
  return (int)((m & 7) | (rexb << 3));
}

// Every compare or store of `imm` in [lo, hi): direct forms fill target, register forms the slot of the nearest .refptr
// load within EK_SF_LOAD_GAP.
inline void ekm_sf_scan(const unsigned char* b, unsigned lo, unsigned hi,
                        bool cmp, int imm, EkSfSite* out) {
  if (!out) return;
  out->count = 0; out->at = 0; out->target = 0; out->slot = 0;
  if (!b || hi <= lo) return;
  for (unsigned p = lo; p < hi; p++) {
    unsigned t = 0, len = 0;
    if (ekm_sf_rip_form(b, p, hi, cmp, imm, &t)) {
      out->count++; out->at = p; out->target = t; out->slot = 0;
      continue;
    }
    const int reg = ekm_sf_reg_form(b, p, hi, cmp, imm, &len);
    if (reg < 0) continue;
    out->count++; out->at = p; out->target = 0; out->slot = 0;
    const unsigned floor = p - lo > EK_SF_LOAD_GAP ? p - EK_SF_LOAD_GAP : lo;
    for (unsigned q = p >= 7 ? p - 7 : 0; q >= floor && q + 7 <= p; q--) {
      unsigned s = 0;
      if (ekm_sf_refptr_load(b, q, p, &s) == reg) { out->slot = s; break; }
      if (q == 0) break;
    }
    p += len - 1;
  }
}

inline unsigned ekm_sf_agree(unsigned a, unsigned b) {
  return (a != 0 && a == b) ? a : 0u;
}

// The base level's call is the one with a store of 8 before it (engine.cpp:1199): exactly one, or -1.
inline int ekm_sf_pick_base(const EkSfSite* sites, int n) {
  if (!sites || n <= 0) return -1;
  int pick = -1;
  for (int i = 0; i < n; i++) {
    if (sites[i].count == 0) continue;
    if (sites[i].count != 1 || pick >= 0) return -1;
    pick = i;
  }
  return pick;
}

// Nothing here writes engine memory. Render thread only, except storeyflush_locate (sampler).

void storeyflush_locate(void);

void storeyflush_frame_begin(void);

// Every renderDrawRoomsQ16 entry: the player view opens the window, a later storey flushes the one before, the base
// level flushes and closes.
void storeyflush_on_q16(const void* ret, int sect);

// The rooms detour skipped the engine for a storey pass, so gotsector is not its.
void storeyflush_pass_skipped(void);
