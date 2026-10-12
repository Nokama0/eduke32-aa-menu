#pragma once

// Which breakable pays cash. `droptile` (AADEFS.CON:3980, per sprite) holds the picnum it spawns on death; preload
// fills it from the map's xvel/lotag and then zeroes the sprite's lotag, so the gamevar is the only surviving copy.
// Pure: the caller reads the table and the var.

#include "gamestate.h"
#include "esp_breakobj_aa.h"
#include "esp_items_runtime_aa.h"

// No credit figure in the label: the stacks do not exist until kill time and their palette (125 or 375 credits) is
// unmeasured, and a wrong number is as wrong as a wrong name.
const char* const EKM_CASH_INSIDE_LABEL = "Cash inside";

// Wide statnum pre-filter (actor and standable lists): where `useractor` places these is unmeasured; droptile is the
// real discriminator.
inline bool ekm_breakobj_statnum_ok(int statnum) {
  return statnum == EK_STAT_ACTOR || statnum == EK_STAT_STANDABLE;
}

// A failed read marks nothing: with the var unread a money crate looks like a beer can. gotDrop and droptile are
// separate parameters, never an out-param pair.
inline bool ekm_breakobj_pays_cash(bool inTable, bool gotDrop, int droptile) {
  if (!inTable) return false;
  if (!gotDrop) return false;
  return droptile == EKM_PIC_SMALLSTACK || droptile == EKM_PIC_BIGSTACK;
}
