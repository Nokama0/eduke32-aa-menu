#pragma once

#include "weapondata.h"
#include "con_stops_aa.h"
#include "con_shots_aa.h"
#include "con_reload_aa.h"

// The fire-rate stops, computed from the engine's own weapon data. weaponspeed.h's table is stock Duke's frame
// numbers; Alien Armageddon rewrites them per character, per purchased upgrade and mid-session (seven of ten rows
// disagreed on hardware, two moved again within one session).

const int EK_WF_HOLSTER_CLEARS_CLIP = 0x00000001;
const int EK_WF_GLOWS               = 0x00000002;
const int EK_WF_AUTOMATIC           = 0x00000004;
const int EK_WF_FIREEVERYOTHER      = 0x00000008;
const int EK_WF_FIREEVERYTHIRD      = 0x00000010;
const int EK_WF_RANDOMRESTART       = 0x00000020;
const int EK_WF_AMMOPERSHOT         = 0x00000040;
const int EK_WF_BOMB_TRIGGER        = 0x00000080;
const int EK_WF_NOVISIBLE           = 0x00000100;
const int EK_WF_THROWIT             = 0x00000200;
const int EK_WF_CHECKATRELOAD       = 0x00000400;
const int EK_WF_STANDSTILL          = 0x00000800;
const int EK_WF_SPAWNTYPE2          = 0x00001000;
const int EK_WF_SPAWNTYPE3          = 0x00002000;
const int EK_WF_SEMIAUTO            = 0x00004000;
const int EK_WF_RELOAD_TIMING       = 0x00008000;
const int EK_WF_RESET               = 0x00010000;

// The two WorksLike values that never reach the counting path: P_ProcessWeapon gives the pipebomb and hand remote
// arms of their own (dukeweapon_t, inv.h).
const int EK_WL_HANDBOMB   = 5;
const int EK_WL_HANDREMOTE = 10;

// A frame count outside this is not a frame count: it bounds the derivation's arithmetic before any runs, so a
// garbage read cannot produce a loop.
const int EK_STOPS_FIELD_MAX = 4096;

// Insert one stop, ascending, dropping duplicates and anything below 1: firerate_advance refuses a counter at 0 or
// below (the engine's fire press sets it to 1), so a stop there can never be what an advance clamps to.
inline bool ekm_firestops_add(FireRateWeapon* t, int stop,
                              unsigned char prov = EK_PROV_ENGINE) {
  if (!t || stop < 1) return true;
  int i = 0;
  while (i < t->nEvents && t->events[i] < stop) i++;
  const bool had = i < t->nEvents && t->events[i] == stop;
  if (!had) {
    if (t->nEvents >= FIRERATE_EVENTS_MAX) return false;
    for (int j = t->nEvents; j > i; j--) { t->events[j] = t->events[j - 1]; t->con[j] = t->con[j - 1]; }
    t->events[i] = stop;
    t->nEvents++;
  }
  const unsigned char merged = (unsigned char)(firerate_prov(prov) |
                                               (had ? firerate_prov(t->con[i]) : 0));
  t->con[i] = (merged == EK_PROV_ENGINE) ? 0 : merged;
  return true;
}

// Whether a frame may be CON_FRAME-only. Skipping at k == F assumes the CON's test saw F; a literal
// `setp[].kickback_pic F` in AA's CON lands there with no test run, so a frame any write can reach stays CON_EDGE.
inline bool ekm_con_frame_only_safe(int frame, const int* writes, int nWrites) {
  if (frame < 1 || nWrites < 0) return false;
  if (nWrites > 0 && !writes) return false;
  for (int i = 0; i < nWrites; i++) if (writes[i] == frame) return false;
  return true;
}

inline int ekm_firestops_at_risk(const FireRateWeapon& t, int totalTime,
                                 const EkConStops& cs, char* out, int cap) {
  if (out && cap > 0) out[0] = 0;
  int used = 0, n = 0;
  for (int i = 0; i < t.nEvents && i < FIRERATE_EVENTS_MAX; i++) {
    const int v = t.events[i];
    if (v > totalTime || v == totalTime - 1 || firerate_prov_has_con(t.con[i])) continue;
    bool inCon = false;
    for (int j = 0; j < cs.nStops; j++) if (cs.stops[j] == v) { inCon = true; break; }
    if (!inCon) continue;
    n++;
    if (out && cap > 0 && used < cap - 1) {
      const int w = _snprintf_s(out + used, (size_t)(cap - used), _TRUNCATE,
                                "%s%d", n > 1 ? "," : "", v);
      if (w >= 0) used += w;
    }
  }
  return n;
}

