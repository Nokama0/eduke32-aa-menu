#pragma once

// THE ITEM REFUSAL CENSUS: which guard in esp_collect_items would refuse a sprite the item table already names.
#include <string.h>
#include "gamestate.h"
#include "esp_items.h"

enum {
  // Passes the four guards esp_collect_items applies before its trace; three more above it (draw distance,
  // project_point, box projection) are not modelled.
  EKM_CENSUS_WOULD_DRAW = 0,
  EKM_CENSUS_STATNUM    = 1,
  EKM_CENSUS_HIDDEN     = 2,
  EKM_CENSUS_ZERO_XREP  = 3,
};

// Which guard would refuse this sprite; the order is the whole instrument.
inline int ekm_item_census_reason(int statnum, int cstat, int xrep) {
  if (statnum != EK_STAT_ACTOR && statnum != EK_STAT_ZOMBIE)
    return EKM_CENSUS_STATNUM;
  if (ekm_item_present(cstat, xrep)) return EKM_CENSUS_WOULD_DRAW;
  return ekm_item_present(cstat, 1) ? EKM_CENSUS_ZERO_XREP : EKM_CENSUS_HIDDEN;
}

inline const char* ekm_item_census_reason_name(int reason) {
  switch (reason) {
    case EKM_CENSUS_WOULD_DRAW: return "would-draw";
    case EKM_CENSUS_STATNUM:    return "statnum";
    case EKM_CENSUS_HIDDEN:     return "hidden";
    case EKM_CENSUS_ZERO_XREP:  return "zero-xrep";
    default:                    return "unknown";
  }
}

// AA's figurine band MINIDUKE 16010..MINIBOSS2FLY 16060: 51 picnums, each a named row in esp_items_aa.h (the suite
// pins it).
inline bool ekm_item_census_is_figurine(int picnum) {
  return picnum >= 16010 && picnum <= 16060;
}

// Not per-frame or per-second: it enumerates all EK_MAXSPRITES slots and can emit up to EK_ITEM_CENSUS_LINE_MAX
// lines.
const unsigned EK_ITEM_CENSUS_PERIOD_MS = 60000;

// Per-sprite lines are capped; the totals never are (a capped total is a count and an enumeration that disagree).
const int EK_ITEM_CENSUS_LINE_MAX = 256;

// Same shape as ekm_wall_scan_due (esp_breakables.h): the signature is mapextent's (numsectors << 16) ^ numwalls and
// lags a load by up to a second.
inline bool ekm_item_census_due(int censused, int sigNow, int sigWas,
                                unsigned nowMs, unsigned lastMs) {
  if (!censused) return true;
  if (sigNow != sigWas) return true;
  return (unsigned)(nowMs - lastMs) >= EK_ITEM_CENSUS_PERIOD_MS;
}
