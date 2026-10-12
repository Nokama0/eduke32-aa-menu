#pragma once
#include "projmath.h"

// World-space quads drawn translucent inside the engine's frame; producers push, one flush per frame draws and empties.
// The list rules are pure and the caller owns the storage, since worldquad.cpp does not link into test_host.

// Corners are DOUBLE: rounding a wall-sprite span to the integer lattice tilts the quad out of the sprite plane (small
// in position, large in depth). The depth bias only breaks a TIE with coplanar real walls, which get no offset; too
// large turns a room solid (derivation: docs/re/polymer-projection.md).
const float EK_WORLD_OFFSET_FACTOR = -1.0f;
const float EK_WORLD_OFFSET_UNITS = -2.0f;

struct EkWorldQuad {
  double x[4], y[4];
  int z[4];
  // Byte order named: canvas.h:2, 0xAARRGGBB.
  unsigned argb;
};

// A lock costs six faces; a secret room one quad per wall, hard-capped at EK_SECVOL_WALLS_MAX = 512. 642 = 512 + an
// allowance of 126 for locks. The tint asks ekm_world_quad_room for all six first, so a box stays whole;
// worldquad_refused is the instrument: nonzero means 642 is too small.
const int EK_WORLD_QUAD_MAX = 642;

// Never negative, so a producer testing `room > 0` cannot be fooled into writing past the end.
inline int ekm_world_quad_room(int count, int max) {
  if (max <= 0) return 0;
  if (count <= 0) return max;
  if (count >= max) return 0;
  return max - count;
}

// Returns the NEW count, unchanged when full: overwriting the last slot would flicker between locks instead of plainly
// stopping at the cap.
inline int ekm_world_quad_append(EkWorldQuad* list, int count, int max,
                                 const EkWorldQuad& q) {
  if (!list || max <= 0 || count < 0 || count >= max) return count;
  list[count] = q;
  return count + 1;
}

// A kind rides beside each quad so a diagnostic can take secret volumes out of the depth test.
enum { EK_WQ_KIND_OTHER = 0, EK_WQ_KIND_SECRET = 1 };

inline bool ekm_world_quad_tests_depth(int kind, int secvolNoDepth) {
  return !(secvolNoDepth != 0 && kind == EK_WQ_KIND_SECRET);
}

inline int ekm_world_quad_in_batch(int kind, int secvolNoDepth, int batch) {
  const bool tests = ekm_world_quad_tests_depth(kind, secvolNoDepth);
  if (batch == 0) return tests ? 1 : 0;
  if (batch == 1) return tests ? 0 : 1;
  return 0;
}

// A quad on another TROR storey is drawn right after that storey masks, before a nearer storey hole plane stamps over
// it; the final flush draws what no level flush took.
inline bool ekm_wq_bit_test(const unsigned char* bits, int bytes, int i) {
  if (!bits || i < 0 || (i >> 3) >= bytes) return false;
  return (bits[i >> 3] & (1u << (i & 7))) != 0;
}
inline void ekm_wq_bit_set(unsigned char* bits, int bytes, int i) {
  if (!bits || i < 0 || (i >> 3) >= bytes) return;
  bits[i >> 3] = (unsigned char)(bits[i >> 3] | (1u << (i & 7)));
}

inline bool ekm_world_quad_at_level(int sector, int drawn, int kind,
                                    int secvolNoDepth,
                                    const unsigned char* pending, int bytes) {
  if (sector < 0 || drawn != 0) return false;
  if (!ekm_world_quad_tests_depth(kind, secvolNoDepth)) return false;
  return ekm_wq_bit_test(pending, bytes, sector);
}
inline bool ekm_world_quad_at_final(int drawn) { return drawn == 0; }

// `seen` is the caller's cleared scratch bitmap.
inline int ekm_world_quad_candidates(const short* sectors,
                                     const unsigned char* drawn, int n,
                                     unsigned char* seen, int seenBytes,
                                     short* out, int max) {
  if (!sectors || !drawn || !seen || !out || n <= 0 || max <= 0) return 0;
  int k = 0;
  for (int i = 0; i < n && k < max; i++) {
    const int s = sectors[i];
    if (s < 0 || drawn[i] != 0 || (s >> 3) >= seenBytes) continue;
    if (ekm_wq_bit_test(seen, seenBytes, s)) continue;
    ekm_wq_bit_set(seen, seenBytes, s);
    out[k++] = (short)s;
  }
  return k;
}

