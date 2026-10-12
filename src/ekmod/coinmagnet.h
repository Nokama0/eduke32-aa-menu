#pragma once
#include <limits.h>
#include <math.h>
#include <string.h>

// Coin magnet: two Weapons-tab rows that bring AA's credit coins (IMPCOIN) to the player so AA's `state givecoin` pays
// them. Pure half; coinmagnet.cpp is the live half.

enum { EK_CM_OFF = 0, EK_CM_4096, EK_CM_8192, EK_CM_16384, EK_CM_WHOLE, EK_CM_COUNT };
enum { EK_CS_100 = 0, EK_CS_200, EK_CS_400, EK_CS_800, EK_CS_INSTANT, EK_CS_COUNT };

// AADEFS.CON:1763. A cross-check: the live half refuses unless AA's names.h calls this picnum IMPCOIN.
const int EK_IMPCOIN_TILE = 12251;
const int EK_COIN_PICKUP  = 896;
const int EK_COIN_CHASE   = 2048;
const int EK_COIN_CHASE_PERK = 6144;
const int EK_COIN_PERK_MAGNET = 32;    // NOTENEMY.CON:896 ring :934, chase :909 (:901 with the perk), perk bit 32 :899.

inline int coinmagnet_clamp(int v) {
  return v < 0 ? 0 : (v >= EK_CM_COUNT ? EK_CM_COUNT - 1 : v);
}
inline int coinspeed_clamp(int v) {
  return v < 0 ? 0 : (v >= EK_CS_COUNT ? EK_CS_COUNT - 1 : v);
}

inline const char* coinmagnet_name(int v) {
  static const char* const kNames[EK_CM_COUNT] = {
    "Off (AA's own)", "4096", "8192", "16384", "Whole map" };
  return kNames[coinmagnet_clamp(v)];
}
inline const char* coinspeed_name(int v) {
  static const char* const kNames[EK_CS_COUNT] = {
    "100% (AA's own)", "200%", "400%", "800%", "Instant" };
  return kNames[coinspeed_clamp(v)];
}

inline int coinmagnet_radius(int v) {
  static const int kRadius[EK_CM_COUNT] = { 0, 4096, 8192, 16384, INT_MAX };
  return kRadius[coinmagnet_clamp(v)];
}
inline int coinspeed_percent(int v) {
  static const int kPct[EK_CS_COUNT] = { 100, 200, 400, 800, 0 };
  return kPct[coinspeed_clamp(v)];
}

// The engine's player distance, A_FP_ManhattanDist (sector.cpp:221): |dx| + |dy| + (|pz - cz + (28<<8)| >> 4), the one
// `ifpdistl` compares (gameexec.cpp:3395). 64-bit and saturated so nothing overflows.
inline int coin_player_dist(int cx, int cy, int cz, int px, int py, int pz) {
  long long dx = (long long)px - cx; if (dx < 0) dx = -dx;
  long long dy = (long long)py - cy; if (dy < 0) dy = -dy;
  long long dz = (long long)pz - cz + (28 << 8); if (dz < 0) dz = -dz;
  const long long d = dx + dy + (dz >> 4);
  return d > INT_MAX ? INT_MAX : (int)d;
}

// Only a coin that survived its first execution is ours: AA's `ifmove 0` block deletes a coin spawned past 20480
// (NOTENEMY.CON:890) and opens with `seta[].shade -80` (:880); nothing else sets an IMPCOIN's shade (:871).
const int EK_COIN_FIRST_RUN_SHADE = -80;
inline bool coin_first_run_done(int shade) { return shade == EK_COIN_FIRST_RUN_SHADE; }

// Build's getangle (engine.cpp:12464); radarang from its initialiser (engine.cpp:8670).
inline int coin_radarang(int i) {
  if (i >= 640) return -coin_radarang(1279 - i);
  const float bang2rad = 3.14159265358979323846f * (1.f / 1024.f);
  return (short)(atanf(((float)(640 - i) - 0.5f) * (1.f / 160.f)) *
                 (-64.f * (1.f / bang2rad)) + 0.0001f);
}
inline int coin_getangle(int x, int y) {
  if ((x | y) == 0) return 0;
  if (x == 0) return 512 + ((y < 0) << 10);
  if (y == 0) return (x < 0) << 10;
  if (x == y) return 256 + ((x < 0) << 10);
  if (x == -y) return 768 + ((x > 0) << 10);
  const long long ax = x < 0 ? -(long long)x : x, ay = y < 0 ? -(long long)y : y;
  if (ax > ay)
    return ((coin_radarang(640 + (int)(160LL * y / x)) >> 6) + ((x < 0) << 10)) & 2047;
  return ((coin_radarang(640 - (int)(160LL * x / y)) >> 6) + 512 + ((y < 0) << 10)) & 2047;
}

inline int coin_planar_dist(int dx, int dy) {
  const double d = sqrt((double)dx * dx + (double)dy * dy);
  return d >= (double)INT_MAX ? INT_MAX : (int)d;
}

