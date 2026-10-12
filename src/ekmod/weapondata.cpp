// The live weapon read: per-player WEAPONx_* fields once a second for every weapon. weapondata.h holds the decisions
// about the dump, firestops.h those about the stops derived from the same rows; it is both a measurement and a
// source.

#include <windows.h>
#include <stdio.h>
#include "log.h"
#include "config.h"
#include "gamevars.h"
#include "playerstate.h"
#include "weapondata.h"
#include "firestops.h"

// The local player is 0, this port's existing answer: playerstate.cpp anchors its locate on g_player[0].ps, so player
// 0 is the DukePlayer_t every read and engine write here already goes to (including weaponspeed's kickback_pic).
// EDuke32 allocates all sixteen slots even in single player, so a wrong index would read plausible zeros.
static const int EK_WD_LOCAL_PLAYER = 0;

static bool s_logged[EK_WD_MAX_WEAPONS];
static EkWeaponRow s_last[EK_WD_MAX_WEAPONS];
static bool s_headerSaid = false;
static bool s_pcharSaid = false;
static int s_lastPchar = 0;
static bool s_lastPcharGot = false;

// A value slot that cannot be mistaken for a value: a failed read printed 0 beside real ones. _TRUNCATE, never
// sprintf_s, which aborts the process over a log line.
static void field_text(bool got, int v, char* out, size_t bytes) {
  if (got) _snprintf_s(out, bytes, _TRUNCATE, "%d", v);
  else     _snprintf_s(out, bytes, _TRUNCATE, "unread");
}

