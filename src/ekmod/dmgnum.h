#pragma once

// Floating damage numbers, rules only. Flags are int: MSVC /O2 loses a bool member.

#include <math.h>

const int EKM_DMG_MAX = 32;

const unsigned EKM_DMG_LIFE_MS = 750;

struct EkDmgEvent { int x, y, z; int damage; int kill; int victim; unsigned t0; int live; };
struct EkDmgRing  { EkDmgEvent e[EKM_DMG_MAX]; int next; };

struct EkDmgPending { int victim, damage, owner, health, x, y, z; };

inline void ekm_dmg_reset(EkDmgRing* r) {
  if (!r) return;
  for (int i = 0; i < EKM_DMG_MAX; i++) {
    r->e[i].x = 0; r->e[i].y = 0; r->e[i].z = 0;
    r->e[i].damage = 0; r->e[i].kill = 0; r->e[i].victim = -1;
    r->e[i].t0 = 0u; r->e[i].live = 0;
  }
  r->next = 0;
}

inline void ekm_dmg_push(EkDmgRing* r, const EkDmgEvent& ev) {
  if (!r) return;
  r->e[r->next] = ev;
  r->e[r->next].live = 1;
  r->next = (r->next + 1) % EKM_DMG_MAX;
}

inline int ekm_dmg_event_wanted(int owner, int localPlayerSprite,
                                int victimIsLiveEnemy, int damage) {
  if (owner < 0) return 0;
  if (localPlayerSprite < 0) return 0;
  if (owner != localPlayerSprite) return 0;
  if (!victimIsLiveEnemy) return 0;
  return damage > 0 ? 1 : 0;
}

inline int ekm_dmg_is_live(const EkDmgEvent& ev, unsigned now) {
  if (!ev.live) return 0;
  return (unsigned)(now - ev.t0) < EKM_DMG_LIFE_MS ? 1 : 0;
}

inline double ekm_dmg_progress(unsigned t0, unsigned now) {
  const unsigned age = now - t0;
  if (age >= EKM_DMG_LIFE_MS) return 1.0;
  return (double)age / (double)EKM_DMG_LIFE_MS;
}

inline int ekm_dmg_rise_px(double p, int textH) {
  if (p < 0.0) p = 0.0;
  if (p > 1.0) p = 1.0;
  const double q = 1.0 - (1.0 - p) * (1.0 - p);
  return (int)floor(q * (double)textH + 0.5);
}

inline int ekm_dmg_alpha(double p) {
  if (p <= 2.0 / 3.0) return 255;
  if (p >= 1.0) return 0;
  return (int)floor(255.0 * (1.0 - p) * 3.0 + 0.5);
}

inline unsigned ekm_dmg_colour(int kill, unsigned hit, unsigned killColour) {
  return kill ? killColour : hit;
}

inline int ekm_dmg_stack_index(const EkDmgRing& r, int idx, unsigned now) {
  if (idx < 0 || idx >= EKM_DMG_MAX) return 0;
  if (!ekm_dmg_is_live(r.e[idx], now)) return 0;
  int n = 0;
  for (int k = 0; k < EKM_DMG_MAX; k++) {
    const int s = (r.next + k) % EKM_DMG_MAX;
    if (s == idx) break;
    if (ekm_dmg_is_live(r.e[s], now) && r.e[s].victim == r.e[idx].victim) n++;
  }
  return n;
}

// dmgnum_pre reads damage, owner and the victim's health BEFORE the original runs, since A_IncurDamage subtracts it
// (actors.cpp:1322); dmgnum_post only if it returned >= 0. Game thread.
int  dmgnum_pre(int spriteNum, EkDmgPending* out);
void dmgnum_post(const EkDmgPending* p);

// Inside the overlay's canvas frame. Unconditional, so drawn always zeroes.
void dmgnum_draw(int active);

// entered 0 = hook not installed or no damage; entered > 0 with kept 0 and rejected 0 = the original vetoed every hit.
int dmgnum_entered(void);
int dmgnum_kept(void);
int dmgnum_rejected(void);
int dmgnum_drawn(void);
