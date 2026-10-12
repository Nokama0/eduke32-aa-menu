#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "aimsolve.h"

// WEAPON SPEED, as pure rules: two rows, one arithmetic. The engine runs two per-tic counters, kickback_pic (the
// firing frame, player.cpp:4588) and weapon_pos (raise/lower, :4212). Advancing a counter PAST its dead steps, never
// past the next value where the engine does something, shortens the cadence without skipping a shot or reset.

const int FIRERATE_MIN = 100, FIRERATE_MAX = 400, FIRERATE_STEP = 50;
const int FIRERATE_DEFAULT = FIRERATE_MIN;

// The stop-set cap (64) is not a bound on anything real: a weapon with WEAPON_FIREEVERYOTHER fires on EVERY frame of
// its window and WEAPON_STANDSTILL pins z on every frame up to FireDelay. A set that will not fit REFUSES the weapon
// (firestops.h): a truncated set is a missing stop, the defect itself, and no speed-up is the smaller fault.
const int FIRERATE_EVENTS_MAX = 64;
struct FireRateWeapon { int weapon; int nEvents; int events[FIRERATE_EVENTS_MAX];
                        unsigned char con[FIRERATE_EVENTS_MAX]; };

// `con[]` HOLDS PROVENANCE FLAGS: one value can be an engine event and a CON test at once. CON_EDGE is 1 so every
// `con[i] = 1` keeps its meaning; ENGINE alone is stored as 0, so `con[i] != 0` still means "some CON flag".
const unsigned char EK_PROV_CON_EDGE  = 1;
const unsigned char EK_PROV_CON_FRAME = 2;
const unsigned char EK_PROV_ENGINE    = 4;

inline unsigned char firerate_prov(unsigned char stored) {
  return stored ? stored : EK_PROV_ENGINE;
}
inline bool firerate_prov_has_con(unsigned char stored) {
  return (stored & (EK_PROV_CON_EDGE | EK_PROV_CON_FRAME)) != 0;
}

// A stop set as one comma-separated field, for the log. Returns the count so an enumeration carries its own total.
// _TRUNCATE throughout, never sprintf_s (which aborts). Flags: `E` ENGINE beside a CON flag, `F` CON_FRAME, `c`
// CON_EDGE.
inline int firerate_stops_text(const FireRateWeapon& t, char* out, int cap) {
  if (!out || cap <= 0) return 0;
  out[0] = 0;
  int used = 0;
  for (int i = 0; i < t.nEvents && i < FIRERATE_EVENTS_MAX; i++) {
    if (used >= cap - 1) break;
    const unsigned char p = t.con[i];
    const bool con = firerate_prov_has_con(p);
    const int n = _snprintf_s(out + used, (size_t)(cap - used), _TRUNCATE,
                              "%s%d%s%s%s", i ? "," : "", t.events[i],
                              (con && (p & EK_PROV_ENGINE)) ? "E" : "",
                              (p & EK_PROV_CON_FRAME) ? "F" : "",
                              (p & EK_PROV_CON_EDGE) ? "c" : "");
    if (n < 0) break;
    used += n;
  }
  return t.nEvents;
}

// EDUKE32'S OWN READING, NOT WORLD TOUR'S NUMBERS: P_ProcessWeapon is data-driven, so the table is the stock defaults
// run through player.cpp:4587-4741; an entry lists the kickback_pic values whose NEXT tic does something other than
// count. Three weapons have no entry (pipebomb, hand remote, tripbomb) and AA's Executioner is refused at runtime.
inline const FireRateWeapon* firerate_weapon(int weapon) {
  static const FireRateWeapon FIRERATE_TABLE[10] = {
    { 0,  2, { 6, 13 } },
    { 1,  5, { 1, 4, 5, 8, 26 } },
    { 2,  5, { 3, 12, 14, 23, 29 } },
    { 3,  5, { 2, 5, 8, 9, 11 } },
    { 4,  2, { 3, 19 } },
    { 6,  2, { 9, 11 } },
    { 7,  2, { 2, 5 } },
    { 9,  2, { 2, 4 } },
    { 11, 2, { 2, 4 } },
    { 12, 2, { 1, 15 } },
  };
  for (int i = 0; i < 10; i++)
    if (FIRERATE_TABLE[i].weapon == weapon) return &FIRERATE_TABLE[i];
  return 0;
}

inline int firerate_rate_clamped(int v) {
  if (v < FIRERATE_MIN) v = FIRERATE_MIN;
  if (v > FIRERATE_MAX) v = FIRERATE_MAX;
  return ((v - FIRERATE_MIN + FIRERATE_STEP / 2) / FIRERATE_STEP) * FIRERATE_STEP + FIRERATE_MIN;
}

