#pragma once

// The engine's own -nologo. g_noLogo (screens.cpp:50) is read by G_DisplayLogo before the first frame, so config_load
// is too late.

#include <stdlib.h>
#include <string.h>

inline bool ekm_nologo_anchors_agree(unsigned a, unsigned b, unsigned lo, unsigned hi) {
  if (a == 0 || a != b) return false;
  if (lo >= hi || a < lo || a >= hi) return false;
  return hi - a >= 4;
}

inline const char* ekm_ini_value_for(const char* line, const char* key) {
  if (!line || !key) return NULL;
  const char* p = line;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == 0 || *p == ';' || *p == '#' || *p == '[' || *p == '\n') return NULL;
  const char* const eq = strchr(p, '=');
  if (!eq) return NULL;
  const char* end = eq;
  while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
  const size_t n = strlen(key);
  if (n == 0 || (size_t)(end - p) != n) return NULL;
  if (_strnicmp(p, key, n) != 0) return NULL;
  return eq + 1;
}

// Same strtol(val, NULL, 0) as config_load.
inline int ekm_skipintro_on(const char* val) {
  if (!val) return 0;
  return strtol(val, NULL, 0) != 0 ? 1 : 0;
}

void skipintro_attach(void);
