#pragma once

// Route from the player's room to the nearest undiscovered secret; pure search (secretpath.cpp reads the engine). A
// hint: it sees geometry only, so it routes through keycard doors. Ports World Tour's, except the door set.

const int EK_SPATH_MAX_SECTORS = 1024;
const int EK_SPATH_MAX_HOPS = 64;
const int EK_SPATH_STEP_UP = 6144;
const int EK_SPATH_MIN_GAP = 10240;

const int EK_SPATH_SUBTICKS   = 7;
const int EK_SPATH_SUBTICK_MS = 125;

enum { EK_SPATH_EDGE_WALK = 0, EK_SPATH_EDGE_TELEPORT = 1 };

struct EkSpathSector { int floorz, ceilingz, lotag; };

struct EkSpathEdge { int from, to, kind, x, y, z; };

struct EkSpathPoint { int x, y, z, kind; };

// Door set: lotags whose sector opens, so its current gap says nothing (the flag waives the GAP test, never the STEP
// test). It is isanearoperator (sector.cpp:199) minus the 15-19 platforms. World Tour's rule differs both ways;
// details: docs/re/secret-path.md.
const int EK_SPATH_ST_9_SLIDING_ST_DOOR   = 9;
const int EK_SPATH_ST_20_CEILING_DOOR     = 20;
const int EK_SPATH_ST_21_FLOOR_DOOR       = 21;
const int EK_SPATH_ST_22_SPLITTING_DOOR   = 22;
const int EK_SPATH_ST_23_SWINGING_DOOR    = 23;
const int EK_SPATH_ST_25_SLIDING_DOOR     = 25;
const int EK_SPATH_ST_26_SPLITTING_ST_DOOR = 26;
const int EK_SPATH_ST_29_TEETH_DOOR       = 29;

// A switch, not a range: the set has holes. Masked to the low byte because the open flag (bit 15) and ACTIVATORLOCKED
// (bit 14) live in the high bits and a raw compare stops seeing a door once it opens.
inline int ekm_spath_lotag_is_door(int lotag) {
  switch (lotag & 0xff) {
    case EK_SPATH_ST_9_SLIDING_ST_DOOR:
    case EK_SPATH_ST_20_CEILING_DOOR:
    case EK_SPATH_ST_21_FLOOR_DOOR:
    case EK_SPATH_ST_22_SPLITTING_DOOR:
    case EK_SPATH_ST_23_SWINGING_DOOR:
    case EK_SPATH_ST_25_SLIDING_DOOR:
    case EK_SPATH_ST_26_SPLITTING_ST_DOOR:
    case EK_SPATH_ST_29_TEETH_DOOR:
      return 1;
    default:
      return 0;
  }
}

inline int ekm_spath_edge_passable(const EkSpathSector& a, const EkSpathSector& b,
                                   int bIsDoor) {
  const long long climb = (long long)a.floorz - (long long)b.floorz;
  if (climb > (long long)EK_SPATH_STEP_UP) return 0;
  if (bIsDoor) return 1;
  const long long gap = (long long)b.floorz - (long long)b.ceilingz;
  return (gap >= (long long)EK_SPATH_MIN_GAP) ? 1 : 0;
}

inline int ekm_spath_search(const EkSpathEdge* edges, int edgeCount,
                            const EkSpathSector* sectors, int sectorCount,
                            const unsigned char* isDoor, const unsigned char* isGoal,
                            int start, EkSpathEdge* out, int outCap) {
  if (!edges || !sectors || !isDoor || !isGoal || !out) return 0;
  if (edgeCount < 0 || outCap <= 0) return 0;
  if (sectorCount <= 0 || sectorCount > EK_SPATH_MAX_SECTORS) return 0;
  if (start < 0 || start >= sectorCount) return 0;
  if (isGoal[start]) return 0;

  unsigned char seen[EK_SPATH_MAX_SECTORS];
  short depth[EK_SPATH_MAX_SECTORS];
  int   via[EK_SPATH_MAX_SECTORS];
  short queue[EK_SPATH_MAX_SECTORS];
  for (int i = 0; i < sectorCount; i++) { seen[i] = 0; depth[i] = 0; via[i] = -1; }

  int head = 0, tail = 0, goal = -1;
  seen[start] = 1;
  queue[tail++] = (short)start;
  while (head < tail && goal < 0) {
    const int s = queue[head++];
    if (depth[s] >= EK_SPATH_MAX_HOPS) continue;
    for (int e = 0; e < edgeCount; e++) {
      const EkSpathEdge& ed = edges[e];
      if (ed.from != s) continue;
      const int to = ed.to;
      if (to < 0 || to >= sectorCount || seen[to]) continue;
      if (ed.kind == EK_SPATH_EDGE_WALK &&
          !ekm_spath_edge_passable(sectors[s], sectors[to], isDoor[to] != 0)) continue;
      seen[to] = 1;
      via[to] = e;
      depth[to] = (short)(depth[s] + 1);
      queue[tail++] = (short)to;
      if (isGoal[to]) { goal = to; break; }
    }
  }
  if (goal < 0) return 0;

  EkSpathEdge rev[EK_SPATH_MAX_HOPS];
  int at = goal, n = 0;
  while (at != start && n < EK_SPATH_MAX_HOPS) {
    const int e = via[at];
    if (e < 0 || e >= edgeCount) break;
    rev[n++] = edges[e];
    at = edges[e].from;
  }
  const int keep = (n < outCap) ? n : outCap;
  for (int i = 0; i < keep; i++) out[i] = rev[n - 1 - i];
  return depth[goal];
}

inline const char* ekm_spath_room_word(int hops) { return hops == 1 ? "room" : "rooms"; }

inline unsigned ekm_spath_graph_sig(int numsectors, int numwalls) {
  return ((unsigned)numsectors << 16) ^ (unsigned)numwalls;
}

// "Never staged" needs its own flag (an empty map hashes to 0). After a level change the route stays drawn over the
// old map's edges until the next full scan (secretvol.h).
inline int ekm_spath_graph_usable(int staged, unsigned sig, unsigned stagedSig,
                                  int nsec, int stagedSectors) {
  if (!staged) return 0;
  if (sig != stagedSig) return 0;
  if (nsec != stagedSectors) return 0;
  return 1;
}

// Stage every two-sided wall and paired transport as an edge, search from the player's sector, publish; 1 Hz sampler
// only. secretpath_refresh re-solves the staged graph between scans so the route follows a door.
bool secretpath_scan(void);

bool secretpath_refresh(void);

int secretpath_refreshes(int gen);

// Acquire once and pass `gen`: the cache is double-buffered and rebuilt each scan (secretvol.h).
int secretpath_generation(void);

int secretpath_count(int gen);
const EkSpathPoint* secretpath_point(int gen, int i);

int secretpath_hops(int gen);

int secretpath_teleports(int gen);
int secretpath_from_sector(int gen);
int secretpath_to_sector(int gen);
int secretpath_edges(int gen);
int secretpath_sectors(int gen);
int secretpath_overflow(int gen);
int secretpath_scans(int gen);