inline int firerate_next_event(const FireRateWeapon& t, int k) {
  for (int i = 0; i < t.nEvents; i++) if (t.events[i] >= k) return t.events[i];
  return -1;
}

inline bool firerate_stop_contains(const FireRateWeapon& t, int value) {
  for (int i = 0; i < t.nEvents && i < FIRERATE_EVENTS_MAX; i++)
    if (t.events[i] == value) return true;
  return false;
}

// Forward by up to `steps`, stopping AT the next event's provenance-decided value (ON it for ENGINE, one below for
// CON), never from rest (`k <= 0`). AA's CON observes the counter after the engine's own `++`, so a CON stop parks
// one below; an engine-derived stop is already what P_ProcessWeapon tests.
inline int firerate_advance(const FireRateWeapon& t, int k, int steps) {
  if (k <= 0 || steps <= 0) return k;
  int i = 0;
  while (i < t.nEvents && t.events[i] < k) i++;
  if (i < t.nEvents && t.events[i] == k && t.con[i] == EK_PROV_CON_FRAME) i++;
  if (i >= t.nEvents) return k;
  const int stop = t.events[i] - (firerate_prov_has_con(t.con[i]) ? 1 : 0);
  if (stop <= k) return k;
  const int target = k + steps;
  return target > stop ? stop : target;
}

// The sibling on weapon_pos. A weapon change sets it to -1 and the engine decrements to -8; at -9 the engine alone
// flips it to 10, then 10..1 decrements back to 0 (rest). Never move rest, never move the flip, never run past -8 or
// past 1: both landings are the engine's and load-bearing (sector.cpp:2875 tests weapon_pos == 1 exactly).
inline int wswitch_advance(int pos, int steps) {
  if (steps <= 0 || pos == 0 || pos == -9) return pos;
  if (pos < 0) {
    const int target = pos - steps;
    return target < -8 ? -8 : target;
  }
  const int target = pos - steps;
  return target < 1 ? 1 : target;
}

// Extra steps for one tic at a rate in percent, with a fractional carry in [0, 1). THE CARRY IS THE CALLER'S AND MUST
// PERSIST ACROSS TICS: reset every tic, 150 would floor to zero. The caller resets it on every refusal.
inline int firerate_steps_extra(double extra, double* carry) {
  if (!carry) return (int)floor(extra);
  *carry += extra;
  const int n = (int)floor(*carry);
  *carry -= n;
  if (*carry < 0.0) *carry = 0.0;
  return n;
}
inline int firerate_steps(int ratePct, double* carry) {
  return firerate_steps_extra((firerate_rate_clamped(ratePct) - 100) / 100.0, carry);
}

inline double firerate_carry_for_weapon(double carry, int lastWeapon, int weapon) {
  return weapon != lastWeapon ? 0.0 : carry;
}

// WHAT THE HELD WEAPON WILL GET, BEFORE IT FIRES: replays the live step from post-tic frame 1 until the counter
// passes cycleEnd and returns cycleEnd / mean tics per cycle, x100. One hook step per engine tic is the
// approximation. `carryRestarts`: held fire passing through rest zeroes the carry.
const int FIRERATE_PREDICT_CYCLES = 64;
const int FIRERATE_PREDICT_END_MAX = 255;
inline int firerate_predict_x100(const FireRateWeapon& t, int rate, int cycleEnd,
                                 bool carryRestarts = false) {
  if (t.nEvents <= 0 || t.nEvents > FIRERATE_EVENTS_MAX) return 100;
  if (cycleEnd < 1 || cycleEnd > FIRERATE_PREDICT_END_MAX) return 100;
  if (firerate_rate_clamped(rate) <= 100) return 100;
  double carry = 0.0;
  int total = 0;
  for (int c = 0; c < FIRERATE_PREDICT_CYCLES; c++) {
    if (carryRestarts) carry = 0.0;
    int k = 1;
    for (int tic = 0; tic < cycleEnd && k <= cycleEnd; tic++) {
      k = firerate_advance(t, k, firerate_steps(rate, &carry)) + 1;
      total++;
    }
  }
  if (total <= 0) return 100;
  return (cycleEnd * FIRERATE_PREDICT_CYCLES * 100 + total / 2) / total;
}

