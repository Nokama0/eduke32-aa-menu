// The cutaway: FIND polymost_domost and polymost_drawpoly in the stripped host, publish their extents and log them. The
// decisions that could be wrong are pure functions here (cutaway.cpp reads the host image; the suite never runs it).
#pragma once

#include "cutpaint.h"

// Each polymost debug string ("polymost_domost(x0:", "polymost_drawpoly(dpxy:", "polymost_drawalls(",
// "polymost_drawrooms()") occurs ONCE with ONE rip-relative lea; requiring count == 1 stops a pooled synthesis from
// silently picking a winner.
inline bool ekm_cutaway_anchor_ok(int occurrences, int refs, unsigned ownerRva) {
  return occurrences == 1 && refs == 1 && ownerRva != 0;
}

// Size window: measured domost 0x2479, drawpoly 0x3BEB, drawalls 0x65DA, drawrooms 0x3D23. A plausibility gate, not a
// law.
const unsigned EK_CUTAWAY_MIN_BYTES = 0x100;
const unsigned EK_CUTAWAY_MAX_BYTES = 0x20000;

inline bool ekm_cutaway_extent_ok(unsigned begin, unsigned end) {
  if (begin == 0 || end <= begin) return false;
  const unsigned bytes = end - begin;
  return bytes >= EK_CUTAWAY_MIN_BYTES && bytes <= EK_CUTAWAY_MAX_BYTES;
}

// vsptyp (polymost.cpp:60-62, VSPMAX 2048): x/cy/fy/tag in 24 bytes, int16 n, p, ctag, ftag at +24, entry 32 bytes,
// array 65536; vsp[VSPMAX-1] is the dummy node whose `n` is the free list head.
const int      EK_CUTAWAY_VSPMAX     = 2048;
const unsigned EK_CUTAWAY_VSP_STRIDE = 32u;
const unsigned EK_CUTAWAY_VSP_BYTES  = (unsigned)EK_CUTAWAY_VSPMAX * EK_CUTAWAY_VSP_STRIDE;
const unsigned EK_CUTAWAY_VSP_LINK   = 24u;

// Is this the span array? domost indexes vsp by `shl reg, 5`, but that look-ahead also hits the function's anchor
// STRING in .rdata, so the array must land in .bss with room for 65536 bytes; `linkSites` (16-bit access at +0x18)
// confirms the layout; baseRegs and loads must both be 1.
inline bool ekm_cutaway_vsp_ok(unsigned rva, unsigned bssEnd, int scaleSites,
                               int linkSites, int baseRegs, int loads) {
  if (rva == 0 || bssEnd == 0 || rva >= bssEnd) return false;
  if (EK_CUTAWAY_VSP_BYTES > bssEnd - rva) return false;
  if (scaleSites <= 0 || linkSites <= 0) return false;
  return baseRegs == 1 && loads == 1;
}

// Do the three readings of vcnt agree? A walk, the head's `p` and the free head must match (polymost.cpp:3742-3754); a
// reading taken after anything painted does not. The floor is two (dummy node 0 plus one strip).
inline bool ekm_cutaway_spans_agree(int walked, int maxIndex, int headPrev,
                                    int freeHead) {
  if (walked < 2 || walked > EK_CUTAWAY_VSPMAX) return false;
  if (maxIndex != walked - 1) return false;
  if (headPrev != walked - 1) return false;
  return freeHead == walked;
}

// May the hook go in yet? The locate answers on the sampler thread, so "not published" arrives first and must WAIT, not
// spend the one attempt.
enum {
  EK_CUTAWAY_INSTALL_WAIT = 0,
  EK_CUTAWAY_INSTALL_GO = 1,
  EK_CUTAWAY_INSTALL_REFUSE = 2
};

inline int ekm_cutaway_install_verdict(bool haveBase, bool located, bool available,
                                       unsigned domostRva, unsigned domostEnd) {
  if (!available) return EK_CUTAWAY_INSTALL_REFUSE;
  if (!haveBase || !located) return EK_CUTAWAY_INSTALL_WAIT;
  // LOCATED with nothing published is a contradiction: refuse.
  if (domostRva == 0 || domostEnd <= domostRva) return EK_CUTAWAY_INSTALL_REFUSE;
  return EK_CUTAWAY_INSTALL_GO;
}

