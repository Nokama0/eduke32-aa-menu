#pragma once

#include "aa_notenemy.h"
#include "aa_worm.h"

#include <stdio.h>
#include <string.h>

// Instrumentation only, nothing here decides a target: names which enemies the collect walk drops, and why.

inline bool ekm_enemy_like(int picnum, bool flagged) {
  return flagged || ekm_aa_hostile_notenemy(picnum) || ekm_worm_admitted(picnum);
}

// The 32 refusal slots are for enemy-like picnums only: props filled all of them at level start and every later enemy
// shared the overflow slot. -1 means no slot.
inline int ekm_refusal_slot(short* pics, int* used, int slots, int picnum,
                            bool enemyLike) {
  if (!enemyLike || !pics || !used || slots <= 0) return -1;
  for (int i = 0; i < *used; i++)
    if (pics[i] == (short)picnum) return i;
  if (*used < slots - 1) {
    pics[*used] = (short)picnum;
    return (*used)++;
  }
  return slots - 1;
}

// Print order. AIMABLE..STATE are in the hostile list the ESP draws; FRIENDLY is the friendly list; STATNUM onward
// are in neither.
enum { EKM_CEN_AIMABLE = 0, EKM_CEN_UNHITTABLE, EKM_CEN_SIDEGUN, EKM_CEN_ASLEEP,
       EKM_CEN_MONSTATUS, EKM_CEN_STATE, EKM_CEN_FRIENDLY, EKM_CEN_STATNUM,
       EKM_CEN_ZEROREP, EKM_CEN_DOWN, EKM_CEN_CRITTER, EKM_CEN_FRIENDLY_EXCL,
       EKM_CEN_NOTENEMY, EKM_CEN_FULL, EKM_CENSUS_REASONS };

const int EKM_CENSUS_ENTRIES  = 128;
const int EKM_CENSUS_LINE_MAX = 900;

inline const char* ekm_census_reason_name(int r) {
  static const char* const k[EKM_CENSUS_REASONS] = {
      "aimable", "unhittable", "sidegun", "asleep", "monstatus", "state",
      "friendly", "statnum", "zerorep", "down", "critter", "friendly-excluded",
      "notenemy", "full"};
  return r >= 0 && r < EKM_CENSUS_REASONS ? k[r] : "?";
}

// aim.cpp's candidate loop, in its order: which of its refusals a listed hostile meets first, or AIMABLE when it
// would be a candidate.
inline int ekm_census_hostile_reason(bool hittable, bool attachment, bool asleep,
                                     bool monstatusShootable, bool stateTargetable) {
  if (!hittable) return EKM_CEN_UNHITTABLE;
  if (attachment) return EKM_CEN_SIDEGUN;
  if (asleep) return EKM_CEN_ASLEEP;
  if (!monstatusShootable) return EKM_CEN_MONSTATUS;
  if (!stateTargetable) return EKM_CEN_STATE;
  return EKM_CEN_AIMABLE;
}

// exA/exB are the FIRST sprite's example values: extra and pal, or the state for STATE. Only STATNUM entries key by
// statnum.
struct EkmCensusEntry { int reason, picnum, statnum, count, exA, exB; };
struct EkmCensus {
  int n;
  int total[EKM_CENSUS_REASONS];
  EkmCensusEntry e[EKM_CENSUS_ENTRIES];
  // Each list's farthest kept entry, Build units from the player (-1 none): shows how far a capped list reaches.
  int farHostile, farFriendly;
};

inline void ekm_census_clear(EkmCensus* c) {
  if (c) memset(c, 0, sizeof(*c));
}

inline void ekm_census_add(EkmCensus* c, int reason, int picnum, int statnum,
                           int exA, int exB) {
  if (!c || reason < 0 || reason >= EKM_CENSUS_REASONS) return;
  c->total[reason]++;
  const int key = reason == EKM_CEN_STATNUM ? statnum : 0;
  for (int i = 0; i < c->n; i++) {
    EkmCensusEntry& x = c->e[i];
    if (x.reason == reason && x.picnum == picnum && x.statnum == key) {
      x.count++;
      return;
    }
  }
  if (c->n >= EKM_CENSUS_ENTRIES) return;
  const EkmCensusEntry x = { reason, picnum, key, 1, exA, exB };
  c->e[c->n++] = x;
}

