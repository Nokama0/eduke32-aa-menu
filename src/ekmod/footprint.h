#pragma once

// The cutaway's footprint cache: every sector's floor plan as triangles, built once per map from sector[] and wall[]
// through the validated readers. Nothing here paints, hooks or writes engine memory.

#include "wallstate.h"

// Where a loop starts, as polymost_scansector walks them: at wallptr, or wherever wall[w-1].point2 does not point at
// w. point2[] holds ABSOLUTE wall numbers, hence the wallptr parameter.
inline int ekm_cutaway_loop_start(const short* point2, int wallptr, int wallnum,
                                  int i) {
  if (!point2 || i < 0 || i >= wallnum) return 0;
  if (i == 0) return 1;
  return point2[i - 1] != wallptr + i ? 1 : 0;
}

// The loop's walls in point2 order, or 0: a chain that leaves the sector or never closes returns nothing, because a
// partial footprint painted later is worse than none.
inline int ekm_cutaway_loop_walls(const short* point2, int wallptr, int wallnum,
                                  int i, short* out, int outMax) {
  if (!point2 || !out || outMax <= 0 || i < 0 || i >= wallnum) return 0;
  int w = wallptr + i, count = 0;
  for (;;) {
    const int local = w - wallptr;
    if (local < 0 || local >= wallnum) return 0;
    if (count >= outMax || count > wallnum) return 0;
    out[count++] = (short)w;
    w = point2[local];
    if (w == wallptr + i) break;
  }
  return count >= 3 ? count : 0;
}

// Whole-map triangle budget: Megaton's dollhouse.cpp number (200,000). AA's map sizes are not measured against it;
// the summary log prints the actual total.
const int EK_FOOTPRINT_TRI_MAX = 200000;

// Vertex slots for one sector's loops plus the slots GLU's COMBINE callback takes at a self-intersection (Megaton's
// numbers). A sector with more walls is skipped rather than half-read.
const int EK_FOOTPRINT_LOOP_VERTS    = 2048;
const int EK_FOOTPRINT_COMBINE_VERTS = 256;

// Is there room for `need` more triangles? The budget is all that stands between a corrupt wall chain and an
// unbounded allocation on the sampler thread.
inline bool ekm_cutaway_tri_room(int triCount, int need) {
  if (triCount < 0 || need <= 0) return false;
  if (triCount > EK_FOOTPRINT_TRI_MAX) return false;
  return need <= EK_FOOTPRINT_TRI_MAX - triCount;
}

// The map's signature, which keys the cache: (numsectors, numwalls) packed, not hashed. 0 means no map: refused
// rather than cached.
inline unsigned ekm_cutaway_map_sig(int numsectors, int numwalls) {
  if (numsectors <= 0 || numsectors > EK_MAXSECTORS) return 0u;
  if (numwalls <= 0 || numwalls > EK_WALL_SLOTS) return 0u;
  return ((unsigned)numsectors << 16) | (unsigned)numwalls;
}

// One clock, the render clock (the 1 Hz sampler would charge the all-sectors walk to a frame otherwise).
void footprint_frame_begin(void);

// One sector's triangles, 6 doubles each. False means no cached footprint, which means "forward to the engine", never
// "paint nothing".
bool footprint_sector_tris(int sect, const double** tris, int* count);
