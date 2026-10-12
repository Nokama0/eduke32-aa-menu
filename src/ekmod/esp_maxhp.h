#pragma once

#include "esp_names.h"

// The one maximum bar and label use. No static table fits AA: it rolls `mlevel` (AAGAME.CON:11344), scales health off
// it (:11527) and buffs for `monstflags` (:11578). AA's `monhud` divides by per-actor `inithp` (weapons_hud.CON:2807):
// 0 means no answer, so stock Duke and World Tour fall through to the old tiers.
inline int ekm_actor_resolved_max(int initHp, int ruledMax, int tableMax,
                                  int seen, int shown) {
  // Floored at the shown value: AA buffs health past `inithp`, and a bar past its end reads as broken.
  if (initHp > 0) return ekm_enemy_bar_max(initHp, shown);

  const int ruledBar = ekm_enemy_bar_max(ruledMax, shown);
  if (ruledBar > 0) return ruledBar;
  return ekm_enemy_shown_max(tableMax, seen);
}

// Which tier supplied the value, shared with the debug line; ties favour the learned ceiling.
enum class EkMaxTier { Inithp, Ruled, Table, Seen };

inline EkMaxTier ekm_actor_max_tier(int initHp, int ruledMax, int tableMax,
                                    int seen, int shown) {
  if (initHp > 0) return EkMaxTier::Inithp;
  if (ekm_enemy_bar_max(ruledMax, shown) > 0) return EkMaxTier::Ruled;
  return tableMax > seen ? EkMaxTier::Table : EkMaxTier::Seen;
}

inline const char* ekm_actor_max_tier_name(EkMaxTier tier) {
  switch (tier) {
    case EkMaxTier::Inithp: return "inithp";
    case EkMaxTier::Ruled:  return "ruled";
    case EkMaxTier::Table:  return "table";
    default:                return "seen";
  }
}