// ONE CLOCK, AND IT IS THE TIC: this port has no tic hook, so the tic boundary is taken from the wall clock. `acc` is
// milliseconds owed, carried across frames (a per-FRAME advance would make the feature frame-rate dependent). CAPPED,
// because a stall is not a burst: an alt-tab or level load must not run a weapon through several events at once.
const int FIRERATE_TICS_MAX = 4;
inline int firerate_tics_due(double* acc, int elapsedMs) {
  if (!acc) return 0;
  if (elapsedMs > 0) *acc += (double)elapsedMs;
  int n = 0;
  while (*acc >= EKM_MS_PER_TIC && n < FIRERATE_TICS_MAX) { *acc -= EKM_MS_PER_TIC; n++; }
  if (n >= FIRERATE_TICS_MAX) *acc = 0.0;
  if (*acc < 0.0) *acc = 0.0;
  return n;
}

// Every condition the kickback_pic write needs: a located player struct, no game menu, no pause, our panel shut, a
// rate above the identity, a running counter, and a weapon this header has a table for.
inline bool firerate_gate(bool ready, bool menuOpen, bool paused, bool panelOpen,
                          int ratePct, int kickback, bool hasTable) {
  return ready && !menuOpen && !paused && !panelOpen
      && firerate_rate_clamped(ratePct) > 100 && kickback > 0 && hasTable;
}

inline const char* firerate_gate_reason(bool ready, bool menuOpen, bool paused, bool panelOpen,
                                        int ratePct, int kickback, bool hasTable) {
  if (!ready)      return "unready";
  if (menuOpen)    return "menu";
  if (paused)      return "paused";
  if (panelOpen)   return "panel";
  if (firerate_rate_clamped(ratePct) <= 100) return "off";
  if (kickback <= 0) return "rest";
  if (!hasTable)   return "notable";
  return "on";
}

inline bool wswitch_gate(bool ready, bool menuOpen, bool paused, bool panelOpen,
                         int ratePct, int pos) {
  return ready && !menuOpen && !paused && !panelOpen
      && firerate_rate_clamped(ratePct) > 100 && pos != 0 && pos != -9;
}

inline const char* wswitch_gate_reason(bool ready, bool menuOpen, bool paused,
                                       bool panelOpen, int ratePct, int pos) {
  if (!ready)    return "unready";
  if (menuOpen)  return "menu";
  if (paused)    return "paused";
  if (panelOpen) return "panel";
  if (firerate_rate_clamped(ratePct) <= 100) return "off";
  if (pos == 0)  return "rest";
  if (pos == -9) return "flip";
  return "on";
}

// Classifies a (wrote, read-back) pair against the ENGINE'S OWN single-tic step (pos-1, with the 1 -> 0 rest landing
// refused), so a foreign write is not mistaken for the engine. -8 is NOT special: its step is to -9, and the flip to
// 10 is a LATER tic. `wrote` is never -9. Anything else (11 by name, AAPLAYER.CON:11627) is a slam.
inline const char* wswitch_readback_kind(int wrote, int readBack) {
  if (readBack == wrote) return "same";
  if (wrote < 0) {
    if (readBack == wrote - 1) return "engine-step";
  } else if (wrote > 0) {
    if (wrote == 1) { if (readBack == 0) return "engine-rest"; }
    else if (readBack == wrote - 1) return "engine-step";
  }
  if (readBack == 11) return "slam-11";
  if (readBack == 0)  return "slam-0";
  return "slam-other";
}

// Only accelerate a switch the engine is actually driving: `wswitch_gate` alone kept advancing weapon_pos after AA's
// idle handler parked it. A new sequence or no previous write ALWAYS allows the advance; otherwise "engine-step"
// advances and "same" (parked) or "slam-*" refuse until it steps or a new sequence begins.
inline bool wswitch_moved(bool newSeq, bool havePrevWrite, int prevWrote, int pos) {
  if (newSeq || !havePrevWrite) return true;
  return !strcmp(wswitch_readback_kind(prevWrote, pos), "engine-step");
}

struct WswitchTrack {
  int  lastPos;
  bool haveWrite;
  int  lastWrote;
};

// One frame's movement decision; updates `track` and returns whether the movement term allows an advance (AND with
// `wswitch_gate`). The flip to 10 does NOT change curr_weapon, so a stale write of one sign against a read of the
// other means the RUN changed: a new sequence.
inline bool wswitch_track_step(WswitchTrack* track, int weapon, int lastWeapon, int pos) {
  const bool signFlip = track->haveWrite &&
      ((track->lastWrote < 0 && pos > 0) || (track->lastWrote > 0 && pos < 0));
  const bool newSeq = (track->lastPos == 0 && pos != 0) || weapon != lastWeapon || signFlip;
  if (newSeq) track->haveWrite = false;
  const bool moved = wswitch_moved(newSeq, track->haveWrite, track->lastWrote, pos);
  track->lastPos = pos;
  return moved;
}

