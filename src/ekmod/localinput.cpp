// Locates localInput, the global input record the tic consumes, and ORs a bit into it for the triggerbot. Order of
// preference: cvar registry > signature scan > hardcoded offset.

#include <windows.h>
#include "log.h"
#include "localinput.h"

// P_GetInput's inlined early-out. See the file comment for the window.
static const unsigned char EK_LOCALINPUT_SIG[] = {
  0x48, 0xC7, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xC7, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xC7, 0x05, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
  0x8B, 0x00,
  0xC1, 0xE0, 0x1A,
  0x89, 0x05, 0x00, 0x00, 0x00, 0x00
};

// 1 where the byte must match, 0 where the image's own displacement sits.
static const unsigned char EK_LOCALINPUT_MASK[] = {
  1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1,
  1, 1, 0, 0, 0, 0, 1, 1, 1, 1,
  1, 1, 0, 0, 0, 0, 1, 1, 1, 1,
  1, 1,
  1, 1, 1,
  1, 1, 0, 0, 0, 0
};

static const size_t EK_LOCALINPUT_LEN = sizeof(EK_LOCALINPUT_SIG);
static const DWORD EK_LOCALINPUT_DISP_AT = 38;   // the store's own displacement

// The two arrays must stay the same length: the scan indexes the mask with the signature's counter, so a short mask
// reads past its end silently on the last byte. A compile error is the place to find out.
static_assert(sizeof(EK_LOCALINPUT_MASK) == sizeof(EK_LOCALINPUT_SIG),
              "the localInput signature and its wildcard mask must match in length");
static_assert(EK_LOCALINPUT_DISP_AT + 4 <= sizeof(EK_LOCALINPUT_SIG),
              "the displacement must lie inside the signature");

// The third store, `mov dword [rip+d], 0x80`, is localInput.extbits = BIT(EK_CHAT_MODE). Its displacement (at 23, the
// instruction ends at 31) must land on bits + EK_INPUT_T_EXTBITS, so the source-derived offset is confirmed by the
// image.
static const DWORD EK_LOCALINPUT_EXT_DISP_AT = 23;
static const DWORD EK_LOCALINPUT_EXT_END = 31;
static_assert(EK_LOCALINPUT_EXT_DISP_AT + 8 == EK_LOCALINPUT_EXT_END &&
              EK_LOCALINPUT_EXT_END <= sizeof(EK_LOCALINPUT_SIG),
              "C7 05 disp32 imm32: the displacement, then the 0x80, then the end");

// input_t is 20 bytes with `bits` at 0 and `extbits` at 16 (localinput.h); the whole struct is bounds-checked because
// it is the object the address is claimed to be.
static const size_t EK_INPUT_T_SIZE = 20;
static_assert(EK_INPUT_T_EXTBITS + 4 <= (int)EK_INPUT_T_SIZE, "extbits lies inside input_t");

static unsigned* volatile s_bits = NULL;
static unsigned* volatile s_extbits = NULL;   // NULL unless the image agreed
static volatile LONG s_rva = 0;

static bool s_refused = false;
static bool s_attempted = false;

struct SigHit { unsigned rva; int count; int faulted; };

// One masked pass over the executable sections, guarded because a section's VirtualSize can run past what is
// committed. A parameterised copy of cansee.cpp's scan, with the mask added.
static bool find_pattern(const unsigned char* base, SigHit* out) {
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = (int)nt->FileHeader.NumberOfSections;
  const size_t len = EK_LOCALINPUT_LEN;

  out->rva = 0;
  out->count = 0;
  out->faulted = 0;

  for (int i = 0; i < nsec; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    DWORD span = sec[i].Misc.VirtualSize;
    if (span > sec[i].SizeOfRawData && sec[i].SizeOfRawData != 0) span = sec[i].SizeOfRawData;
    if (span < (DWORD)len) continue;
    const unsigned char* const start = base + sec[i].VirtualAddress;

    __try {
      const DWORD last = span - (DWORD)len;
      for (DWORD off = 0; off <= last; off++) {
        // First byte before the compare: 0x48 rejects most offsets in one instruction and the section is five
        // megabytes.
        if (start[off] != EK_LOCALINPUT_SIG[0]) continue;
        size_t k = 1;
        for (; k < len; k++) {
          if (!EK_LOCALINPUT_MASK[k]) continue;
          if (start[off + k] != EK_LOCALINPUT_SIG[k]) break;
        }
        if (k != len) continue;
        if (out->count == 0) out->rva = (unsigned)(sec[i].VirtualAddress + off);
        out->count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A section that faulted part-way was not searched: the partial count must not read as complete, since a second
      // match could sit in the bytes never looked at.
      out->faulted = 1;
    }
  }
  return true;
}

// The function containing `rva`, from the PE exception directory; 0 means the match was not inside a function.
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

// Does `rva` land in a writable data section? A displacement into code or read-only data did not resolve to a global.
static bool in_writable_data(const unsigned char* base, unsigned rva, size_t bytes) {
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = (int)nt->FileHeader.NumberOfSections;
  for (int i = 0; i < nsec; i++) {
    const DWORD lo = sec[i].VirtualAddress;
    const DWORD hi = lo + sec[i].Misc.VirtualSize;
    if (rva < lo || rva + (DWORD)bytes > hi) continue;
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) return false;
    if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) return false;
    return true;
  }
  return false;
}

