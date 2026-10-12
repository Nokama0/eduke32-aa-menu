// Floating damage numbers: ring, detour halves and draw (rules: dmgnum.h). dmgnum_pre/post run on the game thread in
// A_IncurDamage's detour, dmgnum_draw on the render thread; they meet only at s_ring. Benign race: one slot filled and
// an index advanced, indices modulo EKM_DMG_MAX. Nothing engine-side is written.
#include <windows.h>
#include "dmgnum.h"
#include "config.h"
#include "gamestate.h"
#include "actorstate.h"
#include "badguy.h"
#include "playerstate.h"
#include "project.h"
#include "esp.h"
#include "aimsolve.h"
#include "ui/canvas.h"
#include "ui/font.h"
#include "ui/wallclock.h"
#include "esp_layout.h"
#include "log.h"

static EkDmgRing s_ring;
// Read on the render thread, so an interlocked scalar.
static volatile LONG s_ringReady = 0;
static int s_entered = 0, s_kept = 0, s_rejected = 0, s_drawn = 0;
static int s_loggedEntry = 0, s_loggedFirst = 0;

int dmgnum_entered(void)  { return s_entered; }
int dmgnum_kept(void)     { return s_kept; }
int dmgnum_rejected(void) { return s_rejected; }
int dmgnum_drawn(void)    { return s_drawn; }

static int dmg_sprite_short(int i, int off, int* out) {
  const unsigned char* const r = gamestate_sprite(i);
  if (!r) return 0;
  *out = (int)*(const short*)(r + off);
  return 1;
}

static int dmg_sprite_int(int i, int off, int* out) {
  const unsigned char* const r = gamestate_sprite(i);
  if (!r) return 0;
  *out = *(const int*)(r + off);
  return 1;
}

static int dmg_actor_owner(int i, int* out) {
  const unsigned char* const r = actorstate_record(i);
  if (!r || !out) return 0;
  // Wrapped as actorstate_pending_damage is (actorstate.cpp:554): a fault here takes the game down.
  __try {
    *out = (int)*(const short*)(r + EK_ACT_HTOWNER);
    return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

int dmgnum_pre(int spriteNum, EkDmgPending* out) {
  if (!out) return 0;
  if (!g_cfg.dmgnumbers) return 0;
  if (spriteNum < 0 || spriteNum >= EK_MAXSPRITES) return 0;
  if (!actorstate_ready()) return 0;
  // Without g_tile, picnum_is_enemy answers false for everything (badguy.h:23), charging every hit to s_rejected.
  if (!badguy_ready()) return 0;

  if (!s_ringReady) { ekm_dmg_reset(&s_ring); InterlockedExchange(&s_ringReady, 1); }

  s_entered++;

  int damage = 0;
  if (!actorstate_pending_damage(spriteNum, &damage)) return 0;
  int owner = 0;
  if (!dmg_actor_owner(spriteNum, &owner)) return 0;
  int health = 0;
  if (!dmg_sprite_short(spriteNum, EK_SPR_EXTRA, &health)) return 0;
  int picnum = 0;
  if (!dmg_sprite_short(spriteNum, EK_SPR_PICNUM, &picnum)) return 0;

  // Proves the detour is entered before any filter: World Tour put this inside the accept, so "never called" and
  // "filtered" looked the same.
  if (!s_loggedEntry && g_cfg.debuglog) {
    s_loggedEntry = 1;
    ek_log("dmgnum: detour entered -- victim sprite %d, damage %d, owner %d, "
           "victim health %d, picnum %d. The event is kept only when the owner "
           "is the player's own sprite and the victim is a live enemy",
           spriteNum, damage, owner, health, picnum);
  }

  const int liveEnemy = (health > 0 && picnum_is_enemy(picnum)) ? 1 : 0;
  // -1 means "cannot tell", which ekm_dmg_event_wanted refuses.
  const int mine = (owner >= 0 && player_sprite_matches(owner)) ? owner : -1;

  if (!ekm_dmg_event_wanted(owner, mine, liveEnemy, damage)) { s_rejected++; return 0; }

  int px = 0, py = 0, pz = 0;
  if (!dmg_sprite_int(spriteNum, EK_SPR_X, &px)) return 0;
  if (!dmg_sprite_int(spriteNum, EK_SPR_Y, &py)) return 0;
  if (!dmg_sprite_int(spriteNum, EK_SPR_Z, &pz)) return 0;

  // Untouched on 0, as dmgnum.h promises: the position reads can still refuse, so the struct is filled last.
  out->victim = spriteNum;
  out->damage = damage;
  out->owner  = owner;
  out->health = health;
  out->x = px; out->y = py; out->z = pz;
  return 1;
}

void dmgnum_post(const EkDmgPending* p) {
  if (!p) return;
  EkDmgEvent ev;
  ev.x = p->x; ev.y = p->y; ev.z = p->z;
  ev.damage = p->damage;
  ev.kill   = (p->health - p->damage) <= 0 ? 1 : 0;
  ev.victim = p->victim;
  ev.t0     = wallclock_ms();
  ev.live   = 1;
  ekm_dmg_push(&s_ring, ev);
  s_kept++;

  if (!s_loggedFirst && g_cfg.debuglog) {
    s_loggedFirst = 1;
    ek_log("dmgnum: first hit KEPT -- victim sprite %d, damage %d, health %d "
           "before the hit, kill=%d. A damage of zero here would mean actor "
           "offset %d is not htextra", p->victim, p->damage, p->health,
           ev.kill, EK_ACT_HTEXTRA);
  }
}

// Projected where each event was ANCHORED AT HIT TIME, not where the victim is: a freed slot keeps its picnum and lotag
// (initspritelists resets only sectnum and statnum). No canvas frame of its own: overlay_frame wraps the pass.
void dmgnum_draw(int active) {
  s_drawn = 0;
  if (!active) return;
  if (!g_cfg.dmgnumbers) return;
  if (!s_ringReady) return;

  EkProjection p;
  const bool haveProj = project_begin(&p, EK_PC_DMGNUM);
  if (!haveProj) return;

  const unsigned now = wallclock_ms();
  const int textH = font_height[FONT_UI];
  const unsigned killColour = canvas_esp_colour(true);
  const bool hivis = g_cfg.hivis != 0;

  for (int k = 0; k < EKM_DMG_MAX; k++) {
    const EkDmgEvent& e = s_ring.e[k];
    if (!ekm_dmg_is_live(e, now)) continue;

    const int dist2d = esp_dist2d_public(p.cam.x, p.cam.y, e.x, e.y);
    if (!ekm_within_draw_distance(dist2d, g_cfg.espdrawdist, g_cfg.espnolimit)) continue;

    float sx = 0.0f, sy = 0.0f;
    const bool onScreen = project_point(p, e.x, e.y, e.z, &sx, &sy);
    if (!onScreen) continue;

    char s[16];
    wsprintfA(s, "%d", e.damage);
    const double prog = ekm_dmg_progress(e.t0, now);
    const int rise = ekm_dmg_rise_px(prog, textH)
                   + ekm_dmg_stack_index(s_ring, k, now) * textH;
    const unsigned col = canvas_with_alpha(ekm_dmg_colour(e.kill, C_HAZARD, killColour),
                                           ekm_dmg_alpha(prog));
    esp_draw_label(hivis, (int)sx - canvas_text_width(s, FONT_UI) / 2, (int)sy - rise,
                   s, col, FONT_UI);
    s_drawn++;
  }
}
