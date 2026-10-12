// "Draw the whole map": call polymost_scansector for every sector the engine's own seed left unscanned, so a flying
// free camera sees a drawn world. Ported from the Megaton/World Tour nocull.h; the DLL half reads live memory, so
// every decision that could be wrong is a pure rule here.
#pragma once

// MAXWALLSB, RE-DERIVED AND NOT COPIED: Megaton's is 6144; build.h:92 makes it MAXWALLS>>1 and EDuke32 builds with
// MAXWALLSV8 = 16384, so it is 8192 here.
const int EK_NOCULL_MAXWALLSB = 8192;

const int EK_NOCULL_CALL_ALLOWANCE = 256;

const int EK_NOCULL_FLOOR_MARGIN = 1024;

// May the extra sweep run this frame? FOUR SEPARATE FACTS: cfgOn (off by default, the sort is O(bunches^2));
// cameraActive (the RENDER-clock snapshot); bracketArmed (the camera may be active while the bracket refuses to arm);
// inMirror (a mirror renders drawrooms twice).
inline bool nocull_should_scan(bool cfgOn, bool cameraActive, bool bracketArmed,
                               bool inMirror) {
  return cfgOn && cameraActive && bracketArmed && !inMirror;
}

// THE SWEEP REMOVES A NATURAL BOUND: polymost_scansector appends to numscans and numbunches with no bound. Reference
// mode spends at most HALF the free room; Full mode relies on the engine's own guard (polymost.cpp:6673) but leaves
// polymost_drawalls' later scan only 1024, so a map above ~6,900 scans can log "!!numscans" and drop geometry.
enum {
  EK_NOCULL_BUDGET_REFERENCE = 0,
  EK_NOCULL_BUDGET_FULL = 1
};

inline int ekm_nocull_budget_mode_clamped(int mode) {
  if (mode == EK_NOCULL_BUDGET_REFERENCE) return EK_NOCULL_BUDGET_REFERENCE;
  return EK_NOCULL_BUDGET_FULL;
}

inline const char* ekm_nocull_budget_mode_name(int mode) {
  return ekm_nocull_budget_mode_clamped(mode) == EK_NOCULL_BUDGET_REFERENCE
             ? "Reference (half)" : "Full";
}

inline bool nocull_budget_allows(int numscansEntry, int numbunchesEntry,
                                 int numscansNow, int numbunchesNow,
                                 int wallnum,
                                 int mode = EK_NOCULL_BUDGET_REFERENCE) {
  if (numscansEntry < 0 || numbunchesEntry < 0 ||
      numscansNow   < 0 || numbunchesNow   < 0 || wallnum < 0) return false;

  const int floorAt = EK_NOCULL_MAXWALLSB - EK_NOCULL_FLOOR_MARGIN;
  if (numscansNow > floorAt || numbunchesNow > floorAt) return false;

  const int nowMax = numscansNow > numbunchesNow ? numscansNow : numbunchesNow;

  if (ekm_nocull_budget_mode_clamped(mode) == EK_NOCULL_BUDGET_FULL)
    return nowMax + wallnum + EK_NOCULL_CALL_ALLOWANCE <= floorAt;

  const int entryMax = numscansEntry > numbunchesEntry ? numscansEntry : numbunchesEntry;
  const int budget = (EK_NOCULL_MAXWALLSB - entryMax) / 2;
  const int usedSinceEntry = nowMax - entryMax;

  return usedSinceEntry + wallnum + EK_NOCULL_CALL_ALLOWANCE <= budget;
}

// NEAREST-FIRST ADMISSION: the sweep stops at its budget every frame on big maps; index order let a low-index sector
// spend budget a high-index one needed (flicker). Keying by distance makes the cut fall on the FAR edge.
enum {
  EK_NOCULL_SWEEPORDER_INDEX = 0,
  EK_NOCULL_SWEEPORDER_NEAREST = 1
};

