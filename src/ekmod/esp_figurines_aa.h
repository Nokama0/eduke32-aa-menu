#pragma once

// Figurines get their own row and counter. An owned figurine stays in the map until the player nears it
// (AAITEMS.CON:4174), so sprites are not counted: AA's collected-bit decides (:4128). collectibles1 bit n = MINIDUKE+n
// (16010..16038), collectibles2 bit n = MINIQUEEN+n (16039..16060), both global (AADEFS.CON:4367).

#include <stdio.h>
#include "itemcensus.h"
#include "gamestate.h"
#include "gamevars.h"
#include "figurines_levels_aa.h"

const char* const EKM_FIGURINE_FALLBACK_LABEL = "Figurine";

// Live only: cstat and xrepeat are NOT asked, because AAITEMS.CON hides an unfound figurine with cstat 32768 until the
// player is close (:4146) and finding those is the row's purpose.
inline bool ekm_figurine_is_live(int statnum, int picnum) {
  if (statnum != EK_STAT_ACTOR && statnum != EK_STAT_ZOMBIE) return false;
  return ekm_item_census_is_figurine(picnum);
}

// A find: EVENT_LOADACTOR moves a figurine's hitag into `mtype` (AAPRELOAD.CON:3677) and AAITEMS.CON:4195 stops any
// mtype != 0 before pickup (~51 hidden trophies). Unread keeps "a find".
inline bool ekm_figurine_is_find(bool haveMtype, int mtype) {
  return !haveMtype || mtype == 0;
}

// Split at MINIQUEEN as AAITEMS.CON does; `.which` is -1 outside the band.
struct EkFigurineBit { int which, bit; };
inline EkFigurineBit ekm_figurine_bit(int picnum) {
  EkFigurineBit r = { -1, -1 };
  if (picnum >= 16010 && picnum <= 16038) {
    r.which = EK_GV_CO_COLLECTIBLES1; r.bit = picnum - 16010;
  } else if (picnum >= 16039 && picnum <= 16060) {
    r.which = EK_GV_CO_COLLECTIBLES2; r.bit = picnum - 16039;
  }
  return r;
}

// Unread bits answer NOT COLLECTED: a lost frame over-marks, never hides a find.
inline bool ekm_figurine_collected(bool gotBits, int bits, int bit) {
  if (!gotBits || bit < 0 || bit >= 32) return false;
  return (bits & (1 << bit)) != 0;
}

// Flat index: 0..28 is collectibles1's bit, 29..50 collectibles2's.
inline int ekm_figurine_flat_index(int which, int bit) {
  if (bit < 0 || bit >= 32) return -1;
  if (which == EK_GV_CO_COLLECTIBLES1) return bit;
  if (which == EK_GV_CO_COLLECTIBLES2) return 29 + bit;
  return -1;
}

// Two sprites of one picnum set the same bit, so a duplicate is not double-counted.
inline unsigned long long ekm_figurine_mark_seen(unsigned long long mask, int flat) {
  if (flat < 0 || flat >= 64) return mask;
  return mask | (1ULL << flat);
}

inline int ekm_figurine_popcount(unsigned long long mask) {
  int n = 0;
  while (mask) { n += (int)(mask & 1ULL); mask >>= 1ULL; }
  return n;
}

// X is derived as Y - remaining; Y only rises to keep X non-negative: Y = max(Yprev, Xprev + remaining). An owned
// duplicate vanishing on approach was never in `remaining`, so it does not raise X.
struct EkFigurineCount { int x, y; };

inline EkFigurineCount ekm_figurine_count_update(EkFigurineCount prev, int remaining) {
  EkFigurineCount c;
  c.y = prev.y;
  const int floor = prev.x + remaining;
  if (floor > c.y) c.y = floor;
  c.x = c.y - remaining;
  return c;
}


// Shown only when Y != 0: zero means no figurines, all owned or unreadable bits, which are indistinguishable in a
// level.
inline bool ekm_figurine_counter_wanted(int figurinecounter, int y) {
  return figurinecounter != 0 && y > 0;
}

// "Figurines: X / Y", never a bare count. Finished reads "All N figurines found"; Y == 0 writes nothing.
const int EKM_FIGURINE_COUNTER_BUFSIZE = 32;
inline bool ekm_figurine_counter_label(char* buf, int bufSize, int x, int y) {
  if (!buf || bufSize <= 0) return false;
  buf[0] = 0;
  if (y <= 0) return false;
  if (x < y)       _snprintf_s(buf, bufSize, _TRUNCATE, "Figurines: %d / %d", x, y);
  else if (y == 1) _snprintf_s(buf, bufSize, _TRUNCATE, "1 figurine found");
  else             _snprintf_s(buf, bufSize, _TRUNCATE, "All %d figurines found", y);
  return buf[0] != 0;
}

// X / Y from the map and AA's own bits, not this visit: rebuilding Y from live sprites lost a pickup after a save load.

// Owned figurines in the flat index (c2's bits 0..21 moved to 29..50; c1's bits 29..31 are not figurines).
inline unsigned long long ekm_figurine_owned_mask(int c1, int c2) {
  return ((unsigned long long)(unsigned)c1 & 0x1FFFFFFFULL) |
         (((unsigned long long)(unsigned)c2 & 0x3FFFFFULL) << 29);
}

// Binary search over a table sorted by (volume, level), pinned by tb_0339.
inline bool ekm_figurine_level_mask(const EkFigLevel* t, int n, int volume, int level,
                                    unsigned long long* out) {
  if (!t || n <= 0) return false;
  const long long key = (long long)volume * 64 + level;
  int lo = 0, hi = n - 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    const long long k = (long long)t[mid].volume * 64 + t[mid].level;
    if (k == key) { if (out) *out = t[mid].mask; return true; }
    if (k < key) lo = mid + 1; else hi = mid - 1;
  }
  return false;
}

// `map` loads a user map into volume 0 level 7 (game.h:295, G_HaveUserMap); boardfilename is not read.
inline bool ekm_figurine_user_map_slot(int volume, int level) {
  return volume == 0 && level == 7;
}

// A known level: Y = its map's distinct figurines, X = those owned; otherwise the visit rule.
struct EkFigurineShown { int x, y; bool fromTable; };
inline EkFigurineShown ekm_figurine_counter_shown(bool haveLevel, int volume, int level,
                                                  bool haveBits, int c1, int c2,
                                                  EkFigurineCount visit,
                                                  const EkFigLevel* t, int n) {
  EkFigurineShown s = { visit.x, visit.y, false };
  unsigned long long mask = 0;
  if (!haveLevel || ekm_figurine_user_map_slot(volume, level) ||
      !ekm_figurine_level_mask(t, n, volume, level, &mask))
    return s;
  s.fromTable = true;
  if (!haveBits) { s.x = 0; s.y = 0; return s; }
  s.y = ekm_figurine_popcount(mask);
  s.x = ekm_figurine_popcount(mask & ekm_figurine_owned_mask(c1, c2));
  return s;
}
