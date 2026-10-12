#pragma once

// Secret volume: geometry and caps for outlining an undiscovered secret room.

const int EK_SECVOL_WALLS_PER_ROOM = 64, EK_SECVOL_WALLS_MAX = 512, EK_SECVOL_ROOMS_MAX = 32;

// `exact` is 1 only when both ends resolved; freecam.h refuses on the same flag.
struct EkSecretWall { int x0, y0, z0top, z0bot, x1, y1, z1top, z1bot, exact; };

inline int ekm_secvol_room_fits(int wallnum, int staged, int rooms) {
  if (wallnum <= 0) return 0;
  if (wallnum > EK_SECVOL_WALLS_PER_ROOM) return 0;
  if (staged + wallnum > EK_SECVOL_WALLS_MAX) return 0;
  if (rooms >= EK_SECVOL_ROOMS_MAX) return 0;
  return 1;
}

// Build z grows downward; each end's inversion is refused alone (both at once passes a bowtie).
inline int ekm_secvol_quad_corners(const EkSecretWall& w, double* x4, double* y4, int* z4) {
  if (!x4 || !y4 || !z4) return 0;
  if (w.z0top > w.z0bot) return 0;
  if (w.z1top > w.z1bot) return 0;
  if (w.x0 == w.x1 && w.y0 == w.y1) return 0;
  x4[0] = w.x0; y4[0] = w.y0; z4[0] = w.z0top;
  x4[1] = w.x0; y4[1] = w.y0; z4[1] = w.z0bot;
  x4[2] = w.x1; y4[2] = w.y1; z4[2] = w.z1bot;
  x4[3] = w.x1; y4[3] = w.y1; z4[3] = w.z1top;
  return 4;
}

// 1 Hz sampler only: changes on map load and when a found secret's lotag is zeroed (sector.cpp:3293).
bool secretvol_scan(void);

bool secretvol_ready(void);

// Acquire once: the cache is double-buffered. Map-change window: docs/re/secretvol.md.
int secretvol_generation(void);

int secretvol_rooms(int gen);
int secretvol_room(int gen, int i, int* sectnum, int* cx, int* cy,
                   int* first, int* n);

const EkSecretWall* secretvol_wall(int gen, int i);

int secretvol_skipped(int gen);
int secretvol_unreadable(int gen);
int secretvol_inexact(int gen);