inline int ekm_nocull_sweeporder_clamped(int mode) {
  if (mode == EK_NOCULL_SWEEPORDER_INDEX) return EK_NOCULL_SWEEPORDER_INDEX;
  return EK_NOCULL_SWEEPORDER_NEAREST;
}

inline const char* ekm_nocull_sweeporder_name(int mode) {
  return ekm_nocull_sweeporder_clamped(mode) == EK_NOCULL_SWEEPORDER_INDEX
             ? "Index" : "Nearest first";
}

struct NcSweepCandidate { int sector; long long dist2; };

// Squared 2D distance from the eye to an axis-aligned box, 0 when inside on both axes. long long and clamped to
// INT32_MAX BEFORE squaring: an int32 span squared overflows int64 on one axis alone; the pin proves the extreme.
inline long long ekm_nocull_bbox_dist2(long long eyeX, long long eyeY,
                                       long long minX, long long maxX,
                                       long long minY, long long maxY) {
  long long dx = 0, dy = 0;
  if (eyeX < minX) dx = minX - eyeX;
  else if (eyeX > maxX) dx = eyeX - maxX;
  if (eyeY < minY) dy = minY - eyeY;
  else if (eyeY > maxY) dy = eyeY - maxY;
  const long long kMaxDelta = 2147483647LL;
  if (dx > kMaxDelta) dx = kMaxDelta;
  if (dy > kMaxDelta) dy = kMaxDelta;
  return dx * dx + dy * dy;
}

inline bool ekm_nocull_candidate_before(const NcSweepCandidate& a,
                                        const NcSweepCandidate& b) {
  if (a.dist2 != b.dist2) return a.dist2 < b.dist2;
  return a.sector < b.sector;
}

inline int ekm_nocull_candidate_cmp(const void* pa, const void* pb) {
  const NcSweepCandidate* a = (const NcSweepCandidate*)pa;
  const NcSweepCandidate* b = (const NcSweepCandidate*)pb;
  if (ekm_nocull_candidate_before(*a, *b)) return -1;
  if (ekm_nocull_candidate_before(*b, *a)) return 1;
  return 0;
}

inline bool ekm_nocull_candidate_farther(const NcSweepCandidate& a,
                                         const NcSweepCandidate& b) {
  if (a.dist2 != b.dist2) return a.dist2 > b.dist2;
  return a.sector < b.sector;
}
inline int ekm_nocull_candidate_cmp_far(const void* pa, const void* pb) {
  const NcSweepCandidate* a = (const NcSweepCandidate*)pa;
  const NcSweepCandidate* b = (const NcSweepCandidate*)pb;
  if (ekm_nocull_candidate_farther(*a, *b)) return -1;
  if (ekm_nocull_candidate_farther(*b, *a)) return 1;
  return 0;
}

// THE EXTRA PASS STOPS WHERE THE MAIN PASS'S REACH ENDS: only the band beyond the main pass's last admitted sector
// needs it. The reach is LAST frame's, so the cutoff keeps a 10% margin (81% of dist2). v/100*81 + (v%100)*81/100 is
// EXACTLY floor(81v/100) for v >= 0 and cannot overflow.
inline long long ekm_nocull_extra_cutoff(long long mainLastAdmittedDist2) {
  if (mainLastAdmittedDist2 < 0) return -1;
  const long long v = mainLastAdmittedDist2;
  return v / 100 * 81 + (v % 100) * 81 / 100;
}

inline bool ekm_nocull_extra_admits(long long dist2, long long cutoff) {
  return cutoff < 0 || dist2 >= cutoff;
}

// THE EXTRA PASS'S TRIGGER, MADE STICKY: deciding it from the latest main pass alone flips it off for any frame whose
// main pass completes (one-frame far pops), so hold it on for a window of publishes after the last stop.
const int EK_NOCULL_EXTRA_HOLD = 32;

struct NcExtraHold { int age; long long reach; };

inline NcExtraHold ekm_nocull_hold_idle(void) {
  NcExtraHold h; h.age = EK_NOCULL_EXTRA_HOLD; h.reach = -1;
  return h;
}

