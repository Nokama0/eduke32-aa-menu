// Tessellates every sector's floor plan once per map and logs it (rules: footprint.h). DLL only: links GLU, reads live
// sector[]/wall[]. GLU setup is Megaton's dollhouse.cpp:88-160; the EDGE_FLAG callback (forces GL_TRIANGLES) and
// gluTessNormal (flat xy data) matter most.

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <GL/GL.h>
#include <GL/GLU.h>

#include "footprint.h"
#include "log.h"
#include "config.h"
#include "mapextent.h"
#include "sectorstate.h"
#include "wallstate.h"
#include "playerstate.h"

// Render clock only: a triangle array that grows by realloc cannot be read from another thread, so the build runs on
// the seed scansector seam, as Megaton's does (dollhouse.cpp:202). Nothing here crosses a clock.

// 6 doubles a triangle (x,y per vertex). Grown by doubling, capped by the budget (dollhouse.cpp:75-85); the full budget
// statically would be 9.6 MB of .bss.
static double* s_tri = 0;
static int s_triCount = 0, s_triCap = 0;

struct FpSectorTris { int off, count; };
static FpSectorTris s_sect[EK_MAXSECTORS];

static double s_vpool[EK_FOOTPRINT_LOOP_VERTS + EK_FOOTPRINT_COMBINE_VERTS][3];
static int s_vpoolUsed = 0;
static short s_point2[EK_FOOTPRINT_LOOP_VERTS];
static short s_loop[EK_FOOTPRINT_LOOP_VERTS];

static double s_triTmp[3][2];
static int s_triTmpN = 0;
static int s_tessFailed = 0;
static int s_capped = 0;

static unsigned s_sig = 0;
static int s_ready = 0;
// Key is (epoch, signature): aa05hub, aa05hub1 and aa05hubfinal share one signature, so the signature alone reused the
// previous map's triangles.
static long s_epoch = 0;
static int s_tessUnavailable = 0;
static int s_oomLogged = 0;

// Cumulative counts over the map's sectors, never a sample: a rate-limited line has misled this project twice.
struct FpCounts {
  int visited, tessellated, failed, noloop, skipped, largest, largestSector;
};

// The decision is ekm_cutaway_tri_room in the header; this is only the realloc.
static int fp_tri_reserve(int need) {
  if (!ekm_cutaway_tri_room(s_triCount, need)) return 0;
  if (s_triCount + need <= s_triCap) return 1;
  int cap = s_triCap > 0 ? s_triCap * 2 : 4096;
  while (cap < s_triCount + need) cap *= 2;
  if (cap > EK_FOOTPRINT_TRI_MAX) cap = EK_FOOTPRINT_TRI_MAX;
  double* p = (double*)realloc(s_tri, (size_t)cap * 6 * sizeof(double));
  if (!p) {
    if (!s_oomLogged) {
      s_oomLogged = 1;
      ek_log("footprint: out of memory growing the triangle cache to %d "
             "triangles -- the cutaway paints no planes this map", cap);
    }
    return 0;
  }
  s_tri = p;
  s_triCap = cap;
  return 1;
}

typedef void (CALLBACK* FpGluCb)();

static void CALLBACK fp_tess_begin(GLenum type) {
  s_triTmpN = 0;
  // The edge-flag callback should have forced GL_TRIANGLES; anything else means the accumulation reads a strip or fan.
  if (type != GL_TRIANGLES) s_tessFailed = 1;
}

static void CALLBACK fp_tess_end(void) { s_triTmpN = 0; }

// Deliberately empty: registering any edge-flag callback makes GLU stop emitting strips and fans. Do not delete it as
// dead code.
static void CALLBACK fp_tess_edge_flag(GLboolean) {}

static void CALLBACK fp_tess_error(GLenum) { s_tessFailed = 1; }

