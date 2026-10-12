// Derived stop sets, and where they cross between the two clocks (derivation: firestops.h). The worker publishes from
// the 1 Hz sampler; the game thread reads inside SwapBuffers. A mix of two sets can MISS a stop, so the record sits
// behind an even/odd counter and a reader refuses the frame if it was odd or moved. It writes only on change.

#include <windows.h>
#include <string.h>
#include "log.h"
#include "config.h"
#include "gamevars.h"
#include "firestops.h"
#include "constops.h"
#include "gamearrays.h"

// Player 0 is the one whose kickback_pic the fire rate steps.
static const int EK_FS_LOCAL_PLAYER = 0;

// 0 means never published; whether that is stock Duke depends on gamevars_ready() (ekm_firestops_verdict). One counter
// covers all fastfire variants, written in one odd/even window, so a reader gets a whole record or refuses.
static volatile LONG s_seq[EK_WD_MAX_WEAPONS];
static FireRateWeapon s_set[EK_WD_MAX_WEAPONS][EK_SHOT_VAR_COUNT];
// Reload set, written in the same window as s_set; two variants (EK_RSET_VAR_*). The one-tic variant holds only while
// AAPLAYER.CON:8848-8930's +1 is off, read live in firestops_source.
static EkReloadStops s_rset[EK_WD_MAX_WEAPONS][EK_RSET_VAR_COUNT];

static FireRateWeapon s_last[EK_WD_MAX_WEAPONS][EK_SHOT_VAR_COUNT];
static EkReloadStops s_rlast[EK_WD_MAX_WEAPONS][EK_RSET_VAR_COUNT];
static bool s_said[EK_WD_MAX_WEAPONS];

// nEvents < 0 is the refusal marker, so two refusals are equal whatever the array holds. con[] is compared too: a
// provenance-only flip must still count as a change or the log keeps the old one.
static bool fs_records_equal(const FireRateWeapon& a, const FireRateWeapon& b) {
  if (a.weapon != b.weapon) return false;
  if (a.nEvents != b.nEvents) return false;
  if (a.nEvents < 0) return true;
  for (int i = 0; i < a.nEvents && i < FIRERATE_EVENTS_MAX; i++) {
    if (a.events[i] != b.events[i]) return false;
    if (a.con[i] != b.con[i]) return false;
  }
  return true;
}

// The key and code are part of the record: a set re-derived for another character or TotalTime must republish.
static bool fs_reload_equal(const EkReloadStops& a, const EkReloadStops& b) {
  if (a.code != b.code) return false;
  if (a.code != EK_RSET_OK) return true;
  return a.tt == b.tt && a.pchar == b.pchar && fs_records_equal(a.set, b.set);
}

// Gate inputs are read on the worker every sample so a character or upgrade change republishes. Fastfire is not read
// here (it flips faster); every variant is published and firestops_source picks live. A failed read stays unread, which
// ekm_shot_gate treats as open.
static EkShotInputs fs_shot_inputs(int weapon) {
  EkShotInputs in;
  int v = 0;
  in.pcharOk = gamevars_pchar(EK_FS_LOCAL_PLAYER, &v);
  in.pchar = in.pcharOk ? v : 0;
  in.ffOk = false;
  in.ff = 0;
  v = 0;
  in.gotdbOk = gamevars_gotdb(EK_FS_LOCAL_PLAYER, &v);
  in.gotdb = in.gotdbOk ? v : 0;
  for (int a = 0; a < EK_GA_UP_COUNT; a++) {
    v = 0;
    in.upOk[a] = gamearrays_upgrade(a, weapon, &v);
    in.up[a] = in.upOk[a] ? v : 0;
  }
  return in;
}