inline void wswitch_track_wrote(WswitchTrack* track, int wrote) {
  track->haveWrite = true;
  track->lastWrote = wrote;
}

// Whether AA missed frame TotalTime-1 because our trace samples per FRAME while AA's script reads per TIC. Each
// advanced tic is attributed to the kickback value CURRENT the frame it completed, so the per-value counts always sum
// to the total. MEASUREMENT ONLY.
class KickTicAccumulator {
public:
  static const int MAXV = 64;

  KickTicAccumulator() : m_cur(0), m_curTics(0), m_span(0) {
    for (int i = 0; i < MAXV; i++) { m_tics[i] = 0; m_seen[i] = false; }
  }

  bool sample(int kickback, bool ticAdvanced, int* doneValue, unsigned* doneTics) {
    bool finished = false;
    if (kickback != m_cur) {
      if (m_cur >= 0 && m_cur < MAXV && m_cur != 0) {
        m_tics[m_cur] += m_curTics;
        m_seen[m_cur] = true;
        if (doneValue) *doneValue = m_cur;
        if (doneTics) *doneTics = m_curTics;
        finished = true;
      }
      m_cur = kickback;
      m_curTics = 0;
    }
    if (ticAdvanced && kickback != 0) {
      m_span++;
      m_curTics++;
    }
    return finished;
  }

  unsigned span() const { return m_span; }
  unsigned valueTics(int v) const { return (v >= 0 && v < MAXV) ? m_tics[v] : 0; }
  bool valueSeen(int v) const { return (v >= 0 && v < MAXV) ? m_seen[v] : false; }
  unsigned sumAll() const {
    unsigned s = 0;
    for (int i = 0; i < MAXV; i++) if (m_seen[i]) s += m_tics[i];
    return s;
  }

private:
  unsigned m_tics[MAXV];
  bool m_seen[MAXV];
  int m_cur;
  unsigned m_curTics;
  unsigned m_span;
};

// THE ACHIEVED MULTIPLIER: the Fire rate PERCENTAGE cannot be made true (AA has spent the headroom), so the ROW
// reports what the weapon ACTUALLY gets, MEASURED never modelled. `stockSpan` is TotalTime - 1 only without
// WEAPON_RESET, else the cycle cuts at TotalTime - HoldDelay (player.cpp:4716-4723).
const int EK_RATE_ACHIEVED_NONE = -1;
const int EK_RATE_MIN_SAMPLES = 3;
const int EK_RATE_SPAN_MAX = 4096;

// THE FIGURE IS THE MEAN, NOT THE LAST CYCLE: one cycle's span flipped the chaingun between 1.2x and 1.1x. `spanSum`
// is the SUM of every published cycle's span since the last reset, `samples` its count. ROUNDED DOWN to one decimal
// twice over, because claiming 1.2x when it is 1.19x overstates. Equal means give 100 and that IS a number.
inline int ekm_rate_achieved_x100(int stockSpan, int spanSum, int samples) {
  if (samples < EK_RATE_MIN_SAMPLES) return EK_RATE_ACHIEVED_NONE;
  if (stockSpan <= 0 || stockSpan > EK_RATE_SPAN_MAX) return EK_RATE_ACHIEVED_NONE;
  if (spanSum <= 0) return EK_RATE_ACHIEVED_NONE;
  if (spanSum > stockSpan * samples) return EK_RATE_ACHIEVED_NONE;
  return ((stockSpan * samples * 100) / spanSum / 10) * 10;
}

// ONE PURE STEP for accumulate/reset/halve, once per PUBLISHED cycle. A FRESH START covers `*samples <= 0` and
// `stock != *accStock` (AA rewrote TotalTime mid-weapon; a mean across two stocks describes neither). HALVE-THEN-ADD
// keeps the mean tracking a held weapon.
inline void ekm_rate_accum_publish(int* spanSum, int* samples, int* accStock,
                                   int span, int stock, int samplesMax) {
  if (!spanSum || !samples || !accStock) return;
  if (*samples <= 0 || stock != *accStock) {
    *spanSum = 0;
    *samples = 0;
    *accStock = stock;
  }
  if (*samples >= samplesMax) {
    *spanSum /= 2;
    *samples /= 2;
  }
  *spanSum += span;
  (*samples)++;
}

