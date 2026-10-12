#pragma once

#include "esp_names.h"
#include "esp_names_aa.h"
#include "esp_names_derived_aa.h"

// Pure name tiers, lifted out of esp.cpp for the suite; esp.cpp keeps the `default` arm (team read, tilenames_name). A
// team-keyed `case` arm is not the `default` arm: default runs only when no case matched, so DEANOVA and the FIGHT*
// civilians keep their own arm's name. A false `have` means no answer, not zero.
inline const char* ekm_actor_name_rules(int picnum, int pal,
                                        bool haveMtype, int mtype,
                                        bool haveTeam, int team) {
  const char* const curated = ekm_enemy_name_pal(picnum, pal);
  if (curated) return curated;

  const char* const base = ekm_enemy_name_aa(picnum);
  if (!base) return 0;

  const char* const palName = ekm_enemy_name_aa_pal(picnum, pal);
  if (palName) return palName;

  if (haveMtype) {
    const char* const mtypeName = ekm_enemy_name_aa_mtype(picnum, mtype);
    if (mtypeName) return mtypeName;
  }
  if (haveTeam) {
    const char* const teamName = ekm_enemy_name_aa_team_case(picnum, team);
    if (teamName) return teamName;
  }
  return base;
}

// Tier 3.5 base step: `derivedBase` is a rule-c base row's target, already resolved. One level, never a loop: if tiers
// 1/2 name nothing, try its row's STRING half only. WESBOTCROUCH reaches "Wes bot" only this way.
inline const char* ekm_actor_name_rules_via_derived_base(
    int derivedBase, int pal, bool haveBaseMtype, int baseMtype,
    bool haveBaseTeam, int baseTeam) {
  const char* const baseNamed = ekm_actor_name_rules(
      derivedBase, pal, haveBaseMtype, baseMtype, haveBaseTeam, baseTeam);
  if (baseNamed) return baseNamed;
  return ekm_enemy_name_derived_aa_string(derivedBase);
}
