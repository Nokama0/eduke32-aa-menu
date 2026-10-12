#pragma once

// The engine's sector array, for the free camera: out of bounds the skybox goes black because polymer_drawrooms'
// external-view arm (polymer.cpp:1214-1251) returns before polymer_drawsky; a sector whose band holds the camera needs
// a readable band. Found as wall[] is (engine.cpp:9154), validated by sectors tiling wall[] with closing loops.

#include "wallstate.h"

// sectortypev7, buildtypes.h:41-56 (40 bytes). Only the camera's six fields are named.
const int EK_SECTOR_STRIDE   = 40;
const int EK_SECT_WALLPTR    = 0x00;
const int EK_SECT_WALLNUM    = 0x02;
const int EK_SECT_CEILINGZ   = 0x04;
const int EK_SECT_FLOORZ     = 0x08;
const int EK_SECT_CEILINGSTAT = 0x0C;
const int EK_SECT_FLOORSTAT  = 0x0E;
// Per-surface heinum for ekm_slope_z (slopez.h), not the ORed `sloped`. buildtypes.h:46-49 puts picnum then heinum
// after each stat: ceiling 0x12, floor 0x1A (floor stat 0x0E + picnum/heinum/shade/pal/xpan/ypan 6 bytes + 2 + 2).
const int EK_SECT_CEILINGHEINUM = 0x12;
const int EK_SECT_FLOORHEINUM   = 0x1A;
// The sector's lotag for the transport rule: G_MoveTransports tells a teleporter (0) from water (1) and its underside
// (2), game.h:535. 0x22 follows the walls, z, stats, per-surface run and visibility/fogpal (buildtypes.h:53).
const int EK_SECT_LOTAG      = 0x22;
// int8 shades right after each heinum (buildtypes.h:47, :50).
const int EK_SECT_CEILINGSHADE = 0x14;
const int EK_SECT_FLOORSHADE   = 0x1C;

// The only sector[] writes this port makes: the two shade bytes, lowered for one G_DrawRooms by Fullbright and restored
// before it returns. The engine writes both itself (actors.cpp:1423, :7033).
inline bool ekm_sector_write_allowed(int off, int n) {
  return n == 1 && (off == EK_SECT_CEILINGSHADE || off == EK_SECT_FLOORSHADE);
}

// A secret area's lotag: Build writes it and sector.cpp:3293 zeroes it when the player stands in it; premap.cpp:1357
// counts the same value (max_secret_rooms). In a header so the suite can pin "0 is not a secret".
const int EKM_SECRET_LOTAG = 32767;

inline bool ekm_sector_is_secret(int lotag) { return lotag == EKM_SECRET_LOTAG; }

// Bit 1: surface is SLOPED. The authority is the engine's test, not buildtypes.h's key (which says "groudraw"):
// getzsofslopeptr returns the flat pair on `((ceilingstat|floorstat)&2) != 2` (engine.cpp:14292), an OR, so the read
// ORs the two.
const int EK_SECT_STAT_SLOPED = 2;

// Bit 0: PARALLAXED (sky or pit), not a plane. The wall-sprite clamp refuses each side on its own bit
// (engine.cpp:6186), so NOT ORed as `sloped` is.
const int EK_SECT_STAT_PARALLAX = 1;

// build.h:630: MAXSECTORS plus the two spare editor slots (build.h:70). The memory bound, not the map's extent.
const int EK_SECTOR_SLOTS = EK_MAXSECTORS + 2;
const int EK_SECTOR_BYTES = EK_SECTOR_SLOTS * EK_SECTOR_STRIDE;

const int EK_SECTOR_SCAN_DEPTH = 256;
const int EK_SECTOR_MIN_CHAIN  = 16;

const int EK_SECTOR_CHAIN_MIN_WALLS = 2;
inline bool ekm_sector_chain_record_ok(int wallptr, int wallnum) {
  if (wallnum < EK_SECTOR_CHAIN_MIN_WALLS || wallnum > EK_WALL_SLOTS) return false;
  if (wallptr < 0 || wallptr > EK_WALL_SLOTS - wallnum) return false;
  return true;
}

// This sector's walls begin where the last one's ended; sector 0 against a notional (0, 0) predecessor.
inline bool ekm_sector_follows(int prevWallptr, int prevWallnum, int wallptr) {
  return wallptr == prevWallptr + prevWallnum;
}

inline bool ekm_sector_locate_step(int wallptr, int wallnum,
                                    int prevWallptr, int prevWallnum) {
  return ekm_sector_chain_record_ok(wallptr, wallnum) &&
         ekm_sector_follows(prevWallptr, prevWallnum, wallptr);
}

