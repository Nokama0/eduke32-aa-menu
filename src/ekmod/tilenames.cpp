// Reads AA's names.h beside eduke32.exe (the game never parses it; it ships for Mapster32). Parsing is in tilenames.h;
// this is the worker-thread I/O and live table.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "tilenames.h"
#include "badguy.h"

// AA's names.h is 58,127 bytes; headroom for a bigger mod.
static const int TILENAMES_MAX_FILE = 131072;

static EkTileName s_table[EK_TILENAME_CAP];
static int s_count = 0;
// One attempt: an unreadable names.h will not become readable, and that is the stock-Duke case too.
static bool s_attempted = false;
// Published once the table is built and sorted, so the render thread never sees a half-filled table.
static volatile LONG s_ready = 0;
static bool s_inventoryLogged = false;

static int ekm_tilename_cmp(const void* a, const void* b) {
  const short pa = ((const EkTileName*)a)->picnum;
  const short pb = ((const EkTileName*)b)->picnum;
  return (int)pa - (int)pb;
}

bool tilenames_scan(void) {
  if (InterlockedCompareExchange(&s_ready, 0, 0)) return true;
  if (s_attempted) return false;
  s_attempted = true;

  // Same way gamecfg.cpp:22-30 finds the game folder.
  char path[MAX_PATH];
  const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
  if (!n || n >= MAX_PATH) {
    ek_log("tilenames: no module path; AA's enemy names beyond the curated "
           "table stay numbers");
    return false;
  }
  char* slash = strrchr(path, '\\');
  if (!slash) {
    ek_log("tilenames: no directory in module path");
    return false;
  }
  slash[1] = 0;
  strncat_s(path, MAX_PATH, "names.h", _TRUNCATE);

  FILE* f = 0;
  if (fopen_s(&f, path, "rb") != 0 || !f) {
    ek_log("tilenames: %s not readable; enemy names beyond the curated table "
           "stay numbers", path);
    return false;
  }
  static char buf[TILENAMES_MAX_FILE];
  const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
  const int readErr = ferror(f);
  fclose(f);
  if (readErr) {
    ek_log("tilenames: read error on %s; enemy names beyond the curated "
           "table stay numbers", path);
    return false;
  }
  // A full buffer may mean the file was cut mid-line, and a truncated #define still parses (a cut "1680" installs at
  // picnum 16): a wrong name, so the whole file is refused. Today's file cannot hit the cap.
  if (got == sizeof(buf) - 1) {
    ek_log("tilenames: %s filled the %d-byte read buffer; refusing rather "
           "than risk a wrong name off a line it may have cut in half",
           path, (int)sizeof(buf) - 1);
    return false;
  }
  buf[got] = 0;

  int parsed = 0, dupes = 0, capped = 0;
  char* ctx = 0;
  for (char* line = strtok_s(buf, "\r\n", &ctx); line; line = strtok_s(0, "\r\n", &ctx)) {
    char name[EK_TILENAME_NAME_CAP];
    int picnum = 0;
    const int ok = ekm_tilename_parse_line(line, name, (int)sizeof(name), &picnum);
    if (!ok) continue;

    // Keep the first name for a picnum and count the rest; AA aliases several picnums under more than one name.
    bool dup = false;
    for (int i = 0; i < s_count; i++) {
      if ((int)s_table[i].picnum == picnum) { dup = true; break; }
    }
    if (dup) { dupes++; continue; }

    if (s_count >= EK_TILENAME_CAP) { capped++; continue; }
    s_table[s_count].picnum = (short)picnum;
    strncpy_s(s_table[s_count].name, sizeof(s_table[s_count].name), name, _TRUNCATE);
    s_count++;
    parsed++;
  }

  qsort(s_table, (size_t)s_count, sizeof(EkTileName), ekm_tilename_cmp);

  // Duplicates are dropped, the first-seen name kept and already counted in `parsed`.
  ek_log("tilenames: %d name(s) parsed from %s, %d duplicate picnum(s) "
         "dropped, first name kept%s",
         parsed, path, dupes, capped > 0 ? ", cap reached" : ", no cap hit");

  InterlockedExchange(&s_ready, 1);
  return true;
}

bool tilenames_ready(void) {
  return InterlockedCompareExchange(&s_ready, 0, 0) != 0;
}

const char* tilenames_name(int picnum) {
  if (picnum < 0 || picnum >= EK_MAXTILES_ARR) return NULL;
  if (!tilenames_ready()) return NULL;
  return ekm_tilename_lookup(s_table, s_count, picnum);
}

// One-time inventory, so NOT behind g_cfg.debuglog. Waits for badguy_ready() and tilenames_ready(): either missing
// makes it empty or all numbers.
void tilenames_maybe_log_inventory(void) {
  if (s_inventoryLogged) return;
  if (!badguy_ready() || !tilenames_ready()) return;
  s_inventoryLogged = true;

  int total = 0;
  for (int pic = 0; pic < EK_MAXTILES_ARR; pic++)
    if (picnum_is_enemy(pic)) total++;
  ek_log("tilenames: inventory of %d enemy picnum(s) the engine flags", total);

  // Unresolved picnums print as a bare number with "?": names.h does not cover them, and they matter most.
  const int kPerLine = 8;
  char line[512];
  int inLine = 0;
  line[0] = 0;
  for (int pic = 0; pic < EK_MAXTILES_ARR; pic++) {
    if (!picnum_is_enemy(pic)) continue;
    const char* nm = tilenames_name(pic);
    char item[40];
    if (nm) _snprintf_s(item, sizeof(item), _TRUNCATE, "%d=%s", pic, nm);
    else    _snprintf_s(item, sizeof(item), _TRUNCATE, "%d=?", pic);

    if (inLine > 0) strncat_s(line, sizeof(line), ", ", _TRUNCATE);
    strncat_s(line, sizeof(line), item, _TRUNCATE);
    inLine++;
    if (inLine >= kPerLine) {
      ek_log("tilenames: %s", line);
      line[0] = 0;
      inLine = 0;
    }
  }
  if (inLine > 0) ek_log("tilenames: %s", line);
}