// WEAPON_RESET's hold cut-off as one expression with two readers: player.cpp sets the counter to !!fire in the same
// tic it passes TotalTime - HoldDelay, so it is both a stop and the highest post-tic frame a cycle can show.
inline int ekm_reset_cutoff(int totalTime, int holdDelay) {
  const int s = totalTime - holdDelay;
  return (s > totalTime - 1) ? totalTime - 1 : s;
}

// The highest kickback_pic a stock cycle leaves, which is also its length in tics (the counter steps once a tic from
// 1). -1 refuses: the readout then says the timing could not be read rather than divide by an invented number.
inline int ekm_stock_cycle_span(int totalTime, int holdDelay, int flags) {
  if (totalTime < 1 || totalTime > EK_STOPS_FIELD_MAX) return -1;
  if (holdDelay < 0 || holdDelay > totalTime) return -1;
  const bool cutoffApplies = (flags & EK_WF_RESET) && (flags & EK_WF_AUTOMATIC)
                             && !(flags & EK_WF_SEMIAUTO);
  const int s = cutoffApplies ? ekm_reset_cutoff(totalTime, holdDelay)
                              : totalTime - 1;
  return (s < 1) ? -1 : s;
}

// Whether held fire rests between cycles: with ammo and fire held, AUTOMATIC and RESET put the counter straight back
// to 1; anything else leaves it 0.
inline bool ekm_cycle_carry_restarts(int flags) {
  return (flags & (EK_WF_AUTOMATIC | EK_WF_RESET)) == 0;
}

