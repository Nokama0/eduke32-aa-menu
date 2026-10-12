#pragma once
#include <stddef.h>
#include <stdint.h>
#include "pickuprange_aa.h"

// The row rewrites the operand of the 75 verified ifpdistl 844 sites in AA's compiled script to the radius (844 when
// Off). Items never move. Pure half.

enum { EK_PR_OFF = 0, EK_PR_1024, EK_PR_1536, EK_PR_2048, EK_PR_4096, EK_PR_8192, EK_PR_COUNT };

const int EK_PR_AA_RETRIEVE = 844;

// All-or-nothing: every (ifpdistl, 844) site, one gate per allowlisted actor, and the seven slides.
const int EK_PR_SITES  = 97;
const int EK_PR_GATES  = 68;
const int EK_PR_RESETS = 7;
static_assert(EK_PR_GATES == EKM_PR_AA_COUNT, "one gate per allowlisted actor");
static_assert(EK_PR_RESETS == EKM_PR_AA_RESETS, "one slide per resetcount weapon");

const int EK_PR_R10669_IFPDISTL = 241, EK_PR_R10669_RESETCOUNT = 312, EK_PR_R10669_ENDA = 64;

inline int pickuprange_clamp(int v) {
  return v < 0 ? 0 : (v >= EK_PR_COUNT ? EK_PR_COUNT - 1 : v);
}

inline const char* pickuprange_name(int v) {
  static const char* const kNames[EK_PR_COUNT] = {
    "Off (AA's own)", "1024", "1536", "2048", "4096", "8192" };
  return kNames[pickuprange_clamp(v)];
}

// In the engine's player distance (sector.cpp:221-240). Capped below 16384, where an out-of-sight item sleeps. 0 is
// Off.
inline int pickuprange_radius(int v) {
  static const int kRadius[EK_PR_COUNT] = { 0, 1024, 1536, 2048, 4096, 8192 };
  return kRadius[pickuprange_clamp(v)];
}

inline long long pickuprange_operand(int v) {
  const int r = pickuprange_radius(v);
  return r == 0 ? EK_PR_AA_RETRIEVE : r;
}

// Instruction word = op | line << 12 (gamedef.cpp:1382); line 0 is never compiled, which tells an instruction from an
// operand holding an opcode's value.
const unsigned long long EK_PR_INST_MASK = 0xFFFFFFFF80000FFFull;

inline bool pr_is_inst(long long w, int op) {
  if (op < 0 || op > 0xFFF) return false;
  const unsigned long long u = (unsigned long long)w;
  return (u & EK_PR_INST_MASK) == (unsigned long long)op && ((u >> 12) & 0x7FFFF) >= 1;
}

inline bool pr_is_enda(long long w, int opEnda) { return pr_is_inst(w, opEnda); }

// A site: ifpdistl at s, 844 at s+1, the absolute fail pointer at s+2 (gamedef.cpp:5252, :5261-5269).
inline bool pr_site_match(const long long* w, size_t n, size_t s, int opIfpdistl, uintptr_t base) {
  if (!w || s + 2 >= n) return false;
  if (!pr_is_inst(w[s], opIfpdistl)) return false;
  if (w[s + 1] != EK_PR_AA_RETRIEVE) return false;
  const unsigned long long p = (unsigned long long)w[s + 2];
  const unsigned long long after = (unsigned long long)base + (s + 3) * 8;
  const unsigned long long end = (unsigned long long)base + n * 8;
  if (p <= after || p >= end) return false;
  return ((p - (unsigned long long)base) & 7) == 0;
}

// The dropped-weapon slide: the branch is exactly one resetcount.
inline bool pr_is_resetcount(const long long* w, size_t n, size_t s, int opReset, uintptr_t base) {
  if (!w || s + 3 >= n) return false;
  if (!pr_is_inst(w[s + 3], opReset)) return false;
  return (unsigned long long)w[s + 2] == (unsigned long long)base + (s + 4) * 8;
}

inline int pr_owner(const uintptr_t* sortedExec, int count, uintptr_t addr) {
  if (!sortedExec || count <= 0 || addr < sortedExec[0]) return -1;
  int lo = 0, hi = count - 1;
  while (lo < hi) {
    const int mid = lo + (hi - lo + 1) / 2;
    if (sortedExec[mid] <= addr) lo = mid; else hi = mid - 1;
  }
  return lo;
}

// An actor owns site s only if no ENDA lies between its body (gamedef.cpp:3270-3318) and s.
inline bool pr_owned(const long long* w, size_t n, size_t execIdx, size_t s, int opEnda) {
  if (!w || s >= n || s < execIdx + 4) return false;
  for (size_t k = s; k > execIdx + 4; k--)
    if (pr_is_enda(w[k - 1], opEnda)) return false;
  return true;
}

inline bool pr_counts_ok(int total, int gates, int resets, const int* gatesPerRow,
                         const int* resetsPerRow) {
  if (!gatesPerRow || !resetsPerRow) return false;
  if (total != EK_PR_SITES || gates != EK_PR_GATES || resets != EK_PR_RESETS) return false;
  const EkPickupAaRow* const rows = pickuprange_aa_rows();
  for (int i = 0; i < EKM_PR_AA_COUNT; i++) {
    if (gatesPerRow[i] != 1) return false;
    if (resetsPerRow[i] != (pickuprange_aa_resets(rows[i].picnum) ? 1 : 0)) return false;
  }
  return true;
}

// Anything but the recorded instruction and our last operand is not ours to overwrite.
inline bool pr_write_ok(long long instNow, long long instRec, long long opNow, long long opLast) {
  return instNow == instRec && opNow == opLast;
}

// From SwapBuffers (the VM's thread).
void pickuprange_frame(void);
bool pickuprange_ready(void);
const char* pickuprange_status(void);