// The sweep's bound: past the loaded map polymer_freeboard NULLs prsectors[]/prwalls[] (polymer.cpp:2487) and
// polymer_drawsector dereferences unchecked (:3043), so the sweep reads prsectors[] (non-NULL = renderer has it). The
// tiling test below can false-accept at a map load (a NULL dereference here); it only bounds the wall span.
struct EkSectorSweep { int stop; int wallFirst; int wallCount; };

// Called with the previous accepted span, (0, 0) before sector 0. Ints, no bool (see wallstate.h's EkWallPair). Bounded
// by EK_MAXWALLS, not EK_WALL_SLOTS: prwalls[] and wallsect[] are [MAXWALLS], and wall[] carries 512 editor spares
// more.
inline EkSectorSweep ekm_sector_sweep_step(int index, int readOk, int prsectorLive,
                                           int wallptr, int wallnum,
                                           int prevWallptr, int prevWallnum,
                                           int maxSectors) {
  EkSectorSweep s = { 1, 0, 0 };
  if (index < 0 || index >= maxSectors) return s;
  if (!readOk) return s;
  if (!prsectorLive) return s;
  if (!ekm_sector_chain_record_ok(wallptr, wallnum)) return s;
  if (wallptr + wallnum > EK_MAXWALLS) return s;
  if (!ekm_sector_follows(prevWallptr, prevWallnum, wallptr)) return s;
  s.stop = 0;
  s.wallFirst = wallptr;
  s.wallCount = wallnum;
  return s;
}

inline bool ekm_point2_ok(int point2, int self, int wallptr, int wallnum) {
  if (point2 == self) return false;
  return point2 >= wallptr && point2 < wallptr + wallnum;
}

// 64-bit sums: Build coordinates span the full int range. `ok` is int for the /O2 reason.
struct EkSectorCentre { long long sx, sy; int n; int x, y, ok; };

inline void ekm_sector_centre_accumulate(EkSectorCentre* c, int wx, int wy) {
  c->sx += wx; c->sy += wy; c->n++;
}

// A sector with no accepted vertices has no centre; it must say so rather than report the origin.
inline bool ekm_sector_centre_finish(EkSectorCentre* c, int accepted) {
  if (accepted <= 0 || c->n != accepted) { c->ok = 0; return false; }
  c->x = (int)(c->sx / accepted);
  c->y = (int)(c->sy / accepted);
  c->ok = 1;
  return true;
}

// Backface culling is OFF when our detour gets the frame back: renderDrawRoomsQ16 ends with
// buildgl_setDisabled(GL_CULL_FACE) (engine.cpp:9563) after polymer_glinit enabled it. Put back exactly as found, not
// set to "right": glbuild.cpp caches enables (buildgl_setEnabled, :114). No entry points, no sweep.
struct EkSweepCull {
  int enable;
  int restore;
  int refuse;
};

inline EkSweepCull ekm_sweep_cull(int entriesOk, int enabledNow) {
  EkSweepCull c = { 0, 0, 0 };
  if (!entriesOk) { c.refuse = 1; return c; }
  if (!enabledNow) { c.enable = 1; c.restore = 1; }
  return c;
}

// domasks is TRUE: FALSE would queue masked floors into cursectormasks (numsectors entries, unbounded appends :3094,
// :3135), but mirror recursion (:2129) shares one bitmap (:711), so a sector can queue twice and overflow the heap.
// TRUE only costs the look of masked floors.

// Accepting prsectors[]'s `.refptr` slot, each rule refusing on ambiguity: the one `mov r64,[base+idx*8]` + `test`/`je`
// in polymer_updatesector's head (polymer.cpp:2624); the slot rip-referenced from drawsector and updatewall, not
// drawwall; room for MAXSECTORS pointers in the image. Detail: docs/re/polymer-sweep.md.
inline bool ekm_prs_shape_ok(int loads, int refptrs) {
  return loads == 1 && refptrs == 1;
}

inline bool ekm_prs_witness_ok(int usRefs, int dsRefs, int uwRefs, int dwRefs) {
  return usRefs >= 1 && dsRefs >= 1 && uwRefs >= 1 && dwRefs == 0;
}

inline bool ekm_prs_span_ok(size_t slotRva, size_t arrayRva, size_t imageSize,
                            int slotInData) {
  if (!slotInData || imageSize == 0) return false;
  if (slotRva + sizeof(void*) > imageSize) return false;
  if (arrayRva == 0 || arrayRva >= imageSize) return false;
  return imageSize - arrayRva >= (size_t)EK_MAXSECTORS * sizeof(void*);
}

