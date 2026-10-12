#pragma once

#include "con_stops_aa.h"

// Staleness guard for AA's CON stops (con_stops_aa.h is a frozen reading of one install's CON). A measurement, not a
// gate: applying all CON frames as stops made the fire rate inert, so the union is deleted and the verdict is only
// logged (firestops.cpp).

// UNKNOWN (not checked) is not ABSENT (no AA CON): opposite answers, as in ekm_firestops_verdict.
enum { EK_CON_UNKNOWN = 0, EK_CON_ABSENT = 1, EK_CON_MATCH = 2, EK_CON_MISMATCH = 3 };

// Zero files is ABSENT (stock Duke and World Tour stay untouched); any other disagreement is MISMATCH.
inline int ekm_con_verdict(int found, int matched) {
  if (found <= 0) return EK_CON_ABSENT;
  if (found != EKM_CON_FILE_COUNT) return EK_CON_MISMATCH;
  if (matched != EKM_CON_FILE_COUNT) return EK_CON_MISMATCH;
  return EK_CON_MATCH;
}

inline const char* ekm_con_state_name(int conState) {
  if (conState == EK_CON_ABSENT)   return "absent";
  if (conState == EK_CON_MATCH)    return "match";
  if (conState == EK_CON_MISMATCH) return "mismatch";
  return "unknown";
}

// Cached; worker thread only (firestops_publish), so no interlocking.
int constops_state(void);
