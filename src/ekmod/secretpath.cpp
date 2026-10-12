// Secret path SCAN (secretpath.h owns caps and pure rules). Runs on the slow sampler clock: it walks every wall loop
// and sprite, then BFS over up to 8192 edges. The answer changes on map load and when a found secret's lotag is zeroed
// (sector.cpp:3293); secretpath_refresh re-solves the staged graph every EK_SPATH_SUBTICK_MS. Read-only.
#include <windows.h>
#include "secretpath.h"
#include "gamestate.h"
#include "sectorstate.h"
#include "wallstate.h"
#include "mapextent.h"
#include "playerstate.h"
#include "esp_breakables.h"
#include "freecam.h"

// Edge cap is World Tour's (secretpathwt.cpp:36). A counted refusal, never a silent truncation: past it the search runs
// on the staged edges and `overflow` is set.
const int EK_SPATH_EDGE_MAX = 8192;

// A blocking two-sided wall (cstat bit 0: force field, railing) is not a way through.
const unsigned EK_SPATH_CSTAT_BLOCK = 1u;

static EkSpathSector s_sec[EK_SPATH_MAX_SECTORS];
static unsigned char s_isDoor[EK_SPATH_MAX_SECTORS];
static unsigned char s_isGoal[EK_SPATH_MAX_SECTORS];
static EkSpathEdge   s_edges[EK_SPATH_EDGE_MAX];
static EkSpathEdge   s_route[EK_SPATH_MAX_HOPS];

static int      s_graphStaged  = 0;
static unsigned s_graphSig     = 0;
static int      s_graphEdges   = 0;
static int      s_graphSectors = 0;
static int      s_graphOverflow = 0;

// Two generations: secretpath_point returns a pointer into the cache.
static EkSpathPoint s_points[2][EK_SPATH_MAX_HOPS];
static int s_count[2], s_hops[2], s_teleports[2];
static int s_fromSect[2], s_toSect[2];
static int s_edgeCount[2], s_sectorCount[2], s_overflow[2], s_scans[2];
static int s_refreshes[2];

static int s_totalScans = 0;
static int s_totalRefreshes = 0;

// -1 until the first walk, so "not scanned" differs from "found nothing".
static volatile LONG s_live = -1;

static int spath_gen_ok(int gen) { return gen == 0 || gen == 1; }

static int spath_stage_walls(int nsec, int at, int* overflow) {
  for (int s = 0; s < nsec; s++) {
    EkSectorInfo si = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!sectorstate_info(s, &si)) continue;

    for (int k = 0; k < si.wallnum; k++) {
      const int wi = si.wallptr + k;

      EkWallFace f = { 0, 0, 0, 0 };
      if (!wallstate_face(wi, &f)) break;
      if (f.nextsector < 0 || f.nextsector >= nsec) continue;
      if (((unsigned)f.cstat & EK_SPATH_CSTAT_BLOCK) != 0) continue;

      if (at >= EK_SPATH_EDGE_MAX) { *overflow = 1; return at; }

      int p2 = 0;
      if (!wallstate_point2(wi, &p2)) break;
      if (!ekm_point2_ok(p2, wi, si.wallptr, si.wallnum)) continue;

      EkWallCorner a = { 0, 0, 0 };
      EkWallCorner b = { 0, 0, 0 };
      if (!wallstate_corner(wi, &a)) break;
      if (!wallstate_corner(p2, &b)) break;

      EkSpathEdge& e = s_edges[at++];
      e.from = s;
      e.to   = f.nextsector;
      e.kind = EK_SPATH_EDGE_WALK;
      e.x = ekm_wall_mid(a.x, b.x);
      e.y = ekm_wall_mid(a.y, b.y);
      e.z = si.florz;
    }
  }
  return at;
}