// One weapon's stop set from one weapon's live fields. False means no speed-up for this weapon, never a fall back to
// stock's numbers (on AA they are the defect itself).
inline bool ekm_firestops_derive(int weapon, const EkWeaponRow& row,
                                 FireRateWeapon* out, bool reloadSounds = true) {
  if (!out) return false;
  out->weapon = weapon;
  out->nEvents = 0;
  // con[] is zeroed beside events[]: every slot below nEvents is written by ekm_firestops_add, but `out` is caller
  // memory of unknown history and the returned record should not depend on that invariant.
  for (int i = 0; i < FIRERATE_EVENTS_MAX; i++) { out->events[i] = 0; out->con[i] = 0; }

  // Every numeric field must read: each of the six carries a stop, and one that cannot be computed cannot be guessed
  // low without skipping the action it stands for.
  const int kNeed[6] = { EK_WD_FIREDELAY, EK_WD_TOTALTIME, EK_WD_RELOAD,
                         EK_WD_SPAWNTIME, EK_WD_SOUND2TIME, EK_WD_HOLDDELAY };
  for (int i = 0; i < 6; i++) {
    const int f = kNeed[i];
    if (!row.got[f]) return false;
    if (row.v[f] < 0 || row.v[f] > EK_STOPS_FIELD_MAX) return false;
  }
  if (!row.got[EK_WD_WORKSLIKE]) return false;

  const int fd = row.v[EK_WD_FIREDELAY];
  const int tt = row.v[EK_WD_TOTALTIME];
  const int rl = row.v[EK_WD_RELOAD];
  const int st = row.v[EK_WD_SPAWNTIME];
  const int s2 = row.v[EK_WD_SOUND2TIME];
  const int hd = row.v[EK_WD_HOLDDELAY];
  const int wl = row.v[EK_WD_WORKSLIKE];
  // FLAGS is the one field allowed not to read, and the direction is keep the stop: every bit reads as set, which
  // turns the window into stops and refuses an advance through it rather than dropping a conditional one.
  const int fl = row.got[EK_WD_FLAGS] ? row.v[EK_WD_FLAGS] : ~0;

  if (tt < 1) return false;
  if (wl == EK_WL_HANDBOMB || wl == EK_WL_HANDREMOTE) return false;

  // The five unconditional stops: FireDelay is P_CheckWeapon and the shot itself; TotalTime the reset; Reload is
  // P_CheckWeapon under CHECKATRELOAD and the ladder's reset; SpawnTime is P_DoWeaponSpawn; Sound2Time is
  // A_PlaySound.
  if (!ekm_firestops_add(out, fd - 1)) return false;
  if (!ekm_firestops_add(out, tt - 1)) return false;
  if (!ekm_firestops_add(out, rl - 1)) return false;
  if (!ekm_firestops_add(out, st - 1)) return false;
  if ((reloadSounds || s2 - 1 < tt) && !ekm_firestops_add(out, s2 - 1)) return false;

  // WEAPON_RESET's hold cut-off is a strict `>` on TotalTime - HoldDelay, so the stop is that value, not one less.
  // Value and upper clamp are ekm_reset_cutoff above; the FireDelay clamp below is this derivation's alone.
  if (fl & EK_WF_RESET) {
    int s = ekm_reset_cutoff(tt, hd);
    if (s < fd - 1) s = fd - 1;
    if (!ekm_firestops_add(out, s)) return false;
  }

  // WEAPON_STANDSTILL pins the player's z on every frame below FireDelay + 1: an engine write on each, not counting,
  // which is why weaponspeed.h excludes the tripbomb by hand.
  if (fl & EK_WF_STANDSTILL) {
    for (int k = 1; k <= fd - 1; k++)
      if (!ekm_firestops_add(out, k)) return false;
  }

  // FIREEVERYTHIRD fires on every third frame of the window and FIREEVERYOTHER on every frame of it (also under
  // SEMIAUTO).
  if ((fl & EK_WF_AUTOMATIC) && (fl & EK_WF_FIREEVERYTHIRD)) {
    for (int f = fd; f < tt; f++)
      if ((f % 3) == 0 && !ekm_firestops_add(out, f - 1)) return false;
  }
  if ((fl & EK_WF_AUTOMATIC) && (fl & EK_WF_FIREEVERYOTHER)) {
    for (int f = fd; f < tt; f++)
      if (!ekm_firestops_add(out, f - 1)) return false;
  }

  // The reload ladder; `Reload > TotalTime` is what makes it reachable: without it the else arm resets the counter at
  // TotalTime and no frame beyond exists.
  if (rl > tt && reloadSounds) {
    if (!ekm_firestops_add(out, tt)) return false;
    const int rt = rl - tt;
    const int f = (fl & EK_WF_RELOAD_TIMING) ? (rl - rt + 4) : (rl - rt / 3);
    if (!ekm_firestops_add(out, f - 1)) return false;
  }
  return true;
}

// Part B, AA's in-cycle CON shots by configuration: one worker-thread reading of what con_shots_aa.h's gates test.
// `*Ok` false is a failed read; `up[a]` is character a's upgrade element for this weapon's slot.
struct EkShotInputs {
  bool pcharOk;  int pchar;
  bool ffOk;     int ff;
  bool gotdbOk;  int gotdb;
  bool upOk[EK_GA_UP_COUNT]; int up[EK_GA_UP_COUNT];
};

enum { EK_GATE_FALSE = 0, EK_GATE_TRUE = 1, EK_GATE_UNKNOWN = 2 };

// A conjunction. A term READ false closes it whatever else is unread; an unread term otherwise leaves it unknown,
// which the caller treats as open: a stop too many costs speed, a stop too few loses the shot.
inline int ekm_shot_gate(const EkConShot& s, const EkShotInputs& in,
                         const EkWeaponRow& row) {
  bool unknown = false;
  if (!in.pcharOk) unknown = true;
  else if (in.pchar != s.pchar) return EK_GATE_FALSE;
  if (s.upArray != EK_SHOT_NONE) {
    if (s.upArray < 0 || s.upArray >= EK_GA_UP_COUNT || !in.upOk[s.upArray]) {
      unknown = true;
    } else {
      const int v = in.up[s.upArray];
      if ((v & s.upSet) != s.upSet || (v & s.upClear) != 0) return EK_GATE_FALSE;
    }
  }
  if (s.gotdbClear) {
    if (!in.gotdbOk) unknown = true;
    else if (in.gotdb & s.gotdbClear) return EK_GATE_FALSE;
  }
  if (s.shoots != EK_SHOT_NONE) {
    if (!row.got[EK_WD_SHOOTS]) unknown = true;
    else if (row.v[EK_WD_SHOOTS] != s.shoots) return EK_GATE_FALSE;
  }
  return unknown ? EK_GATE_UNKNOWN : EK_GATE_TRUE;
}