// A reopen frees only indices >= vcnt: the engine's own rule (polymost.cpp:6824, merge sites :4357 and :7018). Node 0
// and vsp[VSPMAX-1] are never freed.
inline bool ekm_cutaway_freeable(int index, int vcnt) {
  if (vcnt < 2 || vcnt > EK_CUTAWAY_VSPMAX) return false;
  if (index <= 0 || index >= EK_CUTAWAY_VSPMAX - 1) return false;
  return index >= vcnt;
}

// Every node of vsp[0..vcnt) must be live: one also on the free list would be in two lists and the engine walks both (a
// hang); `prefixSeen` proves it. The live extreme is VSPMAX-1; the sentinel in the ring would point the free head into
// it.
inline bool ekm_cutaway_reopen_ok(bool closed, int walked, int prefixSeen,
                                  int vcnt, bool sawSentinel) {
  if (!closed) return false;
  if (sawSentinel) return false;
  if (vcnt < 2 || vcnt > EK_CUTAWAY_VSPMAX) return false;
  if (walked < vcnt || walked > EK_CUTAWAY_VSPMAX - 1) return false;
  return prefixSeen == vcnt;
}

// Is this yax_polymostclearzbuffer? The spike WRITES it, so refuse readily: exactly ONE glClear(GL_DEPTH_BUFFER_BIT)
// block in drawrooms and ONE branch reaching it, and the value at rest must be 1 (engine.cpp:430; only yax_drawrooms
// sets 0, :1131), or nothing is written.
inline bool ekm_zclear_ok(int clearSites, int guardSites, unsigned globalRva,
                          unsigned dataEnd, int valueAtRest) {
  if (clearSites != 1 || guardSites != 1) return false;
  if (globalRva == 0 || dataEnd <= globalRva) return false;
  if (dataEnd - globalRva < 4u) return false;
  return valueAtRest == 1;
}

// polymost.h: DAMETH_NOMASK = 0, MASKPROPS = 3, WALL = 32, BACKFACECULL = -1, CLAMPED = 4 (:218-232).
const int EK_DAMETH_NOMASK       = 0;
const int EK_DAMETH_MASKPROPS    = 3;
const int EK_DAMETH_WALL         = 32;
const int EK_DAMETH_BACKFACECULL = -1;
const int EK_DAMETH_CLAMPED      = 4;

// Look-ahead from an r8d load to its drawpoly call; measured max 0x1E, so 0x40 is slack.
const unsigned EK_CUTAWAY_DPM_WINDOW = 0x40u;

// Is this domostpolymethod? Only READ, but a wrong read drops real geometry: call rel32s to drawpoly inside domost (it
// is the third argument), ONE address fed by several `mov r8d,[rip+d]`, and immediate stores of -1 and 0 to it. The
// value at rest is a SET: drawalls holds DAMETH_WALL across its wall run.
inline bool ekm_dpm_ok(int callSites, int feederSites, int feederTargets,
                       int minusOneStores, int zeroStores, unsigned rva,
                       unsigned bssEnd, int valueAtRest) {
  if (callSites < 2 || feederSites < 2 || feederTargets != 1) return false;
  if (minusOneStores < 1 || zeroStores < 1) return false;
  if (rva == 0 || bssEnd <= rva || bssEnd - rva < 4u) return false;
  const bool plausible =
      valueAtRest == EK_DAMETH_BACKFACECULL || valueAtRest == EK_DAMETH_WALL ||
      (valueAtRest >= EK_DAMETH_NOMASK && valueAtRest <= EK_DAMETH_MASKPROPS);
  return plausible;
}

