// Coin magnet, live half (rules: coinmagnet.h): walks IMPCOIN sprites once per engine tic and writes x/y/z (a snap) or
// ang and xvel (a boost inside AA's chase) through gamestate_sprite_write. A coin in flight in scope gets yvel 0 so the
// engine lands it now. NUKECOIN and COINPROJ follow the same rules via CoinKind.

#include <windows.h>
#include <string.h>
#include "coinmagnet.h"
#include "config.h"
#include "gamestate.h"
#include "playerstate.h"
#include "udstate.h"
#include "input.h"
#include "gamevars.h"
#include "tilenames.h"
#include "weaponspeed.h"
#include "freecam.h"
#include "log.h"
#include "ui/wallclock.h"

static const int EK_CM_LOCAL_PLAYER = 0;

// Per sprite slot, reset when the slot stops holding a live coin, so a coin is counted once however many tics it lives.
// Game thread only. CM_SLOT_HEALTH names the kind, so a slot reused by the other kind is new.
enum {
  CM_SLOT_SEEN = 1, CM_SLOT_ASLEEP = 2, CM_SLOT_PENDING = 4,
  CM_SLOT_BOOSTED = 8, CM_SLOT_UNRUN = 16, CM_SLOT_HEALTH = 32, CM_SLOT_HELD = 64
};
static unsigned char s_slot[EK_MAXSPRITES];
static CoinTrack s_track[EK_MAXSPRITES];
static bool s_slotDirty = false;

static int  s_lastTic = 0;
static bool s_haveTic = false;
// The tic the coins were last walked, apart from s_lastTic: a gated frame skips the walk but those tics still count.
static int  s_walkTic = 0;
static bool s_haveWalkTic = false;
static bool s_tileConfirmed[EK_COIN_KINDS];
static bool s_tileRefused[EK_COIN_KINDS];
static bool s_projConfirmed[EK_COIN_KINDS];
static bool s_projRefused[EK_COIN_KINDS];
static bool s_writeRefused = false;
static bool s_clockSaid = false;

// Cumulative per kind for the debug log. paid: a snapped coin gone the next tic (a health coin: gone inside its pay
// ring); disagreeing with the snap sum is the defect signal. unrun: seen before its first execution. held: a health
// coin whose snap would fail AA's sight test (coin_kind_snap_pays), left alone.
enum {
  CM_N_SEEN, CM_N_INSTANT, CM_N_BUDGET, CM_N_COUNT, CM_N_STUCK, CM_N_SLEEP,
  CM_N_BOOSTED, CM_N_ASLEEP, CM_N_UNRUN, CM_N_PAID, CM_N_FLIGHT, CM_N_HELD, CM_N_ALL
};
static unsigned s_n[EK_COIN_KINDS][CM_N_ALL];
static unsigned s_logN[EK_COIN_KINDS][CM_N_ALL];
static unsigned s_lastLogMs = 0;

static void clear_slots(void) {
  s_haveWalkTic = false;
  if (!s_slotDirty) return;
  memset(s_slot, 0, sizeof(s_slot));
  memset(s_track, 0, sizeof(s_track));
  s_slotDirty = false;
}

static int live_coin(const unsigned char* r) {
  const short picnum  = *(const short*)(r + EK_SPR_PICNUM);
  const short statnum = *(const short*)(r + EK_SPR_STATNUM);
  if (statnum < 0 || statnum >= EK_MAXSTATUS) return -1;
  return coin_kind_of_tile(picnum);
}

static bool has_run(const unsigned char* r, int k) {
  return coin_kind_first_run_done(k, *(const signed char*)(r + EK_SPR_SHADE),
                                  *(const unsigned char*)(r + EK_SPR_XREPEAT),
                                  *(const unsigned char*)(r + EK_SPR_YREPEAT));
}

// AA's names.h must name the picnum as the kind does, or that part stays off: a picnum meaning something else in
// another mod is a sprite we must not move.
static bool name_confirmed(int tile, const char* want, bool* confirmed, bool* refused,
                           const char* offWhat) {
  if (*confirmed) return true;
  if (*refused || !tilenames_ready()) return false;
  const char* const name = tilenames_name(tile);
  if (!name || strcmp(name, want) != 0) {
    *refused = true;
    ek_log("coinmagnet: REFUSED -- names.h calls picnum %d \"%s\", not %s; %s",
           tile, name ? name : "(no name)", want, offWhat);
    return false;
  }
  *confirmed = true;
  ek_log("coinmagnet: picnum %d is %s per AA's names.h", tile, want);
  return true;
}
static bool tile_confirmed(int k) {
  return name_confirmed(coin_kind(k)->tile, coin_kind(k)->name, &s_tileConfirmed[k],
                        &s_tileRefused[k],
                        k == EK_COIN_HEALTH ? "health coins are left alone this session"
                                            : "credit coins are left alone this session");
}