// An address inside the image is not necessarily committed and writable (section alignment leaves gaps); VirtualQuery
// asks without risking the fault.
static bool writable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const DWORD ok = PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & ok)) return false;
  const unsigned char* end = (const unsigned char*)p + bytes;
  const unsigned char* regionEnd = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  return end <= regionEnd;
}

static void locate_once(void) {
  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  if (!base) { s_refused = true; ek_log("localinput: no host module"); return; }

  SigHit hit;
  if (!find_pattern(base, &hit)) {
    s_refused = true;
    ek_log("localinput: the host module has no PE headers; refusing");
    return;
  }

  if (hit.count == 0 && !hit.faulted) {
    s_refused = true;
    ek_log("localinput: signature not found in this build -- the Triggerbot "
           "row stays unavailable and nothing is written");
    return;
  }

  // Refuse rather than guess: a second match means the pattern did not discriminate, and picking one is a coin toss
  // on the address the game reads its input from.
  if (hit.count != 1 || hit.faulted) {
    s_refused = true;
    if (hit.faulted)
      ek_log("localinput: a code section faulted mid-scan, so uniqueness cannot "
             "be claimed (%d match(es) before the fault); refusing to guess",
             hit.count);
    else
      ek_log("localinput: signature is ambiguous -- %d matches, first at rva "
             "0x%07X; refusing to guess", hit.count, hit.rva);
    return;
  }

  const unsigned entry = entry_containing(base, hit.rva);
  if (entry == 0 || entry > hit.rva) {
    s_refused = true;
    ek_log("localinput: match at rva 0x%07X is not inside any function in the "
           "exception directory; refusing", hit.rva);
    return;
  }

  int disp = 0;
  __try {
    const unsigned char* const d = base + hit.rva + EK_LOCALINPUT_DISP_AT;
    disp = (int)((unsigned)d[0] | ((unsigned)d[1] << 8) |
                 ((unsigned)d[2] << 16) | ((unsigned)d[3] << 24));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    s_refused = true;
    ek_log("localinput: could not read the displacement at rva 0x%07X; refusing",
           hit.rva + EK_LOCALINPUT_DISP_AT);
    return;
  }

  const long long target = (long long)hit.rva + (long long)EK_LOCALINPUT_LEN + disp;
  if (target < 0 || target > 0x7FFFFFFF ||
      !in_writable_data(base, (unsigned)target, EK_INPUT_T_SIZE)) {
    s_refused = true;
    ek_log("localinput: the store at rva 0x%07X resolves to 0x%llX, which is not "
           "writable image data; refusing", hit.rva, target);
    return;
  }

  unsigned* const bits = (unsigned*)(void*)(base + (unsigned)target);
  if (!writable(bits, EK_INPUT_T_SIZE)) {
    s_refused = true;
    ek_log("localinput: rva 0x%07X is not committed and writable; refusing",
           (unsigned)target);
    return;
  }

  int extDisp = 0;
  bool extRead = false;
  __try {
    const unsigned char* const d = base + hit.rva + EK_LOCALINPUT_EXT_DISP_AT;
    extDisp = (int)((unsigned)d[0] | ((unsigned)d[1] << 8) |
                    ((unsigned)d[2] << 16) | ((unsigned)d[3] << 24));
    extRead = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    extRead = false;
  }
  const long long extTarget = (long long)hit.rva + (long long)EK_LOCALINPUT_EXT_END + extDisp;
  const bool extAgree = extRead && extTarget >= 0 && extTarget <= 0x7FFFFFFF &&
                        ekm_localinput_extbits_agree((unsigned)target, (unsigned)extTarget);

  InterlockedExchange(&s_rva, (LONG)target);
  if (extAgree)
    InterlockedExchangePointer((void* volatile*)&s_extbits,
                               (void*)(base + (unsigned)extTarget));
  InterlockedExchangePointer((void* volatile*)&s_bits, (void*)bits);
  ek_log("localinput: localInput at rva 0x%07X -- one match at rva 0x%07X inside "
         "the function at rva 0x%07X (%+d), displacement %+d",
         (unsigned)target, hit.rva, entry, (int)(hit.rva - entry), disp);
  ek_log("localinput: the image's extbits store resolves to rva 0x%llX, bits + %d "
         "expected 0x%07X -- %s", extTarget, EK_INPUT_T_EXTBITS,
         (unsigned)target + (unsigned)EK_INPUT_T_EXTBITS,
         extAgree ? "AGREES, Alt Fire armed"
                  : "DISAGREES or unreadable, Alt Fire stays unarmed");
}

