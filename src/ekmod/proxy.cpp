// version.dll proxy, the load vector. EDuke32 is statically linked and ships no DLLs; of its imports only version,
// dsound, winmm, wsock32 and comctl32 are absent from KnownDLLs and can be shadowed beside the exe. version.dll wins:
// three imports, in the static import table, so it loads at process start.

#include <windows.h>
#include <stdio.h>
#include "log.h"
#include "iat.h"
#include "gamehooks.h"
#include "camera.h"
#include "shot.h"
#include "spiketotaltime.h"
#include "maskscan.h"
#include "nocull.h"
#include "cutaway.h"
#include "skipintro.h"
#include "hostcheck.h"

// The real version.dll is resolved LAZILY, not in DllMain: LoadLibrary under the loader lock can deadlock.
static volatile HMODULE s_real = NULL;

static HMODULE real_version(void) {
  HMODULE m = (HMODULE)InterlockedCompareExchangePointer((PVOID volatile*)&s_real, NULL, NULL);
  if (m) return m;

  // By absolute path, always: a bare LoadLibraryA("version.dll") would find this very file and every forward recurse.
  char sys[MAX_PATH];
  const UINT n = GetSystemDirectoryA(sys, MAX_PATH);
  if (n == 0 || n >= MAX_PATH - 16) return NULL;
  strcat_s(sys, MAX_PATH, "\\version.dll");

  m = LoadLibraryA(sys);
  if (!m) return NULL;

  const HMODULE prev = (HMODULE)InterlockedCompareExchangePointer((PVOID volatile*)&s_real, m, NULL);
  if (prev) { FreeLibrary(m); return prev; }
  return m;
}

static FARPROC real_proc(const char* name) {
  const HMODULE m = real_version();
  return m ? GetProcAddress(m, name) : NULL;
}

#define FORWARD(ret, name, params, args, fail)                       \
  extern "C" ret WINAPI name params {                                \
    typedef ret(WINAPI * fn_t) params;                               \
    static fn_t fn = NULL;                                           \
    if (!fn) fn = (fn_t)real_proc(#name);                            \
    if (!fn) return fail;                                            \
    return fn args;                                                  \
  }

FORWARD(BOOL, GetFileVersionInfoA,
        (LPCSTR f, DWORD h, DWORD len, LPVOID data), (f, h, len, data), FALSE)
FORWARD(BOOL, GetFileVersionInfoW,
        (LPCWSTR f, DWORD h, DWORD len, LPVOID data), (f, h, len, data), FALSE)
FORWARD(BOOL, GetFileVersionInfoExA,
        (DWORD fl, LPCSTR f, DWORD h, DWORD len, LPVOID data), (fl, f, h, len, data), FALSE)
FORWARD(BOOL, GetFileVersionInfoExW,
        (DWORD fl, LPCWSTR f, DWORD h, DWORD len, LPVOID data), (fl, f, h, len, data), FALSE)
FORWARD(DWORD, GetFileVersionInfoSizeA, (LPCSTR f, LPDWORD h), (f, h), 0)
FORWARD(DWORD, GetFileVersionInfoSizeW, (LPCWSTR f, LPDWORD h), (f, h), 0)
FORWARD(DWORD, GetFileVersionInfoSizeExA, (DWORD fl, LPCSTR f, LPDWORD h), (fl, f, h), 0)
FORWARD(DWORD, GetFileVersionInfoSizeExW, (DWORD fl, LPCWSTR f, LPDWORD h), (fl, f, h), 0)
FORWARD(BOOL, VerQueryValueA,
        (LPCVOID b, LPCSTR sub, LPVOID* out, PUINT len), (b, sub, out, len), FALSE)
FORWARD(BOOL, VerQueryValueW,
        (LPCVOID b, LPCWSTR sub, LPVOID* out, PUINT len), (b, sub, out, len), FALSE)
FORWARD(DWORD, VerFindFileA,
        (DWORD fl, LPCSTR nm, LPCSTR wd, LPCSTR ad, LPSTR cur, PUINT curlen, LPSTR dst, PUINT dstlen),
        (fl, nm, wd, ad, cur, curlen, dst, dstlen), 0)
FORWARD(DWORD, VerFindFileW,
        (DWORD fl, LPCWSTR nm, LPCWSTR wd, LPCWSTR ad, LPWSTR cur, PUINT curlen, LPWSTR dst, PUINT dstlen),
        (fl, nm, wd, ad, cur, curlen, dst, dstlen), 0)
FORWARD(DWORD, VerInstallFileA,
        (DWORD fl, LPCSTR sfn, LPCSTR dfn, LPCSTR sd, LPCSTR dd, LPCSTR cd, LPSTR tf, PUINT tflen),
        (fl, sfn, dfn, sd, dd, cd, tf, tflen), 0)
FORWARD(DWORD, VerInstallFileW,
        (DWORD fl, LPCWSTR sfn, LPCWSTR dfn, LPCWSTR sd, LPCWSTR dd, LPCWSTR cd, LPWSTR tf, PUINT tflen),
        (fl, sfn, dfn, sd, dd, cd, tf, tflen), 0)

// UNDOCUMENTED, forwarded on an assumption: GetFileVersionInfoByHandle has no public prototype. Four pointer-sized
// args land in RCX/RDX/R8/R9 on x64, so it works up to arity four; eduke32.exe does not import it.
FORWARD(BOOL, GetFileVersionInfoByHandle,
        (HANDLE a, LPVOID b, DWORD c, LPVOID d), (a, b, c, d), FALSE)

// Set only inside eduke32.exe; every other host leaves the menu off and only forwards.
static bool s_active = false;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);

    // Before any log line: a foreign host must not roll the game's ekmod.log either. A cut path fails closed.
    wchar_t exe[1024];
    const DWORD wn = GetModuleFileNameW(NULL, exe, 1024);
    if (wn == 0 || wn >= 1024 || !proxy_host_is_game(exe)) return TRUE;
    s_active = true;

    char host[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, host, MAX_PATH);

    ek_log("---- ekmod version.dll proxy attached ----");
    ek_log("host=%s", host);
    ek_log("proxy built " __DATE__ " " __TIME__);

    iat_install_swap_hook();

    // Here and not at the first frame: G_DisplayLogo reads g_noLogo before anything is presented. This reads one ini
    // key and writes one int, calling nothing that loads, spawns or waits (skipintro.cpp).
    skipintro_attach();
  }
  // Unloading must leave the game's own code behind: the firing hooks and the free camera's draw bracket rewrite
  // eduke32.exe in place, so if the module goes and a patch stays, the next shot jumps into an unmapped page. The
  // SwapBuffers IAT entry (iat.cpp) has no removal: harmless, process exit is the only unload.
  else if (reason == DLL_PROCESS_DETACH && s_active) {
    // Report lost lines once, here: counting per discard would call ek_log inside the contention being measured.
    const long dropped = ek_log_dropped_count();
    if (dropped > 0) ek_log("log: %ld line(s) lost to a failed or short write this session", dropped);
    gamehooks_remove();
    camera_remove();
    // Screenshot mode, the world-quad draw point, the whole-map seed call site and the cutaway's polymost_domost
    // patch also go out with them: each redirects engine code into this module.
    shot_remove();
    maskscan_remove();
    nocull_remove();
    cutaway_remove();
    // Last, after the code patches: a plain four-byte store into three engine cvars, calling nothing. A belt, not the
    // guarantee: on a clean quit the engine has already written settings.cfg.
    shot_restore();
    // Spike-totaltime also goes last: a held row would outlive this module (see spiketotaltime.h).
    spiketotaltime_restore();
  }
  return TRUE;
}
