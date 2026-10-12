#pragma once

#include <stddef.h>
#include "tiles.h"

// Names for picnums the curated table (esp_names.h) does not cover, parsed from AA's names.h (shipped beside
// eduke32.exe for Mapster32). This header is the pure parser and lookup; tilenames.cpp does the I/O and the
// worker-thread scan.

// 20 chars is the longest name measured in AA's names.h (BALLOONCOLORFULBUNCH); 24 rounds that up past the NUL.
const int EK_TILENAME_NAME_CAP = 24;
struct EkTileName { short picnum; char name[EK_TILENAME_NAME_CAP]; };

const int EK_TILENAME_CAP = 4096;

// One `#define NAME picnum` line, or refused; an out-param is never read in the argument list that fills it.
inline int ekm_tilename_parse_line(const char* line, char* nameOut, int nameCap,
                                    int* picnumOut) {
  if (!line || !nameOut || nameCap <= 0 || !picnumOut) return 0;
  const char* p = line;
  while (*p == ' ' || *p == '\t') p++;

  static const char kDefine[] = "#define";
  int i = 0;
  for (; kDefine[i]; i++) if (p[i] != kDefine[i]) return 0;
  p += i;
  if (*p != ' ' && *p != '\t') return 0;
  while (*p == ' ' || *p == '\t') p++;

  const char* nameStart = p;
  while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
         (*p >= '0' && *p <= '9') || *p == '_') p++;
  const int nameLen = (int)(p - nameStart);
  if (nameLen <= 0 || nameLen >= nameCap) return 0;
  if (*p != ' ' && *p != '\t') return 0;
  while (*p == ' ' || *p == '\t') p++;

  // A leading '-' is recognised and then refused, not treated as "not a literal".
  const bool neg = (*p == '-');
  if (neg) p++;
  const char* digStart = p;
  while (*p >= '0' && *p <= '9') p++;
  if (p == digStart) return 0;

  const char* q = p;
  while (*q == ' ' || *q == '\t') q++;
  if (*q != 0 && !(q[0] == '/' && (q[1] == '/' || q[1] == '*'))) return 0;

  if (neg) return 0;

  int picnum = 0;
  for (const char* d = digStart; d < p; d++) {
    picnum = picnum * 10 + (*d - '0');
    if (picnum >= EK_MAXTILES_ARR) { picnum = EK_MAXTILES_ARR; break; }
  }
  if (picnum < 0 || picnum >= EK_MAXTILES_ARR) return 0;

  for (int k = 0; k < nameLen; k++) nameOut[k] = nameStart[k];
  nameOut[nameLen] = 0;
  *picnumOut = picnum;
  return 1;
}

// Binary search over a table SORTED ASCENDING by picnum, null when absent or the table is empty.
inline const char* ekm_tilename_lookup(const EkTileName* table, int count, int picnum) {
  if (!table || count <= 0) return NULL;
  int lo = 0, hi = count - 1, found = -1;
  const short want = (short)picnum;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    const short p = table[mid].picnum;
    if (p == want) { found = mid; hi = mid - 1; }
    else if (p < want) lo = mid + 1;
    else hi = mid - 1;
  }
  return found >= 0 ? table[found].name : NULL;
}

// Guarded because test_host links the pure half only.
#ifndef EK_TILENAMES_PURE_ONLY
bool tilenames_scan(void);
bool tilenames_ready(void);
const char* tilenames_name(int picnum);
void tilenames_maybe_log_inventory(void);
#endif
