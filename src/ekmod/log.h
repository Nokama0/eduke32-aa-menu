#pragma once
// Raw kernel32, no CRT, no allocation: some of this runs during loader initialisation.
void ek_log(const char* fmt, ...);

long ek_log_dropped_count();

inline bool ek_log_write_lost(unsigned long requested, bool ok, unsigned long written) {
  return !ok || written != requested;
}

// Characters to keep. cap-3, not cap-2: vsnprintf was given cap-2 and stores cap-3 characters plus its NUL; cap-2
// shipped an embedded NUL on truncated lines.
inline int ek_log_keep_len(int want, int cap) {
  if (want <= 0 || cap < 3) return 0;
  return want > cap - 3 ? cap - 3 : want;
}