// A CYCLE THAT MAY NOT BE PUBLISHED: a span that is too SHORT inflates stock/measured. Two sources: the engine zeroes
// kickback_pic mid-cycle WITHOUT changing curr_weapon (sector.cpp:2878), and a hitch or alt-tab drops tics out of the
// span. A SUSPECT cycle publishes nothing; a wrong multiplier is what this feature exists to stop.

// ototalclock ADVANCES BY FOUR PER TIC (game.cpp:7122, TICSPERFRAME = TICRATE/REALGAMETICSPERSEC = 120/30): a
// protocol constant read from the engine's source, not an address. A DELTA needs the number; "changed at all" does
// not.
const int EK_OTOTALCLOCK_PER_TIC = 4;

// Whether this frame's raw clock delta means the span can no longer be trusted: nothing moved (a sub-tic frame) and
// exactly one tic are the only innocent answers. More than one tic, or a discontinuity (a level load zeroes
// ototalclock, premap.cpp:1698), or no previous reading, is suspect.
inline bool ekm_tic_delta_suspect(int prevTic, int curTic, bool havePrev) {
  if (!havePrev) return true;
  const int d = curTic - prevTic;
  return d != 0 && d != EK_OTOTALCLOCK_PER_TIC;
}

// A HELD AUTOMATIC NEVER RETURNS TO 0 (player.cpp:4659-4666: the engine sets weaponFrame = 1 in the SAME tic it
// resets), so a held burst would be ONE bracket. A DECREASE IS THE BOUNDARY: inside a cycle the counter only counts
// up, so any step downwards from a non-zero value ends one (the wrap TotalTime-1 -> 1 as much as a tap's ...3 -> 0).
inline bool ekm_cycle_closes(int prevKick, int kickback) {
  return prevKick > 0 && kickback < prevKick;
}

// May the cycle that just closed become a sample? `lastKick` is the last non-zero counter value, `stockSpan` the
// highest frame a stock cycle can reach. THE COMPLETENESS TERM: a cycle cut short by the engine's mid-cycle zero is
// not a cycle. An UNREAD stock (<= 0) does NOT refuse: the row says the timing could not be read.
inline bool ekm_cycle_publishable(bool suspect, bool mixed, int lastKick,
                                  int stockSpan) {
  if (suspect || mixed) return false;
  if (stockSpan <= 0) return true;
  return lastKick >= stockSpan;
}

// A RELOAD IS NOT A FIRE CYCLE: the pistol's last shot runs into the reload ladder without kickback_pic returning to
// 0 (span 24 vs stock 8). The engine resets at `>= TotalTime` (player.cpp:4622), so a fire cycle never passes
// TotalTime - 1; only the ladder (Reload > TotalTime) does.
inline bool ekm_cycle_is_reload(int maxKick, int totalTime) {
  return totalTime > 0 && maxKick > totalTime - 1;
}

enum { EK_CLOSE_IGNORE = 0, EK_CLOSE_PUBLISH = 1 };

inline int ekm_cycle_close_decision(bool isReload, bool publishable) {
  if (isReload) return EK_CLOSE_IGNORE;
  return publishable ? EK_CLOSE_PUBLISH : EK_CLOSE_IGNORE;
}

// The row's answer when it has no multiplier. Distinct states, because merging them claimed a PERMANENT property for
// a weapon that does get a speed-up: an unpublished stop set ("about a second at startup") is not "no speed-up", and
// with no located tic clock every cycle is suspect so nothing can EVER publish.
enum { EK_ACH_MEASURING = 0, EK_ACH_NO_SPEEDUP = 1, EK_ACH_NOT_KNOWN_YET = 2,
       EK_ACH_NO_CLOCK = 3 };

// `hasStaticRow` is false only for weapons 5, 8 and 10; `execRefused` holds while the Executioner is up; both are
// permanent. A declined derivation lands in NOT-KNOWN-YET on purpose ("no speed-up" would claim something about the
// weapon). PERMANENT-FIRST: an unlocated clock never resolves, an unpublished stop set does in a second.
inline int ekm_ach_state(bool hasStaticRow, bool sourceRefused, bool execRefused,
                         bool clockLocated) {
  if (!hasStaticRow || execRefused) return EK_ACH_NO_SPEEDUP;
  if (!clockLocated) return EK_ACH_NO_CLOCK;
  if (sourceRefused) return EK_ACH_NOT_KNOWN_YET;
  return EK_ACH_MEASURING;
}

