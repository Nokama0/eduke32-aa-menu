// Secret-volume scan, on gamestate.cpp's 1 Hz sampler: geometry is fixed while a map is loaded and a found secret
// just stops answering ekm_sector_is_secret.

#include <windows.h>
#include "secretvol.h"
#include "sectorstate.h"
#include "wallstate.h"
#include "mapextent.h"
#include "gamestate.h"
#include "freecam.h"
#include "camera.h"

struct EkSecretRoom { int sectnum, cx, cy, first, n; };

// Two generations: secretvol_wall returns a POINTER into the cache, so the writer fills the idle buffer and flips
// the index; a found secret repacks the array, so `first` from one generation would index another room's wall.
static EkSecretWall s_walls[2][EK_SECVOL_WALLS_MAX];
static EkSecretRoom s_rooms[2][EK_SECVOL_ROOMS_MAX];
static int s_wallCount[2];
static int s_roomCount[2];
static int s_skipCount[2];
static int s_unreadCount[2];
static int s_inexactCount[2];

// -1 until the first walk, so "not scanned" differs from "empty".
static volatile LONG s_live = -1;

static int secvol_gen_ok(int gen) { return gen == 0 || gen == 1; }

static bool secvol_stage_room(int sectnum, const EkSectorInfo& si,
                              EkSecretWall* walls, int base,
                              EkSecretRoom* room, int* inexact) {
  EkSectorCentre c = { 0, 0, 0, 0, 0, 0 };
  int accepted = 0;
  int roomInexact = 0;

  for (int k = 0; k < si.wallnum; k++) {
    const int wi = si.wallptr + k;

    int p2 = 0;
    if (!wallstate_point2(wi, &p2)) return false;
    if (!ekm_point2_ok(p2, wi, si.wallptr, si.wallnum)) return false;

    EkWallCorner a = { 0, 0, 0 }, b = { 0, 0, 0 };
    if (!wallstate_corner(wi, &a)) return false;
    if (!wallstate_corner(p2, &b)) return false;

    // A band at each end: in a sloped sector ceiling and floor differ along the wall.
    FreecamBand ba = { 0, 0, 0, 0 }, bb = { 0, 0, 0, 0 };
    EkSectorInfo za = { 0, 0, 0, 0, 0, 0, 0, 0 }, zb = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!cam_band_fill(sectnum, a.x, a.y, &ba, &za)) return false;
    if (!cam_band_fill(sectnum, b.x, b.y, &bb, &zb)) return false;

    EkSecretWall* const w = &walls[base + k];
    w->x0 = a.x; w->y0 = a.y; w->z0top = ba.ceilz; w->z0bot = ba.florz;
    w->x1 = b.x; w->y1 = b.y; w->z1top = bb.ceilz; w->z1bot = bb.florz;

    // Both ends or neither: cam_band_fill returns TRUE with exact 0 on the flat fallback; a flat panel beats a missing
    // secret.
    w->exact = (ba.exact && bb.exact) ? 1 : 0;
    if (!w->exact) roomInexact++;

    ekm_sector_centre_accumulate(&c, a.x, a.y);
    accepted++;
  }

  if (!ekm_sector_centre_finish(&c, accepted)) return false;

  room->sectnum = sectnum;
  room->cx = c.x;
  room->cy = c.y;
  room->first = base;
  room->n = accepted;
  *inexact += roomInexact;
  return true;
}

bool secretvol_scan(void) {
  if (!sectorstate_ready() || !wallstate_ready()) return false;

  int ns = mapextent_numsectors();
  if (ns < 0) ns = 0;
  if (ns > EK_MAXSECTORS) ns = EK_MAXSECTORS;

  const LONG live = s_live;
  const int next = (live == 0) ? 1 : 0;
  EkSecretWall* const walls = s_walls[next];
  EkSecretRoom* const rooms = s_rooms[next];

  int staged = 0, nrooms = 0, skipped = 0, unreadable = 0, inexact = 0;
  for (int sec = 0; sec < ns; sec++) {
    EkSectorInfo si = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!sectorstate_info(sec, &si)) continue;
    if (!ekm_sector_is_secret(si.lotag)) continue;

    // Asked once per room, before staging: per wall it would leave partial outlines.
    if (!ekm_secvol_room_fits(si.wallnum, staged, nrooms)) { skipped++; continue; }

    if (!secvol_stage_room(sec, si, walls, staged, &rooms[nrooms], &inexact)) {
      unreadable++;
      continue;
    }
    staged += rooms[nrooms].n;
    nrooms++;
  }

  s_wallCount[next] = staged;
  s_roomCount[next] = nrooms;
  s_skipCount[next] = skipped;
  s_unreadCount[next] = unreadable;
  s_inexactCount[next] = inexact;
  InterlockedExchange(&s_live, next);
  return true;
}

int secretvol_generation(void) { return (int)s_live; }

bool secretvol_ready(void) { return secretvol_generation() >= 0; }

int secretvol_rooms(int gen) {
  return secvol_gen_ok(gen) ? s_roomCount[gen] : 0;
}

int secretvol_skipped(int gen) {
  return secvol_gen_ok(gen) ? s_skipCount[gen] : 0;
}

int secretvol_unreadable(int gen) {
  return secvol_gen_ok(gen) ? s_unreadCount[gen] : 0;
}

int secretvol_inexact(int gen) {
  return secvol_gen_ok(gen) ? s_inexactCount[gen] : 0;
}

int secretvol_room(int gen, int i, int* sectnum, int* cx, int* cy,
                   int* first, int* n) {
  if (!secvol_gen_ok(gen)) return 0;
  if (i < 0 || i >= s_roomCount[gen]) return 0;
  const EkSecretRoom& r = s_rooms[gen][i];
  if (sectnum) *sectnum = r.sectnum;
  if (cx) *cx = r.cx;
  if (cy) *cy = r.cy;
  if (first) *first = r.first;
  if (n) *n = r.n;
  return 1;
}

const EkSecretWall* secretvol_wall(int gen, int i) {
  if (!secvol_gen_ok(gen)) return 0;
  if (i < 0 || i >= s_wallCount[gen]) return 0;
  return &s_walls[gen][i];
}
