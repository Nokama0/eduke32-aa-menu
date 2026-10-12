#pragma once

// The map's extent: numsectors and numwalls, derived from the sector chain. A sector past the end of the loaded map
// holds the previous map's record (the arrays are MAXSECTORS long and a load rewrites only the first numsectors
// entries).
#include "sectorstate.h"
#include "wallstate.h"
#include "gamestate.h"

// Three of the original six rules survived; the wall-span sum, the exclusion of sector[]/wall[] from the searched
// bytes and the search backoff existed only to validate a candidate address.

// The counts on their own: at least a triangle's worth of walls and EK_SECTOR_MIN_CHAIN sectors. A pair failing here
// is not published, so a reader refuses rather than bound its loops with a number the chain did not give.
inline bool ekm_extent_counts_plausible(int n, int w) {
  if (n < EK_SECTOR_MIN_CHAIN || n > EK_MAXSECTORS) return false;
  if (w < 3 || w > EK_MAXWALLS)   return false;
  return true;
}

struct EkExtentStep { int stop; };

// The step, one sector at a time, called with the previous accepted span ((0, 0) before sector 0, so sector 0 must
// start at wall 0, Build's layout). An unreadable sector stops the walk rather than being skipped.
inline EkExtentStep ekm_extent_step(int index, int readOk,
                                    int wallptr, int wallnum,
                                    int prevWallptr, int prevWallnum, int n) {
  EkExtentStep s = { 1 };
  if (index < 0 || index >= n) return s;
  if (!readOk) return s;
  // The record test is shared with the locate (ekm_sector_locate_step): a 2-wall sector is real, and stopping on it
  // undercounted 29 of AA's 384 maps.
  if (!ekm_sector_chain_record_ok(wallptr, wallnum)) return s;
  if (wallptr + wallnum > EK_MAXWALLS) return s;
  if (!ekm_sector_follows(prevWallptr, prevWallnum, wallptr)) return s;
  s.stop = 0;
  return s;
}

inline bool ekm_extent_chain_stops(int n, int readOk, int wallptr, int wallnum,
                                   int prevWallptr, int prevWallnum) {
  if (n >= EK_MAXSECTORS) return true;
  return ekm_extent_step(n, readOk, wallptr, wallnum,
                         prevWallptr, prevWallnum, n + 1).stop == 1;
}

// The measured offsets are +8 and +84: 512 bytes forward holds the furthest six times over, 256 back covers a build
// with the pair below the pointer.
const int EK_EXTENT_WINDOW_BACK = 256;
const int EK_EXTENT_WINDOW_FWD  = 512;

inline bool ekm_extent_window_holds(int offset) {
  return offset >= -EK_EXTENT_WINDOW_BACK && offset < EK_EXTENT_WINDOW_FWD;
}

// build.h declares both counts int16_t, so anything outside that range is not one. Equal halves are refused (one
// position would answer for both and be recorded twice); a map cannot produce them, but the rule does not lean on
// that.
inline bool ekm_extent_values_searchable(int n, int w) {
  if (n < 1 || n > 32767) return false;
  if (w < 1 || w > 32767) return false;
  return n != w;
}

// Uniqueness makes this a locate and not a guess: one position holding the value is a global, two is an index table
// counting through it (what the whole-.bss search kept accepting), none is a layout this build lacks.
inline bool ekm_extent_locate_unique(int nHits, int wHits) {
  return nHits == 1 && wHits == 1;
}

// Both halves must agree: an over-run publishes the previous map's pair whole (either half catches it), and both also
// catch a half-written load.
inline bool ekm_extent_agrees(int walkN, int walkW, int globN, int globW) {
  return walkN == globN && walkW == globW;
}

enum {
  EK_EXTENT_HARDEN_PENDING = 0,
  EK_EXTENT_HARDEN_LOCATED = 1,
  EK_EXTENT_HARDEN_NONE    = 2
};

// What to do this second. RELOCATE is a fourth answer, not an error: the addresses stopped holding counts, so they
// are dropped and re-searched and this second's walk is published unhardened.
enum {
  EK_EXTENT_PUBLISH_NOTHING  = 0,
  EK_EXTENT_PUBLISH_WALK     = 1,
  EK_EXTENT_PUBLISH_REFUSE   = 2,
  EK_EXTENT_PUBLISH_RELOCATE = 3
};

// The decision that settles whether the ESP draws anything. Ints rather than bools throughout (/O2 struct returns).
inline int ekm_extent_publish_verdict(int hardenState, int walkOk,
                                      int globPlausible, int agrees) {
  if (!walkOk) return EK_EXTENT_PUBLISH_NOTHING;
  if (hardenState != EK_EXTENT_HARDEN_LOCATED) return EK_EXTENT_PUBLISH_WALK;
  if (!globPlausible) return EK_EXTENT_PUBLISH_RELOCATE;
  return agrees ? EK_EXTENT_PUBLISH_WALK : EK_EXTENT_PUBLISH_REFUSE;
}

// How often the locate may be redone before giving up for the process: a layout cannot move inside a process, so a
// drop means the window named the wrong bytes; stopping avoids a log line every second on a build the search cannot
// read.
const int EK_EXTENT_RELOCATE_MAX = 4;
inline bool ekm_extent_may_relocate(int drops) {
  return drops < EK_EXTENT_RELOCATE_MAX;
}

// The loud line waits for a second look: loadboard writes numsectors/numwalls before all sector records are
// rewritten, so a 1 Hz scan inside that window disagrees with the new globals on an ordinary level change. The
// zero-publish stays immediate; only the shout waits one more second.
inline int ekm_extent_disagree_next(int priorStreak, int disagreeing) {
  if (!disagreeing) return 0;
  return priorStreak + 1;
}

inline bool ekm_extent_disagree_confirmed(int streak) {
  return streak == 2;
}

// Walk the chain and publish what it found. Runs on gamestate.cpp's scan thread only; refuses until
// sectorstate_ready. Returns whether the published pair is plausible.
bool mapextent_scan(void);

// True once a plausible pair has been published. The readers are render-thread safe and walk nothing: each is one
// interlocked 64-bit read of the last published pair.
bool mapextent_ready(void);

// The published sector count, or 0 when not ready: one interlocked read of the packed pair, coherent on its own.
// mapextent_numwalls was removed: no consumer, and numsectors then numwalls is two reads with a publish able to land
// between them.
int mapextent_numsectors(void);

// Both halves from one read; kept though nothing calls it: a separate wall count would be a second expression of a
// quantity the walk already has, and two of those drift.
bool mapextent_extent(int* numsectors, int* numwalls);
