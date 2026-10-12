#pragma once

// The engine's wall array. Breakable glass is a WALL, not a sprite (A_DamageWall_Internal, sector.cpp:1570): a shot
// reports hit.wall == N, hit.sprite == -1. Found as sprite[] is (engine.cpp:9155); evidence is the map geometry
// (engine.cpp:1296-1297), and zeroed memory dies at index 0.

#include "gamestate.h"

// walltypev7 (buildtypes.h:73-88): the loader reads it straight into wall[] (engine.cpp:11200). Fixed-width, so x64
// shifts nothing.
const int EK_WALL_STRIDE     = 32;
const int EK_WALL_X          = 0x00;
const int EK_WALL_Y          = 0x04;
const int EK_WALL_POINT2     = 0x08;
const int EK_WALL_NEXTWALL   = 0x0A;
const int EK_WALL_NEXTSECTOR = 0x0C;
const int EK_WALL_CSTAT      = 0x0E;
const int EK_WALL_PICNUM     = 0x10;
const int EK_WALL_OVERPICNUM = 0x12;

// Derived from 0x12, bracketed by the 32-byte end (buildtypes.h:84-87): lotag 0x1A, hitag 0x1C, extra 0x1E ends flush,
// so a wrong width would show.
const int EK_WALL_SHADE      = 0x14;
const int EK_WALL_PAL        = 0x15;
const int EK_WALL_YREPEAT    = 0x17;
const int EK_WALL_YPANNING   = 0x19;
const int EK_WALL_LOTAG      = 0x1A;
const int EK_WALL_HITAG      = 0x1C;

// Includes M32_FIXME_WALLS (build.h:70): the slack is part of wall_s (engine.cpp:9155).
const int EK_MAXWALLS   = 16384;
const int EK_WALL_SLACK = 512;
const int EK_WALL_SLOTS = EK_MAXWALLS + EK_WALL_SLACK;
const int EK_WALL_BYTES = EK_WALL_SLOTS * EK_WALL_STRIDE;

// The only wall[] write: shade, lowered for one G_DrawRooms by Fullbright and restored. The engine writes it too
// (actors.cpp:1416).
inline bool ekm_wall_write_allowed(int off, int n) {
  return n == 1 && off == EK_WALL_SHADE;
}

// Masking wall (glass, vent fans); A_DamageWall_Internal's breakable branch is gated on it.
const int EK_WALL_CSTAT_MASKED = 16;

// Pure scan rules. The minimum clauses are load-bearing: zero of zero would pass the first candidate.

const int EK_WALL_SCAN_DEPTH   = 256;
const int EK_WALL_MIN_CHECKED  = 64;
// Eight confirmed pairs is past chance for six exact int32s each.
const int EK_WALL_MIN_PAIRS    = 8;

// point2 kills zeros: it is never the wall itself.
inline bool ekm_wall_record_plausible(int index, int point2, int nextwall,
                                      int nextsector, int picnum, int overpicnum) {
  if (point2 < 0 || point2 >= EK_WALL_SLOTS) return false;
  if (point2 == index) return false;
  if (nextwall < -1 || nextwall >= EK_WALL_SLOTS) return false;
  if (nextsector < -1 || nextsector >= EK_MAXSECTORS) return false;
  if (picnum < 0 || picnum >= EK_MAXTILES) return false;
  // overpicnum -1 is the editor's "none" and loadboard keeps it (engine.cpp:11215).
  if (overpicnum < -1 || overpicnum >= EK_MAXTILES) return false;
  return true;
}

// Ints only: MSVC at /O2 loses a bool in a struct returned by an inline function.
struct EkWallPair {
  int index, nextwall, nextOfNext;
  int x1, y1, x2, y2;
  int kx, ky, kp2x, kp2y;
};

// Neighbour runs the other way along the segment and names this wall back (engine.cpp:1373-1376).
inline bool ekm_wall_pair_consistent(const EkWallPair& p) {
  if (p.nextOfNext != p.index) return false;
  if (p.kx != p.x2 || p.ky != p.y2) return false;
  if (p.kp2x != p.x1 || p.kp2y != p.y1) return false;
  return true;
}

inline bool ekm_wall_scan_accept(int checked, int plausible, int pairs) {
  if (checked < EK_WALL_MIN_CHECKED) return false;
  if (plausible != checked) return false;
  if (pairs < EK_WALL_MIN_PAIRS) return false;
  return true;
}

struct EkWallFace { int picnum, overpicnum, cstat, nextsector; };

// Scan thread only, never the render thread.
bool wallstate_scan(void);
bool wallstate_ready(void);

// Evidence only.
unsigned wallstate_rva(void);

// False means "nothing known"; callers must behave as before.
bool wallstate_face(int wallnum, EkWallFace* out);

struct EkWallCorner { int x, y, nextsector; };
bool wallstate_corner(int wallnum, EkWallCorner* out);

// sectorstate.cpp walks it to prove a sector's wall loop closes.
bool wallstate_point2(int wallnum, int* out);

// Tags P_ActivateSwitch's wall branch reads (sector.cpp:1294-1301); split out of EkWallFace.
struct EkWallTags { int pal, lotag, hitag; };
bool wallstate_tags(int wallnum, EkWallTags* out);

// Inputs to calc_ypanning for a masked wall (polymost.cpp:7268-7269); same record, no new locate.
struct EkWallPanel { int yrepeat, ypanning; };
bool wallstate_panel(int wallnum, EkWallPanel* out);

// Bounds-checked address of one wall's record, or NULL; Fullbright validates the span itself.
const void* wallstate_record(int wallnum);