// The view bounds xbl/xbr (polymost.cpp:64, set :6810-6820): domost's head clamps against them, the cutaway paints
// without it. Reached through the .refptr indirection: exactly TWO `mov r64,[rip+d]` + `movss` pairs (rva 0x01ACE26,
// 0x01ACE41; floats 0x13F1D88, 0x13F1D84), four bytes apart; the painter takes min and max.
inline bool ekm_cutaway_xbounds_ok(int pairs, unsigned aRva, unsigned bRva,
                                   unsigned dataEnd) {
  if (pairs != 2) return false;
  if (aRva == 0 || bRva == 0) return false;
  const unsigned hiRva = aRva > bRva ? aRva : bRva;
  const unsigned loRva = aRva > bRva ? bRva : aRva;
  if (hiRva - loRva != 4u) return false;
  if (dataEnd <= hiRva || dataEnd - hiRva < 4u) return false;
  return true;
}

// Search window and look-ahead; measured pairs at 0x18F and 0x1A5, so these are slack.
const unsigned EK_CUTAWAY_XB_WINDOW    = 0x400u;
const unsigned EK_CUTAWAY_XB_LOOKAHEAD = 0x20u;

// DLL only (reads the host image). Every rva is a SEARCH RESULT, zero until answered; run from gamestate.cpp's 1 Hz
// sampler.
void cutaway_locate(void);
bool cutaway_located(void);

unsigned cutaway_domost_rva(void);
unsigned cutaway_domost_end_rva(void);
unsigned cutaway_drawpoly_rva(void);
unsigned cutaway_drawpoly_end_rva(void);

// polymost_internal_nonparallaxed (polymost.cpp:4801) paints every non-parallax floor and ceiling of a bunch; its NINTH
// ARGUMENT is the sector (MAXSECTORS or-ed on the floor side, :4802-4803). Parallax planes take drawalls' other branch
// (:5520), so a call with no plane captured is left alone.
unsigned cutaway_nonpar_rva(void);
unsigned cutaway_nonpar_end_rva(void);

// polymost_drawalls, published only so its calls can be counted.
unsigned cutaway_drawalls_rva(void);
unsigned cutaway_drawalls_end_rva(void);
unsigned cutaway_drawalls_calls(void);

// Masked walls are reached from renderDrawMasks() (game.cpp:1152) whether or not the whole-map sweep is on, so counting
// needs only camera_active().
inline bool ekm_cutaway_maskwall_should_count(bool cameraActive) {
  return cameraActive;
}

// Suppression is separate from counting: the wall is DROPPED only when the row is on.
inline bool ekm_cutaway_maskwall_should_suppress(bool counted, bool suppressRowOn) {
  return counted && suppressRowOn;
}

// The tall-wall refusal also withholds the ENGINE's own domost call whenever the free camera is active, independent of
// the whole-map sweep.
inline bool ekm_cutaway_engine_tall_wall_should_gate(bool cameraActive) {
  return cameraActive;
}

// The degenerate-quad refusal is unconditional (its row is retired): camera_active() alone. Known cost: a black
// triangle at some angles.

// polymost_drawmaskwallinternal makes 0 domost calls and 1 drawpoly call (polymost.cpp:7175-7400), so no other
// instrument here sees one-way walls, fences, grates or window panels; its argument is the wall INDEX.
unsigned cutaway_maskwall_rva(void);
unsigned cutaway_maskwall_end_rva(void);
unsigned cutaway_maskwall_calls(void);

// Whether the entry is patched; the suppress row asks this, not cutaway_available() (the two locates are independent).
bool cutaway_maskwall_hooked(void);

// The three reports are called from nocull.cpp above nocull_should_scan's gate: their gates are camera_active() alone,
// so they must run whenever the thunk could have counted. Render thread.
void cutaway_maskwall_report(void);

void cutaway_engine_tall_report(void);

void cutaway_degenerate_report(void);

// vsp[] and the .refptr slot that names it, as nocull.h publishes for gotsector.
unsigned cutaway_vsp_rva(void);
unsigned cutaway_vsp_slot_rva(void);

// yax_polymostclearzbuffer (engine.cpp:430) and its slot, 0 until accepted; the engine writes it itself (:1131,
// restored :1251), which keeps the write inside this port's rule.
unsigned cutaway_zclear_rva(void);
unsigned cutaway_zclear_slot_rva(void);

