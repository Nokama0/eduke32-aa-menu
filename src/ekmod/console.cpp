// Is EDuke32's console taking typing? console.h has the rules; this file is the guarded reads and the locate.

#include <windows.h>
#include "log.h"
#include "cvars.h"
#include "console.h"

// &osd->flags, published once by the render thread and read by any thread.
// NULL until located, and for good after a refusal.
static void* volatile s_flagsAddr = NULL;

// Heap, not image: osdmain_t is Xcalloc'd (osd.cpp:785), so cvars.cpp's
// image-bound readable() cannot vouch for it.
static bool mem_readable(const void* p, size_t bytes) {
  if (!p) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const unsigned char* end = (const unsigned char*)p + bytes;
  return end <= (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
}

// Guarded as well as queried: the page can go between the two (OSD_Cleanup at
// shutdown frees the block). Own functions so __try sits alone.
static bool read_u32(unsigned long long addr, unsigned* out) {
  const volatile LONG* p = (const volatile LONG*)(ULONG_PTR)addr;
  if (!mem_readable((const void*)p, 4)) return false;
  __try {
    *out = (unsigned)*p;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

static bool read_u64(unsigned long long addr, unsigned long long* out) {
  const volatile unsigned long long* p = (const volatile unsigned long long*)(ULONG_PTR)addr;
  if (!mem_readable((const void*)p, 8)) return false;
  __try {
    *out = *p;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

void console_locate(void) {
  static bool s_tried = false;   // render thread only
  if (s_tried) return;
  s_tried = true;

  const EkCvar* tm = cvars_find_extern("osdtextmode");
  const EkCvar* hd = cvars_find_extern("osdhistorydepth");
  if (!tm || !hd) {
    ek_log("console: refused -- osdtextmode %s, osdhistorydepth %s among the heap-backed "
           "cvar records; the panel's keys will not yield to the console",
           tm ? "found" : "MISSING", hd ? "found" : "MISSING");
    return;
  }
  const unsigned long long mode = (unsigned long long)(ULONG_PTR)tm->ptr;
  const unsigned long long hist = (unsigned long long)(ULONG_PTR)hd->ptr;
  if (!ekm_osd_anchor_pair_ok(hist, mode)) {
    ek_log("console: refused -- history.maxlines %p and draw.mode %p are not one "
           "osdmain_t's two fields", hd->ptr, tm->ptr);
    return;
  }
  const unsigned long long anchorA = (unsigned long long)(ULONG_PTR)tm->rec;
  const unsigned long long anchorB = (unsigned long long)(ULONG_PTR)hd->rec;

  int hits = 0, foundN = 0;
  unsigned long long found = 0;
  for (int off = 4; off <= EK_OSD_SCAN_BYTES; off += 4) {
    const unsigned long long slot = mode + (unsigned long long)off;
    unsigned n32 = 0;
    unsigned long long p = 0;
    if (!read_u32(ekm_osd_count_addr(slot), &n32)) continue;
    if (!read_u64(slot, &p)) continue;
    const int n = (int)n32;
    const int k = ekm_osd_sample(n);
    if (k == 0) continue;

    unsigned long long pData[EK_OSD_SAMPLE_MAX];
    bool known[EK_OSD_SAMPLE_MAX];
    bool readOk = true;
    for (int i = 0; i < k && readOk; i++) {
      readOk = read_u64(p + 16ull * (unsigned long long)i, &pData[i]);
      known[i] = readOk && cvars_is_record((const void*)(ULONG_PTR)pData[i]);
    }
    if (!readOk) continue;
    if (!ekm_osd_cvars_candidate(slot, n, p, pData, known, k, anchorA, anchorB)) {
      // A slot holding both anchors that still failed is the one a refusal
      // must explain: name the first entry the scan did not know.
      bool sawA = false, sawB = false;
      int firstUnknown = -1;
      for (int i = 0; i < k; i++) {
        sawA = sawA || pData[i] == anchorA;
        sawB = sawB || pData[i] == anchorB;
        if (firstUnknown < 0 && !known[i]) firstUnknown = i;
      }
      if (sawA && sawB)
        ek_log("console: draw.mode+0x%X holds both anchors (numcvars %d) but entry %d "
               "(%p) is no record the cvar scan found", off, n, firstUnknown,
               firstUnknown >= 0 ? (void*)(ULONG_PTR)pData[firstUnknown] : NULL);
      continue;
    }
    hits++;
    found = slot;
    foundN = n;
  }

  if (hits != 1) {
    ek_log("console: refused -- %d candidate(s) for osd->cvars within %d bytes of "
           "draw.mode %p (exactly one wanted); the panel's keys will not yield to "
           "the console", hits, EK_OSD_SCAN_BYTES, tm->ptr);
    return;
  }
  const unsigned long long flagsAddr = ekm_osd_flags_addr(found);
  unsigned flags = 0;
  if (!read_u32(flagsAddr, &flags)) {
    ek_log("console: refused -- osd->flags at %p will not read",
           (void*)(ULONG_PTR)flagsAddr);
    return;
  }
  InterlockedExchangePointer((PVOID volatile*)&s_flagsAddr, (PVOID)(ULONG_PTR)flagsAddr);
  // r10669's image puts flags at draw.mode+0x28 (osd+0x8C8 vs +0x8A0).
  ek_log("console: osd->flags located at draw.mode+0x%X (%p), numcvars %d, both "
         "anchors among the first %d known records; flags now 0x%X",
         (unsigned)(flagsAddr - mode), (void*)(ULONG_PTR)flagsAddr, foundN,
         ekm_osd_sample(foundN), flags);
}

// Two clocks: the window thread asks this per key message and the render thread per frame, while the game thread
// writes the flags. One aligned 32-bit load of one self-describing bit; the worst a race costs is one key judged
// by the state a moment before (e.g. V in the same frame as the console key is still swallowed).
bool console_capturing(void) {
  void* a = InterlockedCompareExchangePointer((PVOID volatile*)&s_flagsAddr, NULL, NULL);
  if (!a) return false;
  unsigned flags = 0;
  if (!read_u32((unsigned long long)(ULONG_PTR)a, &flags)) return false;
  return ekm_osd_capturing(flags);
}