// AA's chase: hvel 480 (NOTENEMY.CON:578); VM_AlterAng eases xvel += (480 - xvel)/5 (gameexec.cpp:630) and the coin
// moves xvel/2 (actors.cpp:667, clip.cpp:1495): 240 a tic.
const int EK_COIN_AA_XVEL = 480;
const int EK_COIN_AA_STEP = 240;

// The xvel W to write so the eased W + (480 - W)/5 is twice the step, capped at the planar distance. Below ~48 units
// the 0 clamp lets the coin pass the player by that much.
inline int coin_boost_xvel(int percent, int planarDist) {
  long long target = (long long)EK_COIN_AA_XVEL * percent / 100;
  const long long cap = 2LL * (planarDist < 0 ? 0 : planarDist);
  if (target > cap) target = cap;
  long long w;
  if (target >= EK_COIN_AA_XVEL) {
    const long long d = target - EK_COIN_AA_XVEL;
    w = EK_COIN_AA_XVEL + 5 * (d / 4) + d % 4;
  } else {
    const long long d = EK_COIN_AA_XVEL - target;
    w = EK_COIN_AA_XVEL - 5 * (d / 4) - d % 4;
  }
  if (w < 0) w = 0;
  if (w > 32767) w = 32767;
  return (int)w;
}

// Safety nets: AA despawns a chasing coin at `ifcount 450` (NOTENEMY.CON:931); the engine arms a 1536-tic sleep past
// 16384 and never re-arms it (gameexec.cpp:3397).
const int EK_COIN_COUNT_NET  = 400;
const int EK_COIN_STUCK_TICS = 8;
const int EK_COIN_SLEEP_DIST = 16384;
const int EK_COIN_SLEEP_NET  = 1400;
const int EK_COIN_JUMP = 4096;
const int EK_COIN_TICS_MAX = 64;

struct CoinTrack { int picnum; int x, y; int budget; int chaseTics; int stillTics; int farTics; };
enum { EK_CSNAP_NONE = 0, EK_CSNAP_BUDGET, EK_CSNAP_COUNT, EK_CSNAP_STUCK, EK_CSNAP_SLEEP };

// A thrown coin is IMPCOINPROJ (AADEFS.CON:1765, :4558), a bouncing projectile that spawns the IMPCOIN when its flight
// ends.
const int EK_IMPCOINPROJ_TILE = 12257;

// End an in-flight coin now: at yvel (bounces left) 0 the engine spawns the IMPCOIN and deletes the projectile
// (actors.cpp:3178). Only in the magnet's scope.
inline bool coin_end_flight(int magnetSel, int dist, int yvel) {
  if (yvel <= 0) return false;
  const int r = coinmagnet_radius(magnetSel);
  return r == INT_MAX || (r > 0 && dist < r);
}

// Health coins: NUKECOIN (NOTENEMY.CON:581) is thrown as COINPROJ, which matches IMPCOINPROJ (AADEFS.CON:4544).
enum { EK_COIN_CREDIT = 0, EK_COIN_HEALTH, EK_COIN_KINDS };
struct CoinKind {
  int tile; const char* name;
  int projTile; const char* projName;
  int pickup;
  int chase, chasePerk;
  bool sightTest;
};
inline const CoinKind* coin_kind(int k) {
  static const CoinKind kKinds[EK_COIN_KINDS] = {
    { EK_IMPCOIN_TILE, "IMPCOIN", EK_IMPCOINPROJ_TILE, "IMPCOINPROJ",
      EK_COIN_PICKUP, EK_COIN_CHASE, EK_COIN_CHASE_PERK, false },
    // AADEFS.CON:880; pays behind `ifcanseetarget` (NOTENEMY.CON:630); pulls only with the perk (:607).
    { 3201, "NUKECOIN", 3202, "COINPROJ", 1024, 0, 2048, true },
  };
  return &kKinds[k == EK_COIN_HEALTH ? EK_COIN_HEALTH : EK_COIN_CREDIT];
}
inline int coin_kind_of_tile(int picnum) {
  for (int k = 0; k < EK_COIN_KINDS; k++) if (coin_kind(k)->tile == picnum) return k;
  return -1;
}
inline int coin_kind_of_proj(int picnum) {
  for (int k = 0; k < EK_COIN_KINDS; k++) if (coin_kind(k)->projTile == picnum) return k;
  return -1;
}
inline int coin_kind_chase_radius(int k, int perks) {
  return (perks & EK_COIN_PERK_MAGNET) ? coin_kind(k)->chasePerk : coin_kind(k)->chase;
}

// A health coin's `ifmove 0` block may killit (NOTENEMY.CON:589), then runs `sizeat 28 28` (:597); A_Spawn gives 1x1.
const int EK_HEALTHCOIN_FIRST_RUN_REPEAT = 28;
inline bool coin_kind_first_run_done(int k, int shade, int xrepeat, int yrepeat) {
  if (k != EK_COIN_HEALTH) return coin_first_run_done(shade);
  return xrepeat == EK_HEALTHCOIN_FIRST_RUN_REPEAT && yrepeat == EK_HEALTHCOIN_FIRST_RUN_REPEAT;
}