// Polymost keeps its rotation in the shader u_rotMatrix (identity modelview, polymost.cpp:1465-1466), so a
// fixed-function quad arrives pre-rotated: P * (R * V).
struct EkPmVertex { double x, y, z; };

// V: polymost_drawpoly vertex (:3625), dd = gxyaspect*gyxscale/gdepth (:5436-5438).
inline EkPmVertex ekm_pm_vertex(const EkBuildProj& g, double dx, double dy,
                                double dz, double k) {
  EkPmVertex v;
  v.x = k * ekm_build_glat(g, dx, dy);
  v.y = g.halfx > 0.0 ? -k * dz * g.yxscale / g.halfx : 0.0;
  v.z = k * ekm_build_gdepth_xy(g, dx, dy);
  return v;
}

// R: u_rotMatrix per polymost_updaterotmat (:3070-3077), tilt at identity. Cannot be read back: draws reset it.
inline EkPmVertex ekm_pm_rotate(const EkBuildProj& g, EkPmVertex v) {
  const double gvr = g.gvr > 0.0 ? g.gvr : 1.0;
  EkPmVertex o;
  o.x = v.x;
  o.y = g.chang * v.y + (g.shang / gvr) * v.z;
  o.z = -g.shang * gvr * v.y + g.chang * v.z;
  return o;
}

// near from the LIVE m[3][2] = -2*f*n/(f-n), f = 8 (:1452-1463), not m22 (~3 digits in float). 0 is a refusal.
const double EKM_PM_FARCLIP = 8.0;
inline double ekm_pm_near_from_m14(double m14) {
  if (!(m14 < 0.0)) return 0.0;
  const double n = -m14 * EKM_PM_FARCLIP / (2.0 * EKM_PM_FARCLIP - m14);
  return (n > 0.0 && n < EKM_PM_FARCLIP) ? n : 0.0;
}

inline double ekm_pm_k_from_near(double n) { return n > 0.0 ? n / 8.0 : 0.0; }

// True if the live projection is resizeglcheck is (w row 0,0,1,0); Polymer has -1 (glbuild.cpp:430-434).
inline bool ekm_pm_projection_is_polymost(const float* pr) {
  return pr && pr[3] == 0.0f && pr[7] == 0.0f && pr[11] == 1.0f && pr[15] == 0.0f;
}

// Megaton's proportional bias (WORLD_DEPTH_BIAS): each vertex moves 1/512 toward the eye along its ray; covers
// fixed-function vs shader rounding.
const double EK_WORLD_PM_BIAS = 1.0 / 512.0;
inline EkPmVertex ekm_pm_bias(EkPmVertex v) {
  const double s = 1.0 - EK_WORLD_PM_BIAS;
  v.x *= s; v.y *= s; v.z *= s;
  return v;
}

inline EkPmVertex ekm_pm_world_vertex(const EkBuildProj& g, double dx, double dy,
                                      double dz, double k) {
  return ekm_pm_bias(ekm_pm_rotate(g, ekm_pm_vertex(g, dx, dy, dz, k)));
}

// Producers collect inside the SwapBuffers hook (esp.cpp:68), at the END of the frame G_DrawRooms drew, so a flush
// there draws the previous frame push. Reset at both ends: the producer end is load-bearing, since G_DrawRooms does not
// run in every mode and a flush-only reset latches duplicates until the tint reads solid.
void worldquad_reset(void);

// Set by whoever owns the patch. Without a draw point nothing drains the list, so a push is dropped rather than blaming
// the cap. Default false.
void worldquad_set_drawpoint(bool ok);
int worldquad_no_drawpoint(void);

void worldquad_push(const EkWorldQuad& q);
void worldquad_push_kind(const EkWorldQuad& q, int kind);
void worldquad_push_at(const EkWorldQuad& q, int kind, int sector);
int worldquad_count(void);
// Most quads ONE collect pass pushed past the cap this session; outlives the reset. Nonzero means EK_WORLD_QUAD_MAX is
// too small.
int worldquad_refused(void);
void worldquad_flush(void);

// Storey level flush (storeyflush.cpp), render thread only. Distinct sectors of the undrawn quads.
int worldquad_candidate_sectors(short* out, int max);
// Draws the undrawn quads whose sector is set in `pending` and marks them drawn; 0 when GL refused (they stay final).
int worldquad_flush_level(const unsigned char* pending, int bytes);
// Session totals, Q + F == P: drawn at level time, and per final flush the undrawn handed in and the list's count.
unsigned worldquad_level_drawn(void);
unsigned worldquad_final_undrawn(void);
unsigned worldquad_final_total(void);
