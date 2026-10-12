// Locating cansee, the engine's line-of-sight test, in a stripped binary: a 53-byte signature scan (no wildcards,
// occurs once) whose result is a search, never a constant; the entry is the exception-directory function containing
// the match. Evidence: docs/re/cansee.md.

// cansee mutates engine globals, so every call (self-test included) runs inside the SwapBuffers hook on the game
// thread.

#include <windows.h>
#include "log.h"
#include "cansee.h"
#include "playerstate.h"

// The sector parameters are `short`: the callee reads int16_t and a wider value leaves garbage in the high half.
typedef int (*EkCanseeFn)(int x1, int y1, int z1, short sect1,
                          int x2, int y2, int z2, short sect2, int wallmask);

static const int EK_CANSEE_WALLMASK = 0x0020;

// At or above MAXSECTORS (4096) the engine returns 0 and a point "does not see itself", which would fail a correct
// address permanently.
static const int EK_CANSEE_MAXSECTORS = 4096;

static const unsigned char EK_CANSEE_SIG[] = {
  0x49, 0x0F, 0xBF, 0x57, 0x08,
  0x45, 0x89, 0xF1,
  0x48, 0xC1, 0xE2, 0x05,
  0x48, 0x8D, 0x0C, 0x10,
  0x41, 0x8B, 0x07,
  0x41, 0x8B, 0x57, 0x04,
  0x41, 0x89, 0xC0,
  0x44, 0x2B, 0x01,
  0x41, 0x89, 0xD2,
  0x44, 0x2B, 0x51, 0x04,
  0x44, 0x89, 0xE9,
  0x45, 0x0F, 0xAF, 0xC8,
  0x41, 0x0F, 0xAF, 0xCA,
  0x41, 0x29, 0xC9,
  0x45, 0x85, 0xC9
};

static EkCanseeFn volatile s_fn = NULL;
static volatile LONG s_rva = 0;

static bool s_refused = false;

static const int EK_CANSEE_MAX_TESTS = 4;
static int s_tests = 0;

// Runs in the frame path, so the five-megabyte scan is cached and retries are spaced by wall clock.
static unsigned s_entry = 0;
static DWORD s_lastTry = 0;
static const DWORD EK_CANSEE_RETRY_MS = 2000;

struct SigHit { unsigned rva; int count; int faulted; };