inline NcExtraHold ekm_nocull_hold_update(NcExtraHold h, bool stopped,
                                          long long reachNew) {
  const int age = (h.age < 0 || h.age > EK_NOCULL_EXTRA_HOLD)
                      ? EK_NOCULL_EXTRA_HOLD : h.age;
  NcExtraHold out;
  if (stopped) {
    out.age = 0;
    out.reach = reachNew;
  } else {
    out.age = age < EK_NOCULL_EXTRA_HOLD ? age + 1 : EK_NOCULL_EXTRA_HOLD;
    out.reach = h.reach;
  }
  return out;
}

inline bool ekm_nocull_hold_wanted(NcExtraHold h) {
  return h.age >= 0 && h.age < EK_NOCULL_EXTRA_HOLD;
}

// The next sector strictly after `from` whose gotsector bit is still clear, or -1 at numsectors. gotsector's
// convention is bit `s & 7` of byte `s >> 3`. numsectors is live memory, so a non-positive count refuses.
inline int nocull_next_unscanned(const unsigned char* gotsector, int numsectors,
                                 int from) {
  if (!gotsector || numsectors <= 0) return -1;
  const int start = from < 0 ? 0 : from + 1;
  for (int s = start; s < numsectors; s++) {
    if ((gotsector[s >> 3] & (unsigned char)(1 << (s & 7))) == 0) return s;
  }
  return -1;
}

// The rules the locate decomposes into: nocull.cpp finds five engine symbols by signature in the stripped host and
// reads live memory, so every decision in it that could be wrong is a pure function here.

// MAXWALLSB READ OUT OF THE BINARY: polymost.cpp:6673's `numscans >= MAXWALLSB-1` compiles to `> MAXWALLSB-2`, so the
// immediate is two BELOW the constant. 1024 is a plausibility gate: every value Build has had is a multiple of it.
inline int ekm_nocull_maxwallsb_from_cmp(int imm) {
  if (imm <= 0) return 0;
  const int v = imm + 2;
  if (v < 1024 || v > 65536) return 0;
  if ((v % 1024) != 0) return 0;
  return v;
}

// THE COUNTER PAIR IS TESTED AS A PAIR, AGAINST globalcursectnum: `numscans = numbunches = 0;` emits two adjacent
// 16-bit stores next to it. WHICH IS numscans IS NOT DECIDED: the budget takes max() of the pair, so the answer is
// invariant under swapping them.
inline bool ekm_nocull_counter_pair_ok(unsigned a, unsigned b, unsigned gcsRva) {
  if (gcsRva < 6) return false;
  if (a == b) return false;
  if (a == gcsRva || b == gcsRva) return false;
  const unsigned lo = a < b ? a : b;
  const unsigned hi = a < b ? b : a;
  if (hi - lo != 2) return false;
  return lo >= gcsRva - 6 && hi <= gcsRva - 2;
}

// gotsector's SIZE from the `rep stosq` of its memset: 0x40 qwords is 512 bytes = bitmap_size(MAXSECTORS 4096). A
// count that could not be a sector bitmap answers 0 rather than being scaled up into one.
inline int ekm_nocull_gotsector_bytes(int qwords) {
  if (qwords <= 0 || qwords > 1024) return 0;
  return qwords * 8;
}

inline int ekm_nocull_sector_bound(int gotsectorBytes, int numsectors) {
  if (gotsectorBytes <= 0 || numsectors <= 0) return 0;
  const int cap = gotsectorBytes * 8;
  return numsectors < cap ? numsectors : cap;
}


// May the filter walk this sector's wall span? A BOUND ON WORK, NOT A NEW REFUSAL: every case this turns away is one
// frustum.h already draws. `slots` is passed in so the rule stays pure.
inline bool ekm_nocull_span_walkable(int wallptr, int wallnum, int slots) {
  if (slots <= 0) return false;
  if (wallptr < 0) return false;
  if (wallnum < 3) return false;
  if (wallnum > slots - wallptr) return false;
  return true;
}

