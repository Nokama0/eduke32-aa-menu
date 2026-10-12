// Skip intro: locates g_noLogo by two independent signature scans that must agree (the write, cmdline.cpp:517; the
// LTO-inlined read, screens.cpp:1722 via a .refptr slot) and sets it at DLL attach; non-zero at attach refuses. Anchor
// bytes and r10669 rvas: docs/re/skipintro.md.

#include <windows.h>
#include "log.h"
#include "imagescan.h"
#include "config.h"
#include "skipintro.h"

// Slack past the lea for `call; test; je`.
static const int EK_NOLOGO_FOLLOW = 24;

static const unsigned char EK_NOLOGO_READ_SIG[] = {
  0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
  0x8B, 0x00, 0x85, 0xC0,
  0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x75, 0x00,
  0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
  0x48, 0x83, 0xB8, 0x10, 0x04, 0x00, 0x00, 0x00
};
static const unsigned char EK_NOLOGO_READ_MASK[] = {
  1, 1, 1, 0, 0, 0, 0,
  1, 1, 1, 1,
  1, 1, 1, 0, 0, 0, 0,
  1, 0,
  1, 1, 1, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1
};
static_assert(sizeof(EK_NOLOGO_READ_MASK) == sizeof(EK_NOLOGO_READ_SIG),
              "the g_noLogo read signature and its mask must match in length");

static bool in_spans(const EkSpan* sp, int n, const unsigned char* p, size_t bytes) {
  for (int i = 0; i < n; i++)
    if (p >= sp[i].base && p + bytes <= sp[i].base + sp[i].size) return true;
  return false;
}

struct StoreHit { unsigned store; unsigned target; };

static const char* anchor_store(const unsigned char* base, const EkSpan* ex, int nex,
                                const char* s, size_t len, StoreHit* out) {
  out->store = 0;
  out->target = 0;
  EkStrHit sh;
  ek_image_find_string(base, s, len, &sh);
  if (sh.faulted) return "a data section faulted during the string search";
  if (sh.count != 1) return sh.count ? "a comparison string is not unique" : "a comparison string is absent";
  EkRefHit rh;
  ek_image_find_lea_refs(base, sh.rva, &rh);
  if (rh.faulted) return "a code section faulted during the lea search";
  if (rh.count != 1) return rh.count ? "a comparison string has several leas" : "a comparison string has no lea";
  const unsigned char* const lea = base + rh.rva;
  if (!in_spans(ex, nex, lea, 7 + EK_NOLOGO_FOLLOW + 13)) return "the lea sits at a code section's end";
  __try {
    for (int k = 7; k <= 7 + EK_NOLOGO_FOLLOW; k++) {
      const unsigned char* const c = lea + k;
      if (c[0] != 0xE8 || c[5] != 0x85 || c[6] != 0xC0 || c[7] != 0x0F || c[8] != 0x84) continue;
      const long long t = (long long)ek_image_rva(base, c + 13) + *(const int*)(c + 9);
      if (t <= 0 || t > 0xFFFFFFFFLL) return "the je leaves the image";
      const unsigned char* const st = base + (unsigned)t;
      if (!in_spans(ex, nex, st, 10)) return "the je lands outside code";
      if (st[0] != 0xC7 || st[1] != 0x05 || st[6] != 1 || st[7] || st[8] || st[9])
        return "the je does not land on a store of 1";
      const long long g = t + 10 + *(const int*)(st + 2);
      if (g <= 0 || g > 0xFFFFFFFFLL) return "the store leaves the image";
      out->store = (unsigned)t;
      out->target = (unsigned)g;
      return NULL;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return "a fault reading the comparison";
  }
  return "no call/test/je follows the lea";
}

static const char* anchor_read(const unsigned char* base, const EkSpan* ex, int nex,
                               DWORD imageSize, unsigned* site, unsigned* slot, unsigned* target) {
  *site = 0;
  *slot = 0;
  *target = 0;
  const size_t len = sizeof(EK_NOLOGO_READ_SIG);
  int count = 0;
  for (int i = 0; i < nex; i++) {
    if (ex[i].size < len) continue;
    __try {
      const DWORD last = ex[i].size - (DWORD)len;
      for (DWORD off = 0; off <= last; off++) {
        if (ex[i].base[off] != EK_NOLOGO_READ_SIG[0]) continue;
        size_t k = 1;
        for (; k < len; k++) {
          if (!EK_NOLOGO_READ_MASK[k]) continue;
          if (ex[i].base[off + k] != EK_NOLOGO_READ_SIG[k]) break;
        }
        if (k != len) continue;
        if (count == 0) *site = ek_image_rva(base, ex[i].base + off);
        count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A part-searched section could hide the second match, so a fault refuses.
      return "a code section faulted during the read-signature scan";
    }
  }
  if (count != 1) return count ? "the read signature is ambiguous" : "the read signature is absent";

  unsigned secLo = 0, secHi = 0;
  __try {
    const long long s = (long long)*site + 7 + *(const int*)(base + *site + 3);
    if (s <= 0 || s > 0xFFFFFFFFLL) return "the slot leaves the image";
    *slot = (unsigned)s;
    if (!ek_image_data_extent(base, *slot, &secLo, &secHi) || *slot + 8 > secHi)
      return "the slot is not in a data section";
    const ULONG_PTR p = *(const ULONG_PTR*)(base + *slot);
    const ULONG_PTR b = (ULONG_PTR)base;
    if (p < b || p - b >= imageSize) return "the slot does not point into the image";
    *target = (unsigned)(p - b);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return "a fault reading the slot";
  }
  return NULL;
}

static void writable_section(const unsigned char* base, unsigned rva, unsigned* lo, unsigned* hi) {
  *lo = 0;
  *hi = 0;
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < (int)nt->FileHeader.NumberOfSections; i++) {
    const DWORD b = sec[i].VirtualAddress;
    const DWORD span = sec[i].Misc.VirtualSize;
    if (rva < b || rva - b >= span) continue;
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) return;
    if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) return;
    *lo = b;
    *hi = b + span;
    return;
  }
}