// Stages paired sector effector 7 as teleport edges (statnum 9, paired by owner, sector lotag in {0,1,2} as World
// Tour). Known narrowing: an airborne effector (freecam.h tempData4 == 0) fires on any lotag and is not staged.
static int spath_stage_transports(int nsec, int at, int* overflow) {
  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* const r = gamestate_sprite(i);
    if (!r) break;

    if ((int)*(const short*)(r + EK_SPR_STATNUM) != FREECAM_TRANSPORT_STAT) continue;

    const int owner = (int)*(const short*)(r + EK_SPR_OWNER);
    if (owner < 0 || owner >= EK_MAXSPRITES || owner == i) continue;
    const unsigned char* const o = gamestate_sprite(owner);
    if (!o) continue;

    const int from = (int)*(const short*)(r + EK_SPR_SECTNUM);
    const int to   = (int)*(const short*)(o + EK_SPR_SECTNUM);
    if (from < 0 || from >= nsec) continue;
    if (to < 0 || to >= nsec) continue;
    if (from == to) continue;

    EkSectorInfo si = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!sectorstate_info(from, &si)) continue;
    if (si.lotag != 0 && si.lotag != 1 && si.lotag != 2) continue;

    if (at >= EK_SPATH_EDGE_MAX) { *overflow = 1; return at; }

    EkSpathEdge& e = s_edges[at++];
    e.from = from;
    e.to   = to;
    e.kind = EK_SPATH_EDGE_TELEPORT;
    e.x = *(const int*)(r + EK_SPR_X);
    e.y = *(const int*)(r + EK_SPR_Y);
    e.z = *(const int*)(r + EK_SPR_Z);
  }
  return at;
}

static void spath_publish(int next, int count, int hops, int teleports,
                          int fromSect, int toSect, int edgeCount,
                          int sectorCount, int overflow) {
  s_hops[next]        = hops;
  s_teleports[next]   = teleports;
  s_fromSect[next]    = fromSect;
  s_toSect[next]      = toSect;
  s_edgeCount[next]   = edgeCount;
  s_sectorCount[next] = sectorCount;
  s_overflow[next]    = overflow;
  s_scans[next]       = s_totalScans;
  s_refreshes[next]   = s_totalRefreshes;
  // Count last: the render clock reads s_points under it.
  s_count[next] = count;
  InterlockedExchange(&s_live, next);
}

static void spath_solve(int next, int nsec, int start, int edgeCount, int overflow) {
  int goals = 0;
  for (int s = 0; s < nsec; s++) {
    // An unreadable sector is a zero-height room: impassable, neither door nor goal.
    EkSectorInfo si = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const bool ok = sectorstate_info(s, &si);
    const int ceilz = ok ? si.ceilz : 0;
    const int florz = ok ? si.florz : 0;
    const int lotag = ok ? si.lotag : 0;

    s_sec[s].floorz   = florz;
    s_sec[s].ceilingz = ceilz;
    s_sec[s].lotag    = lotag;
    s_isDoor[s] = (unsigned char)ekm_spath_lotag_is_door(lotag);
    s_isGoal[s] = ekm_sector_is_secret(lotag) ? (unsigned char)1 : (unsigned char)0;
    if (s_isGoal[s]) goals++;
  }

  // No goal, no search: the no-goal case expands the whole graph and is the steady state on a finished map.
  const int hops = goals ? ekm_spath_search(s_edges, edgeCount, s_sec, nsec,
                                            s_isDoor, s_isGoal, start,
                                            s_route, EK_SPATH_MAX_HOPS)
                         : 0;
  int count = (hops < EK_SPATH_MAX_HOPS) ? hops : EK_SPATH_MAX_HOPS;
  // s_route is never cleared; stop at the first edge that does not chain rather than publish a stale point.
  for (int k = 1; k < count; k++)
    if (s_route[k - 1].to != s_route[k].from) { count = k; break; }

  if (hops <= 0) {
    spath_publish(next, 0, 0, 0, -1, -1, edgeCount, nsec, overflow);
    return;
  }

  int teleports = 0;
  for (int k = 0; k < count; k++) {
    s_points[next][k].x    = s_route[k].x;
    s_points[next][k].y    = s_route[k].y;
    s_points[next][k].z    = s_route[k].z;
    s_points[next][k].kind = s_route[k].kind;
    if (s_route[k].kind == EK_SPATH_EDGE_TELEPORT) teleports++;
  }

  spath_publish(next, count, hops, teleports, start, s_route[count - 1].to,
                edgeCount, nsec, overflow);
}