// suspend writes 0 and hands back the old value (false: nothing written, no second pass); restore must run on every
// path out, or the flag left at 0 switches the depth clear off for the game's own draws (black screen). Render thread.
bool cutaway_zclear_suspend(int* saved);
void cutaway_zclear_restore(int saved);

// domostpolymethod (polymost.cpp:3849), READ ONLY on the render thread: at DAMETH_BACKFACECULL a drawpoly returns at
// :3184 without painting.
unsigned cutaway_dpm_rva(void);

// Not named left and right: see ekm_cutaway_xbounds_ok.
unsigned cutaway_xbound_a_rva(void);
unsigned cutaway_xbound_b_rva(void);

// Read FRESH each call (rewritten per frame), returned as min and max; FALSE means nothing was read.
bool cutaway_xbounds(float* lo, float* hi);

// The tilt pair gctang/gstang (polymost.cpp:95): the clip's screen row is a rotation by it. drawrooms ASSIGNS it
// (:6923-6933); drawpoly is not the anchor (its arithmetic is inside `#if SOFTROTMAT`, 0 at :82). Identified by
// sincosf(gtang,&sin,&cos) at rva 0x0157473 storing cos to 0x1123B8C and sin to 0x1123B88; four bytes apart.
inline bool ekm_cutaway_tangs_ok(int sites, int pairs, int snapConsts,
                                 unsigned ctangRva, unsigned stangRva,
                                 unsigned dataEnd) {
  if (sites != 1 || pairs != 1 || snapConsts != 1) return false;
  if (ctangRva == 0 || stangRva == 0 || ctangRva == stangRva) return false;
  const unsigned hiRva = ctangRva > stangRva ? ctangRva : stangRva;
  const unsigned loRva = ctangRva > stangRva ? stangRva : ctangRva;
  if (hiRva - loRva != 4u) return false;
  if (dataEnd <= hiRva || dataEnd - hiRva < 4u) return false;
  return true;
}

// Measured: the `lea`s sit 0x14 and 0x0C ahead of the call, the farthest store 0x56 behind; the gap and window are
// slack.
const unsigned EK_CUTAWAY_TANG_GAP    = 0x20u;
const unsigned EK_CUTAWAY_TANG_WINDOW = 0x100u;

unsigned cutaway_tang_c_rva(void);   // gctang
unsigned cutaway_tang_s_rva(void);   // gstang

// Read FRESH every call; the values check themselves (c*c + s*s is 1): a miss is a mid-write frame or a mislocated
// pair.
bool cutaway_tangs(float* ctang, float* stang);

// Called from nocull.cpp's seed thunk: drawrooms runs initmosts (:6986) and the seed scansector (:7078) with nothing
// between touching vsp[], so this reads the span list as initialisation left it. READS and LOGS only.
void cutaway_measure_spans(void);

// The last agreed live-entry count, or 0; written on the GAME thread.
int cutaway_live_spans(void);

// Installed from overlay.cpp's per-frame path (the writing thread runs the patch; the engine is in our SwapBuffers
// hook). With the row off the thunk is the gate and one forwarded call: domost runs dozens of times per wall.
void cutaway_install(void);

void cutaway_remove(void);

// (Unavailable): session-only, an interlocked scalar (locate on the sampler thread, read on the render thread), true at
// start, marked false once.
bool cutaway_available(void);
void cutaway_mark_unavailable(void);

// Bisection row "Cutaway parts": drop either painter for a flight to tell which produces the stretched texture.
const int CUTAWAY_PARTS_BOTH             = 0;  // today's behaviour: nothing suppressed
const int CUTAWAY_PARTS_FLATS_ONLY       = 1;  // stage C's wall calls are dropped
const int CUTAWAY_PARTS_WALLS_ONLY       = 2;  // stage B's flat calls are dropped
// Walls forwarded: flats paint, the wall call goes to the engine's domost, isolating the wall painter.
const int CUTAWAY_PARTS_WALLS_FORWARDED  = 3;