inline int ekm_shot_frame(const EkConShot& s, int ff) {
  return ff > 0 ? s.frameFast : s.frameIdle;
}

// One row's stops; fastfire decides their shape. While fastfire > 0, AAPLAYER.CON adds one to kickback_pic in most
// tics before the shot tests, so a test sees the hook's k plus two.
inline bool ekm_shot_apply(const EkConShot& s, const EkShotInputs& in,
                           const EkWeaponRow& row, FireRateWeapon* out) {
  if (ekm_shot_gate(s, in, row) == EK_GATE_FALSE) return true;
  const bool fast = in.ffOk && in.ff > 0;
  if (!fast) {
    const EkConWrites cw = ekm_con_writes_aa(s.weapon);
    int writes[EKM_CON_WRITES_MAX + 1];
    int n = 0;
    for (int i = 0; i < cw.n && i < EKM_CON_WRITES_MAX; i++) writes[n++] = cw.v[i];
    if (s.fdPlus1Write && row.got[EK_WD_FIREDELAY]) writes[n++] = row.v[EK_WD_FIREDELAY] + 1;
    const int f = s.frameIdle;
    const bool safe = in.ffOk && !s.anyWrite && ekm_con_frame_only_safe(f, writes, n);
    const unsigned char prov = (unsigned char)(EK_PROV_CON_FRAME | (safe ? 0 : EK_PROV_CON_EDGE));
    if (!ekm_firestops_add(out, f, prov)) return false;
  }
  if (fast || !in.ffOk) {
    if (!ekm_firestops_add(out, s.frameFast - 1, EK_PROV_CON_EDGE)) return false;
  }
  return true;
}

// Three published variants. Steroids flip fastfire (set every tic) far faster than the 1 Hz sample, so the worker
// derives a set for each and the hook picks one per advance from a live fastfire read; a failed read takes UNREAD
// (both placements).
enum { EK_SHOT_VAR_IDLE = 0, EK_SHOT_VAR_FAST = 1, EK_SHOT_VAR_UNREAD = 2,
       EK_SHOT_VAR_COUNT = 3 };

// `ff >= 2`, not `> 0`: the hook reads fastfire after a tic, and the next tic's `sub fastfire 1` runs before its test
// and the shot tests, so a read of 1 counts as 0. Its only writers sit after the shot tests in the same APLAYER
// actor.
inline int ekm_shot_variant(bool ffOk, int ff) {
  if (!ffOk) return EK_SHOT_VAR_UNREAD;
  return ff >= 2 ? EK_SHOT_VAR_FAST : EK_SHOT_VAR_IDLE;
}

inline EkShotInputs ekm_shot_inputs_for(const EkShotInputs& in, int variant) {
  EkShotInputs r = in;
  r.ffOk = variant != EK_SHOT_VAR_UNREAD;
  r.ff = variant == EK_SHOT_VAR_FAST ? 1 : 0;
  return r;
}

inline bool ekm_firestops_derive_aa(int weapon, const EkWeaponRow& row,
                                    bool conMatches, FireRateWeapon* out,
                                    const EkShotInputs* shots = 0) {
  if (!ekm_firestops_derive(weapon, row, out)) return false;
  const int tt = row.v[EK_WD_TOTALTIME];
  if (!ekm_firestops_add(out, tt - 1, EK_PROV_CON_FRAME)) return false;
  if (!conMatches) return true;
  const EkConStops cs = ekm_con_stops_aa(weapon);
  for (int i = 0; i < cs.nStops; i++) {
    if (cs.stops[i] <= tt) continue;
    if (!ekm_firestops_add(out, cs.stops[i], EK_PROV_CON_EDGE)) return false;
  }
  if (!shots) return true;
  const EkConShot* const rows = ekm_con_shots();
  for (int i = 0; i < EKM_CON_SHOT_COUNT; i++) {
    if (rows[i].weapon != weapon) continue;
    if (!ekm_shot_apply(rows[i], *shots, row, out)) return false;
  }
  return true;
}