static void CALLBACK fp_tess_vertex(void* data) {
  const double* p = (const double*)data;
  if (!p || s_tessFailed || s_triTmpN < 0 || s_triTmpN > 2) return;
  s_triTmp[s_triTmpN][0] = p[0];
  s_triTmp[s_triTmpN][1] = p[1];
  if (++s_triTmpN < 3) return;
  s_triTmpN = 0;
  if (!fp_tri_reserve(1)) { s_tessFailed = 1; s_capped = 1; return; }
  double* d = s_tri + (size_t)s_triCount * 6;
  for (int k = 0; k < 3; k++) {
    d[k * 2] = s_triTmp[k][0];
    d[k * 2 + 1] = s_triTmp[k][1];
  }
  s_triCount++;
}

// GLU invents a vertex where contours cross. The pool has its own bound; exhausting it fails the SECTOR, never writes
// past it.
static void CALLBACK fp_tess_combine(GLdouble coords[3], void*[4], GLfloat[4],
                                     void** out) {
  if (!out) return;
  if (s_vpoolUsed >= EK_FOOTPRINT_LOOP_VERTS + EK_FOOTPRINT_COMBINE_VERTS) {
    s_tessFailed = 1;
    *out = s_vpool[0];
    return;
  }
  double* v = s_vpool[s_vpoolUsed++];
  v[0] = coords[0];
  v[1] = coords[1];
  v[2] = coords[2];
  *out = v;
}

// Fail closed and carry on: a bad sectorstate, wall range or read costs that sector only.
static void fp_tess_sector(GLUtesselator* t, int sect, FpCounts* c) {
  s_sect[sect].off = 0;
  s_sect[sect].count = 0;

  EkSectorInfo si;
  const bool haveInfo = sectorstate_info(sect, &si);
  if (!haveInfo) { c->skipped++; return; }

  const int wallptr = si.wallptr, wallnum = si.wallnum;
  if (wallnum < 3 || wallnum > EK_FOOTPRINT_LOOP_VERTS) { c->skipped++; return; }
  if (wallptr < 0 || wallptr > EK_WALL_SLOTS - wallnum) { c->skipped++; return; }

  for (int i = 0; i < wallnum; i++) {
    EkWallCorner wc;
    const bool haveCorner = wallstate_corner(wallptr + i, &wc);
    if (!haveCorner) { c->skipped++; return; }
    int p2 = 0;
    const bool haveP2 = wallstate_point2(wallptr + i, &p2);
    if (!haveP2) { c->skipped++; return; }
    // EK_WALL_SLOTS (16896) fits a signed short; a point2 outside the allocation is a corrupt record.
    if (p2 < 0 || p2 >= EK_WALL_SLOTS) { c->skipped++; return; }
    s_point2[i] = (short)p2;
    s_vpool[i][0] = (double)wc.x;
    s_vpool[i][1] = (double)wc.y;
    s_vpool[i][2] = 0.0;
  }
  s_vpoolUsed = wallnum;

  const int start = s_triCount;
  s_tessFailed = 0;
  s_triTmpN = 0;
  int contours = 0;
  gluTessBeginPolygon(t, 0);
  for (int i = 0; i < wallnum; i++) {
    if (!ekm_cutaway_loop_start(s_point2, wallptr, wallnum, i)) continue;
    const int n = ekm_cutaway_loop_walls(s_point2, wallptr, wallnum, i, s_loop,
                                         EK_FOOTPRINT_LOOP_VERTS);
    if (n < 3) continue;
    gluTessBeginContour(t);
    for (int k = 0; k < n; k++) {
      const int local = s_loop[k] - wallptr;
      if (local < 0 || local >= wallnum) continue;
      gluTessVertex(t, s_vpool[local], s_vpool[local]);
    }
    gluTessEndContour(t);
    contours++;
  }
  gluTessEndPolygon(t);

  // A failed or degenerate sector keeps nothing rather than half a footprint (rewind as dollhouse.cpp:151): half would
  // paint a plausible polygon for a shape the map lacks.
  const int made = s_triCount - start;
  if (s_tessFailed) {
    s_triCount = start;
    c->failed++;
    if (g_cfg.debuglog)
      ek_log("footprint: sector %d FAILED tessellation (%d contour(s), %d "
             "triangle(s) discarded)", sect, contours, made);
    return;
  }
  if (contours == 0 || made <= 0) {
    s_triCount = start;
    c->noloop++;
    if (g_cfg.debuglog)
      ek_log("footprint: sector %d has no usable loop (%d wall(s), %d "
             "contour(s))", sect, wallnum, contours);
    return;
  }

  s_sect[sect].off = start;
  s_sect[sect].count = made;
  c->tessellated++;
  if (made > c->largest) { c->largest = made; c->largestSector = sect; }
}