void firestops_publish(int weapon, const EkWeaponRow& row) {
  if (weapon < 0 || weapon >= EK_WD_MAX_WEAPONS) return;

  // AA's CON is the second author of this counter: its frames above TotalTime are unioned in as stops (unioning all
  // frames made the fire rate inert). conState is read before the derive so union and log agree.
  const int conState = constops_state();
  const EkConStops cs = ekm_con_stops_aa(weapon);
  const int conGap = ekm_con_largest_gap(weapon);

  const bool hasShots = ekm_con_shot_weapon(weapon);
  const EkShotInputs shotIn = hasShots ? fs_shot_inputs(weapon) : EkShotInputs();
  FireRateWeapon all[EK_SHOT_VAR_COUNT];
  bool okv[EK_SHOT_VAR_COUNT];
  for (int k = 0; k < EK_SHOT_VAR_COUNT; k++) {
    if (!hasShots && k > 0) { all[k] = all[0]; okv[k] = okv[0]; continue; }
    const EkShotInputs vin = ekm_shot_inputs_for(shotIn, k);
    okv[k] = ekm_firestops_derive_aa(weapon, row, conState == EK_CON_MATCH,
                                     &all[k], hasShots ? &vin : 0);
    if (!okv[k]) {
      all[k].weapon = weapon;
      all[k].nEvents = -1;
      for (int i = 0; i < FIRERATE_EVENTS_MAX; i++) {
        all[k].events[i] = 0;
        all[k].con[i] = 0;
      }
    }
  }
  const FireRateWeapon& derived = all[EK_SHOT_VAR_IDLE];
  const bool ok = okv[EK_SHOT_VAR_IDLE];

  // Reload set keyed by the pchar it was derived for; an unread pchar publishes none and the full set applies.
  EkReloadStops rsets[EK_RSET_VAR_COUNT];
  int pc = 0;
  const bool ledgered = ekm_reload_floor(weapon) > 0;
  const bool pcOk = ledgered && gamevars_pchar(EK_FS_LOCAL_PLAYER, &pc);
  for (int v = 0; v < EK_RSET_VAR_COUNT; v++) {
    EkReloadStops& rs = rsets[v];
    rs.code = EK_RSET_SLOT;
    rs.tt = row.got[EK_WD_TOTALTIME] ? row.v[EK_WD_TOTALTIME] : -1;
    rs.pchar = -1;
    if (ledgered) {
      rs.pchar = pcOk ? pc : -1;
      rs.code = ekm_firestops_derive_reload_aa(weapon, row, conState == EK_CON_MATCH,
                                               pcOk, pc, &rs.set,
                                               v == EK_RSET_VAR_ONE_TIC);
    }
    if (rs.code != EK_RSET_OK) {
      rs.set.weapon = weapon;
      rs.set.nEvents = -1;
      for (int i = 0; i < FIRERATE_EVENTS_MAX; i++) { rs.set.events[i] = 0; rs.set.con[i] = 0; }
    }
  }
  const EkReloadStops& rset = rsets[EK_RSET_VAR_HELD];

  bool same = s_said[weapon];
  for (int v = 0; same && v < EK_RSET_VAR_COUNT; v++)
    same = fs_reload_equal(rsets[v], s_rlast[weapon][v]);
  for (int k = 0; same && k < EK_SHOT_VAR_COUNT; k++)
    same = fs_records_equal(all[k], s_last[weapon][k]);
  if (same) return;
  s_said[weapon] = true;
  for (int k = 0; k < EK_SHOT_VAR_COUNT; k++) s_last[weapon][k] = all[k];
  for (int v = 0; v < EK_RSET_VAR_COUNT; v++) s_rlast[weapon][v] = rsets[v];

  InterlockedIncrement(&s_seq[weapon]);
  for (int k = 0; k < EK_SHOT_VAR_COUNT; k++) s_set[weapon][k] = all[k];
  for (int v = 0; v < EK_RSET_VAR_COUNT; v++) s_rset[weapon][v] = rsets[v];
  InterlockedIncrement(&s_seq[weapon]);

  // One line per change, behind the Debug log row, so the derivation can be shown rather than inferred from behaviour.
  if (!g_cfg.debuglog) return;
  if (!ok) {
    // With the CON union gone, a refusal can only be the engine's own weapon data declining this weapon.
    ek_log("firestops: w%02d refused -- no speed-up for this weapon "
           "(the engine's own weapon data declined it)", weapon);
    return;
  }
  // 64 stops of up to three digits, a comma and up to three flag letters each; smaller buffers truncated a full set.
  char stops[512];
  const int n = firerate_stops_text(derived, stops, (int)sizeof(stops));
  // Upper bound on engine stops that park on their value and also sit in AA's CON table, from the row's own TotalTime.
  char atRisk[256];
  const int nAtRisk = ekm_firestops_at_risk(derived, row.v[EK_WD_TOTALTIME], cs,
                                            atRisk, (int)sizeof(atRisk));
  // cs.nStops and conGap describe the full CON list; `derived` is the set in force. The gate flags (on|off|unread) tell
  // a never-applied shot stop from a closed gate.
  char gates[160];
  gates[0] = 0;
  int nGates = 0, nOpen = 0;
  if (hasShots) {
    const EkConShot* const rows = ekm_con_shots();
    for (int i = 0; i < EKM_CON_SHOT_COUNT; i++) {
      if (rows[i].weapon != weapon) continue;
      const int g = ekm_shot_gate(rows[i], shotIn, row);
      nGates++;
      if (g != EK_GATE_FALSE) nOpen++;
      char one[32];
      _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%d=%s", nGates > 1 ? "," : "",
                  rows[i].cite[0].line,
                  g == EK_GATE_TRUE ? "on" : (g == EK_GATE_FALSE ? "off" : "unread"));
      strcat_s(gates, sizeof(gates), one);
    }
  }
  ek_log("firestops: w%02d derived %d stop(s): %s -- AA's CON has %d frame(s) "
         "for it (largest gap %d, con %s), applied above TotalTime only -- "
         "at risk: %s (%d) -- CON shot gates %d open of %d: %s (applied only "
         "with con match) -- letters: E engine beside a CON flag, F CON frame "
         "(skipped once reached when F is its only letter), c CON edge (held), "
         "bare = engine only",
         weapon, n, stops, cs.nStops, conGap, ekm_con_state_name(conState),
         nAtRisk > 0 ? atRisk : "none", nAtRisk, nOpen, nGates,
         nGates > 0 ? gates : "none");
  if (rset.code == EK_RSET_OK) {
    char rstops[512];
    const int nR = firerate_stops_text(rset.set, rstops, (int)sizeof(rstops));
    ek_log("firestops: w%02d reload region, pchar %d TotalTime %d: %d stop(s): %s "
           "-- engine state stops plus con_reload_aa.h's E/U rows for this character",
           weapon, rset.pchar, rset.tt, nR, rstops);
    // Equality rows that went one-tic, with the total beside the list; a refused one is a literal write or FIREDELAY+1
    // on F.
    const EkReloadStops& one = rsets[EK_RSET_VAR_ONE_TIC];
    if (one.code == EK_RSET_OK) {
      char conv[256], refused[256];
      conv[0] = 0; refused[0] = 0;
      int nEq = 0, nConv = 0;
      const EkReloadRow* const rows = ekm_reload_rows();
      for (int i = 0; i < EKM_RELOAD_ROW_COUNT; i++) {
        const EkReloadRow& r = rows[i];
        if (!ekm_reload_row_applies(r, weapon, one.pchar)) continue;
        if (strcmp(r.op, "ife") != 0 && strcmp(r.op, "ifvare") != 0) continue;
        nEq++;
        const bool ok1 = ekm_reload_row_one_tic(r, weapon, row);
        if (ok1) nConv++;
        char item[48];
        _snprintf_s(item, sizeof(item), _TRUNCATE, "%s%s:%d %d",
                    (ok1 ? conv[0] : refused[0]) ? "," : "", r.file, r.line, r.frame);
        strncat_s(ok1 ? conv : refused, sizeof(conv), item, _TRUNCATE);
      }
      char ostops[512];
      const int nO = firerate_stops_text(one.set, ostops, (int)sizeof(ostops));
      ek_log("firestops: w%02d reload region one-tic variant (fastfire idle%s): %d stop(s): "
             "%s -- %d of %d equality row(s) CON_FRAME at F: %s -- refused (a write lands "
             "on F): %s",
             weapon, weapon == 1 ? ", no reload perk" : "", nO, ostops, nConv, nEq,
             nConv ? conv : "none", nEq - nConv ? refused : "none");
    }
  } else {
    ek_log("firestops: w%02d reload region uses the full set -- %s (pchar %d)",
           weapon, ekm_rset_name(rset.code), rset.pchar);
  }
  // The other two fastfire variants; the line above is fastfire-0.
  if (!hasShots) return;
  char fast[512], unread[512];
  const int nF = firerate_stops_text(all[EK_SHOT_VAR_FAST], fast, (int)sizeof(fast));
  const int nU = firerate_stops_text(all[EK_SHOT_VAR_UNREAD], unread, (int)sizeof(unread));
  ek_log("firestops: w%02d fastfire variants, picked per advance from a live "
         "read -- fastfire>0 %d stop(s): %s -- fastfire unread %d stop(s): %s",
         weapon, okv[EK_SHOT_VAR_FAST] ? nF : -1,
         okv[EK_SHOT_VAR_FAST] ? fast : "refused",
         okv[EK_SHOT_VAR_UNREAD] ? nU : -1,
         okv[EK_SHOT_VAR_UNREAD] ? unread : "refused");
}

