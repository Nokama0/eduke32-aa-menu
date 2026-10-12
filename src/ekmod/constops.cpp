// File half of the CON staleness guard: compares installed scripts\*.CON with the fingerprint in con_stops_aa.h (rules:
// constops.h). It turns nothing off (CON frames are no longer stops); the verdict is printed beside firestops.cpp's
// sets. Worker thread only, via firestops_publish; the stop set crosses behind firestops.cpp's sequence counter.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "constops.h"

static int s_state = EK_CON_UNKNOWN;

// One attempt only: an unreadable scripts folder will not become readable, and stock Duke is the same case.
static bool s_attempted = false;

int constops_state(void) {
  if (s_attempted) return s_state;
  s_attempted = true;

  // Same way of finding the game folder as gamecfg.cpp and tilenames.cpp.
  char path[MAX_PATH];
  const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
  if (!n || n >= MAX_PATH) {
    s_state = EK_CON_UNKNOWN;
    ek_log("constops: no module path; whether AA's CON still matches "
           "con_stops_aa.h cannot be checked. The fire rate is unaffected -- "
           "its stops come from the engine's own weapon data");
    return s_state;
  }
  char* slash = strrchr(path, '\\');
  if (!slash) {
    s_state = EK_CON_UNKNOWN;
    ek_log("constops: no directory in module path; AA's CON cannot be checked "
           "against con_stops_aa.h. The fire rate is unaffected");
    return s_state;
  }
  slash[1] = 0;
  strncat_s(path, MAX_PATH, "scripts\\*.CON", _TRUNCATE);

  int found = 0, matched = 0, unknownName = 0, wrongSize = 0;
  char firstBad[64];
  firstBad[0] = 0;

  WIN32_FIND_DATAA fd;
  const HANDLE h = FindFirstFileA(path, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    // Not found is absent (stock Duke has no scripts folder); a refusal (sharing, permissions) is unknown.
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
      s_state = EK_CON_ABSENT;
      ek_log("constops: no scripts\\*.CON beside the exe -- not an AA install. "
             "The stops are exactly what the engine's own weapon data derives");
    } else {
      s_state = EK_CON_UNKNOWN;
      ek_log("constops: scripts\\*.CON could not be listed (error %lu); AA's "
             "CON cannot be checked against con_stops_aa.h. The fire rate is "
             "unaffected", (unsigned long)err);
    }
    return s_state;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    found++;
    // A file over 4 GB is not an AA script; a high word is a disagreement, not something to truncate into a match.
    if (fd.nFileSizeHigh != 0) {
      wrongSize++;
      if (!firstBad[0])
        strncpy_s(firstBad, sizeof(firstBad), fd.cFileName, _TRUNCATE);
      continue;
    }
    // -1 means the table has no such script, a different disagreement from a size that moved.
    const long want = ekm_con_recorded_size(fd.cFileName);
    if (want < 0) {
      unknownName++;
      if (!firstBad[0])
        strncpy_s(firstBad, sizeof(firstBad), fd.cFileName, _TRUNCATE);
      continue;
    }
    if (want != (long)fd.nFileSizeLow) {
      wrongSize++;
      if (!firstBad[0])
        strncpy_s(firstBad, sizeof(firstBad), fd.cFileName, _TRUNCATE);
      continue;
    }
    matched++;
  } while (FindNextFileA(h, &fd));
  FindClose(h);

  const int verdict = ekm_con_verdict(found, matched);
  s_state = verdict;

  if (verdict == EK_CON_ABSENT) {
    ek_log("constops: scripts\\ holds no *.CON file. The stops are exactly "
           "what the engine's own weapon data derives");
    return s_state;
  }
  if (verdict == EK_CON_MATCH) {
    // Match means in force ABOVE TotalTime only: those frames join the stop set (ekm_firestops_derive_aa) because the
    // fire rate skipped kickback_pic 26, where AA's CON refills the pistol clip. Frames AT OR BELOW TotalTime made it
    // inert.
    ek_log("constops: %d of %d script(s) match con_stops_aa.h. The table "
           "describes this install, and its frames ABOVE each weapon's "
           "TotalTime ARE applied as stops (D12); the frames at or below it "
           "are not, because applying those made the fire rate inert (zero "
           "advances at 400%%)",
           matched, EKM_CON_FILE_COUNT);
    return s_state;
  }

  // Loud but not a refusal: a stale table costs evidence, not behaviour. The Executioner's refusal reads gotshock at
  // runtime.
  ek_log("constops: the frozen CON table is STALE and is now only evidence. "
         "The installed CON does not match the table this build was generated "
         "from: %d script(s) found against %d recorded, %d matched, %d name(s) "
         "the table does not know, %d of a different size%s%s. Re-run "
         "tools/gen_con_stops_aa.py against this install and rebuild before "
         "trusting any frame number it reports. AND THE FIRE RATE IS NOW "
         "AFFECTED (D12): since the reload-ladder union it applies this "
         "table's frames above TotalTime, so a mismatch means STALE FRAMES "
         "ARE BEING APPLIED -- the union is switched off for safety and the "
         "stops fall back to the engine's weapon data alone, which is D12 "
         "again until the table is regenerated.",
         found, EKM_CON_FILE_COUNT, matched, unknownName, wrongSize,
         firstBad[0] ? "; first disagreement: " : "",
         firstBad[0] ? firstBad : "");
  return s_state;
}
