#include <windows.h>
#include "clipboard.h"

bool clipboard_read_text(char* out, int cap) {
  if (!out || cap <= 0) return false;
  // Terminated first so every early return leaves a defined empty string.
  out[0] = 0;
  // Asked before the clipboard is opened, so the common case never takes it from the desktop.
  if (!IsClipboardFormatAvailable(CF_TEXT)) return false;
  if (!OpenClipboard(NULL)) return false;
  int n = 0;
  const HANDLE h = GetClipboardData(CF_TEXT);
  if (h) {
    // The handle is the clipboard's: lock and unlock, never free (freeing would take the player's contents).
    const char* p = (const char*)GlobalLock(h);
    if (p) {
      // Another process may set a block with no terminator, so the block's own size bounds the read too.
      const SIZE_T size = GlobalSize(h);
      const int lim = size < (SIZE_T)(cap - 1) ? (int)size : cap - 1;
      while (n < lim && p[n]) { out[n] = p[n]; n++; }
      out[n] = 0;
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return n > 0;
}

bool clipboard_write_text(const char* s) {
  if (!s) return false;
  int n = 0;
  while (s[n]) n++;
  if (!OpenClipboard(NULL)) return false;
  bool ok = false;
  if (EmptyClipboard()) {
    const HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n + 1);
    if (h) {
      char* p = (char*)GlobalLock(h);
      if (p) {
        for (int i = 0; i < n; i++) p[i] = s[i];
        p[n] = 0;
        GlobalUnlock(h);
        // After a successful set the handle belongs to the clipboard; free it only on failure.
        if (SetClipboardData(CF_TEXT, h)) ok = true;
        else GlobalFree(h);
      } else {
        GlobalFree(h);
      }
    }
  }
  CloseClipboard();
  return ok;
}
