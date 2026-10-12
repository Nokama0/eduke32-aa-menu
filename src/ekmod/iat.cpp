// IAT hook for SwapBuffers: eduke32.exe imports GDI32!SwapBuffers statically (SDL2's GL backend presents through it),
// so one pointer write suffices and no code is modified.

#include <windows.h>
#include "log.h"
#include "iat.h"
#include "overlay.h"
#include "perf.h"

void* iat_hook(HMODULE mod, const char* dll, const char* fn, void* replacement) {
  if (!mod || !dll || !fn || !replacement) return NULL;

  const BYTE* base = (const BYTE*)mod;
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

  const IMAGE_DATA_DIRECTORY& dir =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (dir.VirtualAddress == 0 || dir.Size == 0) return NULL;

  const IMAGE_IMPORT_DESCRIPTOR* imp =
      (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);

  for (; imp->Name; imp++) {
    const char* name = (const char*)(base + imp->Name);
    if (_stricmp(name, dll) != 0) continue;

    // OriginalFirstThunk keeps the names; FirstThunk holds the addresses we patch.
    const IMAGE_THUNK_DATA* orig =
        (const IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                                 : imp->FirstThunk));
    IMAGE_THUNK_DATA* addr = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);

    for (; orig->u1.AddressOfData; orig++, addr++) {
      if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
      const IMAGE_IMPORT_BY_NAME* ibn =
          (const IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
      if (strcmp((const char*)ibn->Name, fn) != 0) continue;

      void* const previous = (void*)addr->u1.Function;
      if (previous == replacement) return previous;

      DWORD old = 0;
      if (!VirtualProtect(addr, sizeof(*addr), PAGE_READWRITE, &old)) return NULL;
      addr->u1.Function = (ULONGLONG)replacement;
      VirtualProtect(addr, sizeof(*addr), old, &old);
      return previous;
    }
  }
  return NULL;
}

typedef BOOL(WINAPI* SwapBuffersFn)(HDC);
static SwapBuffersFn s_origSwap = NULL;
static volatile LONG s_frames = 0;

static BOOL WINAPI ek_SwapBuffers(HDC hdc) {
  const LONG n = InterlockedIncrement(&s_frames);

  if (n <= 3 || (n % 1000) == 0) {
    const HWND wnd = WindowFromDC(hdc);
    RECT rc = {0, 0, 0, 0};
    if (wnd) GetClientRect(wnd, &rc);
    ek_log("swap #%ld hdc=%p hwnd=%p client=%ldx%ld thread=%lu",
           n, (void*)hdc, (void*)wnd, rc.right - rc.left, rc.bottom - rc.top,
           GetCurrentThreadId());
  }

  // Before the original: drawing after the present lands a frame late.
  overlay_on_swap(hdc);

  // Here, not in overlay_on_swap (several early returns): every frame publishes exactly once.
  perf_frame_end();

  return s_origSwap ? s_origSwap(hdc) : TRUE;
}

bool iat_install_swap_hook(void) {
  const HMODULE host = GetModuleHandleA(NULL);
  void* const prev = iat_hook(host, "GDI32.dll", "SwapBuffers", (void*)ek_SwapBuffers);
  if (!prev) {
    ek_log("iat: SwapBuffers thunk not found in the host import table -- overlay inert");
    return false;
  }
  s_origSwap = (SwapBuffersFn)prev;
  ek_log("iat: SwapBuffers hooked, original=%p", prev);
  return true;
}

long iat_swap_frames(void) { return s_frames; }
