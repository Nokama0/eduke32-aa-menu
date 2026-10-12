#include <windows.h>
#include <stdio.h>
#include "log.h"

// One session per file, a diagnostic property: an appended log reached 63 MB and stale lines were read as evidence
// about today's build. The previous run's log is kept with a .prev suffix. Interlocked: two first calls must roll once.
static void log_roll_once(const char* path) {
  static volatile LONG s_rolled = 0;
  if (InterlockedCompareExchange(&s_rolled, 1, 0) != 0) return;
  char prev[MAX_PATH];
  if (strlen(path) + 6 >= MAX_PATH) return;
  strcpy_s(prev, MAX_PATH, path);
  strcat_s(prev, MAX_PATH, ".prev");
  MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
}

static volatile LONG s_dropped = 0;
// Last s_dropped value announced; interlocked so racing threads cannot announce the same rise.
static volatile LONG s_dropped_reported = 0;

long ek_log_dropped_count() { return s_dropped; }

// The one place that opens the file; takes a terminated line so the drop note can call it without recursing.
// FILE_SHARE_READ | FILE_SHARE_WRITE, not READ alone: a second thread's CreateFileA would get a sharing violation and
// lose the line. Safe because FILE_APPEND_DATA makes each write atomic at end-of-file.
static bool ek_log_raw_write(const char* path, const char* line, int len) {
  const HANDLE h = CreateFileA(path, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    InterlockedIncrement(&s_dropped);
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(h, line, (DWORD)(len + 1), &written, NULL);
  CloseHandle(h);
  if (ek_log_write_lost((unsigned long)(len + 1), ok != 0, written)) {
    InterlockedIncrement(&s_dropped);
    return false;
  }
  return true;
}

void ek_log(const char* fmt, ...) {
  char path[MAX_PATH];
  const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  char* slash = strrchr(path, '\\');
  if (!slash) return;
  slash[1] = 0;
  if (strlen(path) + 12 >= MAX_PATH) return;
  strcat_s(path, MAX_PATH, "ekmod.log");
  log_roll_once(path);

  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(line, sizeof(line) - 2, fmt, ap);
  va_end(ap);
  if (len <= 0) return;
  // Truncate, do not overrun: MSVC's vsnprintf returns the length it WOULD have written, so the newline was stored
  // past the end; an off-by-one clamp once put NULs in the log (see ek_log_keep_len).
  len = ek_log_keep_len(len, (int)sizeof(line));
  if (len <= 0) return;
  line[len] = '\n';

  const bool wrote_ok = ek_log_raw_write(path, line, len);

  // Self-limiting drop note: makes a loss visible on runs that crash. Only after THIS write succeeded; once per rise
  // (InterlockedExchange reads and stores atomically); via ek_log_raw_write, so it cannot recurse.
  if (wrote_ok) {
    const long cur = s_dropped;
    const long last = InterlockedExchange(&s_dropped_reported, cur);
    if (cur > last) {
      char note[128];
      const int nlen = _snprintf_s(note, sizeof(note), _TRUNCATE,
                                    "log: %ld line(s) lost to a failed or short write so far this session", cur);
      if (nlen > 0) {
        note[nlen] = '\n';
        ek_log_raw_write(path, note, nlen);
      }
    }
  }
}
