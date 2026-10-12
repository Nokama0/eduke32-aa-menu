#pragma once

// The menu runs only inside eduke32.exe. mapster32.exe shares its folder and imports VERSION.dll and SwapBuffers too,
// so without this the editor would get the hooks and engine writes. Any other host gets a pure forwarder.
inline bool proxy_host_is_game(const wchar_t* path) {
  if (!path) return false;
  const wchar_t* base = path;
  for (const wchar_t* p = path; *p; ++p)
    if (*p == L'\\' || *p == L'/') base = p + 1;
  static const wchar_t kName[] = L"eduke32.exe";
  int i = 0;
  for (; kName[i]; ++i) {
    wchar_t c = base[i];
    if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + (L'a' - L'A'));
    if (c != kName[i]) return false;
  }
  return base[i] == 0;
}