// The same check for the thrown coin; a mismatch only turns off the in-flight part.
static bool proj_confirmed(int k) {
  return name_confirmed(coin_kind(k)->projTile, coin_kind(k)->projName, &s_projConfirmed[k],
                        &s_projRefused[k], "coins in flight are left to land this session");
}

static bool live_coin_flight(const unsigned char* r, int k) {
  return *(const short*)(r + EK_SPR_PICNUM) == coin_kind(k)->projTile &&
         *(const short*)(r + EK_SPR_STATNUM) == EK_STAT_PROJECTILE;
}

static void maybe_log(void) {
  if (!g_cfg.debuglog) return;
  if (memcmp(s_n, s_logN, sizeof(s_n)) == 0) return;
  const unsigned now = wallclock_ms();
  if (now - s_lastLogMs < 1000) return;
  s_lastLogMs = now;
  for (int k = 0; k < EK_COIN_KINDS; k++) {
    const unsigned* const n = s_n[k];
    if (memcmp(n, s_logN[k], sizeof(s_n[k])) == 0) continue;
    memcpy(s_logN[k], n, sizeof(s_n[k]));
    const unsigned snapped = n[CM_N_INSTANT] + n[CM_N_BUDGET] + n[CM_N_COUNT] +
                             n[CM_N_STUCK] + n[CM_N_SLEEP];
    if (k == EK_COIN_CREDIT) {
      ek_log("coinmagnet: seen %u, snapped %u (instant %u, budget %u, count %u, "
             "stuck %u, sleep %u), boosted %u, asleep %u, unrun %u, paid %u, "
             "flights ended early %u",
             n[CM_N_SEEN], snapped, n[CM_N_INSTANT], n[CM_N_BUDGET],
             n[CM_N_COUNT], n[CM_N_STUCK], n[CM_N_SLEEP], n[CM_N_BOOSTED],
             n[CM_N_ASLEEP], n[CM_N_UNRUN], n[CM_N_PAID], n[CM_N_FLIGHT]);
    } else {
      ek_log("coinmagnet health: seen %u, snapped %u (instant %u, budget %u, count %u, "
             "stuck %u, sleep %u), boosted %u, asleep %u, unrun %u, held (other sector) %u, "
             "paid %u, flights ended early %u",
             n[CM_N_SEEN], snapped, n[CM_N_INSTANT], n[CM_N_BUDGET],
             n[CM_N_COUNT], n[CM_N_STUCK], n[CM_N_SLEEP], n[CM_N_BOOSTED],
             n[CM_N_ASLEEP], n[CM_N_UNRUN], n[CM_N_HELD], n[CM_N_PAID], n[CM_N_FLIGHT]);
    }
  }
}

// Re-read before every write, so a slot that stopped being a live awake coin is never written.
static bool still_ours(const unsigned char* r, int k) {
  return live_coin(r) == k && *(const short*)(r + EK_SPR_STATNUM) == EK_STAT_ACTOR &&
         has_run(r, k);
}

static bool write_field(int i, int off, const void* src, size_t n) {
  if (gamestate_sprite_write(i, off, src, n)) return true;
  s_writeRefused = true;
  return false;
}

