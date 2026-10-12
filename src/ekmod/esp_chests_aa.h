#pragma once

// AA'S EDF CHESTS AND BOMB-PART CHESTS. Neither is ever deleted: an opened chest keeps its picnum and only changes
// action, so picnum alone cannot tell opened from not. The discriminator is AA's own global bit, read once a frame
// (gamevars_collectibles), never per sprite.

#include <stdio.h>
#include "gamestate.h"

const int EKM_PIC_ITEMCHEST  = 177;
const int EKM_PIC_EDFCHEST   = 179;
const int EKM_BOMBPART_FIRST = 2600;
const int EKM_BOMBPART_COUNT = 5;

// The 11 placed chests (hitags 1-9, 11, 12; no freezer chest exists), all in volume 4 (httkb).
const int EKM_EDFCHEST_MASK   = 0xC2FF;
const int EKM_EDFCHEST_TOTAL  = 11;
const int EKM_EDFCHEST_VOLUME = 4;

// AADEFS.CON:3246-3258. 0 for a hitag the table does not name.
inline int ekm_edfchest_bit(int hitag) {
  static const int kBits[13] = { 0, 1, 2, 4, 32768, 8, 16, 32, 64, 128, 256, 512, 16384 };
  return (hitag >= 1 && hitag <= 12) ? kBits[hitag] : 0;
}

inline const char* ekm_edfchest_weapon(int hitag) {
  static const char* const kNames[13] = { 0, "Pistol", "Shotgun", "Chaingun",
      "Bow/10 gauge", "RPG", "Pipebombs", "Shrinker/BMFG", "Devastator",
      "Spider-bombs", "Freezer", "Babifier", "Railgun/Barrett" };
  return (hitag >= 1 && hitag <= 12) ? kNames[hitag] : 0;
}

inline int ekm_bombpart_index(int hitag) {
  const int i = hitag - EKM_BOMBPART_FIRST;
  return (i >= 0 && i < EKM_BOMBPART_COUNT) ? i : -1;
}

inline bool ekm_chest_statnum_live(int statnum) {
  return statnum == EK_STAT_ACTOR || statnum == EK_STAT_ZOMBIE;
}

// Bit clear -> shown; set -> hidden; `gotBits` false -> SHOWN, the figurines' direction (too many marks beats a
// hidden find). No bit -> not a chest.
inline bool ekm_chest_show(bool gotBits, int bits, int bit) {
  if (bit == 0) return false;
  if (!gotBits) return true;
  return (bits & bit) == 0;
}

enum { EKM_CHEST_NONE = 0, EKM_CHEST_EDF = 1, EKM_CHEST_BOMBPART = 2 };

inline int ekm_chest_marked(int picnum, int hitag, bool gotStart, int startguns,
                            bool gotMc, int mcguffins) {
  if (picnum == EKM_PIC_EDFCHEST)
    return ekm_chest_show(gotStart, startguns, ekm_edfchest_bit(hitag)) ? EKM_CHEST_EDF
                                                                        : EKM_CHEST_NONE;
  if (picnum == EKM_PIC_ITEMCHEST) {
    const int i = ekm_bombpart_index(hitag);
    if (i < 0) return EKM_CHEST_NONE;
    return ekm_chest_show(gotMc, mcguffins, 1 << i) ? EKM_CHEST_BOMBPART : EKM_CHEST_NONE;
  }
  return EKM_CHEST_NONE;
}

const int EKM_CHEST_LABEL_BUFSIZE = 40;
inline bool ekm_chest_label(char* buf, int bufSize, int kind, int hitag) {
  if (!buf || bufSize <= 0) return false;
  buf[0] = 0;
  if (kind == EKM_CHEST_EDF) {
    const char* w = ekm_edfchest_weapon(hitag);
    if (w) _snprintf_s(buf, bufSize, _TRUNCATE, "EDF chest: %s", w);
  } else if (kind == EKM_CHEST_BOMBPART) {
    const int i = ekm_bombpart_index(hitag);
    if (i >= 0) _snprintf_s(buf, bufSize, _TRUNCATE, "Bomb part %d", i + 1);
  }
  return buf[0] != 0;
}

// X of "EDF chests: X / 11": the placed chests' bits that are set (AA keeps no count of its own).
inline int ekm_edfchest_found(int startguns) {
  unsigned v = (unsigned)startguns & (unsigned)EKM_EDFCHEST_MASK;
  int n = 0;
  while (v) { n += (int)(v & 1u); v >>= 1u; }
  return n;
}

// A live picnum-179 sprite's hitag bit ORed in; anything else contributes nothing.
struct EkChestMaskSprite { int picnum, hitag; };
inline int ekm_edfchest_level_mask(const EkChestMaskSprite* s, int n) {
  int mask = 0;
  for (int i = 0; i < n; i++)
    if (s[i].picnum == EKM_PIC_EDFCHEST) mask |= ekm_edfchest_bit(s[i].hitag);
  return mask;
}

inline int ekm_popcount16(int v) {
  unsigned u = (unsigned)v & 0xFFFFu;
  int n = 0;
  while (u) { n += (int)(u & 1u); u >>= 1u; }
  return n;
}

// Only in httkb. Unread bits or an unread level hide it rather than claim "0 / 11"; Y == 0 hides it too (the figurine
// counter's y > 0 gate).
inline bool ekm_edfchest_counter_wanted(int espchests, bool haveLevel, int volume,
                                        bool gotBits, int y) {
  return espchests != 0 && haveLevel && volume == EKM_EDFCHEST_VOLUME && gotBits && y > 0;
}

const int EKM_EDFCHEST_COUNTER_BUFSIZE = 48;
inline bool ekm_edfchest_counter_label(char* buf, int bufSize, int x, int y,
                                       int episodeFound) {
  if (!buf || bufSize <= 0) return false;
  buf[0] = 0;
  if (y <= 0) return false;
  if (x < y)
    _snprintf_s(buf, bufSize, _TRUNCATE, "EDF chests: %d / %d (episode %d / %d)",
               x, y, episodeFound, EKM_EDFCHEST_TOTAL);
  else if (y == 1)
    _snprintf_s(buf, bufSize, _TRUNCATE, "EDF chest opened (episode %d / %d)",
               episodeFound, EKM_EDFCHEST_TOTAL);
  else
    _snprintf_s(buf, bufSize, _TRUNCATE, "All %d EDF chests opened (episode %d / %d)",
               y, episodeFound, EKM_EDFCHEST_TOTAL);
  return buf[0] != 0;
}
