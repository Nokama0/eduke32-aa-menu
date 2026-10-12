// Find the five engine symbols the sweep needs, log and publish them; hooks and writes nothing. Every rva is a SEARCH
// RESULT: a source STRING -> its lea -> the containing RUNTIME_FUNCTION -> its call sites -> the globals they touch.
// "!!numscans" is in both renderers (engine.cpp:1889, polymost.cpp:6675); r8d's __LINE__ tells them apart.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "nocull.h"
#include "imagescan.h"
#include "hitscan.h"
#include "camerascan.h"
#include "gamestate.h"
#include "mapextent.h"
#include "hook.h"
#include "camera.h"
#include "config.h"
#include "sectorstate.h"
#include "frustum.h"
#include "project.h"
#include "wallstate.h"
#include "cutaway.h"
#include "cutpaint.h"
#include "ui/wallclock.h"
#include "freecam.h"
#include "nocullburst.h"
#include "perf.h"

// Matched WITH its terminator: a ten-character anchor without one is a pooling accident (as cvars.cpp records for short
// names).
static const char EK_NOCULL_ANCHOR[] = "!!numscans";

// The line number is the discriminator: LOG_F passes __LINE__ in r8d, so the six bytes after the anchor lea are `mov
// r8d, <line>`. 6675 is polymost.cpp's; a synthesis from another revision carries another number and this refuses.
static const int EK_NOCULL_POLYMOST_LINE = 6675;

static const int EK_NOCULL_LINE_WINDOW = 16;

// The seed's lead-in window: the movsx loading globalcursectnum sits 73 bytes before the seed call, so a short window
// is a false negative that looks like the rule failing. The per-site hit count is (0, 1, 0) for every window 96..224
// and breaks at 256; 160 is mid-plateau.
static const int EK_NOCULL_LEAD_WINDOW = 160;

static const int EK_NOCULL_MAX_SITES = 8;

static const int EK_NOCULL_MAX_ANCHORS = 4;

// A small budget that only a FAULT spends: the image does not change, so an absent or ambiguous anchor latches at once;
// a section whose VirtualSize exceeds what is committed can fault part-way, and that is transient.
static const int EK_NOCULL_ATTEMPTS = 4;

static volatile LONG s_state = 0;
static volatile LONG s_attempts = 0;

static volatile LONG s_ssRva = 0;        // polymost_scansector's entry
static volatile LONG s_ssEnd = 0;
static volatile LONG s_seedSiteRva = 0;  // the `call rel32` at polymost.cpp:7078
static volatile LONG s_gotsectorRva = 0;
static volatile LONG s_gotsectorSlot = 0;
static volatile LONG s_gotsectorBytes = 0;
static volatile LONG s_counterARva = 0;
static volatile LONG s_counterBRva = 0;
static volatile LONG s_cursectnumRva = 0;
static volatile LONG s_imageMaxwallsb = 0;
static volatile LONG s_sborderSlot = 0;  // the .refptr slot naming sectorborder[]
static volatile LONG s_sborderRva = 0;   // and the array itself, element 0 only

static volatile LONG s_available = 1;

static unsigned read_pub(volatile LONG* p) {
  return (unsigned)InterlockedCompareExchange(p, 0, 0);
}

bool nocull_located(void) { return InterlockedCompareExchange(&s_state, 0, 0) == 1; }
bool nocull_answered(void) { return InterlockedCompareExchange(&s_state, 0, 0) != 0; }
unsigned nocull_scansector_rva(void) { return read_pub(&s_ssRva); }
unsigned nocull_scansector_end_rva(void) { return read_pub(&s_ssEnd); }
unsigned nocull_gotsector_slot_rva(void) { return read_pub(&s_gotsectorSlot); }
unsigned nocull_seed_site_rva(void)  { return read_pub(&s_seedSiteRva); }
unsigned nocull_gotsector_rva(void)  { return read_pub(&s_gotsectorRva); }
unsigned nocull_counter_a_rva(void)  { return read_pub(&s_counterARva); }
unsigned nocull_counter_b_rva(void)  { return read_pub(&s_counterBRva); }
unsigned nocull_cursectnum_rva(void) { return read_pub(&s_cursectnumRva); }
unsigned nocull_sectorborder_rva(void)      { return read_pub(&s_sborderRva); }
unsigned nocull_sectorborder_slot_rva(void) { return read_pub(&s_sborderSlot); }
int nocull_gotsector_bytes(void)     { return (int)read_pub(&s_gotsectorBytes); }
int nocull_image_maxwallsb(void)     { return (int)read_pub(&s_imageMaxwallsb); }

bool nocull_available(void)          { return read_pub(&s_available) != 0; }
bool nocull_row_unavailable(void)    { return !nocull_available(); }
void nocull_mark_unavailable(void)   { InterlockedExchange(&s_available, 0); }

// An address inside the image is not necessarily committed, and section alignment leaves gaps (cvars.cpp's pattern).
static bool nc_readable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const unsigned char* const end = (const unsigned char*)p + bytes;
  const unsigned char* const regionEnd =
      (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  return end <= regionEnd;
}

// WRITABLE, not merely readable: sectorborder[0] is the only engine memory this file writes.
static bool nc_writable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if ((mbi.Protect & rw) == 0) return false;
  const unsigned char* const end = (const unsigned char*)p + bytes;
  const unsigned char* const regionEnd =
      (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  return end <= regionEnd;
}