// THE SLOT'S NAME, from the engine's weapon enum (AA reskins slots per character; slot 3 carries two weapons). 0 for
// a slot with no name: a mis-indexing caller prints nothing rather than the wrong weapon.
inline const char* ekm_weapon_slot_name(int weapon) {
  switch (weapon) {
    case 0:  return "Mighty Foot";
    case 1:  return "Pistol";
    case 2:  return "Shotgun";
    case 3:  return "Chaingun";
    case 4:  return "RPG";
    case 5:  return "Pipe bomb";
    case 6:  return "Shrinker";
    case 7:  return "Devastator";
    case 8:  return "Trip bomb";
    case 9:  return "Freezethrower";
    case 10: return "Remote";
    case 11: return "Expander";
    case 12: return "Flamethrower";
    default: return 0;
  }
}


struct EkAchTuple { int weapon, stockSpan, span, samples, state, x100; };

inline bool ekm_ach_tuple_changed(bool havePrev, const EkAchTuple& prev,
                                  const EkAchTuple& cur) {
  if (!havePrev) return true;
  return prev.weapon != cur.weapon || prev.stockSpan != cur.stockSpan ||
         prev.span != cur.span || prev.samples != cur.samples ||
         prev.state != cur.state || prev.x100 != cur.x100;
}

struct EkStockFieldRead { bool ttOk, hdOk, flOk; int stock; };

inline bool ekm_stock_fields_changed(bool havePrev, const EkStockFieldRead& prev,
                                     const EkStockFieldRead& cur) {
  if (!havePrev) return true;
  return prev.ttOk != cur.ttOk || prev.hdOk != cur.hdOk ||
         prev.flOk != cur.flOk || prev.stock != cur.stock;
}

// WHY a closed cycle did or did not leave a clean measurement, as one name. UNREADABLE_STOCK is deliberately its OWN
// bucket, not folded into PUBLISHED, though ekm_cycle_publishable's `stockSpan <= 0` test returns true there: folding
// it in would hide exactly the number the FLAGS hypothesis needs. The names are mutually exclusive by the if-chain.
enum { EK_CYCLE_PUBLISHED = 0, EK_CYCLE_SUSPECT = 1, EK_CYCLE_MIXED = 2,
       EK_CYCLE_INCOMPLETE = 3, EK_CYCLE_UNREADABLE_STOCK = 4,
       EK_CYCLE_RELOAD = 5 };

inline int ekm_cycle_close_reason(bool suspect, bool mixed, int lastKick,
                                  int stockSpan, int maxKick, int totalTime) {
  if (ekm_cycle_is_reload(maxKick, totalTime)) return EK_CYCLE_RELOAD;
  if (suspect) return EK_CYCLE_SUSPECT;
  if (mixed) return EK_CYCLE_MIXED;
  if (stockSpan <= 0) return EK_CYCLE_UNREADABLE_STOCK;
  if (lastKick < stockSpan) return EK_CYCLE_INCOMPLETE;
  return EK_CYCLE_PUBLISHED;
}

inline const char* ekm_cycle_close_reason_name(int reason) {
  switch (reason) {
    case EK_CYCLE_PUBLISHED:         return "published";
    case EK_CYCLE_SUSPECT:           return "suspect";
    case EK_CYCLE_MIXED:             return "mixed";
    case EK_CYCLE_INCOMPLETE:        return "incomplete";
    case EK_CYCLE_UNREADABLE_STOCK:  return "unreadable-stock";
    case EK_CYCLE_RELOAD:            return "reload";
    default:                         return "unknown";
  }
}

inline const char* ekm_ach_state_name(int state) {
  switch (state) {
    case EK_ACH_MEASURING:     return "measuring";
    case EK_ACH_NO_SPEEDUP:    return "no-speedup";
    case EK_ACH_NOT_KNOWN_YET: return "not-known-yet";
    case EK_ACH_NO_CLOCK:      return "no-clock";
    default:                   return "unknown";
  }
}

// RELOAD SPEED: Fire rate no longer shortens reloads. Every post-tic frame at or above TotalTime is reloading (the
// engine's ladder, player.cpp:4622-4654, where Reload > TotalTime, and AA's CON reload frames otherwise). TotalTime-1
// is always a held stop, so one write never crosses from firing into reloading.
enum { EK_KICK_FIRE = 0, EK_KICK_RELOAD = 1, EK_KICK_UNREAD = 2 };

// THE RELOAD ROW'S OWN RANGE: 100-1000 in 50s, plus Max, which advances to the next park every tic. The stop set
// guards every frame whatever the step count (firerate_advance clamps). Max is stored as a sentinel.
const int RELOADRATE_MAX = 1000;
const int RELOADRATE_UNBOUNDED = 9999;
const int RELOADRATE_SLIDER_MAX = RELOADRATE_MAX + FIRERATE_STEP;
const int RELOADRATE_MAX_STEPS = 4096;