inline bool ekm_census_same(const EkmCensus& a, const EkmCensus& b) {
  if (a.n != b.n) return false;
  for (int r = 0; r < EKM_CENSUS_REASONS; r++)
    if (a.total[r] != b.total[r]) return false;
  for (int i = 0; i < a.n; i++)
    if (a.e[i].reason != b.e[i].reason || a.e[i].picnum != b.e[i].picnum ||
        a.e[i].statnum != b.e[i].statnum || a.e[i].count != b.e[i].count)
      return false;
  return true;
}

// Room kept back for each reason still to print: its header and its "more". 170 header + 14 x 48 = 842, so every
// reason's header always fits in 900.
const int EKM_CENSUS_RESERVE = 48;

// Totals first, then per reason its picnums by count, largest first, until the line would pass EKM_CENSUS_LINE_MAX;
// whatever is not printed is "and N more", so each reason's printed counts plus its "more" equal its total.
inline int ekm_census_format(const EkmCensus& c, const char* walk, char* buf,
                             int n) {
  if (!buf || n <= 0) return 0;
  const int limit = n < EKM_CENSUS_LINE_MAX ? n : EKM_CENSUS_LINE_MAX;
  int all = 0, aim = 0, listed = 0, unlisted = 0;
  for (int r = 0; r < EKM_CENSUS_REASONS; r++) {
    all += c.total[r];
    if (r == EKM_CEN_AIMABLE) aim += c.total[r];
    else if (r <= EKM_CEN_STATE) listed += c.total[r];
    else if (r > EKM_CEN_FRIENDLY) unlisted += c.total[r];
  }
  char farH[16] = "none", farF[16] = "none";
  if (c.farHostile >= 0) snprintf(farH, sizeof(farH), "%d", c.farHostile);
  if (c.farFriendly >= 0) snprintf(farF, sizeof(farF), "%d", c.farFriendly);
  int pos = snprintf(buf, (size_t)limit,
                     "targets census (%s walk): enemy-like %d = aimable %d + "
                     "listed-not-aimed %d + friendly %d + unlisted %d"
                     " | farthest kept: hostile %s friendly %s",
                     walk ? walk : "?", all, aim, listed,
                     c.total[EKM_CEN_FRIENDLY], unlisted, farH, farF);
  if (pos < 0 || pos >= limit) { buf[limit - 1] = 0; return (int)strlen(buf); }

  for (int r = 0; r < EKM_CENSUS_REASONS; r++) {
    if (c.total[r] == 0) continue;
    int later = 0;
    for (int r2 = r + 1; r2 < EKM_CENSUS_REASONS; r2++)
      if (c.total[r2] > 0) later += EKM_CENSUS_RESERVE;
    int w = snprintf(buf + pos, (size_t)(limit - pos), " | %s %d:",
                     ekm_census_reason_name(r), c.total[r]);
    if (w < 0 || pos + w >= limit) { buf[pos] = 0; break; }
    pos += w;

    bool done[EKM_CENSUS_ENTRIES] = {};
    int printed = 0;
    for (;;) {
      int best = -1;
      for (int i = 0; i < c.n; i++)
        if (!done[i] && c.e[i].reason == r &&
            (best < 0 || c.e[i].count > c.e[best].count))
          best = i;
      if (best < 0) break;
      done[best] = true;
      const EkmCensusEntry& x = c.e[best];
      char item[80];
      int il;
      if (r == EKM_CEN_DOWN)
        il = snprintf(item, sizeof(item), " %dx%d (extra %d pal %d)", x.picnum,
                      x.count, x.exA, x.exB);
      else if (r == EKM_CEN_STATNUM)
        il = snprintf(item, sizeof(item), " %dx%d (st %d)", x.picnum, x.count,
                      x.statnum);
      else if (r == EKM_CEN_STATE)
        il = snprintf(item, sizeof(item), " %dx%d (state %d)", x.picnum, x.count,
                      x.exA);
      else
        il = snprintf(item, sizeof(item), " %dx%d", x.picnum, x.count);
      if (il < 0 || pos + il + 20 + later >= limit) break;
      memcpy(buf + pos, item, (size_t)il + 1);
      pos += il;
      printed += x.count;
    }
    const int more = c.total[r] - printed;
    if (more > 0) {
      w = snprintf(buf + pos, (size_t)(limit - pos), " and %d more", more);
      if (w < 0 || pos + w >= limit) { buf[pos] = 0; break; }
      pos += w;
    }
  }
  return pos;
}