void coinmagnet_frame(void) {
  const int magnetSel = coinmagnet_clamp(g_cfg.coinmagnet);
  const int speedSel  = coinspeed_clamp(g_cfg.coinspeed);
  if (magnetSel == EK_CM_OFF && speedSel == EK_CS_100) {
    clear_slots();
    s_haveTic = false;
    return;
  }
  if (s_writeRefused) return;

  // Once per engine tic, not per frame: g_vm_preempt can present a frame between two actors (game.cpp:93).
  int tic = 0;
  const bool ticOk = weaponspeed_engine_tic(&tic);
  if (!ticOk) {
    if (!s_clockSaid) {
      s_clockSaid = true;
      ek_log("coinmagnet: the engine tic clock is not readable yet; the coin "
             "rows wait for it");
    }
    return;
  }
  const bool advanced = freecam_tic_advanced(s_lastTic, tic, s_haveTic);
  s_lastTic = tic;
  s_haveTic = true;
  if (!advanced) return;

  int gm = 0;
  const bool gmOk = player_read_gm(&gm);
  if (ekm_level_loading(gm, gmOk)) clear_slots();
  if (!esp_level_loaded(gm, gmOk)) return;
  bool pauseOn = true;
  const bool paused = !ud_paused(&pauseOn) || pauseOn;
  if (paused || input_panel_open()) return;
  int hp = 0;
  const bool bodyOk = player_read_body(&hp, NULL);
  if (!bodyOk || hp <= 0) return;
  if (!gamestate_ready()) return;
  bool kindOk[EK_COIN_KINDS];
  bool flightOk[EK_COIN_KINDS];
  bool anyKind = false;
  for (int k = 0; k < EK_COIN_KINDS; k++) {
    kindOk[k] = coin_kind_enabled(k, magnetSel, speedSel, g_cfg.coinheal) && tile_confirmed(k);
    flightOk[k] = kindOk[k] && magnetSel != EK_CM_OFF && proj_confirmed(k);
    anyKind = anyKind || kindOk[k];
  }
  if (!anyKind) return;

  // Snap target is pos, not opos: A_Execute runs the coin's CON before VM_Move and `ifpdistl 896` measures from opos,
  // which P_ProcessInput sets to pos before moving (player.cpp:5316-5319). A coin written at pos reads distance 0 and
  // AA pays it; one run later in a preempted tic sees opos. details: docs/re/targets.md.
  int px = 0, py = 0, pz = 0;
  const bool posOk = player_pos(&px, &py, &pz);
  if (!posOk) return;
  int perks = 0;
  const bool perksOk = gamevars_perks(EK_CM_LOCAL_PLAYER, &perks);
  if (!perksOk) perks = 0;   // unreadable: AA's smaller, perk-less radius
  const int chase[EK_COIN_KINDS] = { coin_kind_chase_radius(EK_COIN_CREDIT, perks),
                                     coin_kind_chase_radius(EK_COIN_HEALTH, perks) };
  // The sector ifcanseetarget reads for the player: his sprite's (gameexec.cpp:3282). Unread, it matches no coin.
  int playerSect = -1;
  if (kindOk[EK_COIN_HEALTH]) {
    int pi = -1;
    const bool piOk = player_sprite_index(&pi);
    const unsigned char* const pr = piOk ? gamestate_sprite(pi) : NULL;
    if (pr) playerSect = *(const short*)(pr + EK_SPR_SECTNUM);
  }
  const int radius  = coinmagnet_radius(magnetSel);
  const int percent = coinspeed_percent(speedSel);
  const int snapXyz[3] = { px, py, pz + (28 << 8) };

  int tics = 1;
  if (s_haveWalkTic) tics = (tic - s_walkTic) / EK_OTOTALCLOCK_PER_TIC;
  if (tics < 1) tics = 1;
  s_walkTic = tic;
  s_haveWalkTic = true;

  for (int i = 0; i < EK_MAXSPRITES; i++) {
    const unsigned char* const r = gamestate_sprite(i);
    if (!r) break;
    unsigned char st = s_slot[i];
    const int k = live_coin(r);
    const int slotKind = (st & CM_SLOT_HEALTH) ? EK_COIN_HEALTH : EK_COIN_CREDIT;
    if (k < 0 || k != slotKind) {
      if (st) {
        if (st & CM_SLOT_PENDING) s_n[slotKind][CM_N_PAID]++;
        st = 0;
        s_slot[i] = 0;
        s_track[i].picnum = 0;
      }
    }
    if (k < 0) {
      const int pk = coin_kind_of_proj(*(const short*)(r + EK_SPR_PICNUM));
      if (pk >= 0 && flightOk[pk] && live_coin_flight(r, pk)) {
        // yvel 0 lands it where it is on its next move, the engine's own ending (actors.cpp:3178-3185).
        const int dist = coin_player_dist(*(const int*)(r + EK_SPR_X),
                                          *(const int*)(r + EK_SPR_Y),
                                          *(const int*)(r + EK_SPR_Z), px, py, pz);
        const short yvel = *(const short*)(r + EK_SPR_YVEL);
        if (coin_end_flight(magnetSel, dist, yvel) && live_coin_flight(r, pk)) {
          const short zero = 0;
          if (!write_field(i, EK_SPR_YVEL, &zero, sizeof(zero))) return;
          s_n[pk][CM_N_FLIGHT]++;
        }
      }
      continue;
    }
    if (!kindOk[k]) {
      if (st) { s_slot[i] = 0; s_track[i].picnum = 0; }
      continue;
    }
    unsigned* const n = s_n[k];
    const unsigned char kindBit = k == EK_COIN_HEALTH ? CM_SLOT_HEALTH : 0;
    if ((st & CM_SLOT_PENDING) && !has_run(r, k)) {
      // A coin that has not run yet is not the one we snapped: that was paid and the slot was reused within the tic.
      n[CM_N_PAID]++;
      st = 0;
      s_track[i].picnum = 0;
    }
    const bool wasPending = (st & CM_SLOT_PENDING) != 0;
    st &= (unsigned char)~CM_SLOT_PENDING;
    st |= kindBit;

    const short statnum = *(const short*)(r + EK_SPR_STATNUM);
    if (statnum == EK_STAT_ZOMBIE) {
      if (!(st & CM_SLOT_ASLEEP)) { st |= CM_SLOT_ASLEEP; n[CM_N_ASLEEP]++; }
    } else if (statnum == EK_STAT_ACTOR) {
      const bool hasRun = has_run(r, k);
      if (!hasRun && !(st & CM_SLOT_UNRUN)) {
        // An unrun coin is always new: a coin AA paid itself may have left this slot to a fresh one within the tic.
        st = kindBit;
        s_track[i].picnum = 0;
      }
      if (!(st & CM_SLOT_SEEN)) { st |= CM_SLOT_SEEN; n[CM_N_SEEN]++; }
      if (!hasRun) {
        // AA may yet delete it unpaid (NOTENEMY.CON:890, :589-596); not ours until it runs.
        if (!(st & CM_SLOT_UNRUN)) { st |= CM_SLOT_UNRUN; n[CM_N_UNRUN]++; }
      } else {
        st &= (unsigned char)~CM_SLOT_UNRUN;
        const int cx = *(const int*)(r + EK_SPR_X);
        const int cy = *(const int*)(r + EK_SPR_Y);
        const int cz = *(const int*)(r + EK_SPR_Z);
        const int dist = coin_player_dist(cx, cy, cz, px, py, pz);
        // A snapped health coin may wait a tic inside its ring for the perk's chase to re-sector it; it is still
        // pending.
        if (wasPending && k == EK_COIN_HEALTH && dist < coin_kind(k)->pickup)
          st |= CM_SLOT_PENDING;
        int why = -1;
        if (coin_kind_snap_instant(k, dist, radius, chase[k], speedSel)) {
          why = CM_N_INSTANT;
        } else if (speedSel != EK_CS_INSTANT) {
          const int reason = coin_kind_track_step(&s_track[i], k, coin_kind(k)->tile, dist,
                                                  chase[k], radius, percent, cx, cy, tics);
          if (reason == EK_CSNAP_BUDGET) why = CM_N_BUDGET;
          else if (reason == EK_CSNAP_COUNT) why = CM_N_COUNT;
          else if (reason == EK_CSNAP_STUCK) why = CM_N_STUCK;
          else if (reason == EK_CSNAP_SLEEP) why = CM_N_SLEEP;
        }
        if (why >= 0 &&
            !coin_kind_snap_pays(k, perks, *(const short*)(r + EK_SPR_SECTNUM), playerSect)) {
          if (!(st & CM_SLOT_HELD)) { st |= CM_SLOT_HELD; n[CM_N_HELD]++; }
        } else if (why >= 0 && still_ours(r, k)) {
          if (!write_field(i, EK_SPR_X, snapXyz, sizeof(snapXyz))) {
            s_slot[i] = st;
            s_slotDirty = true;
            return;
          }
          st |= CM_SLOT_PENDING;
          n[why]++;
        } else if (why < 0 && coin_kind_boost_wanted(k, dist, chase[k], percent) &&
                   still_ours(r, k)) {
          // Face the player exactly and set xvel so AA's own easing reaches the chosen speed this tic; AA's faceplayer
          // then turns 0.
          const short ang  = (short)coin_getangle(px - cx, py - cy);
          const short xvel = (short)coin_boost_xvel(percent, coin_planar_dist(px - cx, py - cy));
          if (!write_field(i, EK_SPR_ANG, &ang, sizeof(ang)) ||
              !write_field(i, EK_SPR_XVEL, &xvel, sizeof(xvel))) {
            s_slot[i] = st;
            s_slotDirty = true;
            return;
          }
          if (!(st & CM_SLOT_BOOSTED)) { st |= CM_SLOT_BOOSTED; n[CM_N_BOOSTED]++; }
        }
      }
    }
    s_slot[i] = st;
    if (st) s_slotDirty = true;
  }
  maybe_log();
}