static void fp_build(int numsectors, int numwalls) {
  memset(s_sect, 0, sizeof s_sect);
  s_triCount = 0;
  s_capped = 0;

  FpCounts c;
  memset(&c, 0, sizeof c);
  c.largestSector = -1;

  GLUtesselator* t = gluNewTess();
  if (!t) {
    s_tessUnavailable = 1;
    ek_log("footprint: gluNewTess failed -- no sector footprints this session");
    return;
  }
  gluTessCallback(t, GLU_TESS_BEGIN, (FpGluCb)fp_tess_begin);
  gluTessCallback(t, GLU_TESS_VERTEX, (FpGluCb)fp_tess_vertex);
  gluTessCallback(t, GLU_TESS_END, (FpGluCb)fp_tess_end);
  gluTessCallback(t, GLU_TESS_ERROR, (FpGluCb)fp_tess_error);
  gluTessCallback(t, GLU_TESS_EDGE_FLAG, (FpGluCb)fp_tess_edge_flag);
  gluTessCallback(t, GLU_TESS_COMBINE, (FpGluCb)fp_tess_combine);
  gluTessProperty(t, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_ODD);
  gluTessNormal(t, 0.0, 0.0, 1.0);

  for (int s = 0; s < numsectors && s < EK_MAXSECTORS; s++) {
    if (!ekm_cutaway_tri_room(s_triCount, 1)) { s_capped = 1; break; }
    c.visited++;
    fp_tess_sector(t, s, &c);
  }
  gluDeleteTess(t);

  // The summary is this stage's whole proof, so it prints regardless of the debug row; once per map build.
  ek_log("footprint: map sig 0x%08X (%d sectors, %d walls) -- %d triangles over "
         "%d/%d sectors (visited %d, failed %d, no loop %d, skipped %d); "
         "largest %d triangles at sector %d; budget %d/%d%s",
         s_sig, numsectors, numwalls, s_triCount, c.tessellated, numsectors,
         c.visited, c.failed, c.noloop, c.skipped, c.largest, c.largestSector,
         s_triCount, EK_FOOTPRINT_TRI_MAX, s_capped ? " -- CAPPED" : "");
}

bool footprint_sector_tris(int sect, const double** tris, int* count) {
  if (tris) *tris = 0;
  if (count) *count = 0;
  if (!tris || !count) return false;
  if (!s_ready || !s_tri) return false;
  if (sect < 0 || sect >= EK_MAXSECTORS) return false;
  const FpSectorTris t = s_sect[sect];
  // Bounded against the live count, not the capacity: a rewound sector leaves s_sect entries from an earlier map.
  if (t.count <= 0 || t.off < 0 || t.off > s_triCount - t.count) return false;
  *tris = s_tri + (size_t)t.off * 6;
  *count = t.count;
  return true;
}

void footprint_frame_begin(void) {
  // Gates the work, not just the log: with freecamwholemap off the map is never walked.
  if (g_cfg.freecamwholemap == 0) return;
  if (s_tessUnavailable) return;

  int numsectors = 0, numwalls = 0;
  const bool haveExtent = mapextent_extent(&numsectors, &numwalls);
  if (!haveExtent) return;

  const unsigned sig = ekm_cutaway_map_sig(numsectors, numwalls);
  if (sig == 0u) return;
  // Read on the game thread inside the engine's draw, so it trails a level entry by at most one frame.
  const long epoch = level_epoch();
  if (s_ready && sig == s_sig && !ekm_level_epoch_changed(s_epoch, epoch)) return;

  s_sig = sig;
  s_epoch = epoch;
  s_ready = 1;
  fp_build(numsectors, numwalls);
}