inline bool ekm_nocull_tally_ok(int walked, int rejected, int drawn) {
  if (walked < 0 || rejected < 0 || drawn < 0) return false;
  return rejected + drawn == walked;
}

const float EK_NOCULL_MARGIN_PX = 1024.0f;

// THE SPAN'S HORIZONTAL VERDICT. The GL route is dead: the view rotation goes to a shader uniform, so frustum.h's
// planes live in a space where the camera is the ORIGIN. This uses project.h; each corner feeds TWO entries, at each
// end of the z band, because Build's pitch comes after the perspective divide.
enum {
  EK_NOCULL_SPAN_KEEP = 0,
  EK_NOCULL_SPAN_LEFT = 1,
  EK_NOCULL_SPAN_RIGHT = 2
};

// AN ENTRY THAT FAILED TO PROJECT KEEPS THE WHOLE SPAN (behind the eye there is no screen x). Every entry outside the
// SAME side, never a mix: entries split across the two edges are a view looking BETWEEN them. Comparisons are
// `!(x < edge)` so a NaN lands on KEEP.
inline int ekm_nocull_span_verdict(const float* sx, const int* ok, int n,
                                   float leftEdge, float rightEdge) {
  if (!sx || !ok || n <= 0) return EK_NOCULL_SPAN_KEEP;
  if (!(leftEdge <= rightEdge)) return EK_NOCULL_SPAN_KEEP;

  int allLeft = 1, allRight = 1;
  for (int i = 0; i < n; i++) {
    if (!ok[i]) return EK_NOCULL_SPAN_KEEP;
    if (!(sx[i] < leftEdge)) allLeft = 0;
    if (!(sx[i] > rightEdge)) allRight = 0;
    if (!allLeft && !allRight) return EK_NOCULL_SPAN_KEEP;
  }
  if (allLeft) return EK_NOCULL_SPAN_LEFT;
  if (allRight) return EK_NOCULL_SPAN_RIGHT;
  return EK_NOCULL_SPAN_KEEP;
}

// ONE BAND PER CORNER: projecting every corner over the WHOLE MAP's z slab made nearly every sector straddle an edge.
// Each corner gets its own band via getzsofslopeptr, so slopes are bounded exactly. The neighbour union is the whole
// span's, not this corner's (a wall quad takes its far-side z from the neighbour).
enum {
  EK_NOCULL_BAND_USABLE = 0,
  EK_NOCULL_BAND_NOT_EXACT = 1,
  EK_NOCULL_BAND_UNREADABLE = 2
};

// THE SPRITE HEADROOM is a judgement: with the HRP on, a sprite is a 3D MODEL standing proud of its flat plane. 16384
// Build z units = 1024 horizontal (z is 16x finer); a typical enemy is ~11520 z tall. Widen if a model pops at a
// screen edge.
const int EK_NOCULL_SPRITE_Z_MARGIN = 16384;