enum { EK_RSET_OK = 0, EK_RSET_NO_CON = 1, EK_RSET_SLOT = 2, EK_RSET_PCHAR = 3,
       EK_RSET_FLOOR = 4, EK_RSET_DERIVE = 5 };

inline const char* ekm_rset_name(int code) {
  switch (code) {
    case EK_RSET_OK:     return "ok";
    case EK_RSET_NO_CON: return "con mismatch";
    case EK_RSET_SLOT:   return "slot not in the ledger";
    case EK_RSET_PCHAR:  return "pchar unread";
    case EK_RSET_FLOOR:  return "TotalTime below the ledger's floor";
    case EK_RSET_DERIVE: return "derivation declined";
    default:             return "unknown";
  }
}

// One-tic equality frames. An `ife F` row is stored as CON_EDGE at F-1, parked at F-2: two held tics. CON_FRAME at F
// parks at F-1 and releases once the tic has shown F, one tic, but only while nothing moves the counter past F
// unseen.
const int EK_AA_PERK_RELOAD = 2048;
enum { EK_RSET_VAR_HELD = 0, EK_RSET_VAR_ONE_TIC = 1, EK_RSET_VAR_COUNT = 2 };

// fastfire as ekm_shot_variant reads it; perks only matters on the pistol. Unread is held. LASERMODE, which also
// gates the perk arm, is not read: held is the safe side.
inline int ekm_reload_variant(int weapon, bool ffOk, int ff, bool perksOk, int perks) {
  if (ekm_shot_variant(ffOk, ff) != EK_SHOT_VAR_IDLE) return EK_RSET_VAR_HELD;
  if (weapon == 1 && (!perksOk || (perks & EK_AA_PERK_RELOAD) != 0)) return EK_RSET_VAR_HELD;
  return EK_RSET_VAR_ONE_TIC;
}

inline bool ekm_reload_row_one_tic(const EkReloadRow& r, int weapon, const EkWeaponRow& row) {
  if (!r.op || (strcmp(r.op, "ife") != 0 && strcmp(r.op, "ifvare") != 0)) return false;
  const EkConWrites cw = ekm_con_writes_aa(weapon);
  int writes[EKM_CON_WRITES_MAX + 1];
  int n = 0;
  for (int i = 0; i < cw.n && i < EKM_CON_WRITES_MAX; i++) writes[n++] = cw.v[i];
  if (weapon == 1) {
    if (!row.got[EK_WD_FIREDELAY]) return false;
    writes[n++] = row.v[EK_WD_FIREDELAY] + 1;
  }
  return ekm_con_frame_only_safe(r.frame, writes, n);
}

// The engine's state stops (no reload sounds), TotalTime-1 as CON_FRAME, and the live character's E and U rows from
// con_reload_aa.h as CON_EDGE (con_stops_aa.h's provenance), so the park rule is unchanged.
inline int ekm_firestops_derive_reload_aa(int weapon, const EkWeaponRow& row,
                                          bool conMatches, bool pcharOk, int pchar,
                                          FireRateWeapon* out, bool oneTic = false) {
  if (!out) return EK_RSET_DERIVE;
  if (!conMatches) return EK_RSET_NO_CON;
  const int ledgerFloor = ekm_reload_floor(weapon);
  if (ledgerFloor <= 0) return EK_RSET_SLOT;
  if (!pcharOk || pchar < 0 || pchar >= EKM_RELOAD_CHARS) return EK_RSET_PCHAR;
  if (!ekm_firestops_derive(weapon, row, out, false)) return EK_RSET_DERIVE;
  const int tt = row.v[EK_WD_TOTALTIME];
  if (tt < ledgerFloor) return EK_RSET_FLOOR;
  if (!ekm_firestops_add(out, tt - 1, EK_PROV_CON_FRAME)) return EK_RSET_DERIVE;
  const EkReloadRow* const rows = ekm_reload_rows();
  for (int i = 0; i < EKM_RELOAD_ROW_COUNT; i++) {
    if (!ekm_reload_row_applies(rows[i], weapon, pchar)) continue;
    if (oneTic && ekm_reload_row_one_tic(rows[i], weapon, row)) {
      if (!ekm_firestops_add(out, rows[i].frame, EK_PROV_CON_FRAME)) return EK_RSET_DERIVE;
      continue;
    }
    int s[2] = { 0, 0 };
    const int n = ekm_reload_row_stops(rows[i], s);
    for (int q = 0; q < n; q++)
      if (!ekm_firestops_add(out, s[q], EK_PROV_CON_EDGE)) return EK_RSET_DERIVE;
  }
  return EK_RSET_OK;
}