// Guarded because the address was derived from a pattern and a wrong one must cost the feature, not the game. A fault
// disarms permanently.
static void or_bits(unsigned* bits, unsigned mask) {
  __try {
    *bits |= mask;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedExchangePointer((void* volatile*)&s_bits, (void*)NULL);
    InterlockedExchangePointer((void* volatile*)&s_extbits, (void*)NULL);
    s_refused = true;
    ek_log("localinput: writing localInput at rva 0x%07X faulted; the triggerbot is "
           "unavailable for the rest of this session", (unsigned)s_rva);
  }
}

static void ensure_located(void) {
  if (s_attempted) return;
  s_attempted = true;
  locate_once();
}

bool localinput_or_bits(unsigned mask) {
  ensure_located();
  unsigned* const bits = s_bits;
  if (!bits || !mask) return false;
  or_bits(bits, mask);
  // or_bits disarms on a fault, so this is the honest "did the write land" without a second flag.
  return s_bits != NULL;
}

// A faulting read disarms like a faulting write: the address is then not the record.
bool localinput_read_bits(unsigned* out) {
  ensure_located();
  unsigned* const bits = s_bits;
  if (!bits || !out) return false;
  unsigned v = 0;
  __try {
    v = *(const volatile unsigned*)bits;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedExchangePointer((void* volatile*)&s_bits, (void*)NULL);
    InterlockedExchangePointer((void* volatile*)&s_extbits, (void*)NULL);
    s_refused = true;
    ek_log("localinput: reading localInput at rva 0x%07X faulted; the triggerbot is "
           "unavailable for the rest of this session", (unsigned)s_rva);
    return false;
  }
  *out = v;
  return true;
}

// The engine writes this field itself: P_GetInput ORs the Alt Fire key's bit 6 into localInput.extbits in the window
// the fire bit uses, and dukeFillInputForTic clears the whole record. Faults disarm both words through or_bits.
bool localinput_or_extbits(unsigned mask) {
  ensure_located();
  unsigned* const ext = s_extbits;
  if (!ext || !s_bits || !mask) return false;
  or_bits(ext, mask);
  return s_extbits != NULL;
}

bool localinput_read_extbits(unsigned* out) {
  ensure_located();
  unsigned* const ext = s_extbits;
  if (!ext || !s_bits || !out) return false;
  unsigned v = 0;
  __try {
    v = *(const volatile unsigned*)ext;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedExchangePointer((void* volatile*)&s_bits, (void*)NULL);
    InterlockedExchangePointer((void* volatile*)&s_extbits, (void*)NULL);
    s_refused = true;
    ek_log("localinput: reading localInput.extbits at rva 0x%07X faulted; the "
           "triggerbot is unavailable for the rest of this session",
           (unsigned)s_rva + (unsigned)EK_INPUT_T_EXTBITS);
    return false;
  }
  *out = v;
  return true;
}

void localinput_locate(void) { ensure_located(); }

bool localinput_ready(void) { return s_bits != NULL; }

unsigned localinput_rva(void) { return (unsigned)s_rva; }
