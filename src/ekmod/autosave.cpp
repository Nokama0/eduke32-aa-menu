// Engine half of the timed autosave: finds the game's own autosave request (g_saveRequested, NOT g_doQuickSave, which
// opens the Save menu and follows the player's last slot) and asks for one. WHEN is autosave.h's rule. Located by two
// independent code witnesses that must agree, else nothing is armed. Details: docs/re/autosave.md.
#include <windows.h>
#include <time.h>
#include "autosave.h"
#include "config.h"
#include "log.h"
#include "input.h"
#include "playerstate.h"
#include "ui/wallclock.h"

// gameexec.cpp:5344's own format literal. Source text, so it survives a rebuild that moves addresses.
static const char EK_FMT_TEXT[] = "%d %b %Y %I:%M%p";

// How far from each anchor the rest of its witness may sit: the shipped image's distances plus room for a rebuild; a
// match further away is refused.
static const unsigned EK_AFTER_SPAN  = 64;
static const unsigned EK_BEFORE_SPAN = 160;

static volatile LONG s_flagRva = 0, s_briefRva = 0;
static unsigned char* volatile s_flag = NULL;
static unsigned char* volatile s_brief = NULL;
static bool s_attempted = false;
static bool s_refused = false;


static const IMAGE_NT_HEADERS* nt_of(const unsigned char* base) {
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
  return nt;
}

// A writable page is not guaranteed just because an address is inside the image (section gaps): VirtualQuery with the
// write bits, not a guess.
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

static bool readable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const unsigned char* end = (const unsigned char*)p + bytes;
  const unsigned char* regionEnd = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  return end <= regionEnd;
}