inline int reloadrate_clamped(int v) {
  if (v >= RELOADRATE_UNBOUNDED) return RELOADRATE_UNBOUNDED;
  if (v > RELOADRATE_MAX)
    return (v - RELOADRATE_MAX < RELOADRATE_UNBOUNDED - v) ? RELOADRATE_MAX : RELOADRATE_UNBOUNDED;
  if (v < FIRERATE_MIN) v = FIRERATE_MIN;
  return ((v - FIRERATE_MIN + FIRERATE_STEP / 2) / FIRERATE_STEP) * FIRERATE_STEP + FIRERATE_MIN;
}

inline int reloadrate_to_slider(int stored) {
  const int r = reloadrate_clamped(stored);
  return r == RELOADRATE_UNBOUNDED ? RELOADRATE_SLIDER_MAX : r;
}
inline int reloadrate_from_slider(int pos) {
  if (pos >= RELOADRATE_SLIDER_MAX - FIRERATE_STEP / 2) return RELOADRATE_UNBOUNDED;
  return reloadrate_clamped(pos > RELOADRATE_MAX ? RELOADRATE_MAX : pos);
}

inline int reloadrate_steps(int ratePct, double* carry) {
  const int r = reloadrate_clamped(ratePct);
  if (r == RELOADRATE_UNBOUNDED) {
    if (carry) *carry = 0.0;
    return RELOADRATE_MAX_STEPS;
  }
  return firerate_steps_extra((r - 100) / 100.0, carry);
}

// Reload is not a term: the reload region's stops protect the CON frames above TotalTime whatever the row says. Which
// set applies there is ekm_firestops_for_region (firestops.h).
inline int firerate_kick_region(int k, bool ttOk, int totalTime) {
  if (!ttOk || totalTime < 1) return EK_KICK_UNREAD;
  return k < totalTime ? EK_KICK_FIRE : EK_KICK_RELOAD;
}

inline int firerate_region_rate(int region, int fireRate, int reloadRate) {
  const int f = firerate_rate_clamped(fireRate), r = reloadrate_clamped(reloadRate);
  if (region == EK_KICK_FIRE) return f;
  if (region == EK_KICK_RELOAD) return r;
  return f < r ? f : r;
}

inline int firerate_region_steps(int region, int fireRate, int reloadRate,
                                 double* fireCarry, double* reloadCarry) {
  if (region == EK_KICK_RELOAD) return reloadrate_steps(reloadRate, reloadCarry);
  return firerate_steps(firerate_region_rate(region, fireRate, reloadRate), fireCarry);
}

inline int firerate_kick_gate_rate(int fireRate, int reloadRate) {
  const int f = firerate_rate_clamped(fireRate), r = reloadrate_clamped(reloadRate);
  return f > r ? f : r;
}

// The reload speed-up at `rate`: stock ladder tics (Reload - TotalTime) over tics replayed from TotalTime, x100. NONE
// when nothing can be skipped. UNBOUNDED (`/?`) when Reload <= TotalTime: the reload is AA's CON and no frame bounds
// it (DBLOAD/DBCLOSE, and :10326's saber write on 98/99).
const int EK_RATE_PREDICT_UNBOUNDED = -2;
inline int firerate_predict_reload_x100(const FireRateWeapon& t, int rate,
                                        int totalTime, int reload) {
  if (t.nEvents <= 0 || t.nEvents > FIRERATE_EVENTS_MAX) return 100;
  if (totalTime < 1 || reload > FIRERATE_PREDICT_END_MAX) return 100;
  if (reloadrate_clamped(rate) <= 100) return 100;
  if (reload <= totalTime) return EK_RATE_PREDICT_UNBOUNDED;
  const int stock = reload - totalTime;
  double carry = 0.0;
  int k = totalTime, total = 0;
  while (k < reload && total < stock) {
    k = firerate_advance(t, k, reloadrate_steps(rate, &carry)) + 1;
    total++;
  }
  if (total >= stock) return EK_RATE_ACHIEVED_NONE;
  return (stock * 100 + total / 2) / total;
}

inline void reloadrate_value_text(int rate, bool havePred, int x100, char* out, int cap) {
  if (!out || cap <= 0) return;
  const int r = reloadrate_clamped(rate);
  char head[16];
  if (r == RELOADRATE_UNBOUNDED) _snprintf_s(head, sizeof(head), _TRUNCATE, "Max");
  else _snprintf_s(head, sizeof(head), _TRUNCATE, "%d%%", r);
  if (!havePred)
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "%s", head);
  else if (x100 == EK_RATE_ACHIEVED_NONE)
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "%s/none", head);
  else if (x100 == EK_RATE_PREDICT_UNBOUNDED)
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "%s/?", head);
  else
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "%s/%d.%dx", head, x100 / 100, (x100 / 10) % 10);
}

