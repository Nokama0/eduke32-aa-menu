// Inline prologue hooks, the one place the port rewrites game instructions, so every step refuses rather than guesses:
// an unrecognised byte, no page in rel32 range or a failed VirtualProtect patches nothing. Memory only; install from
// the render thread in SwapBuffers.

#include <windows.h>
#include <string.h>
#include "hook.h"
#include "log.h"

// The entry jumps to a relay, not the detour: E9 is rel32 and version.dll may load beyond 2 GB of eduke32.exe (ASLR),
// which would fail silently on some machines. The relay is FF 25 + the 8-byte detour address, in the same near block as
// the trampoline (at 0, at most 30 bytes; relay at 32).
static const int EK_BLOCK = 64;
static const int EK_RELAY_OFF = 32;

// A bad scan address must fail, not crash the game.
static bool read_guarded(const void* at, unsigned char* dst, int n) {
  __try { memcpy(dst, at, (size_t)n); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool write_guarded(void* at, const unsigned char* src, int n) {
  __try { memcpy(at, src, (size_t)n); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Restores the old protection even when the write faults.
static bool patch_code(void* at, const unsigned char* src, int n) {
  DWORD old = 0;
  if (!VirtualProtect(at, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) {
    ek_log("hook: VirtualProtect failed at %p (%lu)", at, GetLastError());
    return false;
  }
  const bool ok = write_guarded(at, src, n);
  DWORD back = 0;
  VirtualProtect(at, (SIZE_T)n, old, &back);
  FlushInstructionCache(GetCurrentProcess(), at, (SIZE_T)n);
  return ok;
}

// The image is 26.6 MB, so the first free region above the module base is within rel32 reach; stop when one is not.
static void* alloc_near(unsigned long long anchor) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const unsigned long long gran =
      si.dwAllocationGranularity ? (unsigned long long)si.dwAllocationGranularity : 0x10000ull;

  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery((LPCVOID)anchor, &mbi, sizeof(mbi)) != sizeof(mbi)) return NULL;
  if (!mbi.AllocationBase) return NULL;

  unsigned long long addr = ((unsigned long long)mbi.AllocationBase + gran - 1) & ~(gran - 1);
  while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
    int unused = 0;
    if (!ekm_rel32(anchor, addr, &unused)) return NULL;
    // A hole smaller than the allocation granularity cannot be reserved.
    if (mbi.State == MEM_FREE && mbi.RegionSize >= gran) {
      void* p = VirtualAlloc((LPVOID)addr, (SIZE_T)EK_BLOCK,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      if (p) return p;
    }
    const unsigned long long next =
        (((unsigned long long)mbi.BaseAddress + mbi.RegionSize) + gran - 1) & ~(gran - 1);
    if (next <= addr) return NULL;
    addr = next;
  }
  return NULL;
}

bool hook_install(void* target, void* detour, EkHook* out) {
  if (!target || !detour || !out) return false;
  memset(out, 0, sizeof(*out));

  unsigned char org[EKM_MAX_PROLOGUE];
  if (!read_guarded(target, org, (int)sizeof(org))) {
    ek_log("hook: cannot read %d bytes at %p", (int)sizeof(org), target);
    return false;
  }

  const int len = ekm_prologue_len(org, (int)sizeof(org));
  if (len < 5 || len > (int)sizeof(out->original)) {
    ek_log("hook: refusing %p, prologue not recognised (%02X %02X %02X %02X)",
           target, org[0], org[1], org[2], org[3]);
    return false;
  }

  unsigned char* block = (unsigned char*)alloc_near((unsigned long long)target);
  if (!block) {
    ek_log("hook: no free page within rel32 range of %p", target);
    return false;
  }

  // int3 in the gap: nothing should execute there.
  unsigned char body[EK_BLOCK];
  memset(body, 0xCC, sizeof(body));
  const unsigned long long resume = (unsigned long long)target + (unsigned long long)len;
  const int tlen = ekm_build_trampoline(body, EK_RELAY_OFF, org, len, resume);
  const int rlen = ekm_build_absjmp(body + EK_RELAY_OFF, EK_BLOCK - EK_RELAY_OFF,
                                    (unsigned long long)detour);
  if (tlen <= 0 || rlen != EKM_ABSJMP_LEN || !write_guarded(block, body, EK_BLOCK)) {
    VirtualFree(block, 0, MEM_RELEASE);
    ek_log("hook: could not build the stubs for %p", target);
    return false;
  }
  FlushInstructionCache(GetCurrentProcess(), block, (SIZE_T)EK_BLOCK);

  // Hardening only; a failure is logged, not fatal.
  DWORD old = 0;
  if (!VirtualProtect(block, (SIZE_T)EK_BLOCK, PAGE_EXECUTE_READ, &old))
    ek_log("hook: stub page at %p stays writable (%lu)", block, GetLastError());

  // Fails closed on a jump that does not reach.
  int rel = 0;
  if (!ekm_rel32((unsigned long long)target,
                 (unsigned long long)(block + EK_RELAY_OFF), &rel)) {
    ek_log("hook: relay %p is out of rel32 range of %p", block + EK_RELAY_OFF, target);
    VirtualFree(block, 0, MEM_RELEASE);
    return false;
  }

  unsigned char patch[EKM_MAX_PROLOGUE];
  if (ekm_build_patch(patch, (int)sizeof(patch), len, rel) != len ||
      !patch_code(target, patch, len)) {
    VirtualFree(block, 0, MEM_RELEASE);
    return false;
  }

  out->target = target;
  out->trampoline = block;
  memcpy(out->original, org, (size_t)len);
  out->len = len;
  ek_log("hook: %p patched, %d bytes, trampoline %p, relay %p -> %p",
         target, len, block, block + EK_RELAY_OFF, detour);
  return true;
}

bool hook_remove(EkHook* h) {
  if (!h || !h->target || h->len < 5 || h->len > (int)sizeof(h->original)) return false;

  // A failed restore keeps the record and the block: the entry still jumps to the relay, so freeing would point live
  // code at unmapped memory. The caller retries.
  if (!patch_code(h->target, h->original, h->len)) {
    ek_log("hook: restore of %p FAILED -- still patched, hook left intact", h->target);
    return false;
  }

  if (h->trampoline) VirtualFree(h->trampoline, 0, MEM_RELEASE);
  h->target = NULL;
  h->trampoline = NULL;
  h->len = 0;
  return true;
}

void* hook_trampoline(const EkHook* h) {
  return (h && h->target && h->len >= 5) ? h->trampoline : NULL;
}

// CALL-SITE REDIRECT. A wrong call site calls mid-function. The five bytes must decode as `call rel32` reaching
// `expected`, and freecam_redirect_locate_ok demands exactly one such site (rules out a stray 0xE8). Only the four
// operand bytes change, written last; remove restores them.

bool callsite_install(void* site, void* expected, void* detour, EkCallPatch* out) {
  if (!site || !expected || !detour || !out) return false;
  memset(out, 0, sizeof(*out));

  unsigned char org[EKM_CALL_LEN];
  if (!read_guarded(site, org, EKM_CALL_LEN)) {
    ek_log("hook: cannot read the %d bytes of the call site at %p",
           EKM_CALL_LEN, site);
    return false;
  }

  if (!ekm_call_rel32_targets(org, EKM_CALL_LEN, (unsigned long long)site,
                              (unsigned long long)expected)) {
    ek_log("hook: refusing the call-site redirect at %p -- the bytes there are "
           "%02X %02X %02X %02X %02X, which do not decode as a `call rel32` "
           "reaching %p. Nothing was written",
           site, org[0], org[1], org[2], org[3], org[4], expected);
    return false;
  }

  unsigned char* block = (unsigned char*)alloc_near((unsigned long long)site);
  if (!block) {
    ek_log("hook: no free page within rel32 range of the call site at %p", site);
    return false;
  }

  // One stub, no trampoline: a call site clobbers nothing, and the original is the address just proved. int3 fills the
  // rest.
  unsigned char stub[EK_BLOCK];
  memset(stub, 0xCC, sizeof(stub));
  if (ekm_build_absjmp(stub, EK_BLOCK, (unsigned long long)detour) != EKM_ABSJMP_LEN ||
      !write_guarded(block, stub, EK_BLOCK)) {
    VirtualFree(block, 0, MEM_RELEASE);
    ek_log("hook: could not build the redirect stub for the call site at %p", site);
    return false;
  }
  FlushInstructionCache(GetCurrentProcess(), block, (SIZE_T)EK_BLOCK);

  // Hardening only, as in hook_install.
  DWORD old = 0;
  if (!VirtualProtect(block, (SIZE_T)EK_BLOCK, PAGE_EXECUTE_READ, &old))
    ek_log("hook: redirect stub page at %p stays writable (%lu)", block,
           GetLastError());

  unsigned char op[EKM_CALL_OPERAND];
  if (ekm_build_call_operand(op, (int)sizeof(op), (unsigned long long)site,
                             (unsigned long long)block) != EKM_CALL_OPERAND) {
    ek_log("hook: the redirect stub at %p is out of rel32 range of the call "
           "site at %p; nothing was written", block, site);
    VirtualFree(block, 0, MEM_RELEASE);
    return false;
  }

  // Last, so any earlier failure leaves the game's call intact.
  if (!patch_code((unsigned char*)site + 1, op, EKM_CALL_OPERAND)) {
    VirtualFree(block, 0, MEM_RELEASE);
    return false;
  }

  out->site = site;
  out->block = block;
  out->len = EKM_CALL_OPERAND;
  memcpy(out->original, org + 1, EKM_CALL_OPERAND);
  ek_log("hook: call site at %p redirected from %p to the stub at %p -> %p "
         "(four operand bytes, the opcode untouched)", site, expected, block,
         detour);
  return true;
}

bool callsite_remove(EkCallPatch* p) {
  if (!p || !p->site || p->len != EKM_CALL_OPERAND) return false;

  // Failed restore keeps the record and block, as in hook_remove.
  if (!patch_code((unsigned char*)p->site + 1, p->original, EKM_CALL_OPERAND)) {
    ek_log("hook: restore of the call site at %p FAILED -- still redirected, "
           "the patch record is left intact", p->site);
    return false;
  }

  if (p->block) VirtualFree(p->block, 0, MEM_RELEASE);
  p->site = NULL;
  p->block = NULL;
  p->len = 0;
  return true;
}