static bool in_writable_data(const unsigned char* base, unsigned rva, size_t bytes) {
  const IMAGE_NT_HEADERS* nt = nt_of(base);
  if (!nt) return false;
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

struct Hit { unsigned rva; int count; int faulted; };

// A section's VirtualSize can run past what is committed, so every walk is guarded. A section that faulted part-way
// was NOT searched and its partial count must not be reported: a second match could hide in the unread bytes.
static void scan_bytes(const unsigned char* start, DWORD span, unsigned baseRva,
                       const unsigned char* pat, DWORD patLen, Hit* out) {
  if (span < patLen) return;
  __try {
    const DWORD last = span - patLen;
    for (DWORD off = 0; off <= last; off++) {
      if (start[off] != pat[0]) continue;
      DWORD k = 1;
      for (; k < patLen; k++) if (start[off + k] != pat[k]) break;
      if (k != patLen) continue;
      if (out->count == 0) out->rva = baseRva + off;
      out->count++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// The literal, in the image's data; executable sections are skipped so a stray copy in code cannot be the anchor.
static Hit find_text(const unsigned char* base, const char* text, size_t len) {
  Hit h = { 0, 0, 0 };
  const IMAGE_NT_HEADERS* nt = nt_of(base);
  if (!nt) return h;
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = (int)nt->FileHeader.NumberOfSections;
  for (int i = 0; i < nsec; i++) {
    if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
    if (sec[i].SizeOfRawData == 0) continue;
    DWORD span = sec[i].Misc.VirtualSize;
    if (span > sec[i].SizeOfRawData) span = sec[i].SizeOfRawData;
    // The trailing NUL is part of the needle: a prefix of a longer string is not this literal.
    scan_bytes(base + sec[i].VirtualAddress, span, sec[i].VirtualAddress,
               (const unsigned char*)text, (DWORD)(len + 1), &h);
  }
  return h;
}

// x64 predicates: register-agnostic where the compiler chooses, register-exact where the ABI fixes it. rip+disp32 is
// mod=00 rm=101 and the destination is REX.R:reg, so the REX byte is matched as a range.

static const int EK_RIPLEN = 7;

static bool is_rip_form(const unsigned char* p, unsigned char op) {
  return (p[0] & 0xF8) == 0x48 && p[1] == op && (p[2] & 0xC7) == 0x05;
}
static int rip_reg(const unsigned char* p) {
  return (int)(((p[0] & 4) << 1) | ((p[2] >> 3) & 7));
}
static unsigned rip_target(unsigned rva, const unsigned char* p) {
  return (unsigned)((long long)rva + EK_RIPLEN + *(const int*)(p + 3));
}

// `mov byte [reg], imm8`, no REX: base is one of the low eight, neither rsp (SIB) nor rbp (rip-relative at mod=00).
static bool is_store_byte(const unsigned char* p, int reg, unsigned char imm) {
  if (reg > 7 || reg == 4 || reg == 5) return false;
  return p[0] == 0xC6 && p[1] == (unsigned char)reg && p[2] == imm;
}

static bool is_store_byte_at(const unsigned char* p, int reg, unsigned disp,
                             unsigned char imm) {
  if (reg > 7 || reg == 4) return false;
  if (p[0] != 0xC6 || p[1] != (unsigned char)(0x80 | reg)) return false;
  if (*(const unsigned*)(p + 2) != disp) return false;
  return p[6] == imm;
}

static bool is_lea_plus20(const unsigned char* p, int* dest, int* baseReg) {
  if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8D) return false;
  if ((p[2] >> 6) != 1 || p[3] != 0x20) return false;
  const int b = (int)(((p[0] & 1) << 3) | (p[2] & 7));
  if (b == 4) return false;
  *dest = (int)(((p[0] & 4) << 1) | ((p[2] >> 3) & 7));
  *baseReg = b;
  return true;
}

// A .rdata pointer slot holds the global's address, relocated by the loader; bounds-checked before it is read.
static unsigned deref_slot(const unsigned char* base, unsigned slotRva) {
  const IMAGE_NT_HEADERS* nt = nt_of(base);
  if (!nt) return 0;
  if (slotRva == 0 || slotRva + sizeof(void*) > nt->OptionalHeader.SizeOfImage) return 0;
  if (!readable(base + slotRva, sizeof(void*))) return 0;
  const unsigned char* const target = *(const unsigned char* const*)(base + slotRva);
  if (target < base || target >= base + nt->OptionalHeader.SizeOfImage) return 0;
  return (unsigned)(target - base);
}


struct CodeWalk { const unsigned char* start; DWORD span; unsigned rvaBase; };

static int code_sections(const unsigned char* base, CodeWalk* out, int cap) {
  const IMAGE_NT_HEADERS* nt = nt_of(base);
  if (!nt) return 0;
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = (int)nt->FileHeader.NumberOfSections;
  int n = 0;
  for (int i = 0; i < nsec && n < cap; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    DWORD span = sec[i].Misc.VirtualSize;
    if (sec[i].SizeOfRawData != 0 && span > sec[i].SizeOfRawData) span = sec[i].SizeOfRawData;
    if (span < 32) continue;
    out[n].start = base + sec[i].VirtualAddress;
    out[n].span = span;
    out[n].rvaBase = sec[i].VirtualAddress;
    n++;
  }
  return n;
}

// The one `lea reg,[rip+d]` whose target is `strRva`. REX.W required; the destination register is not constrained
// (it depends on the argument position).
static Hit find_lea_to(const unsigned char* base, unsigned strRva) {
  Hit h = { 0, 0, 0 };
  CodeWalk cw[8];
  const int n = code_sections(base, cw, 8);
  for (int i = 0; i < n; i++) {
    __try {
      const DWORD last = cw[i].span - EK_RIPLEN;
      for (DWORD off = 0; off <= last; off++) {
        if (!is_rip_form(cw[i].start + off, 0x8D)) continue;
        if (rip_target(cw[i].rvaBase + off, cw[i].start + off) != strRva) continue;
        if (h.count == 0) h.rva = cw[i].rvaBase + off;
        h.count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      h.faulted = 1;
    }
  }
  return h;
}

// WITNESS 1's anchor: the main loop's `g_saveRequested = false; walock[TILE_SAVESHOT] = CACHE1D_UNLOCKED;` pair
// (MAXTILES-1 and 199), image-wide and unique.
static Hit find_clear_then_walock(const unsigned char* base, unsigned* slotRva) {
  Hit h = { 0, 0, 0 };
  CodeWalk cw[8];
  const int n = code_sections(base, cw, 8);
  const DWORD need = EK_RIPLEN + 3 + EK_RIPLEN + 7;
  for (int i = 0; i < n; i++) {
    if (cw[i].span < need) continue;
    __try {
      const DWORD last = cw[i].span - need;
      for (DWORD off = 0; off <= last; off++) {
        const unsigned char* const p = cw[i].start + off;
        if (!is_rip_form(p, 0x8B)) continue;
        if (!is_store_byte(p + EK_RIPLEN, rip_reg(p), 0x00)) continue;
        const unsigned char* const q = p + EK_RIPLEN + 3;
        if (!is_rip_form(q, 0x8B)) continue;
        if (!is_store_byte_at(q + EK_RIPLEN, rip_reg(q), 0x77FF, 0xC7)) continue;
        if (h.count == 0) {
          h.rva = cw[i].rvaBase + off;
          *slotRva = rip_target(h.rva, p);
        }
        h.count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      h.faulted = 1;
    }
  }
  return h;
}

// WITNESS 1's second half: in the window before the anchor every rip-relative lea must agree on one address, and
// there must be more than one.
static bool leas_agree(const unsigned char* base, unsigned anchorRva,
                       unsigned* targetRva, int* count) {
  *count = 0;
  *targetRva = 0;
  const IMAGE_NT_HEADERS* nt = nt_of(base);
  if (!nt || anchorRva <= EK_BEFORE_SPAN) return false;
  const unsigned from = anchorRva - EK_BEFORE_SPAN;
  bool agreed = true;
  __try {
    for (unsigned rva = from; rva < anchorRva; rva++) {
      const unsigned char* const p = base + rva;
      if (!is_rip_form(p, 0x8D)) continue;
      const unsigned t = rip_target(rva, p);
      if (*count == 0) *targetRva = t;
      else if (t != *targetRva) agreed = false;
      (*count)++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return agreed && *count >= 2;
}

// WITNESS 2's first half: after the format literal's lea, the strftime call then the store of true; the call
// separates this store from the g_skipReturnToCenter store ten bytes later.
static bool find_call_then_set(const unsigned char* base, unsigned leaRva,
                               unsigned* slotRva, int* count) {
  *count = 0;
  const IMAGE_NT_HEADERS* nt = nt_of(base);
  if (!nt) return false;
  const unsigned from = leaRva + EK_RIPLEN;
  if (from + EK_AFTER_SPAN + 5 + EK_RIPLEN + 3 > nt->OptionalHeader.SizeOfImage) return false;
  __try {
    for (unsigned rva = from; rva < from + EK_AFTER_SPAN; rva++) {
      const unsigned char* const p = base + rva;
      if (p[0] != 0xE8) continue;
      const unsigned char* const q = p + 5;
      if (!is_rip_form(q, 0x8B)) continue;
      if (!is_store_byte(q + EK_RIPLEN, rip_reg(q), 0x01)) continue;
      if (*count == 0) *slotRva = rip_target(rva + 5, q);
      (*count)++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return *count == 1;
}

// WITNESS 2's second half: the strcmp(g_lastautosave.path, g_lastusersave.path) setup; the lea whose destination is
// RCX carries g_lastautosave (strcmp's first argument).
static const int EK_REG_RCX = 1;

static bool find_strcmp_pair(const unsigned char* base, unsigned leaRva,
                             unsigned* slotRva, int* count) {
  *count = 0;
  if (leaRva <= EK_BEFORE_SPAN) return false;
  const unsigned from = leaRva - EK_BEFORE_SPAN;
  __try {
    for (unsigned rva = from; rva < leaRva; rva++) {
      const unsigned char* const a = base + rva;
      if (!is_rip_form(a, 0x8B)) continue;
      const unsigned char* const b = a + EK_RIPLEN;
      if (!is_rip_form(b, 0x8B)) continue;
      int d1 = 0, b1 = 0, d2 = 0, b2 = 0;
      const unsigned char* const l1 = b + EK_RIPLEN;
      if (!is_lea_plus20(l1, &d1, &b1)) continue;
      if (!is_lea_plus20(l1 + 4, &d2, &b2)) continue;
      const int ra = rip_reg(a), rb = rip_reg(b);
      if (!((b1 == ra && b2 == rb) || (b1 == rb && b2 == ra))) continue;
      const int wanted = (d1 == EK_REG_RCX) ? b1 : (d2 == EK_REG_RCX) ? b2 : -1;
      if (wanted < 0) continue;
      if (*count == 0) {
        const unsigned char* const src = (wanted == ra) ? a : b;
        const unsigned srcRva = (unsigned)(src - base);
        *slotRva = rip_target(srcRva, src);
      }
      (*count)++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return *count == 1;
}


static bool witness_main_loop(const unsigned char* base, unsigned* flagRva,
                              unsigned* briefRva) {
  unsigned slot = 0;
  const Hit h = find_clear_then_walock(base, &slot);
  if (h.count != 1 || h.faulted) {
    ek_log("autosave: the main loop's save block matched %d time(s)%s; refusing to guess",
           h.count, h.faulted ? " and a code section faulted mid-scan" : "");
    return false;
  }
  *flagRva = deref_slot(base, slot);
  if (!*flagRva) {
    ek_log("autosave: the main loop's request-flag slot at rva 0x%07X does not hold "
           "an address inside the host image; refusing", slot);
    return false;
  }
  unsigned target = 0;
  int leas = 0;
  if (!leas_agree(base, h.rva, &target, &leas)) {
    ek_log("autosave: the %d lea(s) before the main loop's save block at rva 0x%07X "
           "do not agree on one address; refusing", leas, h.rva);
    return false;
  }
  *briefRva = target;
  ek_log("autosave: witness 1 -- main loop save block at rva 0x%07X, flag slot rva "
         "0x%07X -> rva 0x%07X, %d agreeing lea(s) -> brief rva 0x%07X",
         h.rva, slot, *flagRva, leas, *briefRva);
  return true;
}

static bool witness_con_save(const unsigned char* base, unsigned* flagRva,
                             unsigned* briefRva) {
  const Hit str = find_text(base, EK_FMT_TEXT, sizeof(EK_FMT_TEXT) - 1);
  if (str.count != 1 || str.faulted) {
    ek_log("autosave: the autosave-name format literal is %s (%d match(es)); "
           "refusing to guess",
           str.faulted ? "unverifiable, a data section faulted mid-scan"
                       : (str.count ? "ambiguous" : "absent"), str.count);
    return false;
  }
  const Hit lea = find_lea_to(base, str.rva);
  if (lea.count != 1 || lea.faulted) {
    ek_log("autosave: %d instruction(s) reference the name format at rva 0x%07X%s; "
           "refusing to guess", lea.count, str.rva,
           lea.faulted ? " and a code section faulted mid-scan" : "");
    return false;
  }
  unsigned flagSlot = 0, briefSlot = 0;
  int n = 0;
  if (!find_call_then_set(base, lea.rva, &flagSlot, &n)) {
    ek_log("autosave: %d request-flag store(s) follow the name format's reference at "
           "rva 0x%07X; refusing to guess", n, lea.rva);
    return false;
  }
  if (!find_strcmp_pair(base, lea.rva, &briefSlot, &n)) {
    ek_log("autosave: %d save-brief comparison(s) precede the name format's reference "
           "at rva 0x%07X; refusing to guess", n, lea.rva);
    return false;
  }
  *flagRva = deref_slot(base, flagSlot);
  *briefRva = deref_slot(base, briefSlot);
  if (!*flagRva || !*briefRva) {
    ek_log("autosave: a CON_SAVE pointer slot (flag rva 0x%07X, brief rva 0x%07X) does "
           "not hold an address inside the host image; refusing", flagSlot, briefSlot);
    return false;
  }
  ek_log("autosave: witness 2 -- name format at rva 0x%07X, reference at rva 0x%07X, "
         "flag slot rva 0x%07X -> rva 0x%07X, brief slot rva 0x%07X -> rva 0x%07X",
         str.rva, lea.rva, flagSlot, *flagRva, briefSlot, *briefRva);
  return true;
}

// TWO WITNESSES OR NOTHING: if the searches disagree, one matched something that is not this global. One witness is
// a pattern that matched; two that agree are a fact.
static void locate_once(void) {
  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  if (!base || !nt_of(base)) {
    s_refused = true;
    ek_log("autosave: the host module has no PE headers; refusing");
    return;
  }
  unsigned flagA = 0, briefA = 0, flagB = 0, briefB = 0;
  if (!witness_main_loop(base, &flagA, &briefA)) { s_refused = true; return; }
  if (!witness_con_save(base, &flagB, &briefB)) { s_refused = true; return; }
  if (flagA != flagB || briefA != briefB) {
    s_refused = true;
    ek_log("autosave: the two witnesses disagree (flag 0x%07X vs 0x%07X, brief 0x%07X "
           "vs 0x%07X); the timed autosave is off for this session",
           flagA, flagB, briefA, briefB);
    return;
  }
  const size_t briefBytes = (size_t)EK_SAVEBRIEF_PATH_OFF + 1;
  unsigned char* const flag = (unsigned char*)(base + flagA);
  unsigned char* const brief = (unsigned char*)(base + briefA);
  if (!in_writable_data(base, flagA, 1) || !writable(flag, 1)) {
    s_refused = true;
    ek_log("autosave: the request flag at rva 0x%07X is not committed writable image "
           "data; refusing", flagA);
    return;
  }
  if (!in_writable_data(base, briefA, briefBytes) || !writable(brief, briefBytes)) {
    s_refused = true;
    ek_log("autosave: the save brief at rva 0x%07X is not %u bytes of committed "
           "writable image data; refusing", briefA, (unsigned)briefBytes);
    return;
  }
  InterlockedExchange(&s_flagRva, (LONG)flagA);
  InterlockedExchange(&s_briefRva, (LONG)briefA);
  InterlockedExchangePointer((void* volatile*)&s_brief, (void*)brief);
  InterlockedExchangePointer((void* volatile*)&s_flag, (void*)flag);
  ek_log("autosave: installed. g_saveRequested at rva 0x%07X and g_lastautosave at rva "
         "0x%07X, agreed by the main loop and the CON_SAVE handler; interval %d minute(s)",
         flagA, briefA, autosave_mins_clamped(g_cfg.autosavemins));
}

// GUARDED, and a fault disarms permanently: the addresses came from a pattern, so a wrong one must cost the feature,
// not the game. The save name is filled before the flag is raised, because the flag is what the engine acts on.
static bool request_save(unsigned char* brief, unsigned char* flag, const char* name) {
  __try {
    memcpy(brief, name, (size_t)EK_SAVEBRIEF_NAME_CAP);
    brief[EK_SAVEBRIEF_PATH_OFF] = 0;
    *flag = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool autosave_ready(void) { return s_flag != NULL && s_brief != NULL; }

void autosave_frame(void) {
  // THE SCAN RUNS EVEN WITH THE ROW AT ZERO, once, on the first presented frame: the row's Unavailable label must know
  // whether the flag was found before the player turns the interval up.
  if (!s_attempted) {
    s_attempted = true;
    locate_once();
  }
  unsigned char* const flag = s_flag;
  unsigned char* const brief = s_brief;
  if (!flag || !brief || s_refused) return;

  int gm = 0, hp = 0, pal = 0;
  const bool gmOk = player_read_gm(&gm);
  const bool hpOk = player_read_body(&hp, &pal);
  const int inPlay = autosave_in_play(gmOk ? 1 : 0, gm, hpOk ? 1 : 0, hp,
                                      input_panel_open() ? 1 : 0);
  // THE CLOCK RESTARTS ON A LEVEL ENTRY as on gm 0: a save due over MODE_EOL's bonus screen, where gm never reads 0,
  // otherwise landed on the next level's first playable frame.
  static long s_levelEpoch = 0;
  const long epoch = level_epoch();
  const bool newLevel = ekm_level_epoch_changed(s_levelEpoch, epoch);
  s_levelEpoch = epoch;
  static AutosaveState s_state = { 0, 0 };
  if (!autosave_step(&s_state, g_cfg.autosavemins, wallclock_ms(),
                     (ekm_level_loading(gm, gmOk) || newLevel) ? 1 : 0, inPlay))
    return;

  // The name the save carries, in the engine's own format. A save with an empty name is hidden and then deleted
  // (autosave.h), so a name that cannot be built means no save is asked for.
  char name[EK_SAVEBRIEF_NAME_CAP];
  memset(name, 0, sizeof(name));
  const time_t now = time(NULL);
  struct tm parts;
  const bool clockOk = localtime_s(&parts, &now) == 0;
  if (!autosave_name_from_tm(name, (int)sizeof(name), clockOk ? &parts : NULL)) {
    ek_log("autosave: could not build a save name; this save is skipped");
    return;
  }

  if (!request_save(brief, flag, name)) {
    InterlockedExchangePointer((void* volatile*)&s_flag, (void*)NULL);
    InterlockedExchangePointer((void* volatile*)&s_brief, (void*)NULL);
    s_refused = true;
    ek_log("autosave: writing g_lastautosave at rva 0x%07X or g_saveRequested at rva "
           "0x%07X faulted; the timed autosave is off for the rest of this session",
           (unsigned)s_briefRva, (unsigned)s_flagRva);
    return;
  }
  // One line per save, always, not behind the debug row: a feature that saves on its own must leave a record.
  ek_log("autosave: %d minute(s) elapsed, asked the game for an autosave named \"%s\"",
         autosave_mins_clamped(g_cfg.autosavemins), name);
}