inline bool coin_kind_snap_instant(int k, int dist, int magnetRadius, int chaseRadius, int speedSel) {
  if (speedSel != EK_CS_INSTANT) return false;
  if (dist < coin_kind(k)->pickup) return false;
  const bool inMagnet = magnetRadius == INT_MAX || (magnetRadius > 0 && dist < magnetRadius);
  return inMagnet || dist < chaseRadius;
}
inline bool coin_kind_boost_wanted(int k, int dist, int chaseRadius, int percent) {
  return percent > 100 && dist >= coin_kind(k)->pickup && dist < chaseRadius;
}

// A snap cannot move sectnum, and a health coin's sight test reads it (engine.cpp:12762): a stopped coin is re-sectored
// only if bpos moved in-tic (gameexec.cpp:743), which a between-tic write never does.
inline bool coin_kind_snap_pays(int k, int perks, int coinSect, int playerSect) {
  if (!coin_kind(k)->sightTest) return true;
  if (perks & EK_COIN_PERK_MAGNET) return true;
  return coinSect >= 0 && coinSect == playerSect;
}

inline int coin_kind_track_step(CoinTrack* t, int k, int picnum, int dist, int chaseRadius,
                                int magnetRadius, int percent, int x, int y, int tics) {
  if (tics < 1) tics = 1;
  if (tics > EK_COIN_TICS_MAX) tics = EK_COIN_TICS_MAX;
  const long long jump = (long long)(x > t->x ? x - t->x : t->x - x) +
                         (long long)(y > t->y ? y - t->y : t->y - y);
  const bool fresh = t->picnum != picnum || jump > (long long)EK_COIN_JUMP * tics;
  const int reach = chaseRadius > coin_kind(k)->pickup ? chaseRadius : coin_kind(k)->pickup;
  const bool inChase  = dist < chaseRadius;
  const bool inReach  = dist < reach;
  const bool inMagnet = magnetRadius == INT_MAX || (magnetRadius > 0 && dist < magnetRadius);
  const bool boosted  = percent > 100 && inChase;
  if (fresh) {
    memset(t, 0, sizeof(*t));
    t->picnum = picnum;
    t->budget = dist;
  } else {
    if (boosted && x == t->x && y == t->y) t->stillTics += tics;
    else t->stillTics = 0;
    if (!inReach && inMagnet && percent > 0) {
      const long long b = (long long)t->budget - (long long)EK_COIN_AA_STEP * percent / 100 * tics;
      t->budget = b < dist ? (int)b : dist;
    } else {
      t->budget = dist;
    }
  }
  t->x = x;
  t->y = y;
  t->chaseTics = inChase ? t->chaseTics + tics : 0;
  if (t->farTics > 0) t->farTics += tics;
  else if (dist > EK_COIN_SLEEP_DIST) t->farTics = tics;

  if (percent <= 0 || !(inMagnet || boosted)) return EK_CSNAP_NONE;
  if (t->farTics >= EK_COIN_SLEEP_NET) return EK_CSNAP_SLEEP;
  if (t->chaseTics >= EK_COIN_COUNT_NET) return EK_CSNAP_COUNT;
  if (t->stillTics >= EK_COIN_STUCK_TICS) return EK_CSNAP_STUCK;
  if (!inReach && t->budget <= reach) return EK_CSNAP_BUDGET;
  return EK_CSNAP_NONE;
}

inline int coin_chase_radius(int perks) { return coin_kind_chase_radius(EK_COIN_CREDIT, perks); }
inline bool coin_snap_instant(int dist, int magnetRadius, int chaseRadius, int speedSel) {
  return coin_kind_snap_instant(EK_COIN_CREDIT, dist, magnetRadius, chaseRadius, speedSel);
}
inline bool coin_boost_wanted(int dist, int chaseRadius, int percent) {
  return coin_kind_boost_wanted(EK_COIN_CREDIT, dist, chaseRadius, percent);
}
inline int coin_track_step(CoinTrack* t, int picnum, int dist, int chaseRadius,
                           int magnetRadius, int percent, int x, int y, int tics) {
  return coin_kind_track_step(t, EK_COIN_CREDIT, picnum, dist, chaseRadius, magnetRadius,
                              percent, x, y, tics);
}

inline bool coin_kind_enabled(int k, int magnetSel, int speedSel, int coinheal) {
  if (k == EK_COIN_HEALTH) return coinheal != 0 && coinmagnet_clamp(magnetSel) != EK_CM_OFF;
  return coinmagnet_clamp(magnetSel) != EK_CM_OFF || coinspeed_clamp(speedSel) != EK_CS_100;
}

// --- coinmagnet.cpp (DLL only) ---
void coinmagnet_frame(void);