// AA'S COUNTER-DRIVEN RELOADS: Shelly/Mando's carbine, Wes's Uzi never run the ladder: AA holds kickback_pic at 0 and
// counts its own per-player gamevar until `ifg m4reload 40` / `ifg uzireload 35` refills the clip. The frames between
// are sounds, casings and HUD only.
enum { EK_RCTR_NONE = 0, EK_RCTR_M4 = 1, EK_RCTR_UZI = 2 };
const int EK_RCTR_WEAPON = 3;
const int EK_RCTR_M4_REFILL = 40;
const int EK_RCTR_UZI_REFILL = 35;

// Which counter AA's own gates run, or NONE. Carbine pchar 1 or 3, Uzi pchar 2. NO upgrade-bit test: the CON gate is a
// chained `if A if B X else Y` whose else pairs with BOTH ifs; re-adding a bit-8 test made it inert. `execRefused`
// (gotshock & 2, or unreadable) is a superset of AA's zeroing condition.
inline int ekm_reload_counter_kind(int weapon, bool execRefused, int pchar) {
  if (weapon != EK_RCTR_WEAPON || execRefused) return EK_RCTR_NONE;
  if (pchar == 1 || pchar == 3) return EK_RCTR_M4;
  if (pchar == 2) return EK_RCTR_UZI;
  return EK_RCTR_NONE;
}
inline int ekm_reload_counter_refill(int kind) {
  if (kind == EK_RCTR_M4) return EK_RCTR_M4_REFILL;
  if (kind == EK_RCTR_UZI) return EK_RCTR_UZI_REFILL;
  return 0;
}

// The value to leave in the counter after `tics` tics at `rate`, or `counter` unchanged for no write. CAPPED AT
// `refill`, NOT refill+1: AA adds before it tests, so 40 refills next tic. 0 is AA's rest and past `refill` the
// refill is due; neither is touched and neither spends the carry.
inline int reloadcounter_advance(int counter, int refill, int rate, int tics,
                                 double* carry) {
  if (refill < 1 || counter <= 0 || counter > refill) return counter;
  if (reloadrate_clamped(rate) <= 100 || tics <= 0) return counter;
  int steps = 0;
  for (int i = 0; i < tics && i < FIRERATE_TICS_MAX; i++) {
    steps += reloadrate_steps(rate, carry);
    if (steps >= RELOADRATE_MAX_STEPS) { steps = RELOADRATE_MAX_STEPS; break; }
  }
  const int target = counter + steps;
  return target > refill ? refill : target;
}

// The Reload speed row's figure for a counter reload: stock tics over replayed tics, x100. The start tic is AA's
// alone (`set 1` then `add 1`), so the replay starts from 2 with one tic spent. fastfire's second `add` is not
// modelled.
inline int reloadcounter_predict_x100(int refill, int rate) {
  if (refill < 2 || refill > FIRERATE_PREDICT_END_MAX) return 100;
  if (reloadrate_clamped(rate) <= 100) return 100;
  double carry = 0.0;
  int k = 2, total = 1;
  while (k <= refill && total < refill) {
    k = reloadcounter_advance(k, refill, rate, 1, &carry) + 1;
    total++;
  }
  return (refill * 100 + total / 2) / total;
}

void weaponspeed_frame(void);

bool weaponspeed_engine_tic(int* out);

// THE READOUT'S ONE CROSSING OF THE TWO CLOCKS: written on the GAME thread, drawn on the RENDER thread, crossing as
// INTERLOCKED SCALARS, never a struct or pointer. False when there is nothing coherent to show; out-params are
// written only on true. `outMeasuredSpan` is the RUNNING SUM, passed to ekm_rate_achieved_x100 with `outSamples`.
bool weaponspeed_achieved(int* outWeapon, int* outStockSpan, int* outMeasuredSpan,
                          int* outSamples, int* outState);

// THE PREDICTION for the held weapon at `rate`: firerate_predict_x100 over the stops the advance would use now, or
// EK_RATE_ACHIEVED_NONE in `outX100` when the weapon is refused or has no stops. False for rate 100, no player, or an
// unreadable stock span. SwapBuffers thread only.
bool weaponspeed_predicted(int rate, int* outWeapon, int* outX100);

bool weaponspeed_reload_predicted(int rate, int* outWeapon, int* outX100);