// `exact == 0` IS PART OF THE ANSWER: the band is the sector's FLAT pair standing in for a slope the engine could not
// be asked about, so the caller has no bound and must keep the sector. Unreadable outranks it. Outputs are zeroed on
// every refusal.
inline int ekm_nocull_corner_band(int sCeil, int sFlor, int sExact, int sOk,
                                  int sFlatCeil, int sFlatFlor,
                                  int nCeil, int nFlor, int nExact, int nOk,
                                  int hasNeighbour, int* loOut, int* hiOut) {
  if (loOut) *loOut = 0;
  if (hiOut) *hiOut = 0;
  if (!loOut || !hiOut) return EK_NOCULL_BAND_UNREADABLE;

  if (!sOk) return EK_NOCULL_BAND_UNREADABLE;
  if (hasNeighbour && !nOk) return EK_NOCULL_BAND_UNREADABLE;
  if (!sExact) return EK_NOCULL_BAND_NOT_EXACT;
  if (hasNeighbour && !nExact) return EK_NOCULL_BAND_NOT_EXACT;

  int lo = sCeil < sFlor ? sCeil : sFlor;
  int hi = sCeil < sFlor ? sFlor : sCeil;

  const int fLo = sFlatCeil < sFlatFlor ? sFlatCeil : sFlatFlor;
  const int fHi = sFlatCeil < sFlatFlor ? sFlatFlor : sFlatCeil;
  if (fLo < lo) lo = fLo;
  if (fHi > hi) hi = fHi;

  if (hasNeighbour) {
    const int nLo = nCeil < nFlor ? nCeil : nFlor;
    const int nHi = nCeil < nFlor ? nFlor : nCeil;
    if (nLo < lo) lo = nLo;
    if (nHi > hi) hi = nHi;
  }

  const int zMin = -2147483647 - 1;
  const int zMax = 2147483647;
  lo = (lo < zMin + EK_NOCULL_SPRITE_Z_MARGIN) ? zMin
                                               : lo - EK_NOCULL_SPRITE_Z_MARGIN;
  hi = (hi > zMax - EK_NOCULL_SPRITE_Z_MARGIN) ? zMax
                                               : hi + EK_NOCULL_SPRITE_Z_MARGIN;

  *loOut = lo;
  *hiOut = hi;
  return EK_NOCULL_BAND_USABLE;
}

// polymost_scansector DISCARDS ITS PARAMETER (LTO IPA-SRA): the CALLER stores into a global the callee reads first,
// so `orig(s)` alone re-scans whatever sectorborder[0] held. The store is `mov word ptr [reg], r16` just before each
// `call`; the register match is the real test; the window is 32 bytes (64 finds a second candidate).
const int EK_NOCULL_SBORDER_WINDOW = 32;

// `buf`/`len` are the bytes IMMEDIATELY BEFORE a `call rel32`, so the store must END at buf[len]. On a match
// `outDisp32` is the load's rip displacement and `outLoadEnd` the offset past the load; the slot is
// (callRva - len + *outLoadEnd) + *outDisp32. Both are zero on a refusal.
inline bool ekm_nocull_sectorborder_slot(const unsigned char* buf, int len,
                                         int* outDisp32, int* outLoadEnd) {
  if (outDisp32) *outDisp32 = 0;
  if (outLoadEnd) *outLoadEnd = 0;
  if (!buf || !outDisp32 || !outLoadEnd) return false;
  if (len < 10 || len > EK_NOCULL_SBORDER_WINDOW) return false;

  // The store, decoded backwards: `66 89 modrm`, or `66 REX 89 modrm` for a register above r7; 0x66 is not in the REX
  // range, so the two forms cannot both parse.
  int storeAt = -1;
  unsigned rexB = 0;
  if (buf[len - 3] == 0x66 && buf[len - 2] == 0x89) {
    storeAt = len - 3;
  } else if (buf[len - 4] == 0x66 && buf[len - 3] >= 0x40 &&
             buf[len - 3] <= 0x4F && buf[len - 2] == 0x89) {
    storeAt = len - 4;
    rexB = (unsigned)(buf[len - 3] & 1);
  }
  if (storeAt < 0) return false;

  const unsigned modrm = buf[len - 1];
  if ((modrm & 0xC0) != 0) return false;
  const unsigned rm = modrm & 7;
  if (rm == 4 || rm == 5) return false;
  const unsigned base = rm | (rexB << 3);

  // EXACTLY ONE `mov r64, qword [rip+disp32]` into that base register in the window; counting every qword rip load
  // would refuse the seed site, which has loads into r10 and r8 further back.
  int hits = 0;
  for (int p = 0; p + 7 <= storeAt; p++) {
    if (buf[p] < 0x48 || buf[p] > 0x4F) continue;
    if (buf[p + 1] != 0x8B) continue;
    if ((buf[p + 2] & 0xC7) != 0x05) continue;
    const unsigned reg = (unsigned)((buf[p + 2] >> 3) & 7) |
                         ((((unsigned)buf[p] >> 2) & 1u) << 3);
    if (reg != base) continue;
    if (++hits > 1) { *outDisp32 = 0; *outLoadEnd = 0; return false; }
    *outDisp32 = (int)((unsigned)buf[p + 3] | ((unsigned)buf[p + 4] << 8) |
                       ((unsigned)buf[p + 5] << 16) | ((unsigned)buf[p + 6] << 24));
    *outLoadEnd = p + 7;
  }
  return hits == 1;
}