static bool writable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const DWORD ok = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & ok)) return false;
  return (const unsigned char*)p + bytes <= (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
}

// Under the loader lock, and safe: no LoadLibrary, thread or wait; the exe's relocations are applied before DllMain.
// The value is what -nologo stores (cmdline.cpp:519).
void skipintro_attach(void) {
  if (!config_peek_skipintro()) { ek_log("skipintro: off"); return; }

  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  if (!base) { ek_log("skipintro: refused -- no host module"); return; }
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) { ek_log("skipintro: refused -- no PE headers"); return; }
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) { ek_log("skipintro: refused -- no PE headers"); return; }

  EkSpan ex[16];
  int nex = 0;
  if (!ek_image_sections(base, true, ex, 16, &nex) || nex == 0) {
    ek_log("skipintro: refused -- no code section");
    return;
  }

  StoreHit a1 = {0, 0}, a2 = {0, 0};
  const char* why = anchor_store(base, ex, nex, "nologo", 7, &a1);
  if (!why) why = anchor_store(base, ex, nex, "quick", 6, &a2);
  if (why) { ek_log("skipintro: refused -- anchor A: %s", why); return; }
  if (a1.store != a2.store || a1.target != a2.target) {
    ek_log("skipintro: refused -- \"nologo\" and \"quick\" reach different stores "
           "(0x%07X -> 0x%07X, 0x%07X -> 0x%07X)", a1.store, a1.target, a2.store, a2.target);
    return;
  }

  unsigned site = 0, slot = 0, readTarget = 0;
  why = anchor_read(base, ex, nex, nt->OptionalHeader.SizeOfImage, &site, &slot, &readTarget);
  if (why) { ek_log("skipintro: refused -- anchor B: %s", why); return; }

  unsigned lo = 0, hi = 0;
  writable_section(base, a1.target, &lo, &hi);
  if (!ekm_nologo_anchors_agree(a1.target, readTarget, lo, hi)) {
    ek_log("skipintro: refused -- anchors disagree: store at 0x%07X names 0x%07X, read at "
           "0x%07X names 0x%07X, writable section 0x%07X..0x%07X",
           a1.store, a1.target, site, readTarget, lo, hi);
    return;
  }

  volatile LONG* const flag = (volatile LONG*)(base + a1.target);
  if (!writable((const void*)flag, sizeof(LONG))) {
    ek_log("skipintro: refused -- rva 0x%07X is not committed writable memory", a1.target);
    return;
  }
  __try {
    const LONG was = *flag;
    if (was != 0) {
      ek_log("skipintro: refused -- g_noLogo at rva 0x%07X holds %ld at attach, not .bss's 0",
             a1.target, (long)was);
      return;
    }
    *flag = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ek_log("skipintro: refused -- the write to rva 0x%07X faulted", a1.target);
    return;
  }
  ek_log("skipintro: on, g_noLogo at rva 0x%07X set (store 0x%07X; read 0x%07X via slot 0x%07X)",
         a1.target, a1.store, site, slot);
}