void weapondata_tick(void) {
  // Two reasons to read, neither riding on the other: the dump wants the Debug log, the stops want Fire rate or
  // Reload speed. With both off nothing is read.
  const bool wantLog = g_cfg.debuglog;
  const bool wantStops = firerate_kick_gate_rate(g_cfg.firerate, g_cfg.reloadrate) > 100;
  if (!wantLog && !wantStops) return;
  if (!gamevars_ready() || !player_ready()) return;

  if (wantLog && !s_headerSaid) {
    s_headerSaid = true;
    ek_log("weapondata: AA's live WEAPONx_* for player %d, beside the stops "
           "weaponspeed.h's STATIC table would use. These rows are also what "
           "firestops.h derives the stops actually written from, so this is no "
           "longer a measurement alone. MAX_WEAPONS %d (inv.h:74), %d of %d "
           "field slots resolved. weaponspeed.h's table is STOCK DUKE's "
           "frame numbers, and "
           "each row's stops are derived from FIREDELAY, TOTALTIME and RELOAD; "
           "a DISAGREES below is a weapon the STATIC table does not describe, "
           "and an AGREES covers those three fields only. Since brief-live-"
           "stops a DISAGREES no longer means the write is wrong -- the "
           "firestops: lines say what is actually used.",
           EK_WD_LOCAL_PLAYER, EK_WD_MAX_WEAPONS,
           gamevars_weapon_resolved(), EK_WD_MAX_WEAPONS * EK_WD_FIELD_COUNT);
  }

  if (wantLog) {
    int pchar = 0;
    const bool pcharGot = gamevars_pchar(EK_WD_LOCAL_PLAYER, &pchar);
    if (!s_pcharSaid || pcharGot != s_lastPcharGot ||
        (pcharGot && pchar != s_lastPchar)) {
      s_pcharSaid = true;
      s_lastPcharGot = pcharGot;
      s_lastPchar = pchar;
      char pText[24];
      field_text(pcharGot, pchar, pText, sizeof(pText));
      ek_log("weapondata: pchar = %s (AADEFS.CON:3267 -- 0 duke, 1 bombshell, "
             "2 wes, 3 mando, 4 deanova)", pText);
    }
  }

  for (int weapon = 0; weapon < EK_WD_MAX_WEAPONS; weapon++) {
    EkWeaponRow row;
    for (int f = 0; f < EK_WD_FIELD_COUNT; f++) {
      int v = 0;
      const bool got = gamevars_weapon_field(weapon, f, EK_WD_LOCAL_PLAYER, &v);
      row.got[f] = got ? 1 : 0;
      row.v[f] = got ? v : 0;
    }

    // The stops are derived and published before anything is printed, on every sample, not behind the log throttle:
    // the throttle keys on a row having been printed, and a stop set gone stale because a diagnostic was off is the
    // missing-stop failure. firestops_publish writes nothing when the derived set has not moved.
    firestops_publish(weapon, row);
    if (!wantLog) continue;

    // Keyed on the field values: a purchased upgrade rewrites WEAPONx_* from CON with nothing to announce it. One
    // weapon's change reprints that weapon's line only.
    if (!ekm_weapon_row_should_log(s_logged[weapon], row, s_last[weapon]))
      continue;
    s_logged[weapon] = true;
    s_last[weapon] = row;

    char fd[24], tt[24], rl[24], hd[24], fl[24], sb[24], wl[24], st[24], s2[24],
         sh[24];
    field_text(row.got[EK_WD_FIREDELAY] != 0, row.v[EK_WD_FIREDELAY], fd, sizeof(fd));
    field_text(row.got[EK_WD_TOTALTIME] != 0, row.v[EK_WD_TOTALTIME], tt, sizeof(tt));
    field_text(row.got[EK_WD_RELOAD] != 0, row.v[EK_WD_RELOAD], rl, sizeof(rl));
    field_text(row.got[EK_WD_HOLDDELAY] != 0, row.v[EK_WD_HOLDDELAY], hd, sizeof(hd));
    field_text(row.got[EK_WD_FLAGS] != 0, row.v[EK_WD_FLAGS], fl, sizeof(fl));
    field_text(row.got[EK_WD_SHOTSPERBURST] != 0, row.v[EK_WD_SHOTSPERBURST], sb, sizeof(sb));
    field_text(row.got[EK_WD_WORKSLIKE] != 0, row.v[EK_WD_WORKSLIKE], wl, sizeof(wl));
    field_text(row.got[EK_WD_SPAWNTIME] != 0, row.v[EK_WD_SPAWNTIME], st, sizeof(st));
    field_text(row.got[EK_WD_SOUND2TIME] != 0, row.v[EK_WD_SOUND2TIME], s2, sizeof(s2));
    // SHOOTS is printed because the throttle keys on it (ekm_weapon_row_differs watches all ten fields): a line
    // without it would be a reprint with nothing visibly different.
    field_text(row.got[EK_WD_SHOOTS] != 0, row.v[EK_WD_SHOOTS], sh, sizeof(sh));

    char stops[96];
    const int nStops = ekm_firerate_stops_text(weapon, stops, (int)sizeof(stops));
    // All three numbers the stops derive from are printed together: a FIREDELAY-only pairing let a shotgun on
    // FIREDELAY 4 / TOTALTIME 11 read as agreement while four of five stops sat past the engine's reset.
    const EkFireRateAssumed* const a = ekm_firerate_assumed(weapon);
    char aFd[16], aTt[16], aRl[16];
    if (a) {
      _snprintf_s(aFd, sizeof(aFd), _TRUNCATE, "%d", a->fireDelay);
      _snprintf_s(aTt, sizeof(aTt), _TRUNCATE, "%d", a->totalTime);
      _snprintf_s(aRl, sizeof(aRl), _TRUNCATE, "%d", a->reload);
    } else {
      _snprintf_s(aFd, sizeof(aFd), _TRUNCATE, "none");
      _snprintf_s(aTt, sizeof(aTt), _TRUNCATE, "none");
      _snprintf_s(aRl, sizeof(aRl), _TRUNCATE, "none");
    }
    char verdict[256];
    ekm_firerate_verdict_text(weapon, row, verdict, (int)sizeof(verdict));

    // One line, both sides: live on the left, what our table believes on the right, and a verdict naming the fields
    // compared.
    ek_log("weapondata: w%02d AA live FIREDELAY=%s TOTALTIME=%s RELOAD=%s "
           "SPAWNTIME=%s SOUND2TIME=%s HOLDDELAY=%s FLAGS=%s SHOTSPERBURST=%s "
           "WORKSLIKE=%s SHOOTS=%s || weaponspeed.h assumes FIREDELAY=%s "
           "TOTALTIME=%s RELOAD=%s, static stops (%d): %s -- %s",
           weapon, fd, tt, rl, st, s2, hd, fl, sb, wl, sh, aFd, aTt, aRl,
           nStops, stops, verdict);
  }
}