// MAY THE REDIRECT GO IN YET? THREE ANSWERS, NOT TWO: the locate answers on the sampler thread, so "not published
// yet" arrives first and must WAIT rather than spend the one attempt; a locate that REFUSED clears the latch for
// good.
enum {
  EK_NOCULL_INSTALL_WAIT = 0,
  EK_NOCULL_INSTALL_GO = 1,
  EK_NOCULL_INSTALL_REFUSE = 2
};

inline int ekm_nocull_install_verdict(bool haveBase, bool located, bool available,
                                      unsigned siteRva, unsigned targetRva) {
  if (!available) return EK_NOCULL_INSTALL_REFUSE;
  if (!haveBase || !located) return EK_NOCULL_INSTALL_WAIT;
  if (siteRva == 0 || targetRva == 0 || siteRva == targetRva)
    return EK_NOCULL_INSTALL_REFUSE;
  return EK_NOCULL_INSTALL_GO;
}

// LOCATE AND LOG, NOTHING REDIRECTED. DLL ONLY: nocull.cpp reads the host image and the suite links none of it. Every
// rva is a SEARCH RESULT, zero until the locate has answered. Run from gamestate.cpp's 1 Hz sampler.
void nocull_locate(void);
bool nocull_located(void);
bool nocull_answered(void);

unsigned nocull_scansector_rva(void);
unsigned nocull_scansector_end_rva(void);
unsigned nocull_seed_site_rva(void);
unsigned nocull_cursectnum_rva(void);
unsigned nocull_gotsector_rva(void);
unsigned nocull_gotsector_slot_rva(void);

// sectorborder[], written by the seed call site because IPA-SRA took the parameter away, and its .refptr slot. BOTH
// STAY ZERO WHEN THE SHAPE DID NOT MATCH: a wrong address written every frame is worse than an unchanged feature.
unsigned nocull_sectorborder_rva(void);
unsigned nocull_sectorborder_slot_rva(void);

unsigned nocull_counter_a_rva(void);
unsigned nocull_counter_b_rva(void);

int nocull_gotsector_bytes(void);
int nocull_image_maxwallsb(void);

// "(Unavailable)", session-only: the install is one call-site redirect made once. Written on the sampler thread, read
// on the render thread inside SwapBuffers, so the storage is an interlocked scalar in nocull.cpp, declared here.
bool nocull_available(void);
void nocull_mark_unavailable(void);
bool nocull_row_unavailable(void);

// THE REDIRECT ITSELF, driven from overlay.cpp's per-frame path like camera_install and shot_install: the thread
// writing the four operand bytes must be the thread that runs them. Does nothing until the sampler has published.
void nocull_install(void);

// Did the last MAIN-pass sweep stop at its budget? An extra pass's own stop never writes it, so it cannot keep itself
// on. Interlocked scalar; reset to 0 while the camera or the whole-map row is off.
bool nocull_last_main_stopped(void);

// EXTRA-PASS CUTOFF: the last main pass's reach (its last admitted dist2 when it stopped at budget in Nearest-first
// order), else -1; reset wherever the stop flag is. Interlocked 64-bit.
long long nocull_last_main_reach(void);

// The EK_NOCULL_KIND_* of the pass the last seed entry classified, OTHER until one does; kept current only while
// Frame timings or a burst is on. Render thread only.
int nocull_pass_kind(void);

int nocull_bunches_now(void);

void nocull_remove(void);