inline int ekm_cutaway_parts_clamped(int parts) {
  if (parts == CUTAWAY_PARTS_FLATS_ONLY || parts == CUTAWAY_PARTS_WALLS_ONLY ||
      parts == CUTAWAY_PARTS_WALLS_FORWARDED)
    return parts;
  return CUTAWAY_PARTS_BOTH;
}

bool cutaway_row_unavailable(void);

// Tall-wall refusal: clipping bounds the geometry but not the wrapped texture's UV gradient; a near-behind endpoint is
// clamped to SCISDIST (1.f), the projected line is enormous and the texture tiles across the screen (offending quads
// reach 1222x-2245x screen height against ~1x). Stored as an index into (8, 16, 32, 64); 0 is OFF and the default.
const int CUTAWAY_TALL_WALL_OFF  = 0;
const int CUTAWAY_TALL_WALL_8    = 1;
const int CUTAWAY_TALL_WALL_16   = 2;
const int CUTAWAY_TALL_WALL_32   = 3;
const int CUTAWAY_TALL_WALL_64   = 4;
const int CUTAWAY_TALL_WALL_MAX  = CUTAWAY_TALL_WALL_64;

inline int ekm_cutaway_tall_wall_clamped(int idx) {
  if (idx < CUTAWAY_TALL_WALL_OFF || idx > CUTAWAY_TALL_WALL_MAX)
    return CUTAWAY_TALL_WALL_OFF;
  return idx;
}

inline int ekm_cutaway_tall_wall_multiple(int idx) {
  switch (ekm_cutaway_tall_wall_clamped(idx)) {
    case CUTAWAY_TALL_WALL_8:  return 8;
    case CUTAWAY_TALL_WALL_16: return 16;
    case CUTAWAY_TALL_WALL_32: return 32;
    case CUTAWAY_TALL_WALL_64: return 64;
    default:                   return 0;
  }
}

// On a TROR map the engine calls domost with g_nodraw set: it records a hole in yax_holecf and paints nothing
// (polymost.cpp:4139-4150); the thunk painted those passes anyway, so they go to the engine's body.
inline bool ekm_cutaway_forward_nodraw(bool located, bool rowOn, int nodrawValue) {
  return located && rowOn && nodrawValue != 0;
}

// TROR support needs BOTH locates; either missing, neither runs.
inline bool ekm_tror_support_located(bool nodrawLocated, bool yaxLocated) {
  return nodrawLocated && yaxLocated;
}

inline bool ekm_tror_row_unavailable(bool nodrawAnswered, bool nodrawLocated,
                                     bool yaxAnswered, bool yaxLocated) {
  return (nodrawAnswered && !nodrawLocated) || (yaxAnswered && !yaxLocated);
}

// The .refptr slot both g_nodraw sites store through must agree and the domost body must read it; 0 for a refusal.
inline unsigned ekm_cutaway_nodraw_slot(unsigned roomsSlot, unsigned allsSlot,
                                        int allsDistinct, int domostReads) {
  if (roomsSlot == 0 || allsDistinct != 1 || allsSlot != roomsSlot) return 0;
  return domostReads >= 1 ? roomsSlot : 0u;
}

// Measured: stores sit 0x85 (drawrooms), 0x24 and 0x51 (drawalls) before their calls; the windows are slack.
const unsigned EK_CUTAWAY_ND_ROOMS_WINDOW = 0x100u;
const unsigned EK_CUTAWAY_ND_ALLS_WINDOW  = 0x60u;
const unsigned EK_CUTAWAY_ND_LOAD_GAP     = 0x40u;

unsigned cutaway_nodraw_rva(void);
// drawrooms' `g_nodraw = 1` store (:6993), 0 unless accepted.
unsigned cutaway_nodraw_store_rva(void);
bool cutaway_answered(void);
int cutaway_nodraw_value(void);

unsigned cutaway_domost_calls(void);
unsigned cutaway_domost_gated_calls(void);
unsigned cutaway_domost_painted_calls(void);
// Forwarded by choice: the g_nodraw forward and both wall-forwards; a lower bound (a refused flat also forwards,
// uncounted).
unsigned cutaway_domost_forwarded_calls(void);
unsigned cutaway_domost_hole_calls(void);