// The `len` bytes ending before `site` are copied out first so the decode is a pure function over a buffer; a faulted
// read is a refusal.
static bool nc_copy_before(const unsigned char* base, unsigned site,
                           unsigned char* out, int len) {
  if (len <= 0 || site < (unsigned)len) return false;
  __try {
    for (int k = 0; k < len; k++) out[k] = base[site - (unsigned)len + k];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

// True when `rva` lands in a readable, non-executable section (.bss and .data included), hence ek_image_data_extent and
// not the search filter.
static bool nc_in_data(const unsigned char* base, unsigned rva) {
  unsigned begin = 0, end = 0;
  const bool ok = ek_image_data_extent(base, rva, &begin, &end);
  return ok && rva >= begin && rva < end;
}

// Every occurrence of the anchor: ek_image_find_string reports only the first rva, but both are load-bearing here. A
// faulted span forces a refusal rather than a partial count.
struct NcStrHits { unsigned rva[EK_NOCULL_MAX_ANCHORS]; int count; int faulted; };

static void nc_find_string_all(const unsigned char* base, const char* s,
                               size_t len, NcStrHits* out) {
  out->count = 0; out->faulted = 0;
  for (int k = 0; k < EK_NOCULL_MAX_ANCHORS; k++) out->rva[k] = 0;
  if (!s || len == 0) { out->faulted = 1; return; }

  EkSpan spans[16];
  int n = 0;
  if (!ek_image_sections(base, false, spans, 16, &n)) { out->faulted = 1; return; }

  for (int i = 0; i < n; i++) {
    if (spans[i].size < len) continue;
    __try {
      const DWORD last = spans[i].size - (DWORD)len;
      for (DWORD off = 0; off <= last; off++) {
        if (spans[i].base[off] != (unsigned char)s[0]) continue;
        size_t k = 1;
        for (; k < len; k++)
          if (spans[i].base[off + k] != (unsigned char)s[k]) break;
        if (k != len) continue;
        if (out->count < EK_NOCULL_MAX_ANCHORS)
          out->rva[out->count] = ek_image_rva(base, spans[i].base + off);
        out->count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      out->faulted = 1;
    }
  }
}

// `41 B8 imm32` is `mov r8d, imm32`, loguru's third argument. Returns -1 when the window has none or was unreadable.
static int nc_line_after_lea(const unsigned char* base, unsigned leaRva) {
  int line = -1;
  __try {
    const unsigned char* const p = base + leaRva;
    for (int k = 7; k <= EK_NOCULL_LINE_WINDOW; k++) {
      if (p[k] != 0x41 || p[k + 1] != 0xB8) continue;
      line = *(const int*)(p + k + 2);
      break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    line = -1;
  }
  return line;
}

// MAXWALLSB from the image: polymost.cpp:6673's `numscans >= MAXWALLSB-1` compiles to `cmp ax, imm16` with imm =
// MAXWALLSB-2 = 0x1FFE, twice. Only the `66 3D iw` form is scanned (the one measured); another form leaves it
// UNCONFIRMED, the safe failure.
struct NcCmpScan { int hits; int value; int disagreed; int faulted; };

static void nc_find_maxwallsb(const unsigned char* base, unsigned begin,
                              unsigned end, NcCmpScan* out) {
  out->hits = 0; out->value = 0; out->disagreed = 0; out->faulted = 0;
  if (end <= begin + 4) return;
  __try {
    for (unsigned q = begin; q + 4 <= end; q++) {
      if (base[q] != 0x66 || base[q + 1] != 0x3D) continue;
      const int imm = (int)(*(const unsigned short*)(base + q + 2));
      const int v = ekm_nocull_maxwallsb_from_cmp(imm);
      if (v == 0) continue;
      if (out->hits == 0) out->value = v;
      else if (v != out->value) out->disagreed = 1;
      out->hits++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

struct NcSites { unsigned site[EK_NOCULL_MAX_SITES]; int count; int faulted; };

static void nc_find_call_sites(const unsigned char* base, unsigned entry,
                               NcSites* out) {
  out->count = 0; out->faulted = 0;
  for (int k = 0; k < EK_NOCULL_MAX_SITES; k++) out->site[k] = 0;

  EkSpan spans[16];
  int n = 0;
  if (!ek_image_sections(base, true, spans, 16, &n)) { out->faulted = 1; return; }

  for (int i = 0; i < n; i++) {
    if (spans[i].size < 5) continue;
    __try {
      const DWORD last = spans[i].size - 5;
      for (DWORD off = 0; off <= last; off++) {
        if (spans[i].base[off] != 0xE8) continue;
        const unsigned site = ek_image_rva(base, spans[i].base + off);
        const int disp = *(const int*)(spans[i].base + off + 1);
        if (ekm_us_rel32_target(site, disp) != entry) continue;
        if (out->count < EK_NOCULL_MAX_SITES) out->site[out->count] = site;
        out->count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      out->faulted = 1;
    }
  }
}

// `movsx r32, word ptr [rip+disp32]` in a call site's lead-in with a data target: the inlined head of the seed, reading
// a 16-bit GLOBAL (`globalcursectnum < MAXSECTORS`); the YAX site (polymost.cpp:7073) and drawalls load their sector
// elsewhere. Scanned, not decoded, and checked against a capstone decode at every window 96..224.
struct NcLead { int hits; unsigned target; int faulted; };

static void nc_find_cursectnum_load(const unsigned char* base, unsigned site,
                                    unsigned ownerBegin, NcLead* out) {
  out->hits = 0; out->target = 0; out->faulted = 0;
  unsigned from = site > (unsigned)EK_NOCULL_LEAD_WINDOW
                      ? site - (unsigned)EK_NOCULL_LEAD_WINDOW : 0;
  if (from < ownerBegin) from = ownerBegin;   // never read below the function
  if (site <= from + 7) return;

  // The same instruction is reached from two positions (itself and through a preceding byte in the REX range), so an
  // accepted match records where it decoded FROM and is not counted twice: a doubled hit would refuse the seed.
  unsigned lastAt = 0xFFFFFFFFu;
  __try {
    for (unsigned q = from; q + 7 <= site; q++) {
      unsigned at = q;
      if (base[at] >= 0x40 && base[at] <= 0x4F) at++;   // an optional REX
      if (at + 7 > site) continue;
      if (base[at] != 0x0F || base[at + 1] != 0xBF) continue;
      if ((base[at + 2] & 0xC7) != 0x05) continue;
      if (at == lastAt) continue;
      const int disp = *(const int*)(base + at + 3);
      const unsigned next = at + 7;
      const unsigned target = (unsigned)((int)next + disp);
      if (!nc_in_data(base, target)) continue;
      lastAt = at;
      if (out->hits == 0) out->target = target;
      out->hits++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// polymost.cpp:7059's `numscans = numbunches = 0;` emits two adjacent `mov word ptr [rip+disp32], r16` stores (GCC
// zeroed a register), not an immediate store; they are found by their targets' adjacency to globalcursectnum.
struct NcPair { unsigned a; unsigned b; int found; int faulted; };

static void nc_find_counters(const unsigned char* base, unsigned begin,
                             unsigned end, unsigned gcsRva, NcPair* out) {
  out->a = 0; out->b = 0; out->found = 0; out->faulted = 0;
  if (gcsRva < 6 || end <= begin + 7) return;
  const unsigned lo = gcsRva - 6;
  const unsigned hi = gcsRva - 2;

  __try {
    for (unsigned q = begin; q + 7 <= end; q++) {
      if (base[q] != 0x66) continue;
      unsigned at = q + 1;
      if (base[at] >= 0x40 && base[at] <= 0x4F) at++;   // an optional REX
      if (at + 6 > end) continue;
      if (base[at] != 0x89) continue;
      if ((base[at + 1] & 0xC7) != 0x05) continue;
      const int disp = *(const int*)(base + at + 2);
      const unsigned next = at + 6;
      const unsigned target = (unsigned)((int)next + disp);
      if (target < lo || target > hi) continue;
      if (out->found == 0) { out->a = target; out->found = 1; continue; }
      if (target == out->a) continue;                  // the same word, stored twice
      if (out->b == 0) { out->b = target; out->found = 2; continue; }
      if (target != out->b) out->found++;              // a third distinct word: refuse
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// gotsector: engine.cpp:9502's `Bmemset(gotsector, 0, sizeof(gotsector))` compiles to `mov rdi, [rip+d]; xor eax,eax;
// mov ecx, 0x40; rep stosq`; RDI is the test (rep stosq writes through it). MinGW reaches a global through a .refptr
// slot, an 8-byte slot in .rdata holding the global's address, so the slot is resolved, not used.
struct NcGot { unsigned slot; unsigned rva; int qwords; int sites; int faulted; };

static void nc_find_gotsector(const unsigned char* base, unsigned begin,
                              unsigned end, NcGot* out) {
  out->slot = 0; out->rva = 0; out->qwords = 0; out->sites = 0; out->faulted = 0;
  if (end <= begin + 3) return;

  __try {
    for (unsigned q = begin; q + 3 <= end; q++) {
      if (base[q] != 0xF3 || base[q + 1] != 0x48 || base[q + 2] != 0xAB) continue;
      const unsigned from = q > begin + 24 ? q - 24 : begin;
      int qwords = 0;
      unsigned slot = 0;
      for (unsigned r = from; r + 5 <= q; r++) {
        if (base[r] == 0xB9) qwords = *(const int*)(base + r + 1);
        if (r + 7 <= q && base[r] == 0x48 && base[r + 1] == 0x8B &&
            base[r + 2] == 0x3D)
          slot = (unsigned)((int)(r + 7) + *(const int*)(base + r + 3));
      }
      if (qwords == 0 || slot == 0) continue;
      out->sites++;
      if (out->sites == 1) { out->slot = slot; out->qwords = qwords; }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// Corroboration: scansector reaches sprite[] through a .refptr slot; resolving each slot it loads finds the one whose
// contents are the sprite array gamestate.cpp's live scan names. The ARRAY is compared, never the slot (it varies
// between `sprite` and the CON VM's vm.pSprite, which hold the same address). Non-gating.
struct NcSpr { int slots; int matches; unsigned matchSlot; int faulted; };

static void nc_corroborate_sprite(const unsigned char* base, unsigned begin,
                                  unsigned end, unsigned arrayRva, NcSpr* out) {
  out->slots = 0; out->matches = 0; out->matchSlot = 0; out->faulted = 0;
  if (arrayRva == 0 || end <= begin + 7) return;
  const unsigned char* const want = base + arrayRva;

  __try {
    for (unsigned q = begin; q + 7 <= end; q++) {
      if (base[q] < 0x48 || base[q] > 0x4F) continue;     // REX.W
      if (base[q + 1] != 0x8B) continue;
      if ((base[q + 2] & 0xC7) != 0x05) continue;
      const unsigned slot = (unsigned)((int)(q + 7) + *(const int*)(base + q + 3));
      if (!nc_in_data(base, slot)) continue;
      const size_t globalRva = slot_target_rva(base, slot);
      if (globalRva == 0 || !nc_in_data(base, (unsigned)globalRva)) continue;
      out->slots++;
      const unsigned char* const g = base + globalRva;
      if (!nc_readable(g, sizeof(void*))) continue;
      if (*(const unsigned char* const*)g != want) continue;
      if (out->matches == 0) out->matchSlot = slot;
      out->matches++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

enum NcVerdict { NC_OK = 0, NC_REFUSED = 1, NC_FAULTED = 2 };

static NcVerdict locate_now(const unsigned char* base) {
  NcStrHits sh;
  nc_find_string_all(base, EK_NOCULL_ANCHOR, sizeof(EK_NOCULL_ANCHOR), &sh);
  if (sh.faulted) {
    ek_log("nocull: a data section faulted mid-scan, so the \"!!numscans\" "
           "anchor cannot be counted (%d occurrence(s) before the fault)",
           sh.count);
    return NC_FAULTED;
  }
  if (sh.count < 1 || sh.count > EK_NOCULL_MAX_ANCHORS) {
    ek_log("nocull: the \"!!numscans\" anchor occurs %d time(s) in this build; "
           "both renderers' scansectors log it and two is what r10669 has. "
           "Refusing: there is no seam to sweep from", sh.count);
    return NC_REFUSED;
  }

  unsigned leaRva = 0, strRva = 0;
  int matches = 0;
  for (int i = 0; i < sh.count; i++) {
    EkRefHit h;
    ek_image_find_lea_refs(base, sh.rva[i], &h);
    if (h.faulted) {
      ek_log("nocull: a code section faulted mid-scan, so the anchor at rva "
             "0x%07X cannot be claimed unique (%d reference(s) before the "
             "fault)", sh.rva[i], h.count);
      return NC_FAULTED;
    }
    const int line = h.count == 1 ? nc_line_after_lea(base, h.rva) : -1;
    ek_log("nocull: anchor %d/%d at rva 0x%07X -- %d lea reference(s)%s, "
           "LOG_F line %d", i + 1, sh.count, sh.rva[i], h.count,
           h.count == 1 ? "" : " (ambiguous)", line);
    if (h.count != 1 || line != EK_NOCULL_POLYMOST_LINE) continue;
    if (matches == 0) { leaRva = h.rva; strRva = sh.rva[i]; }
    matches++;
  }
  if (matches != 1) {
    ek_log("nocull: %d of the %d anchor(s) carry LOG_F line %d "
           "(polymost.cpp's). Exactly one must. Refusing rather than sweeping "
           "the classic renderer's state, which this game never draws",
           matches, sh.count, EK_NOCULL_POLYMOST_LINE);
    return NC_REFUSED;
  }

  unsigned ssBegin = 0, ssEnd = 0;
  const bool haveExtent = ek_image_extent_containing(base, leaRva, &ssBegin, &ssEnd);
  if (!haveExtent || ssBegin == 0 || ssBegin > leaRva) {
    // ek_image_extent_containing answers false for no exception directory, no covering entry and a faulted walk alike,
    // so this line may not claim which.
    ek_log("nocull: the polymost anchor's lea at rva 0x%07X is not inside any "
           "function the exception directory names, or the image has no usable "
           "exception directory; refusing", leaRva);
    return NC_REFUSED;
  }
  ek_log("nocull: polymost_scansector is the function at rva 0x%07X..0x%07X "
         "(0x%X bytes) -- reached from the \"!!numscans\" literal at 0x%07X "
         "through the one lea at 0x%07X carrying LOG_F line %d",
         ssBegin, ssEnd, ssEnd - ssBegin, strRva, leaRva,
         EK_NOCULL_POLYMOST_LINE);

  NcCmpScan cmp;
  nc_find_maxwallsb(base, ssBegin, ssEnd, &cmp);
  const int imageMax = (cmp.hits > 0 && !cmp.disagreed && !cmp.faulted) ? cmp.value : 0;
  if (imageMax == 0) {
    ek_log("nocull: MAXWALLSB is UNCONFIRMED from the image -- %d plausible "
           "`cmp ax, imm16` in polymost_scansector%s%s. The budget still uses "
           "%d from build.h:92 over MAXWALLSV8, which nothing here contradicts",
           cmp.hits, cmp.disagreed ? ", and they disagree" : "",
           cmp.faulted ? ", and the scan faulted" : "", EK_NOCULL_MAXWALLSB);
  } else if (imageMax != EK_NOCULL_MAXWALLSB) {
    // A CONTRADICTION, not a missing confirmation: a wrong budget constant is what stands between the sweep and an
    // overflow of fixed engine arrays, so the row goes unavailable.
    ek_log("nocull: !! the image states MAXWALLSB = %d (from `cmp ax, 0x%04X` "
           "x%d), not the %d this build was compiled with. The sweep's budget "
           "would be wrong in exactly the direction that overflows numscans. "
           "Marking the row unavailable",
           imageMax, (unsigned)(imageMax - 2), cmp.hits, EK_NOCULL_MAXWALLSB);
    nocull_mark_unavailable();
    return NC_REFUSED;
  } else {
    ek_log("nocull: MAXWALLSB = %d, confirmed by the image -- %d x "
           "`cmp ax, 0x%04X`, which is the `numscans >= MAXWALLSB-1` guard at "
           "polymost.cpp:6673 compiled as `> MAXWALLSB-2`",
           imageMax, cmp.hits, (unsigned)(imageMax - 2));
  }

  EkCallCount cc;
  ek_image_count_calls(base, ssBegin, &cc);
  if (cc.faulted) {
    ek_log("nocull: a code section faulted while counting calls to "
           "polymost_scansector (%d site(s) before the fault)", cc.sites);
    return NC_FAULTED;
  }
  ek_log("nocull: polymost_scansector is called from %d site(s) in %d distinct "
         "function(s)%s -- polymost.cpp has three (:6548 in polymost_drawalls, "
         ":7073 the YAX loop and :7078 the seed), two of them in one function",
         cc.sites, cc.owners, cc.capped ? " (owner list capped)" : "");

  NcSites sites;
  nc_find_call_sites(base, ssBegin, &sites);
  if (sites.faulted) {
    ek_log("nocull: a code section faulted while listing the call sites (%d "
           "before the fault)", sites.count);
    return NC_FAULTED;
  }
  if (sites.count < 1 || sites.count > EK_NOCULL_MAX_SITES) {
    ek_log("nocull: %d call site(s) reach polymost_scansector; the table holds "
           "%d. Refusing", sites.count, EK_NOCULL_MAX_SITES);
    return NC_REFUSED;
  }

  unsigned seedSite = 0, gcsRva = 0, seedOwner = 0;
  int seedMatches = 0;
  for (int i = 0; i < sites.count; i++) {
    const unsigned owner = ek_image_entry_containing(base, sites.site[i]);
    NcLead lead;
    nc_find_cursectnum_load(base, sites.site[i], owner, &lead);
    if (lead.faulted) {
      ek_log("nocull: the lead-in before the call site at rva 0x%07X faulted "
             "mid-scan", sites.site[i]);
      return NC_FAULTED;
    }
    ek_log("nocull: call site %d/%d at rva 0x%07X in the function at 0x%07X -- "
           "%d `movsx r32, word [rip+d]` naming a data rva in its %d-byte "
           "lead-in, first target rva 0x%07X",
           i + 1, sites.count, sites.site[i], owner, lead.hits,
           EK_NOCULL_LEAD_WINDOW, lead.target);
    if (lead.hits != 1) continue;
    if (seedMatches == 0) { seedSite = sites.site[i]; gcsRva = lead.target;
                            seedOwner = owner; }
    seedMatches++;
  }
  if (seedMatches != 1 || seedOwner == 0) {
    ek_log("nocull: %d of the %d call site(s) load their sector from a 16-bit "
           "global. Exactly one must -- the seed at polymost.cpp:7078 reads "
           "globalcursectnum, where the YAX loop reads a bunch table and "
           "polymost_drawalls a wall record. Refusing", seedMatches, sites.count);
    return NC_REFUSED;
  }

  unsigned ownerBegin = 0, ownerEnd = 0;
  const bool haveOwner = ek_image_extent_containing(base, seedSite, &ownerBegin,
                                                    &ownerEnd);
  if (!haveOwner || ownerEnd <= ownerBegin) {
    ek_log("nocull: the seed call at rva 0x%07X has no extent in the exception "
           "directory; refusing", seedSite);
    return NC_REFUSED;
  }
  ek_log("nocull: the SEED is the call at rva 0x%07X (polymost.cpp:7078), "
         "inside renderDrawRoomsQ16 (polymost_drawrooms inlined) at "
         "0x%07X..0x%07X, and its argument is globalcursectnum at rva 0x%07X",
         seedSite, ownerBegin, ownerEnd, gcsRva);

  NcPair pair;
  nc_find_counters(base, ownerBegin, ownerEnd, gcsRva, &pair);
  if (pair.faulted) {
    ek_log("nocull: the seed's own function faulted while looking for the "
           "counter stores");
    return NC_FAULTED;
  }
  if (pair.found != 2 || !ekm_nocull_counter_pair_ok(pair.a, pair.b, gcsRva)) {
    ek_log("nocull: %d distinct 16-bit rip store target(s) in the 6 bytes below "
           "globalcursectnum (0x%07X, 0x%07X); polymost.cpp:7059's "
           "`numscans = numbunches = 0` puts exactly two there. Refusing",
           pair.found, pair.a, pair.b);
    return NC_REFUSED;
  }
  ek_log("nocull: numscans/numbunches are the two words at rva 0x%07X and "
         "0x%07X, 2 and 4 bytes below globalcursectnum. WHICH IS WHICH IS NOT "
         "DECIDED and does not need to be: nocull_budget_allows takes max() of "
         "the pair and refuses when either is above the floor, so its answer is "
         "invariant under swapping them", pair.a, pair.b);

  NcGot got;
  nc_find_gotsector(base, ownerBegin, ownerEnd, &got);
  if (got.faulted) {
    ek_log("nocull: the seed's own function faulted while looking for the "
           "gotsector memset");
    return NC_FAULTED;
  }
  if (got.sites != 1) {
    ek_log("nocull: %d `rep stosq` with a `mov ecx, imm32` and a "
           "`mov rdi, [rip+d]` in front of it in the seed's function; "
           "engine.cpp:9502's `Bmemset(gotsector, 0, sizeof(gotsector))` is "
           "exactly one. Refusing", got.sites);
    return NC_REFUSED;
  }
  const int gotBytes = ekm_nocull_gotsector_bytes(got.qwords);
  const size_t gotRva = slot_target_rva(base, got.slot);
  if (gotBytes == 0 || gotRva == 0 || !nc_in_data(base, (unsigned)gotRva)) {
    ek_log("nocull: the gotsector memset at the slot 0x%07X clears %d qword(s) "
           "into rva 0x%07X, which is not a readable data address or not a "
           "plausible sector bitmap. Refusing",
           got.slot, got.qwords, (unsigned)gotRva);
    return NC_REFUSED;
  }
  if (gotBytes != 512) {
    ek_log("nocull: !! gotsector is %d bytes (%d qword(s)), not the 512 that "
           "bitmap_size(MAXSECTORS) is for MAXSECTORS 4096. This build has a "
           "different MAXSECTORS and the sweep will be bounded by %d sectors",
           gotBytes, got.qwords, gotBytes * 8);
  }
  ek_log("nocull: gotsector is %d bytes at rva 0x%07X, reached through the "
         "mingw .refptr slot at rva 0x%07X -- %d qwords cleared, which is "
         "bitmap_size(MAXSECTORS) for MAXSECTORS %d",
         gotBytes, (unsigned)gotRva, got.slot, got.qwords, gotBytes * 8);

  const int ns = mapextent_numsectors();
  ek_log("nocull: numsectors is %d (mapextent's, %s), so the sweep would walk "
         "%d sector(s)", ns, mapextent_ready() ? "published" : "not ready yet",
         ekm_nocull_sector_bound(gotBytes, ns));

  // sectorborder[]: anchored at the seed call site, not searched for. IPA-SRA made the CALLER do the store, so the
  // bytes ending where that `call` begins are the evidence (nocull.h decodes them). Non-gating and quiet: a wrong
  // address written before a few hundred calls a frame is far worse than a feature that stays as it is.
  {
    unsigned char win[EK_NOCULL_SBORDER_WINDOW];
    if (!nc_copy_before(base, seedSite, win, EK_NOCULL_SBORDER_WINDOW)) {
      ek_log("nocull: the %d bytes before the seed call at rva 0x%07X could not "
             "be read, so sectorborder[] stays unlocated and the sweep keeps "
             "the behaviour it has today", EK_NOCULL_SBORDER_WINDOW, seedSite);
    } else {
      int sbDisp = 0, sbLoadEnd = 0;
      const bool sbOk = ekm_nocull_sectorborder_slot(
          win, EK_NOCULL_SBORDER_WINDOW, &sbDisp, &sbLoadEnd);
      const unsigned sbSlot =
          sbOk ? (unsigned)((int)(seedSite - (unsigned)EK_NOCULL_SBORDER_WINDOW +
                                  (unsigned)sbLoadEnd) + sbDisp)
               : 0u;
      const size_t sbRva = (sbSlot != 0 && nc_in_data(base, sbSlot))
                               ? slot_target_rva(base, sbSlot) : 0;
      if (!sbOk) {
        ek_log("nocull: no `mov r64,[rip+d]` with a `mov word [r64],r16` ending "
               "at the seed call at rva 0x%07X -- either the shape changed or "
               "this synthesis honours polymost_scansector's parameter. "
               "sectorborder[] stays unlocated and the sweep is unchanged",
               seedSite);
      } else if (sbRva == 0 || !nc_in_data(base, (unsigned)sbRva) ||
                 !nc_writable(base + sbRva, sizeof(short))) {
        ek_log("nocull: the store before the seed call at rva 0x%07X names the "
               "slot at rva 0x%07X, which resolves to rva 0x%07X -- not a "
               "writable data address. Publishing NOTHING rather than storing a "
               "guess into engine memory every frame",
               seedSite, sbSlot, (unsigned)sbRva);
      } else {
        InterlockedExchange(&s_sborderSlot, (LONG)sbSlot);
        InterlockedExchange(&s_sborderRva, (LONG)sbRva);
        ek_log("nocull: sectorborder[] is at rva 0x%07X, through the mingw "
               ".refptr slot at rva 0x%07X, found ADJACENT TO the seed call "
               "site at rva 0x%07X -- the store ends exactly where that call "
               "begins, %d byte(s) after the load that named the slot. This is "
               "the argument polymost_scansector really reads: LTO's IPA-SRA "
               "took its parameter away and made every caller store here",
               (unsigned)sbRva, sbSlot, seedSite,
               EK_NOCULL_SBORDER_WINDOW - sbLoadEnd);
      }
    }
  }

  const unsigned sprArray = gamestate_sprite_array_rva();
  if (sprArray == 0) {
    ek_log("nocull: the sprite corroboration is unavailable -- gamestate has "
           "not published an array yet. The locate stands on its own evidence");
  } else {
    NcSpr spr;
    nc_corroborate_sprite(base, ssBegin, ssEnd, sprArray, &spr);
    if (spr.matches == 1)
      ek_log("nocull: CORROBORATED -- of %d .refptr global(s) polymost_scansector "
             "loads, exactly 1 (slot rva 0x%07X) holds the sprite array at rva "
             "0x%07X, which is the array gamestate.cpp's live scan names%s",
             spr.slots, spr.matchSlot, sprArray,
             spr.faulted ? " (the scan faulted part-way, so these counts are "
                           "partial)" : "");
    else
      ek_log("nocull: the sprite corroboration DISAGREES -- %d of %d .refptr "
             "global(s) in polymost_scansector hold the array at rva 0x%07X "
             "that gamestate.cpp names%s. Not a refusal: the sprite scan is the "
             "thing being corroborated here, not a precondition",
             spr.matches, spr.slots, sprArray,
             spr.faulted ? " (the scan faulted part-way)" : "");
  }

  InterlockedExchange(&s_ssRva, (LONG)ssBegin);
  InterlockedExchange(&s_ssEnd, (LONG)ssEnd);
  InterlockedExchange(&s_seedSiteRva, (LONG)seedSite);
  InterlockedExchange(&s_cursectnumRva, (LONG)gcsRva);
  InterlockedExchange(&s_counterARva, (LONG)pair.a);
  InterlockedExchange(&s_counterBRva, (LONG)pair.b);
  InterlockedExchange(&s_gotsectorSlot, (LONG)got.slot);
  InterlockedExchange(&s_gotsectorRva, (LONG)gotRva);
  InterlockedExchange(&s_gotsectorBytes, (LONG)gotBytes);
  InterlockedExchange(&s_imageMaxwallsb,
                      (LONG)(imageMax != 0 ? imageMax : EK_NOCULL_MAXWALLSB));
  return NC_OK;
}

void nocull_locate(void) {
  if (InterlockedCompareExchange(&s_state, 0, 0) != 0) return;

  // s_base is stored after s_drawBegin in camera_scan and written by the game thread, so a NULL here costs nothing: the
  // sampler returns a second later. No attempt is charged.
  const unsigned char* const base = s_base;
  if (base == NULL) return;
  if (host_image_size(base) == 0) {
    ek_log("nocull: the host image's PE headers did not parse; the whole-map "
           "sweep stays unavailable");
    InterlockedExchange(&s_state, 2);
    nocull_mark_unavailable();
    return;
  }

  const NcVerdict v = locate_now(base);
  if (v == NC_OK) {
    InterlockedExchange(&s_state, 1);
    ek_log("nocull: LOCATED. Nothing is redirected and no engine memory is "
           "written -- pass 2 only publishes what pass 3 will need");
    return;
  }
  if (v == NC_REFUSED) {
    InterlockedExchange(&s_state, 2);
    nocull_mark_unavailable();
    return;
  }
  const LONG n = InterlockedIncrement(&s_attempts);
  if (n >= EK_NOCULL_ATTEMPTS) {
    ek_log("nocull: giving up after %d faulted attempt(s); the whole-map sweep "
           "stays unavailable for the life of this process", (int)n);
    InterlockedExchange(&s_state, 2);
    nocull_mark_unavailable();
  }
}

// GAME THREAD ONLY below this line: everything above runs on gamestate.cpp's sampler thread and publishes through
// interlocked scalars; everything here runs inside renderDrawRoomsQ16 on the draw thread (overlay.cpp's SwapBuffers
// hook and camera.cpp's bracket use it too).

typedef void (*ScanSectorFn)(int sectnum);
static ScanSectorFn s_origScanSector = NULL;
static EkCallPatch s_seedPatch;

static bool s_saidNoOrig = false;
static bool s_saidNoRead = false;
static bool s_saidNoBorder = false;

// The sweep's own session latch: a failed engine read logs once and gives up. The row's "(Unavailable)" is not that
// latch: the config stays on and the gate would send the sweep back into the faulted read.
static bool s_sweepDead = false;

static unsigned s_sweptFrames = 0;
static unsigned s_sweptSectors = 0;

// How often does this seam fire per frame? The engine logs "Tried to draw wall N twice" 4.7 million times a session
// (six walls of sector 177). The sweep cannot re-submit a sector within one pass in either order mode, so repeats are
// ACROSS sweeps: entries per second against the frame rate settles it. s_submits[] names the flood's sector.
static unsigned s_submits[EK_MAXSECTORS];
static unsigned s_seedEntries = 0;     // every entry, gated or not
static unsigned s_seedEntriesAtLog = 0;
static unsigned s_sweepsAtLog = 0;
static unsigned s_scansAtLog = 0;
// Drawalls per drawrooms pass, not per second: a rate divided by armed sweeps mixes in YAX sub-passes and the mirror
// site (polymost.cpp:7100). Contamination only ADDS, so a LOW max is conclusive alone; a HIGH max only on a level with
// no TROR sub-pass (E1L1 is version 7 at 326 sectors, hence `bound` 326 is right against the GRP copy's 325).
static unsigned s_allsAtSeed = 0;      // reading at the previous seed
static unsigned s_allsLast = 0;        // calls the previous SWEPT pass made
static unsigned s_allsMax = 0;         // the most any swept pass has made
static bool s_allsSeeded = false;      // the first delta has no baseline
static bool s_prevSeedSwept = false;   // was the previous pass one of ours
static bool s_prevSeedWasExtra = false;
static int s_allsLastExtra = -1;       // -1: no extra-pass drawalls figure yet
static int s_allsLastMain = -1;        // -1: no main-pass drawalls figure yet
static unsigned long long s_lastRateMs = 0;
static unsigned s_budgetStops = 0;        // main-pass sweeps that stopped
static unsigned s_budgetStopsExtra = 0;   // extra-pass sweeps that stopped
static unsigned s_cutoffStopsExtra = 0;

static volatile LONG s_lastMainStopped = 0;
bool nocull_last_main_stopped(void) {
  return InterlockedCompareExchange(&s_lastMainStopped, 0, 0) != 0;
}
static volatile LONG64 s_lastMainReach = -1;
long long nocull_last_main_reach(void) {
  return InterlockedCompareExchange64(&s_lastMainReach, 0, 0);
}
// The hold: render-thread owned, so it needs no interlocking; only the two published scalars cross to the camera. They
// publish ekm_nocull_hold_wanted(s_extraHold) and its reach, so the trigger is sticky instead of flipping on any one
// main pass that completes.
static NcExtraHold s_extraHold = ekm_nocull_hold_idle();
static int s_bunchMax = 0;
static unsigned s_unreadableSectors = 0;

static bool s_haveFullDelta = false;
static int s_deltaCallCount = 0;
static int s_deltaFirstBunch[8];
static int s_deltaFirstCount = 0;
static int s_deltaBunchZero = 0, s_deltaBunchMin = 0, s_deltaBunchMax = 0;
static int s_deltaScanZero = 0, s_deltaScanMin = 0, s_deltaScanMax = 0;

// Visibility filter before the budget: a sector whose footprint falls outside the view is skipped BEFORE the budget is
// consulted, so the budget reaches what the camera looks at. The GL frustum route is closed (polymost transforms on the
// CPU); this port's own projection is used. Skipped sectors skip their sprites: models may pop in at the edge.

// The frame's projection plus the two screen edges every corner is measured against; no slab here: each corner derives
// its own band through cam_band_fill (a whole-map slab made the test inert).
struct NcView {
  EkProjection proj;
  float leftEdge, rightEdge;
  int ready;
};

struct NcViewTrace {
  int projOk;                   // project_begin answered with a valid projection
  int fromGL;                   // the viewport came from GL rather than the client rect
  int vpx, vpy, vpw, vph;
  float leftEdge, rightEdge;
  int slabOk, slabLo, slabHi, slabCount;
};

// One byte per sector, rebuilt on every swept pass. A sector that NEIGHBOURS a sloped one is kept: a two-sided wall's
// panels use the neighbour's floor and ceiling vertices and a sloped neighbour's band cannot be bounded (frustum.h). A
// sector's own slope bit refuses nothing (cam_band_fill bounds it exactly).
static unsigned char s_cullSloped[EK_MAXSECTORS];

// The gather pass's candidate list, sized to EK_MAXSECTORS and never grown past it; static, since a stack array this
// size on a hooked render-thread call is a risk.
static NcSweepCandidate s_sweepCandidates[EK_MAXSECTORS];

// QueryPerformanceCounter, like aim.cpp's aim_now_ms and perf.cpp's perf_now: a third small clock rather than perf.h's
// zone system for one number the log wants once a second. Returns 0 on failure so a bad clock reads as no time
// measured.
static long long nc_now_us(void) {
  static long long freqHz = 0;
  static bool tried = false;
  static bool broken = false;
  if (broken) return 0;
  if (!tried) {
    tried = true;
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) {
      broken = true;
      ek_log("nocull: QueryPerformanceFrequency failed, sweep-order timing is off");
      return 0;
    }
    freqHz = (long long)f.QuadPart;
  }
  LARGE_INTEGER now;
  if (!QueryPerformanceCounter(&now)) {
    broken = true;
    ek_log("nocull: QueryPerformanceCounter failed, sweep-order timing is off");
    return 0;
  }
  return (long long)((double)now.QuadPart * 1000000.0 / (double)freqHz);
}

static bool s_saidNoProjection = false;

// Pass one (camera.cpp's, over this sweep's bound): the band every cullable sector's geometry lies in, and which
// sectors are sloped. A record that would not read is SLOPED: it cannot be bounded, so its neighbours are drawn and it
// stays out of the band.
static EkCullSlab nc_scan_slab(int slopedEnd) {
  EkCullSlab slab = ekm_slab_init();
  for (int i = 0; i < slopedEnd; i++) {
    EkSectorInfo si = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const bool ok = sectorstate_info(i, &si);
    if (!ok) { s_cullSloped[i] = 1; continue; }
    s_cullSloped[i] = (unsigned char)(si.sloped ? 1 : 0);
    slab = ekm_slab_add(slab, si.ceilz, si.florz);
  }
  return slab;
}

static void nc_say_gl_route_closed(void) {
  static bool said = false;
  if (said) return;
  said = true;
  ek_log("nocull frustum: THE GL ROUTE IS CLOSED AND THE PER-FRAME READ IS "
         "GONE. Pass 1 measured it: Polymost's resizeglcheck leaves "
         "GL_MODELVIEW at identity and sends the view rotation to a shader "
         "uniform (polymost_updaterotmat, polymost.cpp:3089), so the side "
         "planes frustum.h extracts are EYE-SPACE planes about an origin "
         "camera -- a Build-world eye read 26157 and 93908 against a tolerance "
         "of %.0f. The visibility test below uses this port's own projection "
         "(project.h) instead, which is what the item ESP's boxes already "
         "frame AA's items with under this renderer",
         EK_CULL_APEX_TOL);
}

// The projection is taken here, where it is valid: project.h requires the render thread with the GL context current;
// the seam is inside renderDrawRoomsQ16 after resizeglcheck, so GL_VIEWPORT is the 3D view's and the camera is the
// ud.camera* pose camera.cpp substituted for this draw. Once per swept pass.
static void nc_build_view(EkCullSlab slab, NcView* out, NcViewTrace* tr) {
  out->ready = 0;
  out->leftEdge = 0.0f;
  out->rightEdge = 0.0f;
  tr->projOk = 0; tr->fromGL = 0;
  tr->vpx = tr->vpy = tr->vpw = tr->vph = 0;
  tr->leftEdge = 0.0f; tr->rightEdge = 0.0f;
  tr->slabOk = ekm_slab_usable(slab) ? 1 : 0;
  tr->slabLo = slab.lo; tr->slabHi = slab.hi; tr->slabCount = slab.count;

  nc_say_gl_route_closed();

  const bool ok = project_begin(&out->proj, EK_PC_NOCULL);
  if (!ok || !out->proj.valid) {
    if (!s_saidNoProjection) {
      s_saidNoProjection = true;
      ek_log("nocull cull: no projection at the scansector seam -- &ud would "
             "not read, or the viewport is degenerate. Nothing is filtered and "
             "the sweep submits exactly the sectors it did before this filter "
             "existed");
    }
    return;
  }
  tr->projOk = 1;
  tr->fromGL = out->proj.vp.fromGL ? 1 : 0;
  tr->vpx = out->proj.vp.x; tr->vpy = out->proj.vp.y;
  tr->vpw = out->proj.vp.w; tr->vph = out->proj.vp.h;

  // The margin is EK_NOCULL_MARGIN_PX, in screen pixels: frustum.h's EK_CULL_MARGIN is in build units and a world
  // margin subtends a different pixel count at every depth. 1024 px errs toward drawing, as an HRP model stands proud
  // of the flat plane its sprite describes. A judgement a flight may revise.
  tr->leftEdge = out->leftEdge = (float)out->proj.vp.x - EK_NOCULL_MARGIN_PX;
  tr->rightEdge = out->rightEdge =
      (float)(out->proj.vp.x + out->proj.vp.w) + EK_NOCULL_MARGIN_PX;

  // The slab no longer decides anything: each corner bounds its own column through cam_band_fill, so `ready` is the
  // projection alone. (A whole-map slab, 911,360 units tall, skipped 1 sector of 1,259,096.) Still measured and printed
  // for comparison.
  out->ready = 1;
}

// One verdict per sector and several ways to say "draw it": a run that rejects almost nothing must say WHICH refusal it
// kept hitting. The sector's own slope bit is no longer one (cam_band_fill bounds a slope exactly); two narrower
// refusals replace it.
enum {
  NC_DRAW_SPAN = 0,     // shorter than a polygon, or outside wall[]
  NC_DRAW_UNREAD,       // a wall record would not read
  NC_DRAW_NEIGHBOUR,    // a sloped, or past-the-bound, neighbour
  NC_DRAW_NOTEXACT,     // a band that is a flat pair standing in for a slope
  NC_DRAW_BANDUNREAD,   // a sector record the band could not be read from
  NC_DRAW_NOPROJECT,    // a corner behind the eye or inside the near plane
  NC_DRAW_INSIDE,       // on screen, or straddling an edge
  NC_REJECTED,
  NC_VERDICTS
};

// Two projections per corner: pitch is a rotation AFTER the perspective divide, so screen x depends on height and one z
// is unsound. Projecting the band's two ends bounds the column exactly (oz2 is affine in dz); either end failing keeps
// the sector. The band is unioned with the span's neighbours, the flat pair and EK_NOCULL_SPRITE_Z_MARGIN.
static int nc_sector_verdict(int sectnum, int wallFirst, int wallCount,
                             int slopedEnd, const NcView* vw) {
  if (!ekm_nocull_span_walkable(wallFirst, wallCount, EK_WALL_SLOTS))
    return NC_DRAW_SPAN;

  const int last = wallFirst + wallCount;

  // Sub-pass: the span's neighbour pair, once for all corners. Per corner it is wrong: `wall[w].nextsector` belongs to
  // the wall LEAVING corner w, so that wall's far end would be bounded by the NEXT wall's neighbour, and at a doorway
  // beside a one-sided wall by nothing. No engine call; a second wall walk on nearly every sector.
  EkCullSlab nband = ekm_slab_init();
  int nExact = 1, nOk = 1;
  for (int w = wallFirst; w < last; w++) {
    EkWallCorner c = { 0, 0, 0 };
    const bool haveCorner = wallstate_corner(w, &c);
    if (!haveCorner) return NC_DRAW_UNREAD;
    const int nsSloped = (c.nextsector >= 0 && c.nextsector < slopedEnd)
                             ? (int)s_cullSloped[c.nextsector] : 1;
    if (ekm_cull_neighbour_blocks(c.nextsector, slopedEnd, nsSloped))
      return NC_DRAW_NEIGHBOUR;
    if (c.nextsector < 0) continue;          // one-sided: nothing to fold

    FreecamBand bandNext = { 0, 0, 0, 0 };
    EkSectorInfo siNext = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const bool nextOk =
        cam_band_fill(c.nextsector, c.x, c.y, &bandNext, &siNext);
    nband = ekm_slab_add(nband, bandNext.ceilz, bandNext.florz);
    if (!nextOk) { nOk = 0; break; }
    if (!bandNext.exact) { nExact = 0; break; }
  }
  const int hasNeighbour = nband.count > 0 ? 1 : 0;

  int corners = 0, outL = 0, outR = 0;
  for (int w = wallFirst; w < last; w++) {
    EkWallCorner c = { 0, 0, 0 };
    const bool haveCorner = wallstate_corner(w, &c);
    if (!haveCorner) return NC_DRAW_UNREAD;

    FreecamBand bandHere = { 0, 0, 0, 0 };
    EkSectorInfo siHere = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const bool hereOk = cam_band_fill(sectnum, c.x, c.y, &bandHere, &siHere);

    int loZ = 0, hiZ = 0;
    const int bandV = ekm_nocull_corner_band(
        bandHere.ceilz, bandHere.florz, bandHere.exact, hereOk ? 1 : 0,
        siHere.ceilz, siHere.florz, nband.lo, nband.hi, nExact, nOk,
        hasNeighbour, &loZ, &hiZ);
    // Not exact means no bound, so the sector is kept (camera.h:279): the pair is a flat stand-in for a slope the
    // engine could not be asked about.
    if (bandV == EK_NOCULL_BAND_UNREADABLE) return NC_DRAW_BANDUNREAD;
    if (bandV == EK_NOCULL_BAND_NOT_EXACT) return NC_DRAW_NOTEXACT;

    const EkmVec3 relLo = project_to_build(vw->proj, c.x, c.y, loZ);
    const EkmVec3 relHi = project_to_build(vw->proj, c.x, c.y, hiZ);
    float sx[2] = { 0.0f, 0.0f };
    float syLo = 0.0f, syHi = 0.0f;
    int ok[2] = { 0, 0 };
    const bool okLo = project_build_point(vw->proj, relLo, &sx[0], &syLo);
    const bool okHi = project_build_point(vw->proj, relHi, &sx[1], &syHi);
    ok[0] = okLo ? 1 : 0;
    ok[1] = okHi ? 1 : 0;

    const int v = ekm_nocull_span_verdict(sx, ok, 2, vw->leftEdge,
                                          vw->rightEdge);
    if (!okLo || !okHi) return NC_DRAW_NOPROJECT;
    if (v == EK_NOCULL_SPAN_LEFT) outL++;
    else if (v == EK_NOCULL_SPAN_RIGHT) outR++;
    corners++;
    if (outL != corners && outR != corners) return NC_DRAW_INSIDE;
  }
  return ekm_cull_reject(corners, outL, outR, 0, 1) ? NC_REJECTED
                                                    : NC_DRAW_INSIDE;
}

// The last SWEPT pass's measurement, carried to the once-a-second print. Every swept pass, not only a full one: on the
// map this exists to explain every pass is budget-stopped.
static NcViewTrace s_cullTrace;
static bool s_cullHaveTrace = false;
static int s_cullSwept = 0;      // sectors the sweep actually submitted
static int s_cullExamined = 0;   // candidates the filter got to judge
static int s_cullVerdict[NC_VERDICTS];
static int s_cullFirst[8];
static int s_cullListed = 0;
// An independent count of readable sectors the walk reached, counted where the loop learns a sector IS readable, not
// where it files a verdict, so the tally can disagree with itself. Meaningful only when the pass filtered.
static int s_cullWalked = 0;
static unsigned s_cullExaminedTotal = 0;
static unsigned s_cullRejectTotal = 0;
// Kept-but-not-admitted, cumulative: in Nearest mode the gather tallies every unscanned candidate regardless of where
// the admit loop's budget stop lands, so the gap is not always one.
static unsigned s_cullUnadmittedTotal = 0;

static unsigned s_lastDiagMs = 0;
static bool s_diagFirst = true;

static bool nc_diag_due(void) {
  const unsigned now = wallclock_ms();
  if (!s_diagFirst && (now - s_lastDiagMs) < 1000u) return false;
  s_diagFirst = false;
  s_lastDiagMs = now;
  return true;
}

// The detour. The original runs FIRST, UNCONDITIONALLY and on every path: it is the seed call the engine makes every
// frame and everything after is pure addition. Nothing here writes engine memory. glDepthFunc is OpenGL 1.1, exported
// from opengl32.dll like camera.cpp's glClear: no loader; a failure logs once and the spike is simply absent.
typedef void(WINAPI* GlDepthFuncFn)(unsigned int);
static GlDepthFuncFn s_glDepthFunc = NULL;
static bool s_depthTried = false;
static bool s_saidDepth = false;

static void nocull_spike_depth_test(void) {
  if (!s_depthTried) {
    s_depthTried = true;
    const HMODULE gl = GetModuleHandleA("opengl32.dll");
    s_glDepthFunc = gl ? (GlDepthFuncFn)GetProcAddress(gl, "glDepthFunc") : NULL;
    if (!s_glDepthFunc)
      ek_log("nocull: no glDepthFunc, so the depth-test spike cannot run; "
             "everything else keeps working");
  }
  if (!s_glDepthFunc) return;
  s_glDepthFunc(0x0203u);                 // GL_LEQUAL, against the engine's GL_ALWAYS
  if (g_cfg.debuglog != 0 && !s_saidDepth) {
    s_saidDepth = true;
    ek_log("nocull SPIKE: the world pass now runs GL_LEQUAL where the engine "
           "set GL_ALWAYS (polymost.cpp:6867). With the claims still on this "
           "should look IDENTICAL -- any visible change kills the premise the "
           "cutaway design rests on");
  }
}

// Per-pass burst log: one record per armed drawrooms pass, closed at the NEXT seed entry (drawalls, the domost thunk
// and cutpaint run after this hook returns, so a pass's counts are complete only then). Render thread only. Idle until
// a burst is due.
struct NcBurstSnap {
  unsigned drawalls, maskwall;
  unsigned domostCalls, domostGated, domostPainted, domostForwarded, domostHole;
  unsigned flatPainted, flatNoFootprint, flatNoSector, flatNoProj;
  unsigned wallPainted, wallInert, wallNoQuad, wallDropTilt, wallDropWind;
  unsigned wallTallRefused;
};

struct NcBurstRow {
  unsigned frame;
  int passIdx;
  const char* kind;
  int nodraw;
  unsigned swept;
  unsigned fpSum, fpXor;
  NcBurstSnap delta;
};

static bool s_burstActive = false;
static bool s_burstStarted = false;      // has a burst ever started this session
static unsigned s_burstLastStartMs = 0;
static int s_burstFilled = 0;            // rows completed in the CURRENT burst
static NcBurstRow s_burstRows[EK_NOCULL_BURST_SIZE];

static bool s_burstPendingReady = false;
static bool s_burstPendingWasActive = false;
static NcBurstRow s_burstPending;
static NcBurstSnap s_burstSnapAtSeed;

static unsigned s_burstFrameSeen = 0xFFFFFFFFu;
static int s_burstPassIdx = -1;

static int s_passKind = EK_NOCULL_KIND_OTHER;
int nocull_pass_kind(void) { return s_passKind; }

// The counter pair as one validated view per G_DrawRooms: one VirtualQuery over both words, never one per call. Which
// word is numbunches stays undecided; ekm_perf_bunches takes the smaller.
static unsigned s_bvFrame = 0xFFFFFFFFu;
static volatile const short* s_bvA = NULL;
static volatile const short* s_bvB = NULL;
static bool s_bvSaid = false;

int nocull_bunches_now(void) {
  const unsigned frame = camera_frame_number();
  if (frame != s_bvFrame) {
    s_bvFrame = frame;
    s_bvA = s_bvB = NULL;
    const unsigned char* const base = s_base;
    const unsigned a = nocull_counter_a_rva();
    const unsigned b = nocull_counter_b_rva();
    bool located = false;
    if (base != NULL && a != 0 && b != 0) {
      located = true;
      const unsigned lo = a < b ? a : b;
      const unsigned hi = a < b ? b : a;
      if (nc_readable(base + lo, (size_t)(hi - lo) + sizeof(short))) {
        s_bvA = (volatile const short*)(base + a);
        s_bvB = (volatile const short*)(base + b);
      }
    }
    if ((s_bvA == NULL || s_bvB == NULL) && !s_bvSaid) {
      s_bvSaid = true;
      ek_log("nocull: numscans/numbunches are %s (rva 0x%07X, 0x%07X), so every "
             "`perf: q16 residual` line reads n = 0 and files each pass in [0,64)",
             located ? "located but not readable" : "not located", a, b);
    }
  }
  if (s_bvA == NULL || s_bvB == NULL) return -1;
  return ekm_perf_bunches((int)*s_bvA, (int)*s_bvB);
}

static NcBurstSnap nc_burst_snap(void) {
  NcBurstSnap s;
  s.drawalls = cutaway_drawalls_calls();
  s.maskwall = cutaway_maskwall_calls();
  s.domostCalls = cutaway_domost_calls();
  s.domostGated = cutaway_domost_gated_calls();
  s.domostPainted = cutaway_domost_painted_calls();
  s.domostForwarded = cutaway_domost_forwarded_calls();
  s.domostHole = cutaway_domost_hole_calls();
  s.flatPainted = cutpaint_flat_painted();
  s.flatNoFootprint = cutpaint_flat_no_footprint();
  s.flatNoSector = cutpaint_flat_no_sector();
  s.flatNoProj = cutpaint_flat_no_proj();
  s.wallPainted = cutpaint_wall_painted();
  s.wallInert = cutpaint_wall_inert();
  s.wallNoQuad = cutpaint_wall_no_quad();
  s.wallDropTilt = cutpaint_wall_drop_tilt();
  s.wallDropWind = cutpaint_wall_drop_wind();
  s.wallTallRefused = cutpaint_wall_tall_refused();
  return s;
}

// The header line, once per burst: the settings in force for all 16 rows, so a mid-burst change is visible as which
// header a row falls under.
static void nc_burst_log_header(void) {
  const bool trorLocated =
      ekm_tror_support_located(cutaway_nodraw_rva() != 0,
                               camera_tror_base_located());
  ek_log("nocull burst: starting %d pass(es) -- TROR support %s (mode %s), "
         "sweep passes %s, sweep order %s, budget mode %s",
         EK_NOCULL_BURST_SIZE, trorLocated ? "located" : "NOT located",
         freecam_tror_mode_name(g_cfg.freecamtrornodraw),
         freecam_passes_name(g_cfg.freecamsweeppasses),
         ekm_nocull_sweeporder_name(g_cfg.freecamsweeporder),
         ekm_nocull_budget_mode_name(
             ekm_nocull_budget_mode_clamped(g_cfg.freecamsweepbudget)));
}

static void nc_burst_flush(void) {
  nc_burst_log_header();
  for (int i = 0; i < s_burstFilled; i++) {
    const NcBurstRow& r = s_burstRows[i];
    const NcBurstSnap& d = r.delta;
    ek_log("nocull burst %d/%d: frame %u pass %d kind %s nodraw %d swept %u "
           "fp %08X/%08X drawalls %u maskwall %u domost %u (gated %u painted "
           "%u forwarded %u hole %u) flats painted %u noFootprint %u "
           "noSector %u noProj %u walls painted %u inert %u noQuad %u "
           "dropTilt %u dropWind %u tallRefused %u",
           i + 1, EK_NOCULL_BURST_SIZE, r.frame, r.passIdx, r.kind, r.nodraw,
           r.swept, r.fpSum, r.fpXor, d.drawalls, d.maskwall, d.domostCalls,
           d.domostGated, d.domostPainted, d.domostForwarded, d.domostHole,
           d.flatPainted, d.flatNoFootprint, d.flatNoSector, d.flatNoProj,
           d.wallPainted, d.wallInert, d.wallNoQuad, d.wallDropTilt,
           d.wallDropWind, d.wallTallRefused);
  }
  s_burstActive = false;
  s_burstFilled = 0;
}

// Close the previous pass at the top of every seed entry, before anything else touches the counters it reads, so the
// snapshot is "as of this seed".
static void nc_burst_close_pending(const NcBurstSnap& nowSnap) {
  if (!s_burstPendingReady) return;
  s_burstPendingReady = false;
  if (!s_burstPendingWasActive) return;  // that burst already ended, or never started
  NcBurstRow row = s_burstPending;
  const NcBurstSnap& was = s_burstSnapAtSeed;
  row.delta.drawalls = nowSnap.drawalls - was.drawalls;
  row.delta.maskwall = nowSnap.maskwall - was.maskwall;
  row.delta.domostCalls = nowSnap.domostCalls - was.domostCalls;
  row.delta.domostGated = nowSnap.domostGated - was.domostGated;
  row.delta.domostPainted = nowSnap.domostPainted - was.domostPainted;
  row.delta.domostForwarded = nowSnap.domostForwarded - was.domostForwarded;
  row.delta.domostHole = nowSnap.domostHole - was.domostHole;
  row.delta.flatPainted = nowSnap.flatPainted - was.flatPainted;
  row.delta.flatNoFootprint = nowSnap.flatNoFootprint - was.flatNoFootprint;
  row.delta.flatNoSector = nowSnap.flatNoSector - was.flatNoSector;
  row.delta.flatNoProj = nowSnap.flatNoProj - was.flatNoProj;
  row.delta.wallPainted = nowSnap.wallPainted - was.wallPainted;
  row.delta.wallInert = nowSnap.wallInert - was.wallInert;
  row.delta.wallNoQuad = nowSnap.wallNoQuad - was.wallNoQuad;
  row.delta.wallDropTilt = nowSnap.wallDropTilt - was.wallDropTilt;
  row.delta.wallDropWind = nowSnap.wallDropWind - was.wallDropWind;
  row.delta.wallTallRefused = nowSnap.wallTallRefused - was.wallTallRefused;

  if (s_burstActive && ekm_nocull_burst_room(s_burstFilled)) {
    s_burstRows[s_burstFilled++] = row;
    if (ekm_nocull_burst_should_flush(s_burstFilled)) nc_burst_flush();
  }
}

static void nc_burst_open_pending(unsigned frameNow, const char* kind,
                                  int nodraw, unsigned swept, unsigned fpSum,
                                  unsigned fpXor) {
  if (frameNow != s_burstFrameSeen) { s_burstFrameSeen = frameNow; s_burstPassIdx = 0; }
  else s_burstPassIdx++;
  s_burstPending.frame = frameNow;
  s_burstPending.passIdx = s_burstPassIdx;
  s_burstPending.kind = kind;
  s_burstPending.nodraw = nodraw;
  s_burstPending.swept = swept;
  s_burstPending.fpSum = fpSum;
  s_burstPending.fpXor = fpXor;
  s_burstPendingWasActive = s_burstActive;
  s_burstPendingReady = true;
}

static void nc_scan_sector_body(int sectnum) {
  ScanSectorFn const orig = s_origScanSector;
  if (!orig) {
    if (!s_saidNoOrig) {
      s_saidNoOrig = true;
      ek_log("nocull: the scansector thunk ran with no original to forward to, "
             "which nothing in this build can produce -- please report it. The "
             "frame draws no world while this lasts");
    }
    return;
  }

  // The engine first, and unguarded on purpose (as camera.cpp and maskscan.cpp): these are the engine's own arguments
  // on its own call, so a fault is one the game would take without the mod, and a __try would turn a reportable crash
  // into a frame nobody drew.
  orig(sectnum);

  // Mirror term: under forced Polymost a mirror DOES re-enter renderDrawRoomsQ16, but camera_render_armed() is raised
  // around one substituted call and lowered on entry to every other, so a mirror pass reads false; YAX per-level passes
  // take polymost.cpp:7073, not the seed (:7078). On a TROR map it is also raised for the base level, swept on purpose.
  const bool inMirror = false;

  s_seedEntries++;
  s_passKind = EK_NOCULL_KIND_OTHER;

  // Burst log: close the previous pass, then maybe start a burst. Snapshotted as early as this seam allows (after
  // orig(), before anything touches drawalls, the domost thunk or cutpaint's counts), so it is "as of this seed entry"
  // and the baseline for the pass starting now. Only the SCHEDULE decision stays idle until due.
  {
    const NcBurstSnap nowSnap = nc_burst_snap();
    nc_burst_close_pending(nowSnap);
    s_burstSnapAtSeed = nowSnap;
  }
  if (!s_burstActive && g_cfg.debuglog != 0 && camera_render_armed()) {
    const unsigned nowMs = wallclock_ms();
    if (ekm_nocull_burst_due(nowMs, s_burstLastStartMs, !s_burstStarted)) {
      s_burstActive = true;
      s_burstStarted = true;
      s_burstLastStartMs = nowMs;
      s_burstFilled = 0;
    }
  }

  {
    const unsigned allsNow = cutaway_drawalls_calls();
    if (s_allsSeeded) {
      if (s_prevSeedSwept) {
        s_allsLast = allsNow - s_allsAtSeed;
        if (s_allsLast > s_allsMax) s_allsMax = s_allsLast;
        if (s_prevSeedWasExtra) s_allsLastExtra = (int)s_allsLast;
        else s_allsLastMain = (int)s_allsLast;
      }
    }
    s_allsAtSeed = allsNow;
    s_allsSeeded = true;
    s_prevSeedSwept = false;
  }

  // The cutaway's frame reset, ABOVE THE GATE on purpose: this seam and the paint share a four-fact gate, so "the paint
  // cannot run without the reset" holds only until one edit breaks it. Resetting on every drawrooms costs two memsets
  // and removes the argument (dollhouse.cpp:181 does the same).
  cutpaint_frame_begin();

  // The masked-wall report is above this gate on purpose: the thunk's count gate is camera_active() alone (cutaway.h),
  // not this function's four facts, so a report behind nocull_should_scan printed nothing whenever freecamwholemap was
  // off. Likewise the tall-wall and degenerate-quad reports below.
  cutaway_maskwall_report();

  cutaway_engine_tall_report();

  cutaway_degenerate_report();

  if (!nocull_should_scan(g_cfg.freecamwholemap != 0, camera_active(),
                          camera_render_armed(), inMirror)) {
    // The extra pass's trigger drops only when the sweep is OFF, not when merely unarmed: a mirror pass reads unarmed
    // every frame it is visible and would starve the extra pass.
    if (g_cfg.freecamwholemap == 0 || !camera_active()) {
      s_extraHold = ekm_nocull_hold_idle();
      InterlockedExchange(&s_lastMainStopped, 0);
      InterlockedExchange64(&s_lastMainReach, -1);
    }
    // Unarmed, still worth a row: the sweep gate refused this seed entry but the frame still had a pass here and the
    // counter deltas are real.
    if (s_burstActive)
      nc_burst_open_pending(camera_frame_number(), "unarmed",
                            cutaway_nodraw_value(), 0, 0, 0);
    return;
  }

  // Load-bearing: the cutaway paints overlapping footprints and ONLY the depth test sorts them; polymost.cpp:6867 sets
  // GL_ALWAYS (restored at :7168), so without this override the result is last-paint-wins. Set behind
  // buildgl_setDepthFunc's cache (glbuild.cpp:95), which the :7168 call then corrects.
  nocull_spike_depth_test();

  // How big is the span list here? A read and a log, borrowing the only point in the frame where vsp[] is pristine:
  // polymost_drawrooms builds it at :6986 and reaches this seed call at :7078 with nothing between touching it. Placed
  // before the sweep-dead check; it touches nothing of the sweep's state.
  cutaway_measure_spans();


  if (s_sweepDead) {
    s_extraHold = ekm_nocull_hold_idle();
    InterlockedExchange(&s_lastMainStopped, 0);
    InterlockedExchange64(&s_lastMainReach, -1);
    return;
  }

  const unsigned char* const base = s_base;
  if (base == NULL) return;

  const unsigned gotRva = nocull_gotsector_rva();
  const int gotBytes = nocull_gotsector_bytes();
  const unsigned aRva = nocull_counter_a_rva();
  const unsigned bRva = nocull_counter_b_rva();
  if (gotRva == 0 || gotBytes <= 0 || aRva == 0 || bRva == 0) return;

  const unsigned char* const gotsector = base + gotRva;
  // int16_t in the engine (engine.cpp:1549), widened before the arithmetic so the budget guard cannot overflow.
  // Volatile because the engine writes these while the sweep runs and the budget re-reads them each iteration; the
  // opaque orig() call is barrier enough today, but volatile removes a silent dependency on a compiler flag.
  volatile const short* const counterA = (volatile const short*)(base + aRva);
  volatile const short* const counterB = (volatile const short*)(base + bRva);

  // Re-validated here, not trusted from the publish: the locate ran on another thread, possibly minutes ago. Three
  // VirtualQuery calls, only on a frame the gate let through.
  if (!nc_readable(gotsector, (size_t)gotBytes) ||
      !nc_readable((const void*)counterA, sizeof(short)) ||
      !nc_readable((const void*)counterB, sizeof(short))) {
    s_sweepDead = true;
    if (!s_saidNoRead) {
      s_saidNoRead = true;
      ek_log("nocull: gotsector (rva 0x%07X, %d bytes) or a counter word is no "
             "longer readable; the sweep gives up for this session and the "
             "engine's own seed keeps drawing", gotRva, gotBytes);
    }
    nocull_mark_unavailable();
    return;
  }

  // sectorborder[0]: the argument polymost_scansector actually reads on this build. Zero when the locate refused, and
  // then the loop passes the register argument alone, as before. Writable-checked: the one address this file stores
  // into.
  const unsigned sbRva = nocull_sectorborder_rva();
  volatile short* const sectorborder =
      (sbRva != 0 && nc_writable(base + sbRva, sizeof(short)))
          ? (volatile short*)(void*)(unsigned char*)(base + sbRva)
          : NULL;
  if (sectorborder == NULL && !s_saidNoBorder) {
    s_saidNoBorder = true;
    ek_log("nocull: sectorborder[0] is unavailable (rva 0x%07X), so each swept "
           "sector is handed over in the register argument alone -- which this "
           "build's polymost_scansector discards. The sweep runs exactly as it "
           "did before, and the engine's own seed keeps drawing", sbRva);
  }

  // Gotsector save/restore seam (1 of 2): if EDuke32's room-over-room passes are to keep working on the 44 AA maps with
  // real TROR, the 512 bytes at `gotsector` are copied here (after the seed call, before the loop) and camera.cpp's
  // seam (2 of 2) copies them back after the draw. Deliberately empty.

  const int numscansEntry = (int)*counterA;
  const int numbunchesEntry = (int)*counterB;

  // This pass's own sweep time, seed-hook entry to after the admit loop (gather, sort, scansector calls), so it can be
  // told apart from the extra pass's share of the `engine` zone.
  const long long sweepStart = nc_now_us();
  perf_zone_begin(EK_PZ_SWEEP_POLY);

  // The player's choice of how much free room the sweep may spend (nocull.h). budgetCeilingForLog is the same quantity
  // in both modes: the ABSOLUTE ceiling on nowMax the mode enforces this frame (Full's floor itself; Reference's
  // entryMax + its reserve, the bound nocull_budget_allows tests), so the two modes' logged numbers compare.
  const int budgetMode = ekm_nocull_budget_mode_clamped(g_cfg.freecamsweepbudget);
  const int budgetFloor = EK_NOCULL_MAXWALLSB - EK_NOCULL_FLOOR_MARGIN;
  const int budgetEntryMax =
      numscansEntry > numbunchesEntry ? numscansEntry : numbunchesEntry;
  const int budgetCeilingForLog =
      budgetMode == EK_NOCULL_BUDGET_FULL
          ? budgetFloor
          : budgetEntryMax + (EK_NOCULL_MAXWALLSB - budgetEntryMax) / 2;

  // The SMALLER of the live count and what the located bitmap can describe: trusting numsectors alone reads past
  // gotsector where the two disagree.
  const int bound = ekm_nocull_sector_bound(gotBytes, mapextent_numsectors());

  // Built before the loop, consulted inside it: the slab and sloped map are the two terms a per-sector test cannot get
  // one sector at a time. s_cullSloped is indexed by a NEIGHBOUR's sectnum, so its own array bound, not `bound`,
  // decides how far the map is filled; a neighbour past it blocks, which draws.
  const int slopedEnd = bound < EK_MAXSECTORS ? bound : EK_MAXSECTORS;
  const EkCullSlab slab = nc_scan_slab(slopedEnd);
  NcView view;
  NcViewTrace trace;
  nc_build_view(slab, &view, &trace);
  int verdicts[NC_VERDICTS] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  int firstRejected[8];
  int listed = 0;
  int examined = 0;
  int walked = 0;

  unsigned swept = 0;
  bool stopped = false;
  int from = -1;

  unsigned fpSum = 0, fpXor = 0;

  // Per-call delta within this sweep: `after == 326 * seed` on a full sweep reads as "every call adds the seed's
  // share", but four analytic answers were wrong, so the step is measured per call.
  int firstBunchDelta[8];
  int firstBunchCount = 0;
  int bunchZero = 0, bunchMin = 0, bunchMax = 0;
  bool haveBunchDelta = false;
  int scanZero = 0, scanMin = 0, scanMax = 0;
  bool haveScanDelta = false;

  // Sweep order: nearest-first needs a real eye, so it replaces index order only on a frame the visibility filter could
  // build one (`view.ready`); otherwise index order, the same "keep drawing, never worse" rule as every refusal. The
  // eye is view.proj.cam.x/y, not camera.cpp's s_sweptEyeX/Y (the blended pose of the retired Polymer cull).
  const int sweepOrderMode = ekm_nocull_sweeporder_clamped(g_cfg.freecamsweeporder);
  const bool nearestFirst =
      sweepOrderMode == EK_NOCULL_SWEEPORDER_NEAREST && view.ready;
  const bool extraPass = camera_extra_pass_active();
  const bool gatherSorted = nearestFirst || (extraPass && view.ready);
  const long long extraCutoff =
      extraPass ? ekm_nocull_extra_cutoff(nocull_last_main_reach()) : -1;
  bool stoppedAtCutoff = false;
  const char* const sweepOrderRanName =
      !gatherSorted ? ekm_nocull_sweeporder_name(EK_NOCULL_SWEEPORDER_INDEX)
      : extraPass   ? "Farthest first (extra pass)"
                    : ekm_nocull_sweeporder_name(EK_NOCULL_SWEEPORDER_NEAREST);
  const long long eyeX = (long long)view.proj.cam.x;
  const long long eyeY = (long long)view.proj.cam.y;

  int candCount = 0;
  int candDropped = 0;
  long long gatherFilterSortUs = 0;
  long long lastAdmittedDist2 = -1;
  bool haveLastAdmitted = false;
  long long firstRefusedDist2 = -1;
  bool haveFirstRefused = false;

  if (gatherSorted) {
    // Gather: same visit order and filter, nothing admitted yet. The tallies are filled here in index order since none
    // depends on the later admit order; only kept candidates are pushed (a rejected sector is excluded from the sort as
    // from admission).
    const long long gatherStart = nc_now_us();
    int gfrom = -1;
    for (;;) {
      const int s = nocull_next_unscanned(gotsector, bound, gfrom);
      if (s < 0) break;
      gfrom = s;

      EkSectorInfo si;
      const bool haveSector = sectorstate_info(s, &si);
      if (!haveSector) {
        s_unreadableSectors++;
        continue;
      }
      walked++;

      const int v = nc_sector_verdict(s, si.wallptr, si.wallnum, slopedEnd,
                                      &view);
      verdicts[v]++;
      examined++;
      if (v == NC_REJECTED) {
        if (listed < 8) firstRejected[listed++] = s;
        continue;
      }

      // The box is read the same guarded way as nc_sector_verdict's span walk. An unwalkable span or unreadable corner
      // gets distance 0, the "cannot bound it, so keep it" bias: an unreadable candidate sorts FIRST, not behind a
      // sentinel that could sort it last forever.
      long long dist2 = 0;
      if (ekm_nocull_span_walkable(si.wallptr, si.wallnum, EK_WALL_SLOTS)) {
        const int last = si.wallptr + si.wallnum;
        bool haveBox = false;
        long long minX = 0, maxX = 0, minY = 0, maxY = 0;
        for (int w = si.wallptr; w < last; w++) {
          EkWallCorner c = { 0, 0, 0 };
          if (!wallstate_corner(w, &c)) { haveBox = false; break; }
          if (!haveBox) {
            minX = maxX = c.x; minY = maxY = c.y; haveBox = true;
          } else {
            if (c.x < minX) minX = c.x;
            if (c.x > maxX) maxX = c.x;
            if (c.y < minY) minY = c.y;
            if (c.y > maxY) maxY = c.y;
          }
        }
        if (haveBox)
          dist2 = ekm_nocull_bbox_dist2(eyeX, eyeY, minX, maxX, minY, maxY);
      }

      // Bounds: the array is EK_MAXSECTORS and candidates never exceed it; `bound` may answer past it on a bigger
      // bitmap, so a candidate past it is dropped but COUNTED, not silent.
      if (candCount < EK_MAXSECTORS) {
        s_sweepCandidates[candCount].sector = s;
        s_sweepCandidates[candCount].dist2 = dist2;
        candCount++;
      } else {
        candDropped++;
      }
    }

    qsort(s_sweepCandidates, (size_t)candCount, sizeof(NcSweepCandidate),
          extraPass ? ekm_nocull_candidate_cmp_far : ekm_nocull_candidate_cmp);
    const long long gatherEnd = nc_now_us();
    // Spans the whole gather loop, which RUNS THE VISIBILITY FILTER for every candidate as well as building the box and
    // sorting, so it is not comparable to Index mode's untimed inline filter.
    if (gatherStart > 0 && gatherEnd > 0)
      gatherFilterSortUs = gatherEnd - gatherStart;

    for (int i = 0; i < candCount; i++) {
      const int s = s_sweepCandidates[i].sector;

      // The cutoff goes before the budget, and is a break: candidates run farthest first, so every later one is nearer
      // and already the main pass's. Not a budget stop.
      if (!ekm_nocull_extra_admits(s_sweepCandidates[i].dist2, extraCutoff)) {
        stoppedAtCutoff = true;
        break;
      }

      // Re-check gotsector before each call: the engine's own BFS continuation inside orig() can mark a sector already
      // listed as a candidate, and re-admitting it would re-submit a sector within one pass.
      if ((gotsector[s >> 3] & (unsigned char)(1 << (s & 7))) != 0) continue;

      EkSectorInfo si;
      if (!sectorstate_info(s, &si)) continue;  // already tallied in gather

      const int numscansNow = (int)*counterA;
      const int numbunchesNow = (int)*counterB;
      if (!nocull_budget_allows(numscansEntry, numbunchesEntry, numscansNow,
                                numbunchesNow, si.wallnum, budgetMode)) {
        stopped = true;
        firstRefusedDist2 = s_sweepCandidates[i].dist2;
        haveFirstRefused = true;
        break;
      }

      if (s >= 0 && s < EK_MAXSECTORS) {
        s_submits[s]++;
        ekm_nocull_fp_add((unsigned)s, &fpSum, &fpXor);
      }
      if (sectorborder) *sectorborder = (short)s;
      orig(s);
      swept++;
      lastAdmittedDist2 = s_sweepCandidates[i].dist2;
      haveLastAdmitted = true;

      const int bunchDelta = (int)*counterB - numbunchesNow;
      const int scanDelta = (int)*counterA - numscansNow;
      if (firstBunchCount < 8) firstBunchDelta[firstBunchCount] = bunchDelta;
      firstBunchCount++;
      if (bunchDelta == 0) bunchZero++;
      if (!haveBunchDelta || bunchDelta < bunchMin) bunchMin = bunchDelta;
      if (!haveBunchDelta || bunchDelta > bunchMax) bunchMax = bunchDelta;
      haveBunchDelta = true;
      if (scanDelta == 0) scanZero++;
      if (!haveScanDelta || scanDelta < scanMin) scanMin = scanDelta;
      if (!haveScanDelta || scanDelta > scanMax) scanMax = scanDelta;
      haveScanDelta = true;
    }
  } else {
    for (;;) {
      const int s = nocull_next_unscanned(gotsector, bound, from);
      if (s < 0) break;

      EkSectorInfo si;
      const bool haveSector = sectorstate_info(s, &si);
      if (!haveSector) {
        s_unreadableSectors++;
        from = s;
        continue;
      }
      walked++;

      // The visibility filter goes BEFORE the budget, which is the whole fix: a sector the camera cannot see must not
      // spend a scan the sector behind it needs. And `continue`, never `break`: this is a fact about ONE sector's
      // footprint; a break would reintroduce index-order truncation.
      if (view.ready) {
        const int v = nc_sector_verdict(s, si.wallptr, si.wallnum, slopedEnd,
                                        &view);
        verdicts[v]++;
        examined++;
        if (v == NC_REJECTED) {
          if (listed < 8) firstRejected[listed++] = s;
          from = s;
          continue;
        }
      }

      const int numscansNow = (int)*counterA;
      const int numbunchesNow = (int)*counterB;
      if (!nocull_budget_allows(numscansEntry, numbunchesEntry, numscansNow,
                                numbunchesNow, si.wallnum, budgetMode)) {
        // Break, not continue: a smaller sector would keep spending a budget that has already said no; the refusal is
        // about the counters.
        stopped = true;
        break;
      }

      // The decisive count: per-sector submissions, so the flood's sector is named rather than inferred. ~1 per frame
      // for sector 177 means the sweep is innocent; ~158 means the loop is broken.
      if (s >= 0 && s < EK_MAXSECTORS) {
        s_submits[s]++;
        ekm_nocull_fp_add((unsigned)s, &fpSum, &fpXor);
      }

      // The argument the engine actually reads, and the register one STAYS: polymost_scansector discards its parameter
      // on this build (IPA-SRA moved it into this global), so without the store every call re-scans whatever
      // sectorborder[0] held. The engine's own store at its own call site; the VALUE is ours.
      if (sectorborder) *sectorborder = (short)s;
      orig(s);
      swept++;
      from = s;

      const int bunchDelta = (int)*counterB - numbunchesNow;
      const int scanDelta = (int)*counterA - numscansNow;
      if (firstBunchCount < 8) firstBunchDelta[firstBunchCount] = bunchDelta;
      firstBunchCount++;
      if (bunchDelta == 0) bunchZero++;
      if (!haveBunchDelta || bunchDelta < bunchMin) bunchMin = bunchDelta;
      if (!haveBunchDelta || bunchDelta > bunchMax) bunchMax = bunchDelta;
      haveBunchDelta = true;
      if (scanDelta == 0) scanZero++;
      if (!haveScanDelta || scanDelta < scanMin) scanMin = scanDelta;
      if (!haveScanDelta || scanDelta > scanMax) scanMax = scanDelta;
      haveScanDelta = true;
    }
  }

  const long long sweepEnd = nc_now_us();
  perf_zone_end(EK_PZ_SWEEP_POLY, (int)swept);
  const long long sweepUs =
      (sweepStart > 0 && sweepEnd > 0) ? sweepEnd - sweepStart : 0;

  // A FULL sweep is one the budget did not cut short: only that population can stand in for the real distribution at
  // the once-a-second print. A cutoff stop is short on purpose, so no more "full" than a budget stop.
  if (!stopped && !stoppedAtCutoff && swept > 0) {
    s_haveFullDelta = true;
    s_deltaCallCount = (int)swept;
    s_deltaFirstCount = firstBunchCount < 8 ? firstBunchCount : 8;
    for (int i = 0; i < s_deltaFirstCount; i++)
      s_deltaFirstBunch[i] = firstBunchDelta[i];
    s_deltaBunchZero = bunchZero;
    s_deltaBunchMin = bunchMin;
    s_deltaBunchMax = bunchMax;
    s_deltaScanZero = scanZero;
    s_deltaScanMin = scanMin;
    s_deltaScanMax = scanMax;
  }

  if (swept > 0 || examined > 0) {
    s_cullTrace = trace;
    s_cullHaveTrace = true;
    s_cullSwept = (int)swept;
    s_cullExamined = examined;
    s_cullWalked = walked;
    for (int i = 0; i < NC_VERDICTS; i++) s_cullVerdict[i] = verdicts[i];
    s_cullListed = listed;
    for (int i = 0; i < listed; i++) s_cullFirst[i] = firstRejected[i];
    s_cullExaminedTotal += (unsigned)examined;
    s_cullRejectTotal += (unsigned)verdicts[NC_REJECTED];
    // Kept but not admitted this frame, cumulative: in Index mode that stops where the budget stopped (0 or 1), in
    // Nearest mode the gather tallies every unscanned candidate, so the gap varies and can be large.
    int drawnThisFrame = 0;
    for (int i = 0; i < NC_VERDICTS; i++)
      if (i != NC_REJECTED) drawnThisFrame += verdicts[i];
    const int unadmittedThisFrame = drawnThisFrame - (int)swept;
    if (unadmittedThisFrame > 0)
      s_cullUnadmittedTotal += (unsigned)unadmittedThisFrame;
  }

  if (swept > 0) {
    s_sweptFrames++;
    s_sweptSectors += swept;
    s_prevSeedSwept = true;
    s_prevSeedWasExtra = extraPass;
  }
  if (stopped) {
    if (extraPass) s_budgetStopsExtra++;
    else s_budgetStops++;
  }
  if (stoppedAtCutoff) s_cutoffStopsExtra++;

  // Burst log: kind, g_nodraw and the sweep's count and fingerprint are known now; PREPARE (g_nodraw armed) takes
  // priority over extra/main. The classifier is nocullburst.h's, shared with the drawalls split.
  if (s_burstActive || g_cfg.perfzones != 0) {
    const int ndVal = cutaway_nodraw_value();
    s_passKind = ekm_nocull_pass_kind(ndVal, extraPass);
    if (s_burstActive)
      nc_burst_open_pending(camera_frame_number(),
                            ekm_nocull_pass_kind_name(s_passKind), ndVal, swept,
                            fpSum, fpXor);
  }

  if (!extraPass) {
    const long long reachNew = (stopped && nearestFirst && haveLastAdmitted)
                                    ? lastAdmittedDist2 : -1;
    // Held, not latest-only: a single main pass that happens to complete no longer switches the extra pass off for that
    // frame (nocull.h). s_extraHold is render-thread owned; only the two scalars below cross threads.
    s_extraHold = ekm_nocull_hold_update(s_extraHold, stopped, reachNew);
    InterlockedExchange(&s_lastMainStopped,
                        ekm_nocull_hold_wanted(s_extraHold) ? 1 : 0);
    InterlockedExchange64(&s_lastMainReach,
                          ekm_nocull_hold_wanted(s_extraHold)
                              ? s_extraHold.reach : -1);
  }

  // What the sweep produced: it reaches 325 of 326 sectors every frame yet only ~158 domost calls follow. How many
  // BUNCHES it left behind separates the explanations: hundreds means drawalls is not drawing them (downstream), about
  // a dozen means scansector reaches sectors without producing bunches (upstream).
  const int bunchesAfter = (int)*counterB;
  const int scansAfter = (int)*counterA;
  if (bunchesAfter > s_bunchMax) s_bunchMax = bunchesAfter;

  // The rate against the wall clock, printed whether or not debuglog is on, at most once a second: a row the player has
  // not switched on must not hide the measurement.
  {
    const unsigned long long now = wallclock_ms();
    if (s_lastRateMs == 0) s_lastRateMs = now;
    else if (now - s_lastRateMs >= 1000) {
      const double secs = (double)(now - s_lastRateMs) / 1000.0;
      int topSect = -1; unsigned topCount = 0;
      for (int i = 0; i < EK_MAXSECTORS; i++)
        if (s_submits[i] > topCount) { topCount = s_submits[i]; topSect = i; }
      const double sweepsPerSec = (double)(s_sweptFrames - s_sweepsAtLog) / secs;

      // Drawalls per pass: the sweep leaves 652 bunches and only ~300 domost calls arrive; drawalls sits between and
      // was never counted. A max near 20 puts the loss in the bunch loop (conclusive alone); near 652 points inside
      // drawalls and is conclusive here because E1L1 has no TROR.
      const double sweeps = sweepsPerSec * secs;
      char extraBuf[16];
      char mainBuf[16];
      if (s_allsLastExtra < 0)
        _snprintf_s(extraBuf, sizeof(extraBuf), _TRUNCATE, "none yet");
      else
        _snprintf_s(extraBuf, sizeof(extraBuf), _TRUNCATE, "%d", s_allsLastExtra);
      if (s_allsLastMain < 0)
        _snprintf_s(mainBuf, sizeof(mainBuf), _TRUNCATE, "none yet");
      else
        _snprintf_s(mainBuf, sizeof(mainBuf), _TRUNCATE, "%d", s_allsLastMain);
      ek_log("nocull rate: %.0f seed entr(ies)/s, %.0f sweep(s)/s (one per "
             "drawrooms pass; a D11 extra-pass frame makes two), %.0f "
             "scansector call(s)/s. BUSIEST SECTOR %d submitted %u time(s) in "
             "%.1fs = %.1f per sweep -- ONE per sweep means this sweep is "
             "innocent and the engine reaches that sector the other times; "
             "many per sweep means the loop is at fault. DRAWALLS %u call(s) "
             "in the last swept pass, %u at most in any swept pass, last "
             "extra pass %s, last main pass %s -- whether the hook went in "
             "at all is the HOOKED/NOT hooked line from cutaway_install, not "
             "this number",
             (double)(s_seedEntries - s_seedEntriesAtLog) / secs,
             sweepsPerSec,
             (double)(s_sweptSectors - s_scansAtLog) / secs,
             topSect, topCount, secs,
             sweeps > 0.0 ? (double)topCount / sweeps : 0.0,
             s_allsLast, s_allsMax,
             extraBuf, mainBuf);

      // The per-call delta, printed whether or not debuglog is on: decides whether `after == 326 * seed` means every
      // call adds the seed's share (ALL EQUAL) or an average of different amounts (VARYING). Only a FULL sweep's
      // figures are saved.
      if (!s_haveFullDelta) {
        ek_log("nocull delta: no FULL sweep in this window (every pass was "
               "budget-stopped); nothing to report");
      } else {
        char first[96];
        first[0] = 0;
        for (int i = 0; i < s_deltaFirstCount; i++) {
          char one[16];
          _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%d",
                      i == 0 ? "" : ",", s_deltaFirstBunch[i]);
          strcat_s(first, sizeof(first), one);
        }
        ek_log("nocull delta: FULL sweep of %d call(s). Bunch delta per "
               "call: first %d %s; %d call(s) added zero; min %d, max %d. "
               "Scan delta: min %d, max %d, %d zero. ALL EQUAL means every "
               "call does the seed call's work and the sweep is not "
               "scanning distinct sectors; VARYING means the 326x is an "
               "average and the loss is downstream of the sweep.",
               s_deltaCallCount, s_deltaFirstCount, first, s_deltaBunchZero,
               s_deltaBunchMin, s_deltaBunchMax, s_deltaScanMin,
               s_deltaScanMax, s_deltaScanZero);
      }
      s_haveFullDelta = false;

      // Printed whether or not debuglog is on: a single flight has to answer it. Two lines because ek_log's buffer is
      // 1024 bytes (log.h's ek_log_keep_len records what truncation costs).
      if (!s_cullHaveTrace) {
        ek_log("nocull view: no swept pass in this window, so the visibility "
               "test was never attempted");
      } else {
        const NcViewTrace t = s_cullTrace;
        // The opening word says what is happening: the filter runs only behind view.ready (projOk alone), so printing
        // "FILTERING." unconditionally could sit above "Projection NOT BUILT" and contradict itself.
        ek_log("nocull view: %s Projection %s; viewport %d,%d %dx%d "
               "(%s); screen x must be left of %.0f or right of %.0f for every "
               "corner of a sector before it is skipped -- that is the "
               "viewport plus a margin of %.0f pixels each side, which errs "
               "toward drawing because the HRP draws sprites as MODELS "
               "standing proud of their plane. EACH CORNER IS PROJECTED AT "
               "BOTH ENDS OF ITS OWN BAND -- the sector's true z there, "
               "unioned with its own flat pair and with EVERY neighbour of "
               "the span, then widened by %d z units (%d horizontal) for the "
               "model geometry that stands proud of the room -- because a "
               "pitched camera makes screen x depend on height; the map's "
               "whole slab (%s, %d..%d over %d sector(s)) is printed for "
               "scale only and is no longer what any corner is projected over",
               t.projOk ? "FILTERING." : "NOT FILTERING.",
               t.projOk ? "built" : "NOT BUILT -- nothing is filtered",
               t.vpx, t.vpy, t.vpw, t.vph,
               t.fromGL ? "from GL" : "ASSUMED client rect",
               t.leftEdge, t.rightEdge, EK_NOCULL_MARGIN_PX,
               EK_NOCULL_SPRITE_Z_MARGIN, EK_NOCULL_SPRITE_Z_MARGIN / 16,
               t.slabOk ? "usable" : "NOT usable",
               t.slabLo, t.slabHi, t.slabCount);

        // The count and its enumeration side by side (ekm_nocull_tally_ok): every examined sector got exactly one
        // verdict, so the parts sum to the whole or the instrument measures something else.
        int drawn = 0;
        for (int i = 0; i < NC_VERDICTS; i++)
          if (i != NC_REJECTED) drawn += s_cullVerdict[i];
        // An independently derived total: `s_cullWalked` is counted where the walk learns a sector is readable. It
        // matches the buckets' population only when the pass filtered (view.ready); otherwise `s_cullExamined` (0) is
        // the honest comparison.
        const int tallyBase = t.projOk ? s_cullWalked : s_cullExamined;
        char which[48];
        which[0] = 0;
        for (int i = 0; i < s_cullListed; i++) {
          char one[16];
          _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%d",
                      i == 0 ? "" : ",", s_cullFirst[i]);
          strcat_s(which, sizeof(which), one);
        }
        // Computed, not assumed: `drawn` is every KEEP verdict this pass filed and s_cullSwept what was admitted, so
        // the difference is kept-not-admitted for any reason (a budget stop, or the engine's own scan marking a
        // candidate first). 192: the longest message below is 165.
        char keptMsg[192];
        const int keptNotSubmitted = drawn - s_cullSwept;
        if (keptNotSubmitted > 0) {
          _snprintf_s(keptMsg, sizeof(keptMsg), _TRUNCATE,
                      "Kept exceeds submitted by %d sector(s) not admitted "
                      "this frame (a budget stop, the extra pass's cutoff, or "
                      "the engine's own scan reaching a candidate first).",
                      keptNotSubmitted);
        } else {
          _snprintf_s(keptMsg, sizeof(keptMsg), _TRUNCATE,
                      "The sweep completed, so kept and submitted agree.");
        }
        ek_log("nocull cull: SKIPPED %d of %d sector(s) examined (%.1f%%), and "
               "the sweep then submitted %d; tally %s. First %d skipped: %s. "
               "Kept: %d span shorter than a polygon or outside "
               "wall[], %d with a wall that would not read, %d with a sloped "
               "or unbounded neighbour, %d whose band is NOT EXACT (a flat "
               "pair standing in for a slope -- no bound, so no skip), %d "
               "whose band would not read at all, %d with a corner that would "
               "not project (behind the eye or inside the near plane -- a "
               "KEEP, never a skip), %d on screen or straddling an edge. %s "
               "Cumulative %u skipped of %u examined, %u kept-not-admitted",
               s_cullVerdict[NC_REJECTED], s_cullExamined,
               s_cullExamined > 0
                   ? 100.0 * (double)s_cullVerdict[NC_REJECTED] /
                         (double)s_cullExamined
                   : 0.0,
               s_cullSwept,
               ekm_nocull_tally_ok(tallyBase, s_cullVerdict[NC_REJECTED],
                                   drawn)
                   ? "OK"
                   : "MISMATCH -- the instrument is wrong, not the map",
               s_cullListed, which,
               s_cullVerdict[NC_DRAW_SPAN], s_cullVerdict[NC_DRAW_UNREAD],
               s_cullVerdict[NC_DRAW_NEIGHBOUR],
               s_cullVerdict[NC_DRAW_NOTEXACT],
               s_cullVerdict[NC_DRAW_BANDUNREAD],
               s_cullVerdict[NC_DRAW_NOPROJECT], s_cullVerdict[NC_DRAW_INSIDE],
               keptMsg,
               s_cullRejectTotal, s_cullExaminedTotal, s_cullUnadmittedTotal);
      }
      s_cullHaveTrace = false;

      for (int i = 0; i < EK_MAXSECTORS; i++) s_submits[i] = 0;
      s_seedEntriesAtLog = s_seedEntries;
      s_sweepsAtLog = s_sweptFrames;
      s_scansAtLog = s_sweptSectors;
      s_lastRateMs = now;
    }
  }

  if (g_cfg.debuglog != 0 && swept > 0 && nc_diag_due()) {
    char holdAgeSuffix[48] = "";
    if (!extraPass)
      _snprintf_s(holdAgeSuffix, sizeof(holdAgeSuffix), _TRUNCATE,
                  ", hold age %d of %d", s_extraHold.age, EK_NOCULL_EXTRA_HOLD);
    // Sweep order: the mode that ACTUALLY RAN (a frame with no projection falls back to Index), candidates, how many
    // were dropped for exceeding EK_MAXSECTORS (should be 0), the gather cost (it INCLUDES the visibility filter, so no
    // fair A/B against Index) and the last admitted and first refused distance; -1 is n/a.
    ek_log("nocull sweep: pass %s, %u sector(s) beyond the seed this pass of "
           "%d in bound (budget mode %s, ceiling %d scan(s) on "
           "numscans/numbunches this pass -- the same quantity in both "
           "modes); order %s (as run this pass), %d candidate(s) gathered, %d "
           "dropped (over EK_MAXSECTORS), gather+filter+sort %lld "
           "microsecond(s) (Index mode runs the same filter inline and does "
           "not time it); sweep %lld microsecond(s) (seed-hook entry to after "
           "the admit loop, this pass -- gather, sort and the scansector "
           "calls); last admitted dist2 %lld, first refused dist2 %lld; "
           "cutoff dist2 %lld (-1 = none), stopped at the cutoff %s; "
           "cumulative %u sector(s) over %u swept pass(es) (extra and main "
           "both count), %u main-pass and %u extra-pass budget stop(s), %u "
           "extra-pass cutoff stop(s), %u "
           "unreadable sector record(s); BUNCHES %d after the sweep (was %d "
           "at the seed), scans %d (was %d), worst bunches %d%s",
           extraPass ? "extra" : "main", swept, bound,
           ekm_nocull_budget_mode_name(budgetMode), budgetCeilingForLog,
           sweepOrderRanName, candCount, candDropped,
           gatherFilterSortUs, sweepUs,
           haveLastAdmitted ? lastAdmittedDist2 : -1,
           haveFirstRefused ? firstRefusedDist2 : -1,
           extraCutoff, stoppedAtCutoff ? "yes" : "no",
           s_sweptSectors, s_sweptFrames, s_budgetStops, s_budgetStopsExtra,
           s_cutoffStopsExtra,
           s_unreadableSectors, bunchesAfter, numbunchesEntry, scansAfter,
           numscansEntry, s_bunchMax,
           holdAgeSuffix);
  }
}

// One branch with Frame timings off. On, a second seed inside one Q16 call starts a new engine pass, and the body's
// return reads the bunch count the selection loop starts from (polymost.cpp:7111).
static void EK_ScanSectorThunk(int sectnum) {
  if (g_cfg.perfzones == 0) {
    nc_scan_sector_body(sectnum);
    return;
  }
  camera_q16_seed_entry();
  nc_scan_sector_body(sectnum);
  camera_q16_seed_exit();
}

// Deliberately not wired into any aggregate verification flag: it is a single call site inside renderDrawRoomsQ16, and
// a mismatch says nothing about the camera's bracket, the firing hooks or any cvar. A failure logs once, marks the row
// unavailable and leaves the game running.
void nocull_install(void) {
  static bool tried = false;
  if (tried) return;

  const unsigned char* const base = s_base;
  const unsigned site = nocull_seed_site_rva();
  const unsigned target = nocull_scansector_rva();
  const int verdict = ekm_nocull_install_verdict(base != NULL, nocull_located(),
                                                 nocull_available(), site, target);
  if (verdict == EK_NOCULL_INSTALL_WAIT) return;

  tried = true;

  if (verdict != EK_NOCULL_INSTALL_GO) {
    nocull_mark_unavailable();
    ek_log("nocull: no redirect -- the locate did not reach a usable LOCATED "
           "state (seed site rva 0x%07X, polymost_scansector rva 0x%07X). "
           "\"Draw the whole map\" stays unavailable for this session and "
           "everything else keeps running", site, target);
    return;
  }

  // The recovered original target, read back and named before anything is written: callsite_install refuses a site
  // whose call does not reach `expected`, but cannot say where the call goes, and that number identifies a mislocated
  // site.
  unsigned char org[EKM_CALL_LEN];
  if (!nc_readable(base + site, sizeof(org))) {
    nocull_mark_unavailable();
    ek_log("nocull: the five bytes of the seed call site at rva 0x%07X are not "
           "readable; nothing was written and the row stays unavailable", site);
    return;
  }
  for (int i = 0; i < EKM_CALL_LEN; i++) org[i] = base[site + i];

  if (org[0] != 0xE8) {
    nocull_mark_unavailable();
    ek_log("nocull: the byte at the seed call site rva 0x%07X is %02X, not the "
           "E8 of a `call rel32`; this is not the call site the locate claims. "
           "Refusing, and nothing was written", site, org[0]);
    return;
  }
  unsigned u = 0;
  for (int i = 0; i < 4; i++) u |= (unsigned)org[1 + i] << (i * 8);
  const unsigned recovered = ekm_us_rel32_target(site, (int)u);
  if (recovered != target) {
    nocull_mark_unavailable();
    ek_log("nocull: the seed call site at rva 0x%07X reaches rva 0x%07X, not "
           "the polymost_scansector at 0x%07X the locate named -- MISMATCH, so "
           "this is not the call site we think it is. Refusing, and nothing was "
           "written", site, recovered, target);
    return;
  }

  s_origScanSector = (ScanSectorFn)(const void*)(base + target);
  if (!callsite_install((void*)(base + site), (void*)(base + target),
                        (void*)&EK_ScanSectorThunk, &s_seedPatch)) {
    s_origScanSector = NULL;
    nocull_mark_unavailable();
    ek_log("nocull: the seed scansector call at rva 0x%07X was NOT redirected; "
           "\"Draw the whole map\" stays unavailable for this session and "
           "everything else keeps running", site);
    return;
  }

  ek_log("nocull: the seed scansector call at rva 0x%07X is REDIRECTED -- it "
         "reached rva 0x%07X (recovered and matched), which our thunk now runs "
         "first and then sweeps behind. The row is off by default and the "
         "sweep is inert until the free camera flies with it on", site,
         recovered);
}

void nocull_remove(void) {
  if (s_seedPatch.site == NULL) return;
  if (callsite_remove(&s_seedPatch)) s_origScanSector = NULL;
}
