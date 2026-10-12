#pragma once

#include "esp_items_aa.h"
#include "esp_items_runtime_aa.h"

// Curated tier for AA's own pickups, hand-written unlike esp_items_aa.h: the last 27 pickups the generator could not
// name from a userquote, quote or *UPGRADE de-camel. RAILGUNSPRITE/SMALLSTACK/BIGSTACK come from live state instead
// (esp_items_runtime_aa.h). Evidence is cited file:line into AA's scripts/*.CON.
inline const char* ekm_item_name_curated_aa(int picnum) {
  switch (picnum) {
    case 21:    return "Pistol";
    case 5178:  return "Swinelord's gun";
    case 8176:  return "Glock 18";
    case 8938:  return "Versa-bow";
    case 11005: return "Dual pistols (Duke)";
    case 11006: return "Dual pistols (Bombshell)";
    case 19960: return "Stake";
    case 22639: return "RadAway";
    case 24560: return "Item";
                // TOKYOJIHEN: decided no text names the object (AAITEMS.CON:4556-4638 fires only a map trigger and a
                // dialogue).

    case 16014: return "Wes Wolf figurine";
    case 16016: return "Figurine";
                // MINIPIG: refused Pigcop/Swinelord/Armored Pig; all three already have their own figurine rows.
    case 16019: return "Figurine";
                // MINIBEAST: refused Drone/Cyberbeast.
    case 16021: return "EDF figurine";
                // MINIEDF1-3: three EDF actors share nameplate "EDF SOLDIER" but "EDF" also names the drone/tank/pig,
                // so generic.
    case 16022: return "EDF figurine";
    case 16023: return "EDF figurine";
    case 16027: return "Figurine";
                // MINIMECH: refused Mechbrain/Big Mech.
    case 16034: return "Emperor Braccus figurine";
                // MINIBRACCUS: the speaker is Emperor Braccus; Natty Braccus is the TRANSFORMED form with his own
                // figurine (MININATTY).
    case 16036: return "Cycloid Emperor figurine";
                // MINICYCLOID: corrects the generated "Cycloid Maximus figurine", which collides with MINICYCJR's
                // real Maximus Jr; the tile is the classic Cycloid/BOSS2. Checked ahead of the generated tier.
    case 16038: return "Overlord figurine";
    case 16045: return "Bigboi figurine";
    case 16046: return "Littleman figurine";
    case 16048: return "Cycloid Maximus Jr figurine";
                // MINICYCJR: CYCLOIDMAX.CON:1 "CYCLODIUS MAXIMUS JR"; disambiguates from 16036.
    case 16054: return "Female fighter figurine";
    case 16055: return "Male fighter figurine";
    case 16056: return "General Graves figurine";
    case 16057: return "Axon figurine";
    case 16058: return "Natty Braccus figurine";
    case 16059: return "Armored Trooper figurine";
    default:    return 0;
  }
}

// The MINI* figurine band, AADEFS.CON:2108-2158 (MINIDUKE..MINIBOSS2FLY).
const int EKM_MINI_FIGURINE_FIRST = 16010;
const int EKM_MINI_FIGURINE_LAST  = 16060;

// A category name for a picnum whose name is still its own CON symbol: the guard for a future AA update adding a
// pickup no tier resolves. Unreachable for the 27 above, so it is pinned directly.
inline const char* ekm_item_category_aa(int picnum) {
  if (picnum >= EKM_MINI_FIGURINE_FIRST && picnum <= EKM_MINI_FIGURINE_LAST)
    return "Figurine";
  return "Item";
}

// Whether the generated tier's own row kept the CON symbol AND no later tier claims it; a picnum absent from that
// table is not a kept-symbol case. The runtime trio is excluded: esp_items_runtime_aa.h names them from live state
// one step later, and a failed live read must fall through to the CON symbol, not "Item".
inline bool ekm_item_aa_symbol_kept(int picnum) {
  if (picnum == EKM_PIC_RAILGUNSPRITE || picnum == EKM_PIC_SMALLSTACK ||
      picnum == EKM_PIC_BIGSTACK)
    return false;
  for (int i = 0; i < EKM_ITEM_AA_COUNT; i++) {
    const EkItemAaRow row = ekm_item_aa_rows()[i];
    if ((int)row.picnum == picnum) return row.rule == 0;
  }
  return false;
}
