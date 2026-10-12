#pragma once

// The triggerbot yields to the player's weapon switch. Pure: tbot.cpp gathers one TswFrame per frame, applies
// TswStep. A weapon key is an EDGE and a switch is refused unless kickback_pic == 0, which a held fire bit prevents.
// The field we read is the player's unless our own replay is in the record (ourField).

const int EK_SK_WEAPON_SHIFT = 8;
const unsigned EK_SK_WEAPON_MASK = 0xFu << EK_SK_WEAPON_SHIFT;

// 1500 ms, not 0.5 s: the replay waits for kickback_pic to reach 0 and the shotgun's TotalTime is 30 tics (a full
// second). Counted from the LAST frame the key was seen.
const int TSW_TIMEOUT_MS = 1500;
const int TSW_AGE_CAP_MS = 1000000;

const int TSW_IDLE = 0;
const int TSW_PENDING = 1;
const int TSW_REPLAYED = 2;

const int TSW_OUT_NONE = 0;
const int TSW_OUT_ENGINE = 1;
const int TSW_OUT_REPLAYED = 2;
const int TSW_OUT_TIMEOUT = 3;
const int TSW_OUT_REPLACED = 4;
const int TSW_OUT_DROPPED = 5;

struct TswState {
  int phase;
  int slot;           // the SK weapon field value pending, 1..15
  int weaponAt;       // curr_weapon when adopted, -1 unread
  int weaponPosAt;
  int ageMs;
  int ourField;
  int prevEndField;
  int lastConsumed;   // the field the most recent tic took, -1 unknown
};

inline TswState tsw_blank() {
  TswState s = { TSW_IDLE, 0, -1, 0, 0, 0, 0, -1 };
  return s;
}

// One presented frame. live is "not in a menu, not paused, not dead": a record written otherwise is cleared before
// any tic reads it.
struct TswFrame {
  int clockOk;
  int ticAdvanced;
  int field;
  int botHolding;
  int live;
  int animOk;
  int weapon, kickback, weaponPos, reloading;
  int elapsedMs;
};

struct TswStep {
  int suppressFire;
  int orField;
  int outcome;
  int slot;
  int weaponAt;
};

inline int tsw_field(unsigned bits) {
  return (int)((bits & EK_SK_WEAPON_MASK) >> EK_SK_WEAPON_SHIFT);
}

inline void tsw_resolve(TswState* st, TswStep* r, int outcome) {
  r->outcome = outcome;
  r->slot = st->slot;
  r->weaponAt = st->weaponAt;
  st->phase = TSW_IDLE;
  st->slot = 0;
  st->weaponAt = -1;
  st->ageMs = 0;
}

inline TswStep tsw_drop(TswState* st) {
  TswStep r = { 0, 0, TSW_OUT_NONE, 0, -1 };
  if (!st) return r;
  if (st->phase != TSW_IDLE) tsw_resolve(st, &r, TSW_OUT_DROPPED);
  *st = tsw_blank();
  return r;
}

inline TswStep tsw_step(TswState* st, const TswFrame& f) {
  TswStep r = { 0, 0, TSW_OUT_NONE, 0, -1 };
  if (!st) return r;
  if (!f.clockOk) return tsw_drop(st);

  if (f.ticAdvanced) {
    st->lastConsumed = st->prevEndField;
    st->ourField = 0;
    if (st->phase == TSW_REPLAYED) tsw_resolve(st, &r, TSW_OUT_REPLAYED);
  }

  const int field = f.field & 0xF;
  const int playerField = st->ourField ? 0 : field;
  if (st->phase != TSW_IDLE) {
    const int add = f.elapsedMs > 0 ? f.elapsedMs : 0;
    st->ageMs = (st->ageMs > TSW_AGE_CAP_MS - add) ? TSW_AGE_CAP_MS : st->ageMs + add;
  }

  if (playerField != 0 && f.live && (f.botHolding || st->phase == TSW_PENDING)) {
    const bool replacing = st->phase == TSW_PENDING && st->slot != playerField;
    if (replacing) tsw_resolve(st, &r, TSW_OUT_REPLACED);
    // From IDLE only on a NEW press: P_GetInput rewrites a held key every frame, so a field the last tic carried is
    // the same press.
    if (st->phase != TSW_PENDING && (replacing || playerField != st->lastConsumed)) {
      st->phase = TSW_PENDING;
      st->slot = playerField;
      st->weaponAt = f.animOk ? f.weapon : -1;
      st->weaponPosAt = f.animOk ? f.weaponPos : 0;
    }
    st->ageMs = 0;
  }

  if (st->phase == TSW_PENDING && r.outcome == TSW_OUT_NONE) {
    // weapon_pos 0 -> -1 is P_ChangeWeapon starting a switch, caught even where a CON event keeps curr_weapon.
    const bool switched = f.animOk && st->weaponAt >= 0 &&
        (f.weapon != st->weaponAt || (f.weaponPos < 0 && st->weaponPosAt >= 0));
    if (switched) {
      tsw_resolve(st, &r, TSW_OUT_ENGINE);
    } else if (st->ageMs > TSW_TIMEOUT_MS) {
      tsw_resolve(st, &r, TSW_OUT_TIMEOUT);
    } else if (f.live && f.animOk && f.kickback == 0 && f.reloading != 1 &&
               field == 0 && !st->ourField && st->lastConsumed == 0) {
      r.orField = st->slot;
      st->ourField = 1;
      st->phase = TSW_REPLAYED;
      st->ageMs = 0;
    }
  } else if (st->phase == TSW_REPLAYED && st->ageMs > TSW_TIMEOUT_MS) {
    tsw_resolve(st, &r, TSW_OUT_TIMEOUT);
  }

  st->prevEndField = field | r.orField;
  r.suppressFire = st->phase != TSW_IDLE ? 1 : 0;
  return r;
}