struct EkReloadStops { int code; int tt; int pchar; FireRateWeapon set; };

// Which set one advance uses. Only the reload region takes the reload set, and only while the live TotalTime and
// pchar still match what the 1 Hz sample derived it from, so another character's frames are never used.
inline const FireRateWeapon* ekm_firestops_for_region(int region, const FireRateWeapon* full,
                                                     const EkReloadStops* rs, int liveTt,
                                                     bool livePcharOk, int livePchar) {
  if (!full) return 0;
  if (region != EK_KICK_RELOAD || !rs || rs->code != EK_RSET_OK) return full;
  if (rs->set.nEvents < 0 || rs->set.nEvents > FIRERATE_EVENTS_MAX) return full;
  if (liveTt != rs->tt || !livePcharOk || livePchar != rs->pchar) return full;
  return &rs->set;
}

// Where a weapon's stops came from. STATIC is no live weapon data at all (failed locate, or no such gamevars): stock
// Duke's and World Tour's behaviour exactly. REFUSE is no speed-up, whether declined or not yet read.
enum { EK_STOPS_STATIC = 0, EK_STOPS_LIVE = 1, EK_STOPS_REFUSE = 2 };

// The sequence-counter protocol's whole decision, pure so it pins without live state. `seq0`/`seq1` are the counter
// before and after the copy; 0 means nothing published yet. "Not published yet" is two situations that must not share
// an answer.
inline int ekm_firestops_verdict(long seq0, long seq1, int nEvents,
                                 bool haveGamevars) {
  if (seq0 == 0) return haveGamevars ? EK_STOPS_REFUSE : EK_STOPS_STATIC;
  if (seq0 & 1) return EK_STOPS_REFUSE;
  if (seq1 != seq0) return EK_STOPS_REFUSE;
  // The refusal marker firestops_publish writes (a negative length) and an impossible length take the same exit. An
  // empty set is neither: firerate_next_event returns -1 for it and nothing is advanced.
  if (nEvents < 0 || nEvents > FIRERATE_EVENTS_MAX) return EK_STOPS_REFUSE;
  return EK_STOPS_LIVE;
}

// The fourth refusal, the only one that is not a whole slot: AA's EXECUTIONER shares weapon slot 3 with the chaingun
// (AADEFS.CON), and the chaingun works.
const int EK_EXECUTIONER_WEAPON = 3;
const int EK_AA_GOTSHOCK_ON_TOP = 2;

// `haveVar` is whether this game's CON declares `gotshock`; `readOk` and `gotshock` are one guarded read for the
// local player. The two failures are different facts and get opposite answers.
inline bool ekm_executioner_refuses(int weapon, bool haveVar, bool readOk,
                                    int gotshock) {
  if (weapon != EK_EXECUTIONER_WEAPON) return false;
  if (!haveVar) return false;
  if (!readOk) return true;
  return (gotshock & EK_AA_GOTSHOCK_ON_TOP) != 0;
}

// The choice in one place so it pins without live memory. A NULL `stat` is the answer for weapons 5, 8 and 10 and
// outranks live data: those get no speed-up by design (weaponspeed.h).
inline const FireRateWeapon* ekm_firestops_choose(int source,
                                                 const FireRateWeapon* stat,
                                                 const FireRateWeapon* live) {
  if (!stat) return 0;
  if (source == EK_STOPS_LIVE) return live;
  if (source == EK_STOPS_STATIC) return stat;
  return 0;
}

// Two clocks, and the set crosses between them. firestops_publish runs on the worker thread from the 1 Hz sample that
// reads these gamevars; firestops_source runs on the game thread inside the SwapBuffers hook. The set is not
// self-describing (nEvents is a length), so it crosses behind a sequence counter and a torn read refuses the frame.
void firestops_publish(int weapon, const EkWeaponRow& row);
int firestops_source(int weapon, FireRateWeapon* out, EkReloadStops* outReload = 0);