int firestops_source(int weapon, FireRateWeapon* out, EkReloadStops* outReload) {
  if (outReload) { outReload->code = EK_RSET_DERIVE; outReload->set.nEvents = -1; }
  if (!out || weapon < 0 || weapon >= EK_WD_MAX_WEAPONS) return EK_STOPS_STATIC;

  // Same volatile read the ESP uses; s_vars is published last, so true means the index table is whole.
  const bool haveGamevars = gamevars_ready();

  // Fastfire and reload variants are read live on the game thread, not at 1 Hz. Nothing of ours crosses: s_vars and the
  // index are Interlocked, the record and pValues are VirtualQuery-checked per call. A torn fastfire picks the wrong
  // variant for one frame; a failed reload read is the held variant.
  const bool rateOn = firerate_kick_gate_rate(g_cfg.firerate, g_cfg.reloadrate) > 100;
  const bool wantShot = ekm_con_shot_weapon(weapon) && rateOn;
  const bool wantReload = outReload && ekm_reload_floor(weapon) > 0 && rateOn;
  int variant = EK_SHOT_VAR_IDLE;
  int rvariant = EK_RSET_VAR_HELD;
  if (wantShot || wantReload) {
    int ff = 0;
    const bool ffOk = gamevars_fastfire(EK_FS_LOCAL_PLAYER, &ff);
    if (wantShot) variant = ekm_shot_variant(ffOk, ff);
    if (wantReload) {
      int perks = 0;
      bool perksOk = true;
      if (weapon == 1) perksOk = gamevars_perks(EK_FS_LOCAL_PLAYER, &perks);
      rvariant = ekm_reload_variant(weapon, ffOk, ff, perksOk, perks);
    }
  }

  const LONG s0 = InterlockedCompareExchange(&s_seq[weapon], 0, 0);
  // Not published or write in flight: ask the verdict with an unchanged counter and an unreachable length.
  if (s0 == 0 || (s0 & 1)) return ekm_firestops_verdict(s0, s0, 0, haveGamevars);

  const FireRateWeapon tmp = s_set[weapon][variant];
  EkReloadStops rtmp;
  rtmp.code = EK_RSET_DERIVE;
  if (outReload) rtmp = s_rset[weapon][rvariant];

  const LONG s1 = InterlockedCompareExchange(&s_seq[weapon], 0, 0);
  const int verdict = ekm_firestops_verdict(s0, s1, tmp.nEvents, haveGamevars);
  if (verdict == EK_STOPS_LIVE) {
    *out = tmp;
    if (outReload) *outReload = rtmp;
  }
  return verdict;
}