// Accepting prwalls[]'s slot: polymer_drawwall dereferences prwalls[wallnum] (:3652, :3664) with no NULL or numwalls
// test, so an index past the loaded map crashes. Shape and witnesses mirror prsectors[]'s, swapped (drawwall positive,
// drawsector negative; polymer_pokesector is inlined so not a term); room for MAXWALLS pointers.
inline bool ekm_prw_shape_ok(int loads, int refptrs) {
  return loads == 1 && refptrs == 1;
}

inline bool ekm_prw_witness_ok(int dsRefs, int uwRefs, int dwRefs,
                               int sameSlotAsPrsectors) {
  if (sameSlotAsPrsectors) return false;
  return dsRefs == 0 && uwRefs >= 1 && dwRefs >= 1;
}

inline bool ekm_prw_span_ok(size_t slotRva, size_t arrayRva, size_t imageSize,
                            int slotInData) {
  if (!slotInData || imageSize == 0) return false;
  if (slotRva + sizeof(void*) > imageSize) return false;
  if (arrayRva == 0 || arrayRva >= imageSize) return false;
  return imageSize - arrayRva >= (size_t)EK_MAXWALLS * sizeof(void*);
}

// A NULL slot inside a live sector's span costs that wall, not the rest of the map.
inline bool ekm_wall_drawable(int prwallLive) { return prwallLive != 0; }

// Which sectors the portal walk already drew: drawing every sector again cost 30-40 fps. drawingstate is a bit per
// sector set when the walk queues one (polymer.cpp:1832, :1991, :2004, :2030) and every queued sector is drawn. A
// bitmap (bitmap_size(N) = (N+7)>>3, compat.h:1173), not a byte array: byte reads skip seven sectors in eight.
const int EK_SECTOR_BITMAP_BYTES = (EK_MAXSECTORS + 7) / 8;

inline int ekm_bitmap_byte(int n) { return n >> 3; }
inline int ekm_bitmap_mask(int n) { return 1 << (n & 7); }
inline bool ekm_bitmap_test(int byteValue, int n) {
  return (byteValue & ekm_bitmap_mask(n)) != 0;
}

// Accepting drawingstate's address, via the call graph (polymer_displayrooms is static): the one callee of the located
// drawrooms calling drawsector and drawwall twice each; a 512-byte `rep stosq` (mov ecx,0x40) into one `lea`, reused by
// bitmap_set; 5..9 uses; fits its containing section (.bss is unbacked by the file). Detail: docs/re/polymer-sweep.md.
inline bool ekm_dstate_owner_ok(int candidates, int dsCalls, int dwCalls) {
  return candidates == 1 && dsCalls >= 2 && dwCalls >= 2;
}

inline bool ekm_dstate_shape_ok(int stosSites, int countSites, int leaSites,
                                int confirmSites) {
  return stosSites == 1 && countSites == 1 && leaSites == 1 &&
         confirmSites == 1;
}

inline bool ekm_dstate_use_ok(int uses) { return uses >= 5 && uses <= 9; }

inline bool ekm_dstate_span_ok(size_t rva, size_t secBegin, size_t secEnd,
                               int inData) {
  if (!inData || rva == 0) return false;
  if (secEnd <= secBegin) return false;
  if (rva < secBegin || rva >= secEnd) return false;
  return secEnd - rva >= (size_t)EK_SECTOR_BITMAP_BYTES;
}

// `sloped` is 1 when either surface carries the slope bit: ceilz/florz are then the plane's z at the first wall.
// Parallax flags are kept apart because the clamp refuses each surface on its own bit. Int flags: MSVC /O2 loses a bool
// member.
struct EkSectorInfo { int ceilz, florz, wallptr, wallnum, sloped, lotag,
                      ceilparallax, florparallax;
                      // Raw per-surface stat and heinum for ekm_slope_z (`sloped` cannot tell ceiling from floor).
                      int ceilStat, florStat, ceilHeinum, florHeinum; };

// Refuses until wallstate_ready(). Driven from gamestate.cpp's scan thread, never the render thread.
bool sectorstate_scan(void);
bool sectorstate_ready(void);

// Where the `sector` pointer lives, as an rva: logged evidence; both counts sit within a hundred bytes of it
// (mapextent.h).
unsigned sectorstate_rva(void);

bool sectorstate_info(int sectnum, EkSectorInfo* out);

// The record's address, bounds-checked: getzsofslopeptr (engine.cpp:14288) takes a pointer, and camera.cpp calls it for
// a sloped sector's true band.
const void* sectorstate_record(int sectnum);