bool secretpath_scan(void) {
  if (!sectorstate_ready() || !wallstate_ready()) return false;
  if (!gamestate_sprite(0)) return false;
  if (!mapextent_ready()) return false;

  PlayerSnap ps = { 0, 0, 0, 0, 0, 0, 0 };
  const bool haveSnap = player_snap(&ps);
  if (!haveSnap) return false;

  const int nsec = mapextent_numsectors();
  if (nsec <= 0) return false;

  const LONG live = s_live;
  const int next = (live == 0) ? 1 : 0;

  s_totalScans++;

  if (nsec > EK_SPATH_MAX_SECTORS) {
    // Refuse the map, do not search part of it: the arrays hold EK_SPATH_MAX_SECTORS and a route over the first 1024
    // sectors of a bigger map is confidently wrong. Published empty with the real count; the staged graph is dropped.
    s_graphStaged = 0;
    spath_publish(next, 0, 0, 0, -1, -1, 0, nsec, 0);
    return true;
  }

  const int start = ps.sect;
  if (start < 0 || start >= nsec) {
    // No start room: published empty so the last map's route does not stay latched through a load; the staged graph is
    // dropped.
    s_graphStaged = 0;
    spath_publish(next, 0, 0, 0, -1, -1, 0, nsec, 0);
    return true;
  }

  // Read above the walk so the signature belongs to this iteration. It protects nothing today: mapextent_extent returns
  // a cached pair republished only by this thread (mapextent.cpp:315), so ekm_spath_graph_usable cannot refuse
  // mid-second.
  int sigSectors = 0, sigWalls = 0;
  const bool haveExtent = mapextent_extent(&sigSectors, &sigWalls);

  int overflow = 0;
  int edgeCount = spath_stage_walls(nsec, 0, &overflow);
  edgeCount = spath_stage_transports(nsec, edgeCount, &overflow);

  s_graphStaged   = haveExtent ? 1 : 0;
  s_graphSig      = ekm_spath_graph_sig(sigSectors, sigWalls);
  s_graphEdges    = edgeCount;
  s_graphSectors  = nsec;
  s_graphOverflow = overflow;

  spath_solve(next, nsec, start, edgeCount, overflow);
  return true;
}

bool secretpath_refresh(void) {
  if (!sectorstate_ready() || !wallstate_ready()) return false;
  if (!gamestate_sprite(0)) return false;
  if (!mapextent_ready()) return false;

  PlayerSnap ps = { 0, 0, 0, 0, 0, 0, 0 };
  const bool haveSnap = player_snap(&ps);
  if (!haveSnap) return false;

  int nsec = 0, nwall = 0;
  const bool haveExtent = mapextent_extent(&nsec, &nwall);
  if (!haveExtent) return false;
  const unsigned sig = ekm_spath_graph_sig(nsec, nwall);

  if (!ekm_spath_graph_usable(s_graphStaged, sig, s_graphSig, nsec, s_graphSectors))
    return false;

  const LONG live = s_live;
  const int next = (live == 0) ? 1 : 0;

  s_totalRefreshes++;

  const int start = ps.sect;
  if (start < 0 || start >= nsec) {
    spath_publish(next, 0, 0, 0, -1, -1, 0, nsec, 0);
    return true;
  }

  spath_solve(next, nsec, start, s_graphEdges, s_graphOverflow);
  return true;
}

int secretpath_generation(void) { return (int)s_live; }

int secretpath_count(int gen) { return spath_gen_ok(gen) ? s_count[gen] : 0; }

const EkSpathPoint* secretpath_point(int gen, int i) {
  if (!spath_gen_ok(gen)) return 0;
  if (i < 0 || i >= s_count[gen]) return 0;
  return &s_points[gen][i];
}

int secretpath_hops(int gen)       { return spath_gen_ok(gen) ? s_hops[gen] : 0; }
int secretpath_teleports(int gen)  { return spath_gen_ok(gen) ? s_teleports[gen] : 0; }
int secretpath_from_sector(int gen){ return spath_gen_ok(gen) ? s_fromSect[gen] : -1; }
int secretpath_to_sector(int gen)  { return spath_gen_ok(gen) ? s_toSect[gen] : -1; }
int secretpath_edges(int gen)      { return spath_gen_ok(gen) ? s_edgeCount[gen] : 0; }
int secretpath_sectors(int gen)    { return spath_gen_ok(gen) ? s_sectorCount[gen] : 0; }
int secretpath_overflow(int gen)   { return spath_gen_ok(gen) ? s_overflow[gen] : 0; }
int secretpath_scans(int gen)      { return spath_gen_ok(gen) ? s_scans[gen] : 0; }
int secretpath_refreshes(int gen)  { return spath_gen_ok(gen) ? s_refreshes[gen] : 0; }