static bool find_pattern(const unsigned char* base, SigHit* out) {
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = (int)nt->FileHeader.NumberOfSections;
  const size_t len = sizeof(EK_CANSEE_SIG);

  out->rva = 0;
  out->count = 0;
  out->faulted = 0;

  for (int i = 0; i < nsec; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    DWORD span = sec[i].Misc.VirtualSize;
    if (span > sec[i].SizeOfRawData && sec[i].SizeOfRawData != 0) span = sec[i].SizeOfRawData;
    if (span < len) continue;
    const unsigned char* const start = base + sec[i].VirtualAddress;

    __try {
      const DWORD last = span - (DWORD)len;
      for (DWORD off = 0; off <= last; off++) {
        if (start[off] != EK_CANSEE_SIG[0]) continue;
        size_t k = 1;
        for (; k < len; k++) if (start[off + k] != EK_CANSEE_SIG[k]) break;
        if (k != len) continue;
        if (out->count == 0) out->rva = (unsigned)(sec[i].VirtualAddress + off);
        out->count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A section that faulted part-way was not searched; the flag forces a refusal rather than arm a scan while a
      // second match sits in unread bytes.
      out->faulted = 1;
    }
  }
  return true;
}

static unsigned entry_containing(const unsigned char* base, unsigned rva) {
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY* dir =
      &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (dir->VirtualAddress == 0 || dir->Size < sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY))
    return 0;

  unsigned found = 0;
  __try {
    const IMAGE_RUNTIME_FUNCTION_ENTRY* fn =
        (const IMAGE_RUNTIME_FUNCTION_ENTRY*)(base + dir->VirtualAddress);
    const DWORD n = dir->Size / (DWORD)sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
    for (DWORD i = 0; i < n; i++) {
      if (fn[i].EndAddress <= fn[i].BeginAddress) continue;
      if (rva < fn[i].BeginAddress || rva >= fn[i].EndAddress) continue;
      found = fn[i].BeginAddress;
      break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    found = 0;
  }
  return found;
}

// The self-test makes a signature scan acceptable: `same` (equal points) must be true and `bad` (sector -1) false;
// neither reaches the wall loop. No far-point probe: it overflows clipsectorlist (MAXCLIPSECTORS 512), a write no
// __except catches.
struct SelfTest { int same, bad, near_; int faulted; };

static void self_test(EkCanseeFn fn, const PlayerSnap* p, SelfTest* r) {
  r->same = r->bad = r->near_ = -1;
  r->faulted = 1;
  __try {
    const short ps = (short)p->sect;
    r->same  = fn(p->x, p->y, p->z, ps, p->x, p->y, p->z, ps, EK_CANSEE_WALLMASK);
    r->bad   = fn(p->x, p->y, p->z, ps, p->x, p->y, p->z, (short)-1, EK_CANSEE_WALLMASK);
    r->near_ = fn(p->x, p->y, p->z, ps, p->x + 64, p->y, p->z, ps, EK_CANSEE_WALLMASK);
    r->faulted = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static unsigned locate_once(const unsigned char* base) {
  SigHit hit;
  if (!find_pattern(base, &hit)) {
    s_refused = true;
    ek_log("cansee: the host module has no PE headers; refusing");
    return 0;
  }

  if (hit.count == 0 && !hit.faulted) {
    s_refused = true;
    ek_log("cansee: signature not found in this build -- line of sight stays "
           "unavailable, and nothing will aim");
    return 0;
  }

  if (hit.count != 1 || hit.faulted) {
    s_refused = true;
    if (hit.faulted)
      ek_log("cansee: a code section faulted mid-scan, so uniqueness cannot be "
             "claimed (%d match(es) before the fault); refusing to guess",
             hit.count);
    else
      ek_log("cansee: signature is ambiguous -- %d matches, first at rva 0x%07X; "
             "refusing to guess", hit.count, hit.rva);
    return 0;
  }

  const unsigned entry = entry_containing(base, hit.rva);
  if (entry == 0 || entry > hit.rva) {
    s_refused = true;
    ek_log("cansee: match at rva 0x%07X is not inside any function in the "
           "exception directory; refusing", hit.rva);
    return 0;
  }

  ek_log("cansee: signature matched once at rva 0x%07X, inside the function at "
         "rva 0x%07X (%+d); self-testing", hit.rva, entry,
         (int)(hit.rva - entry));
  return entry;
}

bool cansee_scan(void) {
  if (s_fn) return true;
  if (s_refused) return false;

  if (!player_ready()) return false;
  PlayerSnap snap;
  if (!player_snap(&snap)) return false;
  // Bounded at both ends (noclip and level transitions produce sectors outside it).
  if (snap.sect < 0 || snap.sect >= EK_CANSEE_MAXSECTORS) return false;

  if (s_tests >= EK_CANSEE_MAX_TESTS) return false;
  const DWORD now = GetTickCount();
  if (s_tests > 0 && (DWORD)(now - s_lastTry) < EK_CANSEE_RETRY_MS) return false;
  s_lastTry = now;
  s_tests++;

  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  if (!base) { s_refused = true; ek_log("cansee: no host module"); return false; }

  if (s_entry == 0) {
    s_entry = locate_once(base);
    if (s_entry == 0) return false;
  }

  EkCanseeFn const fn = (EkCanseeFn)(void*)(base + s_entry);
  SelfTest r;
  self_test(fn, &snap, &r);

  if (r.faulted || r.same == 0 || r.bad != 0) {
    const char* which = r.faulted ? "it faulted"
                      : (r.same == 0) ? "a point could not see itself"
                                      : "sector -1 was reported visible";
    ek_log("cansee: self-test FAILED at rva 0x%07X -- %s [same=%d bad=%d]; "
           "line of sight stays unavailable", s_entry, which, r.same, r.bad);
    if (s_tests >= EK_CANSEE_MAX_TESTS)
      ek_log("cansee: giving up; the aimbot will show as unavailable");
    return false;
  }

  InterlockedExchange(&s_rva, (LONG)s_entry);
  InterlockedExchangePointer((void* volatile*)&s_fn, (void*)fn);

  ek_log("cansee: armed at rva 0x%07X -- self-test passed from (%d,%d,%d) "
         "sector %d: sees itself=%d, sector -1=%d",
         s_entry, snap.x, snap.y, snap.z, snap.sect, r.same, r.bad);
  ek_log("cansee:   probe (not gating): 64 units away=%d", r.near_);
  return true;
}

bool cansee_ready(void) { return s_fn != NULL; }

unsigned cansee_rva(void) { return (unsigned)s_rva; }

// Guarded: the engine frees and reloads its sector and wall arrays between levels.
static int call_guarded(EkCanseeFn fn, int x1, int y1, int z1, short s1,
                        int x2, int y2, int z2, short s2) {
  int r = 0;
  __try {
    r = fn(x1, y1, z1, s1, x2, y2, z2, s2, EK_CANSEE_WALLMASK);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    r = 0;
  }
  return r;
}

bool ek_cansee(int x1, int y1, int z1, int sect1,
               int x2, int y2, int z2, int sect2) {
  EkCanseeFn const fn = s_fn;
  if (!fn) return false;
  // A sector that does not fit an int16 would be truncated into a valid one by the cast.
  if (sect1 < 0 || sect1 > 0x7FFF || sect2 < 0 || sect2 > 0x7FFF) return false;
  return call_guarded(fn, x1, y1, z1, (short)sect1,
                      x2, y2, z2, (short)sect2) != 0;
}
