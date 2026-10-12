// The cutaway's locate: FIND polymost_domost and polymost_drawpoly in the stripped host and log their extents (rules:
// cutaway.h). Hooks nothing, writes no engine memory. Every RVA is a SEARCH RESULT: a source STRING, the one lea
// reaching it, then the containing RUNTIME_FUNCTION. Quoted rvas are evidence. DLL only.

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "log.h"
#include "cutaway.h"
#include "imagescan.h"
#include "hitscan.h"
#include "camerascan.h"
#include "nocull.h"
#include "hook.h"
#include "camera.h"
#include "config.h"
#include "ui/wallclock.h"
#include "cutpaint.h"
#include "gamestate.h"
#include "wallstate.h"
#include "sectorstate.h"
#include "freecam.h"
#include "perf.h"

// Only the head of each anchor is matched: each is the START of a longer printf format string. Uniqueness comes from
// the COUNT, which ekm_cutaway_anchor_ok requires.
enum { CW_DOMOST = 0, CW_DRAWPOLY = 1, CW_DRAWALLS = 2, CW_DRAWROOMS = 3,
       CW_NONPARALLAX = 4, CW_MASKWALL = 5 };

struct CwAnchor { const char* name; const char* text; };

// Measured in the shipped eduke32.exe: literals at rva 0x052B3A8, 0x052B340, 0x052B4BC, 0x0523062; functions at
// 0x01ACC90..0x01AF109, 0x01A90A0..0x01ACC8B, 0x01B0570..0x01B6B4A, 0x01561B0..0x0159ED3. Evidence only.
static const CwAnchor EK_CUTAWAY_ANCHORS[6] = {
  { "polymost_domost",    "polymost_domost(x0:"     },
  { "polymost_drawpoly",  "polymost_drawpoly(dpxy:" },
  { "polymost_drawalls",  "polymost_drawalls("      },
  { "polymost_drawrooms", "polymost_drawrooms()"    },
  // Stage B's own: literal at rva 0x052B440, function 0x01AF110..0x01B02F2, one occurrence and one lea reference (a
  // REAL function; two inlined sites would give two).
  { "polymost_internal_nonparallaxed",
    "polymost_internal_nonparallaxed(n0:" },
  // The masked-wall path: literal at rva 0x052B970, ONE occurrence, ONE lea reference at 0x01BE329, .pdata owner
  // 0x01BE270..0x01BEFBA (0xD4A bytes).
  { "polymost_drawmaskwallinternal",
    "polymost_drawmaskwallinternal(wallIndex:" },
};

// Only a FAULT spends the budget (nocull.cpp's rule): the image does not change, so an absent or ambiguous anchor
// latches at once.
static const int EK_CUTAWAY_ATTEMPTS = 4;

// 0 trying, 1 located, 2 latched off; written on the sampler thread, read by the hooks on the render thread.
static volatile LONG s_state = 0;
static volatile LONG s_attempts = 0;

static volatile LONG s_domostRva = 0;
static volatile LONG s_domostEnd = 0;
static volatile LONG s_drawpolyRva = 0;
static volatile LONG s_drawpolyEnd = 0;
static volatile LONG s_vspRva = 0;
static volatile LONG s_vspSlot = 0;

// polymost_drawrooms' extent for the multipass spike, plus its global and slot; 0 until accepted.
static volatile LONG s_zclearRva = 0;
static volatile LONG s_zclearSlot = 0;

// The backface spike's global, domostpolymethod. Read only.
static volatile LONG s_dpmRva = 0;

// The two horizontal view bounds, xbl and xbr in some order, hence A and B (cutaway.h). Read only.
static volatile LONG s_xbA = 0;
static volatile LONG s_xbB = 0;

// The tilt pair, named by ROLE because the locate earns the names (cutaway.h's three agreeing facts). Read only.
static volatile LONG s_tangC = 0;
static volatile LONG s_tangS = 0;

// polymost_internal_nonparallaxed, whose ninth argument names the sector every flat call belongs to.
static volatile LONG s_nonparRva = 0;
static volatile LONG s_nonparEnd = 0;
// polymost_drawalls, published only so its calls can be COUNTED.
static volatile LONG s_allsRva = 0;
static volatile LONG s_allsEnd = 0;

// polymost_drawmaskwallinternal's extent.
static volatile LONG s_maskRva = 0;
static volatile LONG s_maskEnd = 0;

// g_nodraw (engine.cpp:423). Read only.
static volatile LONG s_nodrawRva = 0;
// Where polymost_drawrooms' `g_nodraw = 1` sits (:6993); the yax_globallev locate reads the compare just before it.
static volatile LONG s_nodrawStoreRva = 0;

// Written on the render thread by cutaway_measure_spans, so interlocked.
static volatile LONG s_liveSpans = 0;

// The "(Unavailable)" latch (cutaway.h): 1 until something refuses, one-way.
static volatile LONG s_available = 1;

static unsigned read_pub(volatile LONG* p) {
  return (unsigned)InterlockedCompareExchange(p, 0, 0);
}

bool cutaway_located(void) { return InterlockedCompareExchange(&s_state, 0, 0) == 1; }
bool cutaway_answered(void) { return InterlockedCompareExchange(&s_state, 0, 0) != 0; }
unsigned cutaway_nodraw_store_rva(void) { return read_pub(&s_nodrawStoreRva); }
unsigned cutaway_domost_rva(void)       { return read_pub(&s_domostRva); }
unsigned cutaway_domost_end_rva(void)   { return read_pub(&s_domostEnd); }
unsigned cutaway_drawpoly_rva(void)     { return read_pub(&s_drawpolyRva); }
unsigned cutaway_drawpoly_end_rva(void) { return read_pub(&s_drawpolyEnd); }
unsigned cutaway_vsp_rva(void)          { return read_pub(&s_vspRva); }
unsigned cutaway_vsp_slot_rva(void)     { return read_pub(&s_vspSlot); }
unsigned cutaway_zclear_rva(void)       { return read_pub(&s_zclearRva); }
unsigned cutaway_zclear_slot_rva(void)  { return read_pub(&s_zclearSlot); }
unsigned cutaway_dpm_rva(void)          { return read_pub(&s_dpmRva); }
unsigned cutaway_xbound_a_rva(void)     { return read_pub(&s_xbA); }
unsigned cutaway_xbound_b_rva(void)     { return read_pub(&s_xbB); }
unsigned cutaway_tang_c_rva(void)       { return read_pub(&s_tangC); }
unsigned cutaway_tang_s_rva(void)       { return read_pub(&s_tangS); }
unsigned cutaway_nonpar_rva(void)       { return read_pub(&s_nonparRva); }
unsigned cutaway_nonpar_end_rva(void)   { return read_pub(&s_nonparEnd); }
unsigned cutaway_drawalls_rva(void)     { return read_pub(&s_allsRva); }
unsigned cutaway_drawalls_end_rva(void) { return read_pub(&s_allsEnd); }
unsigned cutaway_maskwall_rva(void)     { return read_pub(&s_maskRva); }
unsigned cutaway_maskwall_end_rva(void) { return read_pub(&s_maskEnd); }
unsigned cutaway_nodraw_rva(void)       { return read_pub(&s_nodrawRva); }
int cutaway_live_spans(void)            { return (int)read_pub(&s_liveSpans); }

bool cutaway_available(void)        { return read_pub(&s_available) != 0; }
bool cutaway_row_unavailable(void)  { return !cutaway_available(); }
void cutaway_mark_unavailable(void) { InterlockedExchange(&s_available, 0); }

// Bounds before any dereference (cvars.cpp's pattern); the rest goes through imagescan.h.
static bool cw_readable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const unsigned char* const end = (const unsigned char*)p + bytes;
  const unsigned char* const regionEnd =
      (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  return end <= regionEnd;
}

// Readable is not writable (.rdata, .text fault on a store). One VirtualQuery run proves the WHOLE 65536 bytes share
// the protection.
static bool cw_writable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if ((mbi.Protect & writable) == 0) return false;
  const unsigned char* const end = (const unsigned char*)p + bytes;
  const unsigned char* const regionEnd =
      (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  return end <= regionEnd;
}

enum CwVerdict { CW_OK = 0, CW_REFUSED = 1, CW_FAULTED = 2 };

static CwVerdict cw_locate_one(const unsigned char* base, int idx,
                               unsigned* begin, unsigned* end) {
  *begin = 0;
  *end = 0;
  const char* const name = EK_CUTAWAY_ANCHORS[idx].name;
  const char* const text = EK_CUTAWAY_ANCHORS[idx].text;

  EkStrHit sh;
  ek_image_find_string(base, text, strlen(text), &sh);
  if (sh.faulted) {
    ek_log("cutaway: a data section faulted mid-scan, so the \"%s\" anchor for "
           "%s cannot be counted (%d occurrence(s) before the fault)",
           text, name, sh.count);
    return CW_FAULTED;
  }

  EkRefHit rh;
  rh.rva = 0; rh.count = 0; rh.raw = 0; rh.faulted = 0;
  if (sh.count == 1) {
    ek_image_find_lea_refs(base, sh.rva, &rh);
    if (rh.faulted) {
      ek_log("cutaway: a code section faulted mid-scan, so the \"%s\" anchor at "
             "rva 0x%07X cannot be claimed unique (%d reference(s) before the "
             "fault)", text, sh.rva, rh.count);
      return CW_FAULTED;
    }
  }

  unsigned b = 0, e = 0;
  if (sh.count == 1 && rh.count == 1) {
    const bool haveExtent = ek_image_extent_containing(base, rh.rva, &b, &e);
    if (!haveExtent) { b = 0; e = 0; }
  }
  const unsigned bytes = e > b ? e - b : 0u;

  ek_log("cutaway: \"%s\" -- %d occurrence(s), first at rva 0x%07X; %d lea "
         "reference(s)%s of %d raw displacement match(es), first at rva "
         "0x%07X; .pdata owner 0x%07X..0x%07X (0x%X bytes)",
         text, sh.count, sh.rva, rh.count,
         rh.count == 1 ? "" : " (ambiguous)", rh.raw, rh.rva, b, e, bytes);

  if (!ekm_cutaway_anchor_ok(sh.count, rh.count, b)) {
    ek_log("cutaway: %s REFUSED -- its anchor needs exactly one occurrence, "
           "exactly one rip-relative reference and a function containing that "
           "reference, and this build gives %d, %d and 0x%07X. Nothing here "
           "picks a winner from an ambiguous anchor",
           name, sh.count, rh.count, b);
    return CW_REFUSED;
  }
  if (!ekm_cutaway_extent_ok(b, e)) {
    ek_log("cutaway: %s REFUSED -- the function at rva 0x%07X..0x%07X is 0x%X "
           "bytes, outside the 0x%X..0x%X window a Polymost painter of this "
           "shape occupies; the reference is not where it was thought to be",
           name, b, e, bytes, EK_CUTAWAY_MIN_BYTES, EK_CUTAWAY_MAX_BYTES);
    return CW_REFUSED;
  }

  ek_log("cutaway: %s is the function at rva 0x%07X..0x%07X (0x%X bytes) -- "
         "reached from the \"%s\" literal at rva 0x%07X through the one lea at "
         "rva 0x%07X", name, b, e, bytes, text, sh.rva, rh.rva);
  *begin = b;
  *end = e;
  return CW_OK;
}

// Bounded call count: `call rel32` inside ONE function reaching `entry`. polymost_drawrooms calls domost exactly twice
// (polymost.cpp:6999, :7006; rva 0x01587D2, 0x0158870).
struct CwCalls { int sites; unsigned first; unsigned second; int faulted; };

static void cw_count_calls_in(const unsigned char* base, unsigned begin,
                              unsigned end, unsigned entry, CwCalls* out) {
  out->sites = 0; out->first = 0; out->second = 0; out->faulted = 0;
  if (entry == 0 || begin == 0 || end <= begin + 5) { out->faulted = 1; return; }
  if (!cw_readable(base + begin, (size_t)(end - begin))) {
    out->faulted = 1;
    return;
  }
  __try {
    for (unsigned q = begin; q + 5 <= end; q++) {
      if (base[q] != 0xE8) continue;
      const int disp = *(const int*)(base + q + 1);
      if (ekm_us_rel32_target(q, disp) != entry) continue;
      if (out->sites == 0) out->first = q;
      else if (out->sites == 1) out->second = q;
      out->sites++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// .bss is the section the file does not back: ek_image_data_extent SEES it (VirtualSize), ek_image_sections cannot.
// This discards polymost_domost's own anchor string (.rdata, rva 0x0052B3A8) as a false hit.
static unsigned cw_bss_end(const unsigned char* base, unsigned rva) {
  unsigned begin = 0, end = 0;
  const bool haveExtent = ek_image_data_extent(base, rva, &begin, &end);
  if (!haveExtent || rva < begin || rva >= end) return 0;

  EkSpan spans[16];
  int n = 0;
  if (!ek_image_sections(base, false, spans, 16, &n)) return 0;
  for (int i = 0; i < n; i++) {
    const unsigned secRva = ek_image_rva(base, spans[i].base);
    if (rva >= secRva && rva < secRva + (unsigned)spans[i].size) return 0;
  }
  return end;
}

// Step 1, vsp[]. polymost_domost indexes vsp with `shl reg, 5` (x32 = sizeof(vsptyp)); a 16-bit access at +0x18 off the
// same base picks the array (n, p, ctag, ftag, polymost.cpp:60-62). Required: one such base and ONE rip-relative load
// setting it (.refptr mov at rva 0x001ACEAD, slot 0x0059FB70). Scanned, checked against a capstone decode.
struct CwVsp {
  unsigned rva;
  unsigned slot;
  unsigned loadRva;
  unsigned bssEnd;
  int scaleSites;
  int linkSites;
  int baseRegs;
  int loads;
  int faulted;
};

static void cw_scan_vsp(const unsigned char* base, unsigned begin, unsigned end,
                        CwVsp* out) {
  unsigned scaled = 0;
  int perBase[16];
  for (int i = 0; i < 16; i++) perBase[i] = 0;
  int chosen = -1;

  __try {
    // 1 of 3: `shl r/m64, 5`: REX.W (48|49), C1, modrm mod=11 /4, 05.
    for (unsigned q = begin; q + 4 <= end; q++) {
      if (base[q] != 0x48 && base[q] != 0x49) continue;
      if (base[q + 1] != 0xC1) continue;
      const unsigned char m = base[q + 2];
      if ((m & 0xF8) != 0xE0) continue;
      if (base[q + 3] != 0x05) continue;
      scaled |= 1u << ((m & 7) | ((base[q] & 1) << 3));
      out->scaleSites++;
    }
    if (scaled == 0) return;

    // 2 of 3: a 16-bit access at [base + idx*1 + 0x18] (`0F BF`, `0F B7`, 66-prefixed `8B`/`89`). One instruction is
    // reached from two positions when the prior byte is REX-range, so a match records its modrm to avoid a double count
    // reading as ambiguity.
    unsigned lastAt = 0xFFFFFFFFu;
    for (unsigned q = begin; q + 8 <= end; q++) {
      unsigned at = q;
      bool op16 = false;
      if (base[at] == 0x66) { op16 = true; at++; }
      unsigned char rex = 0;
      if (base[at] >= 0x40 && base[at] <= 0x4F) { rex = base[at]; at++; }
      unsigned o = 0;
      if (base[at] == 0x0F && (base[at + 1] == 0xBF || base[at + 1] == 0xB7))
        o = at + 2;
      else if (op16 && (base[at] == 0x8B || base[at] == 0x89))
        o = at + 1;
      else continue;
      if (o + 3 > end) continue;
      const unsigned char modrm = base[o];
      if ((modrm >> 6) != 1) continue;
      if ((modrm & 7) != 4) continue;
      const unsigned char sib = base[o + 1];
      if ((sib >> 6) != 0) continue;
      if ((sib & 7) == 5) continue;
      const int idx = (int)(((sib >> 3) & 7) | (((rex >> 1) & 1) << 3));
      if (idx == 4) continue;
      if ((scaled & (1u << idx)) == 0) continue;
      if (base[o + 2] != (unsigned char)EK_CUTAWAY_VSP_LINK) continue;
      if (o == lastAt) continue;
      lastAt = o;
      perBase[(sib & 7) | ((rex & 1) << 3)]++;
      out->linkSites++;
    }

    for (int r = 0; r < 16; r++) {
      if (perBase[r] == 0) continue;
      out->baseRegs++;
      if (chosen < 0) chosen = r;
    }
    // Ambiguity is the verdict, not a thing to resolve: two base registers mean two candidate arrays.
    if (out->baseRegs != 1) return;

    // 3 of 3: the rip-relative load that set that register: a .refptr `mov r64,[rip+d]`, or a `lea` naming the array
    // outright (refusing it would be a guess about the compiler).
    for (unsigned q = begin; q + 7 <= end; q++) {
      const unsigned char b0 = base[q];
      if (b0 < 0x48 || b0 > 0x4F) continue;
      const unsigned char op = base[q + 1];
      if (op != 0x8B && op != 0x8D) continue;
      const unsigned char m = base[q + 2];
      if ((m & 0xC7) != 0x05) continue;
      const int reg = (int)(((m >> 3) & 7) | (((b0 >> 2) & 1) << 3));
      if (reg != chosen) continue;
      const int disp = *(const int*)(base + q + 3);
      const unsigned target = (unsigned)((int)(q + 7) + disp);
      out->loads++;
      if (out->loads > 1) { out->rva = 0; out->slot = 0; continue; }
      out->loadRva = q;
      if (op == 0x8D) out->rva = target;
      else out->slot = target;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

static void cw_find_vsp(const unsigned char* base, unsigned begin, unsigned end,
                        CwVsp* out) {
  out->rva = 0; out->slot = 0; out->loadRva = 0; out->bssEnd = 0;
  out->scaleSites = 0; out->linkSites = 0; out->baseRegs = 0; out->loads = 0;
  out->faulted = 0;
  if (begin == 0 || end <= begin + 8) { out->faulted = 1; return; }
  if (!cw_readable(base + begin, (size_t)(end - begin))) {
    out->faulted = 1;
    return;
  }

  cw_scan_vsp(base, begin, end, out);
  if (out->faulted) return;

  if (out->loads == 1 && out->slot != 0) {
    unsigned sb = 0, se = 0;
    const bool slotInData = ek_image_data_extent(base, out->slot, &sb, &se);
    if (!slotInData || out->slot < sb || out->slot >= se) { out->rva = 0; return; }
    const size_t global = slot_target_rva(base, out->slot);
    out->rva = (unsigned)global;
  }
  if (out->rva != 0) out->bssEnd = cw_bss_end(base, out->rva);
}

// Step 2: is polymost_initmosts inlined? `vsp[VSPMAX-1].n = vcnt` (polymost.cpp:3754) is written by vsp_finalize_init,
// which only initmosts calls (:6808), so the function referencing that address carries it. A `mov word ptr [rip+d],
// imm16` would find NOTHING: reported as not established, never "inlined".
const int EK_CUTAWAY_MAX_VSP_OWNERS = 6;

struct CwOwners {
  unsigned begin[EK_CUTAWAY_MAX_VSP_OWNERS];
  unsigned end[EK_CUTAWAY_MAX_VSP_OWNERS];
  unsigned firstRef[EK_CUTAWAY_MAX_VSP_OWNERS];
  int count;
  int hits;
  int capped;
  int faulted;
};

static void cw_owners_of_disp(const unsigned char* base, unsigned target,
                              CwOwners* out) {
  out->count = 0; out->hits = 0; out->capped = 0; out->faulted = 0;
  for (int k = 0; k < EK_CUTAWAY_MAX_VSP_OWNERS; k++) {
    out->begin[k] = 0; out->end[k] = 0; out->firstRef[k] = 0;
  }
  if (target == 0) { out->faulted = 1; return; }

  EkSpan spans[16];
  int n = 0;
  if (!ek_image_sections(base, true, spans, 16, &n)) { out->faulted = 1; return; }

  for (int i = 0; i < n; i++) {
    if (spans[i].size < 4) continue;
    __try {
      const DWORD last = spans[i].size - 4;
      for (DWORD off = 0; off <= last; off++) {
        const int disp = *(const int*)(spans[i].base + off);
        const unsigned at = ek_image_rva(base, spans[i].base + off);
        if ((unsigned)((int)(at + 4) + disp) != target) continue;
        out->hits++;
        unsigned b = 0, e = 0;
        const bool haveOwner = ek_image_extent_containing(base, at, &b, &e);
        if (!haveOwner) continue;
        bool seen = false;
        for (int k = 0; k < out->count; k++) if (out->begin[k] == b) seen = true;
        if (seen) continue;
        if (out->count >= EK_CUTAWAY_MAX_VSP_OWNERS) { out->capped = 1; continue; }
        out->begin[out->count] = b;
        out->end[out->count] = e;
        out->firstRef[out->count] = at;
        out->count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A span that faulted part-way was not searched; its unread bytes could hold the reference.
      out->faulted = 1;
    }
  }
}

// The verdict is logged either way: "inlined, not callable" is a good answer, asserting either way without the settling
// reference is not.
static void cw_initmosts_verdict(const unsigned char* base, unsigned vspRva,
                                 unsigned slotRva, unsigned roomsB,
                                 unsigned roomsE) {
  const unsigned dummyRva = vspRva
      + (unsigned)(EK_CUTAWAY_VSPMAX - 1) * EK_CUTAWAY_VSP_STRIDE
      + EK_CUTAWAY_VSP_LINK;

  CwOwners dn;
  cw_owners_of_disp(base, dummyRva, &dn);
  if (dn.faulted) {
    ek_log("cutaway: the initmosts verdict is unavailable -- a code section "
           "faulted while looking for a reference to vsp[%d].n at rva 0x%07X "
           "(%d hit(s) in %d function(s) before the fault)",
           EK_CUTAWAY_VSPMAX - 1, dummyRva, dn.hits, dn.count);
    return;
  }

  if (slotRva != 0) {
    CwOwners sn;
    cw_owners_of_disp(base, slotRva, &sn);
    if (sn.faulted)
      ek_log("cutaway: the vsp[] reader list is unavailable -- a code section "
             "faulted while looking for loads of the .refptr slot at rva 0x%07X",
             slotRva);
    else
      for (int k = 0; k < sn.count; k++)
        ek_log("cutaway: vsp[] reader %d/%d -- the function at rva "
               "0x%07X..0x%07X loads the .refptr slot at rva 0x%07X (first at "
               "rva 0x%07X)%s", k + 1, sn.count, sn.begin[k], sn.end[k], slotRva,
               sn.firstRef[k], sn.capped ? " (list capped)" : "");
  }

  if (dn.count != 1) {
    ek_log("cutaway: polymost_initmosts is NOT ESTABLISHED -- vsp[%d].n at rva "
           "0x%07X, which only vsp_finalize_init writes (polymost.cpp:3754), is "
           "referenced by %d raw displacement(s) in %d function(s). Exactly one "
           "owner is what settles this, so nothing is claimed either way",
           EK_CUTAWAY_VSPMAX - 1, dummyRva, dn.hits, dn.count);
    return;
  }

  if (roomsB != 0 && dn.begin[0] == roomsB && dn.end[0] == roomsE) {
    ek_log("cutaway: polymost_initmosts is INLINED and NOT CALLABLE. The one "
           "reference to vsp[%d].n (rva 0x%07X) in the whole image sits at rva "
           "0x%07X, inside rva 0x%07X..0x%07X -- which is the extent the "
           "\"polymost_drawrooms()\" literal names. vsp_finalize_init's tail "
           "(polymost.cpp:3754) is therefore compiled into drawrooms itself, so "
           "there is no initmosts to call and a reopen must rebuild the list",
           EK_CUTAWAY_VSPMAX - 1, dummyRva, dn.firstRef[0], dn.begin[0],
           dn.end[0]);
    return;
  }

  // A separate function would change the reopen completely, so it is named with the fact that makes it initmosts: the
  // source calls it exactly once, from drawrooms (polymost.cpp:6986).
  EkCallCount cc;
  ek_image_count_calls(base, dn.begin[0], &cc);
  const bool onceFromRooms = !cc.faulted && cc.sites == 1 && cc.owners == 1 &&
                             roomsB != 0 && cc.owner[0] == roomsB;
  ek_log("cutaway: polymost_initmosts MAY BE CALLABLE -- the one reference to "
         "vsp[%d].n (rva 0x%07X) sits at rva 0x%07X inside rva 0x%07X..0x%07X, "
         "which is NOT polymost_drawrooms (rva 0x%07X..0x%07X). That function is "
         "called from %d site(s) in %d function(s)%s, and polymost.cpp:6986 "
         "calls initmosts exactly once from drawrooms: %s",
         EK_CUTAWAY_VSPMAX - 1, dummyRva, dn.firstRef[0], dn.begin[0], dn.end[0],
         roomsB, roomsE, cc.sites, cc.owners, cc.faulted ? " (count faulted)" : "",
         onceFromRooms ? "AGREED, so this is polymost_initmosts and the cutaway hook can "
                         "call it"
                       : "NOT AGREED, so this is some other vsp writer and "
                         "nothing here claims it is initmosts");
}

// nocull.cpp reached two of these functions by a different route ("!!numscans" plus __LINE__, then .pdata, then a call
// walk), so agreement is mutual evidence. A disagreement is a loud log line, not a refusal.

static void cw_corroborate_with_nocull(const unsigned char* base,
                                       unsigned roomsB, unsigned roomsE,
                                       unsigned allsB, unsigned allsE) {
  if (!nocull_located()) {
    ek_log("cutaway: the two nocull corroborations are unavailable -- nocull's "
           "own locate has not answered yet. This locate stands on its own "
           "evidence and nothing waits on them");
    return;
  }

  const unsigned seedSite = nocull_seed_site_rva();
  unsigned sb = 0, se = 0;
  const bool haveSeedOwner = ek_image_extent_containing(base, seedSite, &sb, &se);
  if (!haveSeedOwner) { sb = 0; se = 0; }
  if (roomsB == 0) {
    ek_log("cutaway: the drawrooms corroboration cannot run -- this locate has "
           "no extent for polymost_drawrooms. nocull's seed call at rva 0x%07X "
           "owns 0x%07X..0x%07X", seedSite, sb, se);
  } else if (sb == roomsB && se == roomsE) {
    ek_log("cutaway: CORROBORATED -- nocull's seed call at rva 0x%07X lies in "
           "the function at rva 0x%07X..0x%07X, which is exactly the extent the "
           "\"polymost_drawrooms()\" literal names here. Two independent "
           "locates, one function", seedSite, sb, se);
  } else {
    ek_log("cutaway: !! the drawrooms corroboration DISAGREES -- NOCULL's seed "
           "call at rva 0x%07X owns rva 0x%07X..0x%07X, while THIS locate's "
           "\"polymost_drawrooms()\" anchor names rva 0x%07X..0x%07X. Not a "
           "refusal: nocull walks a string plus LOG_F's line number and this "
           "walks a format string, so either could be the one that slipped",
           seedSite, sb, se, roomsB, roomsE);
  }

  // polymost_drawalls CALLS polymost_scansector (nocull's call site 3/3, polymost.cpp:6548); the owner LIST is asked,
  // since which site is third is a scan ordering.
  const unsigned ssRva = nocull_scansector_rva();
  EkCallCount cc;
  ek_image_count_calls(base, ssRva, &cc);
  if (cc.faulted) {
    ek_log("cutaway: the drawalls corroboration is unavailable -- a code "
           "section faulted while counting calls to nocull's "
           "polymost_scansector at rva 0x%07X (%d site(s) before the fault)",
           ssRva, cc.sites);
    return;
  }
  if (allsB == 0) {
    ek_log("cutaway: the drawalls corroboration cannot run -- this locate has "
           "no extent for polymost_drawalls. nocull's polymost_scansector at "
           "rva 0x%07X is called from %d site(s) in %d function(s)",
           ssRva, cc.sites, cc.owners);
    return;
  }
  bool amongOwners = false;
  for (int i = 0; i < cc.owners; i++)
    if (cc.owner[i] == allsB) amongOwners = true;
  if (amongOwners) {
    ek_log("cutaway: CORROBORATED -- polymost_drawalls at rva 0x%07X..0x%07X is "
           "one of the %d function(s) calling nocull's polymost_scansector at "
           "rva 0x%07X across %d site(s), which is polymost.cpp:6548. Two "
           "independent locates, one function",
           allsB, allsE, cc.owners, ssRva, cc.sites);
  } else {
    ek_log("cutaway: !! the drawalls corroboration DISAGREES -- NOCULL says "
           "polymost_scansector at rva 0x%07X is called from %d site(s) in %d "
           "function(s)%s and none of them is the rva 0x%07X..0x%07X THIS "
           "locate's \"polymost_drawalls(\" anchor names. Not a refusal: the "
           "two routes are independent and either could be the one that "
           "slipped", ssRva, cc.sites, cc.owners,
           cc.capped ? " (owner list capped)" : "", allsB, allsE);
  }
}

// Multipass spike locate (throwaway): yax_polymostclearzbuffer, found by the depth clear it guards
// (polymost.cpp:6858-6862): a .refptr load, `test`, `jne` to a block calling glClear's import slot (rva 0x05A2680,
// shared with :6856). The flag holds rva 0x04D50C8 (.data, initialised to 1, engine.cpp:430).

struct CwZClear {
  int clearSites;
  unsigned clearRva;
  unsigned iatSlot;
  int guardSites;
  unsigned guardRva;
  int shapeOk;
  unsigned slot;
  int faulted;
};

static void cw_find_zclear(const unsigned char* base, unsigned roomsB,
                           unsigned roomsE, CwZClear* out) {
  memset(out, 0, sizeof(*out));
  if (roomsB == 0 || roomsE <= roomsB + 14) { out->faulted = 1; return; }
  if (!cw_readable(base + roomsB, (size_t)(roomsE - roomsB))) {
    out->faulted = 1;
    return;
  }

  __try {
    // 1 of 3: every glClear(GL_DEPTH_BUFFER_BIT) block, counted rather than stopped at: two is a question nothing here
    // answers by picking.
    for (unsigned q = roomsB; q + 14 <= roomsE; q++) {
      if (base[q] != 0x48 || base[q + 1] != 0x8B || base[q + 2] != 0x05) continue;
      if (base[q + 7] != 0xB9) continue;
      if (*(const unsigned*)(base + q + 8) != 0x00000100u) continue;
      if (base[q + 12] != 0xFF || base[q + 13] != 0x10) continue;
      if (out->clearSites == 0) {
        out->clearRva = q;
        out->iatSlot = (unsigned)((int)(q + 7) + *(const int*)(base + q + 3));
      }
      out->clearSites++;
    }
    if (out->clearSites != 1) return;

    // 2 of 3: every conditional branch reaching it. A stray 0x7X operand could read as a short jump; a false positive
    // lands as a REFUSAL, the right way to fail.
    for (unsigned p = roomsB; p + 2 <= roomsE; p++) {
      unsigned target;
      if ((base[p] & 0xF0) == 0x70) {
        target = (unsigned)((int)(p + 2) + (int)(signed char)base[p + 1]);
      } else if (base[p] == 0x0F && (base[p + 1] & 0xF0) == 0x80 &&
                 p + 6 <= roomsE) {
        target = (unsigned)((int)(p + 6) + *(const int*)(base + p + 2));
      } else {
        continue;
      }
      if (target != out->clearRva) continue;
      if (out->guardSites == 0) out->guardRva = p;
      out->guardSites++;
    }
    if (out->guardSites != 1) return;

    // 3 of 3: the guard's three instructions. Without them no slot resolves and the verdict refuses on a zero rva.
    const unsigned p = out->guardRva;
    if (p < roomsB + 11) return;
    if (base[p - 2] != 0x85 || base[p - 1] != 0xC0) return;
    if (base[p - 4] != 0x8B || base[p - 3] != 0x00) return;
    if (base[p - 11] != 0x48 || base[p - 10] != 0x8B ||
        base[p - 9] != 0x05) return;
    out->shapeOk = 1;
    out->slot = (unsigned)((int)(p - 4) + *(const int*)(base + p - 8));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// Non-gating: a refusal costs the spike its second pass; the log says which of five links broke.
static void cw_locate_zclear(const unsigned char* base, unsigned roomsB,
                             unsigned roomsE) {
  if (roomsB == 0) {
    ek_log("cutaway zspike: no extent for polymost_drawrooms, so "
           "yax_polymostclearzbuffer is not looked for and no second pass can "
           "be armed");
    return;
  }

  CwZClear z;
  cw_find_zclear(base, roomsB, roomsE, &z);
  if (z.faulted) {
    ek_log("cutaway zspike: polymost_drawrooms' own bytes at rva "
           "0x%07X..0x%07X were not readable or faulted mid-scan, so "
           "yax_polymostclearzbuffer is not located", roomsB, roomsE);
    return;
  }

  unsigned rva = 0;
  if (z.slot != 0) rva = (unsigned)slot_target_rva(base, z.slot);
  unsigned dataB = 0, dataE = 0;
  if (rva != 0) {
    const bool haveExtent = ek_image_data_extent(base, rva, &dataB, &dataE);
    if (!haveExtent) { dataB = 0; dataE = 0; }
  }

  // The value identifies it, not the walk: bounds-checked first, writable too since it will be stored into.
  int value = 0;
  bool canWrite = false;
  if (rva != 0 && cw_readable(base + rva, 4) && cw_writable(base + rva, 4)) {
    value = *(const volatile int*)(base + rva);
    canWrite = true;
  }

  ek_log("cutaway zspike: %d GL_DEPTH_BUFFER_BIT clear(s) in rva "
         "0x%07X..0x%07X (first at 0x%07X, through the import slot at 0x%07X), "
         "%d branch(es) reaching it (first at 0x%07X), guard shape %s, .refptr "
         "slot 0x%07X, global rva 0x%07X in 0x%07X..0x%07X, %s, reads %d at rest",
         z.clearSites, roomsB, roomsE, z.clearRva, z.iatSlot, z.guardSites,
         z.guardRva, z.shapeOk ? "MATCHED" : "NO", z.slot, rva, dataB, dataE,
         canWrite ? "readable and writable" : "NOT readable and writable", value);

  if (!canWrite || !ekm_zclear_ok(z.clearSites, z.guardSites, rva, dataE, value)) {
    ek_log("cutaway zspike: REFUSED -- yax_polymostclearzbuffer needs exactly "
           "one depth clear, exactly one guard reaching it, the .refptr/test "
           "shape, a writable 32-bit home inside its section and the value 1 at "
           "rest that engine.cpp:430 gives it. No second drawrooms pass is "
           "armed and nothing is written; everything else keeps running");
    return;
  }

  InterlockedExchange(&s_zclearSlot, (LONG)z.slot);
  InterlockedExchange(&s_zclearRva, (LONG)rva);
  ek_log("cutaway zspike: yax_polymostclearzbuffer is the int32 at rva 0x%07X, "
         "named by the .refptr slot at rva 0x%07X that polymost.cpp:6860's "
         "guard loads, and it reads 1 at rest as engine.cpp:430 sets it. The "
         "second drawrooms pass may be armed", rva, z.slot);
  ek_log("cutaway zspike: (the two rvas above are 0x04D50C8 and 0x05A4CC0 in "
         "the synthesis this was written against -- an EXPECTED value for this "
         "line and never a constant anything reads)");
}

// Backface spike locate (throwaway): domostpolymethod (polymost.cpp:3849), found by what it FEEDS: drawpoly's third
// argument is r8d (Microsoft x64), loaded before each of eight calls, all naming ONE address (rva 0x1991650, .bss),
// confirmed by the source's stores of -1, 0 and 0x20. The rel32 must land EXACTLY on drawpoly's entry.
enum { EK_CUTAWAY_DPM_MAXTARGETS = 4 };

struct CwDpm {
  int callSites;
  int feederSites;
  int feederTargets;
  int otherLoads;
  int minusOne;
  int zeroes;
  int wallStores;
  unsigned rva;
  unsigned bssEnd;
  int faulted;
};

static void cw_scan_dpm_feeders(const unsigned char* base, unsigned begin,
                                unsigned end, unsigned drawpolyRva, CwDpm* out) {
  unsigned seen[EK_CUTAWAY_DPM_MAXTARGETS];
  for (int k = 0; k < EK_CUTAWAY_DPM_MAXTARGETS; k++) seen[k] = 0;

  __try {
    for (unsigned q = begin; q + 7 <= end; q++) {
      if (base[q] != 0x44 || base[q + 1] != 0x8B || base[q + 2] != 0x05) continue;
      const int disp = *(const int*)(base + q + 3);
      const unsigned target = (unsigned)((int)(q + 7) + disp);

      unsigned lim = q + 7 + EK_CUTAWAY_DPM_WINDOW;
      if (lim > end) lim = end;
      bool feeds = false;
      for (unsigned p = q + 7; p + 5 <= lim; p++) {
        if (base[p] != 0xE8) continue;
        const int d2 = *(const int*)(base + p + 1);
        if (ekm_us_rel32_target(p, d2) == drawpolyRva) { feeds = true; break; }
      }
      if (!feeds) { out->otherLoads++; continue; }

      out->feederSites++;
      bool known = false;
      for (int k = 0; k < out->feederTargets &&
                      k < EK_CUTAWAY_DPM_MAXTARGETS; k++)
        if (seen[k] == target) known = true;
      if (known) continue;
      if (out->feederTargets < EK_CUTAWAY_DPM_MAXTARGETS)
        seen[out->feederTargets] = target;
      out->feederTargets++;
      // The first is the candidate; a second clears it (ekm_dpm_ok refuses on the count).
      if (out->feederTargets == 1) out->rva = target;
      else out->rva = 0;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// `C7 05 d32 imm32`: displacement at +2, instruction ends at +10, the trailing-immediate case the four-byte convention
// of cw_owners_of_disp MISSES, so this scan is its own.
static void cw_scan_dpm_stores(const unsigned char* base, unsigned target,
                               CwDpm* out) {
  EkSpan spans[16];
  int n = 0;
  if (!ek_image_sections(base, true, spans, 16, &n)) { out->faulted = 1; return; }

  for (int i = 0; i < n; i++) {
    if (spans[i].size < 10) continue;
    __try {
      const DWORD last = spans[i].size - 10;
      for (DWORD off = 0; off <= last; off++) {
        const unsigned char* const p = spans[i].base + off;
        if (p[0] != 0xC7 || p[1] != 0x05) continue;
        const int disp = *(const int*)(p + 2);
        const unsigned at = ek_image_rva(base, p);
        if ((unsigned)((int)(at + 10) + disp) != target) continue;
        const int imm = *(const int*)(p + 6);
        if (imm == EK_DAMETH_BACKFACECULL) out->minusOne++;
        else if (imm == EK_DAMETH_NOMASK) out->zeroes++;
        else if (imm == EK_DAMETH_WALL) out->wallStores++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A span that faulted part-way was not searched and could hold the identifying store.
      out->faulted = 1;
    }
  }
}

// Non-gating: a refusal costs only the skip; the log says which link broke.
static void cw_locate_dpm(const unsigned char* base, unsigned domostB,
                          unsigned domostE, unsigned drawpolyB) {
  CwDpm d;
  memset(&d, 0, sizeof(d));

  if (domostB == 0 || drawpolyB == 0 || domostE <= domostB + 12) {
    ek_log("cutaway bfspike: no usable extent for polymost_domost or no entry "
           "for polymost_drawpoly, so domostpolymethod is not looked for and no "
           "call is skipped");
    return;
  }
  if (!cw_readable(base + domostB, (size_t)(domostE - domostB))) {
    ek_log("cutaway bfspike: polymost_domost's own bytes at rva 0x%07X..0x%07X "
           "were not readable, so domostpolymethod is not located",
           domostB, domostE);
    return;
  }

  CwCalls calls;
  cw_count_calls_in(base, domostB, domostE, drawpolyB, &calls);
  d.callSites = calls.sites;
  if (calls.faulted) d.faulted = 1;

  cw_scan_dpm_feeders(base, domostB, domostE, drawpolyB, &d);

  if (!d.faulted && d.rva != 0) {
    cw_scan_dpm_stores(base, d.rva, &d);
    d.bssEnd = cw_bss_end(base, d.rva);
  }

  // Read once and bounds-checked first (cvars.cpp's readable()); .bss is not file-backed, so this is the only way to
  // see it.
  int value = 0;
  bool canRead = false;
  if (d.rva != 0 && cw_readable(base + d.rva, 4)) {
    value = *(const volatile int*)(base + d.rva);
    canRead = true;
  }

  ek_log("cutaway bfspike: domostpolymethod scan -- %d call(s) to "
         "polymost_drawpoly (rva 0x%07X) inside polymost_domost (rva "
         "0x%07X..0x%07X), %d r8d feeder load(s) naming %d distinct address(es) "
         "and %d other r8d load(s); candidate rva 0x%07X, .bss ends at 0x%07X; "
         "immediate stores into it: %d of -1, %d of 0, %d of 32; %s, reads %d "
         "at rest%s",
         d.callSites, drawpolyB, domostB, domostE, d.feederSites,
         d.feederTargets, d.otherLoads, d.rva, d.bssEnd, d.minusOne, d.zeroes,
         d.wallStores, canRead ? "readable" : "NOT readable", value,
         d.faulted ? " -- A SCAN FAULTED" : "");

  if (d.faulted || !canRead ||
      !ekm_dpm_ok(d.callSites, d.feederSites, d.feederTargets, d.minusOne,
                  d.zeroes, d.rva, d.bssEnd, value)) {
    ek_log("cutaway bfspike: REFUSED -- domostpolymethod needs several r8d "
           "feeder loads all naming ONE address in .bss with room for 32 bits, "
           "that same address written -1 and 0 by the engine's own immediate "
           "stores (polymost.cpp:4998/:5008 and :5019/:6544), and a value at "
           "rest the engine's own writers can produce. The spike is INERT: "
           "every polymost_domost call forwards exactly as before and nothing "
           "is read or written");
    return;
  }

  InterlockedExchange(&s_dpmRva, (LONG)d.rva);
  ek_log("cutaway bfspike: domostpolymethod is the int32 at rva 0x%07X -- fed "
         "to %d polymost_drawpoly call(s) in r8d, and written DAMETH_BACKFACECULL "
         "(-1) %d time(s) and DAMETH_NOMASK (0) %d time(s) by the engine's own "
         "immediate stores, which is the binary stating polymost.h:232 and :218 "
         "itself. Gated calls holding -1 will be SKIPPED", d.rva, d.feederSites,
         d.minusOne, d.zeroes);
  ek_log("cutaway bfspike: (the rva above is 0x1991650 in the synthesis this "
         "was written against -- an EXPECTED value for this line and never a "
         "constant anything reads)");
}

// The horizontal view bounds (xbl, xbr, polymost.cpp:64): stage C paints a wall quad WITHOUT the head of domost that
// clamps against them (cutaway.h). Image: slot rva 0x059FBA0 -> global 0x13F1D88, `comiss`/`ja` to one early-out
// (:3953), slot 0x059FBB0 -> 0x13F1D84. The LOWER address is xbr, which a first-one-wins rule gets backwards.
enum { EK_CW_XB_MAX = 4 };

struct CwXBounds {
  int candidates;
  int capped;
  unsigned slot[EK_CW_XB_MAX];
  unsigned movss[EK_CW_XB_MAX];
  int pairs;
  unsigned slotA, slotB;
  // The rvas of the two pairs that RESOLVED, not necessarily candidates 0 and 1.
  unsigned movA, movB;
  unsigned rvaA, rvaB;
  unsigned dataEnd;
  int faulted;
};

// Scanned, not decoded: a byte shape with the COUNT doing the identifying.
static void cw_scan_xbounds(const unsigned char* base, unsigned begin,
                            unsigned end, CwXBounds* out) {
  __try {
    for (unsigned q = begin; q + 7 <= end; q++) {
      const unsigned char b0 = base[q];
      if (b0 < 0x48 || b0 > 0x4F) continue;
      if (base[q + 1] != 0x8B) continue;
      const unsigned char m = base[q + 2];
      if ((m & 0xC7) != 0x05) continue;
      const int reg = (int)(((m >> 3) & 7) | (((b0 >> 2) & 1) << 3));
      const int disp = *(const int*)(base + q + 3);
      const unsigned slot = (unsigned)((int)(q + 7) + disp);

      unsigned lim = q + 7 + EK_CUTAWAY_XB_LOOKAHEAD;
      if (lim > end) lim = end;
      for (unsigned p = q + 7; p + 4 <= lim; p++) {
        if (base[p] != 0xF3) continue;
        unsigned at = p + 1;
        unsigned char rex = 0;
        if (base[at] >= 0x40 && base[at] <= 0x4F) { rex = base[at]; at++; }
        if (at + 3 > end) break;
        if (base[at] != 0x0F || base[at + 1] != 0x10) continue;
        const unsigned char modrm = base[at + 2];
        if ((modrm >> 6) != 0) continue;
        if ((modrm & 7) == 4) continue;
        if ((modrm & 7) == 5) continue;
        const int rm = (int)((modrm & 7) | ((rex & 1) << 3));
        if (rm != reg) continue;
        if (out->candidates < EK_CW_XB_MAX) {
          out->slot[out->candidates] = slot;
          out->movss[out->candidates] = p;
        } else {
          out->capped = 1;
        }
        out->candidates++;
        break;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

static void cw_find_xbounds(const unsigned char* base, unsigned domostB,
                            unsigned domostE, CwXBounds* out) {
  memset(out, 0, sizeof(*out));
  if (domostB == 0 || domostE <= domostB + 8) { out->faulted = 1; return; }
  unsigned end = domostB + EK_CUTAWAY_XB_WINDOW;
  if (end > domostE) end = domostE;
  if (!cw_readable(base + domostB, (size_t)(end - domostB))) {
    out->faulted = 1;
    return;
  }

  cw_scan_xbounds(base, domostB, end, out);
  if (out->faulted) return;

  // The slot is what the code carries and the global what it means; each resolution is a statement of its own. A slot
  // that names nothing in a data section is not one of these two.
  const int seen = out->candidates < EK_CW_XB_MAX ? out->candidates
                                                  : EK_CW_XB_MAX;
  for (int i = 0; i < seen; i++) {
    unsigned sb = 0, se = 0;
    const bool slotInData = ek_image_data_extent(base, out->slot[i], &sb, &se);
    if (!slotInData) continue;
    const size_t global = slot_target_rva(base, out->slot[i]);
    if (global == 0) continue;
    unsigned gb = 0, ge = 0;
    const bool globalInData =
        ek_image_data_extent(base, (unsigned)global, &gb, &ge);
    if (!globalInData) continue;
    if (out->pairs == 0) {
      out->slotA = out->slot[i]; out->rvaA = (unsigned)global;
      out->movA = out->movss[i];
    } else if (out->pairs == 1) {
      out->slotB = out->slot[i]; out->rvaB = (unsigned)global;
      out->movB = out->movss[i];
    }
    out->pairs++;
  }

  if (out->rvaA != 0 && out->rvaB != 0) {
    const unsigned hiRva = out->rvaA > out->rvaB ? out->rvaA : out->rvaB;
    unsigned hb = 0, he = 0;
    const bool haveExtent = ek_image_data_extent(base, hiRva, &hb, &he);
    out->dataEnd = haveExtent ? he : 0u;
  }
}

// Non-gating: a refusal costs stage C its x-clamp; the wall quads keep painting as today, stretching and all.
static void cw_locate_xbounds(const unsigned char* base, unsigned domostB,
                              unsigned domostE) {
  CwXBounds x;
  cw_find_xbounds(base, domostB, domostE, &x);

  ek_log("cutaway xbounds: %d pair(s) of `mov r64,[rip+d]` + `movss xmm,[r64]` "
         "in polymost_domost's head (rva 0x%07X, first 0x%X bytes), %d "
         "candidate(s) before resolution%s; slots 0x%07X and 0x%07X naming "
         "globals 0x%07X and 0x%07X, %u bytes apart; section ends at 0x%07X%s",
         x.pairs, domostB, EK_CUTAWAY_XB_WINDOW, x.candidates,
         x.capped ? " (CAPPED)" : "", x.slotA, x.slotB, x.rvaA, x.rvaB,
         x.rvaA > x.rvaB ? x.rvaA - x.rvaB : x.rvaB - x.rvaA, x.dataEnd,
         x.faulted ? " -- A SCAN FAULTED" : "");

  if (x.faulted || x.capped ||
      !ekm_cutaway_xbounds_ok(x.pairs, x.rvaA, x.rvaB, x.dataEnd)) {
    ek_log("cutaway xbounds: REFUSED -- the view bounds need EXACTLY two such "
           "pairs naming two floats FOUR bytes apart in a data section, which is "
           "what `static float xbl, xbr, xbt, xbb` (polymost.cpp:64) looks like. "
           "Stage C paints its wall quads exactly as it does today, unclamped: "
           "the stretching is cosmetic and a wrong clip rectangle would remove "
           "geometry the player should see");
    return;
  }

  InterlockedExchange(&s_xbA, (LONG)x.rvaA);
  InterlockedExchange(&s_xbB, (LONG)x.rvaB);
  ek_log("cutaway xbounds: the two horizontal view bounds are the floats at rva "
         "0x%07X and 0x%07X, named by the .refptr slots at rva 0x%07X and "
         "0x%07X, found in polymost_domost's own head at rva 0x%07X and 0x%07X "
         "-- the two loads :3953 tests a span's ends against. WHICH IS LEFT AND "
         "WHICH IS RIGHT IS NOT CLAIMED: the painter reads both and takes min "
         "and max, and the left bound is always below the right one",
         x.rvaA, x.rvaB, x.slotA, x.slotB, x.movA, x.movB);
  ek_log("cutaway xbounds: (the four rvas above are 0x13F1D88, 0x13F1D84, "
         "0x059FBA0 and 0x059FBB0 in the synthesis this was written against -- "
         "an EXPECTED value for this line and never a constant anything reads)");
}

// Render thread: ONE VirtualQuery pair per session (a .bss page keeps its protection; domost runs dozens of times per
// wall) and the VALUES read every call (the engine rewrites them per frame, polymost.cpp:6810-6820).
static const volatile float* s_xbPtrA = NULL;
static const volatile float* s_xbPtrB = NULL;
static bool s_xbSettled = false;
static bool s_saidXbInert = false;
static bool s_saidXbUnreadable = false;

bool cutaway_xbounds(float* lo, float* hi) {
  if (lo == NULL || hi == NULL) return false;

  if (!s_xbSettled) {
    const unsigned char* const base = s_base;
    if (base == NULL) return false;
    const unsigned a = cutaway_xbound_a_rva();
    const unsigned b = cutaway_xbound_b_rva();
    if (a == 0 || b == 0) {
      // Not settled yet is not refused: the locate runs on the sampler and a gated frame can arrive first. Once it
      // answers with nothing it never will.
      if (!cutaway_located()) return false;
      s_xbSettled = true;
      if (!s_saidXbInert) {
        s_saidXbInert = true;
        ek_log("cutaway xbounds: INERT -- the locate published no view bounds, "
               "so stage C paints every wall quad unclamped, exactly as it did "
               "before this existed");
      }
      return false;
    }
    s_xbSettled = true;
    if (!cw_readable((const void*)(base + a), 4) ||
        !cw_readable((const void*)(base + b), 4)) {
      if (!s_saidXbUnreadable) {
        s_saidXbUnreadable = true;
        ek_log("cutaway xbounds: the floats at rva 0x%07X and 0x%07X are not "
               "readable from the render thread, so nothing is clamped for the "
               "rest of this session and everything else keeps running", a, b);
      }
      return false;
    }
    s_xbPtrA = (const volatile float*)(base + a);
    s_xbPtrB = (const volatile float*)(base + b);
  }

  if (s_xbPtrA == NULL || s_xbPtrB == NULL) return false;

  const float va = *s_xbPtrA;
  const float vb = *s_xbPtrB;
  const float l = va < vb ? va : vb;
  const float h = va < vb ? vb : va;
  // A positive comparison, so a NaN or a mid-write frame refuses and paints as before.
  if (!(h > l)) return false;
  *lo = l;
  *hi = h;
  return true;
}

// gctang/gstang locate (argument: cutaway.h). Four readers, one instruction form each, since pairing must know WHICH
// REGISTER a value came back in. Not a decoder: a misread ADDS a candidate, which is a REFUSAL, the right way to fail.
enum { EK_CW_TANG_MAX = 8 };

// movss xmm, [rsp+disp]: the sincos out-slot read back. Returns the xmm register or -1.
static int cw_movss_rsp(const unsigned char* base, unsigned p, unsigned e,
                        int* disp) {
  if (p + 5 > e || base[p] != 0xF3) return -1;
  unsigned at = p + 1;
  unsigned char rex = 0;
  if (base[at] >= 0x40 && base[at] <= 0x4F) { rex = base[at]; at++; }
  if (at + 4 > e) return -1;
  if (base[at] != 0x0F || base[at + 1] != 0x10) return -1;
  const unsigned char m = base[at + 2];
  if ((m & 7) != 4) return -1;
  if (base[at + 3] != 0x24) return -1;
  const int mod = m >> 6;
  if (mod == 1) {
    if (at + 5 > e) return -1;
    *disp = (int)(signed char)base[at + 4];
  } else if (mod == 2) {
    if (at + 8 > e) return -1;
    *disp = *(const int*)(base + at + 4);
  } else {
    return -1;
  }
  return (int)(((m >> 3) & 7) | (((rex >> 2) & 1) << 3));
}

// mov r64, [rip+disp32]: the mingw .refptr load. Returns the register or -1.
static int cw_refptr_load(const unsigned char* base, unsigned p, unsigned e,
                          unsigned* slot) {
  if (p + 7 > e) return -1;
  if (base[p] < 0x48 || base[p] > 0x4F || base[p + 1] != 0x8B) return -1;
  const unsigned char m = base[p + 2];
  if ((m & 0xC7) != 0x05) return -1;
  *slot = (unsigned)((int)(p + 7) + *(const int*)(base + p + 3));
  return (int)(((m >> 3) & 7) | (((base[p] >> 2) & 1) << 3));
}

// movss [r64], xmm: the store through it. Returns the SOURCE xmm register.
static int cw_movss_store(const unsigned char* base, unsigned p, unsigned e,
                          int* baseReg) {
  if (p + 4 > e || base[p] != 0xF3) return -1;
  unsigned at = p + 1;
  unsigned char rex = 0;
  if (base[at] >= 0x40 && base[at] <= 0x4F) { rex = base[at]; at++; }
  if (at + 3 > e) return -1;
  if (base[at] != 0x0F || base[at + 1] != 0x11) return -1;
  const unsigned char m = base[at + 2];
  if ((m >> 6) != 0) return -1;
  if ((m & 7) == 4 || (m & 7) == 5) return -1;
  *baseReg = (int)((m & 7) | ((rex & 1) << 3));
  return (int)(((m >> 3) & 7) | (((rex >> 2) & 1) << 3));
}

// movss xmm, [rip+disp32] whose target holds polymost.cpp:6926's .001f.
static bool cw_movss_snapconst(const unsigned char* base, unsigned p,
                               unsigned e) {
  if (p + 8 > e || base[p] != 0xF3) return false;
  if (base[p + 1] != 0x0F || base[p + 2] != 0x10) return false;
  if ((base[p + 3] & 0xC7) != 0x05) return false;
  const unsigned tgt = (unsigned)((int)(p + 8) + *(const int*)(base + p + 4));
  unsigned db = 0, de = 0;
  const bool inData = ek_image_data_extent(base, tgt, &db, &de);
  if (!inData || de <= tgt || de - tgt < 4u) return false;
  // The bit pattern (0x3A83126F), not a float compare.
  return *(const unsigned*)(base + tgt) == 0x3A83126Fu;
}

struct CwTangs {
  int sites;
  unsigned callRva;
  int dSin, dCos;
  int cosReg, sinReg;
  int cosReads, sinReads;
  int snapConsts;
  int cCount, sCount;
  unsigned cCand[EK_CW_TANG_MAX];
  unsigned sCand[EK_CW_TANG_MAX];
  int capped;
  int pairs;
  unsigned cRva, sRva;
  unsigned dataEnd;
  int faulted;
};

static void cw_scan_tangs(const unsigned char* base, unsigned b, unsigned e,
                          CwTangs* out) {
  __try {
    // 1 of 3: the one sincosf call: `lea rdx,[rsp+d]`, `lea r8,[rsp+d']` four bytes apart, then `call rel32` (gcc's
    // sincosf out-pointers in the Win64 second and third integer slots).
    for (unsigned q = b; q + 8 <= e; q++) {
      if (base[q] != 0x48 || base[q + 1] != 0x8D ||
          base[q + 2] != 0x94 || base[q + 3] != 0x24) continue;
      const int dsin = *(const int*)(base + q + 4);
      unsigned lim = q + 8 + EK_CUTAWAY_TANG_GAP;
      if (lim > e) lim = e;
      unsigned r8 = 0;
      for (unsigned p = q + 8; p + 8 <= lim; p++) {
        if (base[p] == 0x4C && base[p + 1] == 0x8D &&
            base[p + 2] == 0x84 && base[p + 3] == 0x24) { r8 = p; break; }
      }
      if (r8 == 0) continue;
      const int dcos = *(const int*)(base + r8 + 4);
      if (dsin - dcos != 4 && dcos - dsin != 4) continue;
      unsigned lim2 = r8 + 8 + EK_CUTAWAY_TANG_GAP;
      if (lim2 > e) lim2 = e;
      unsigned call = 0;
      for (unsigned p = r8 + 8; p + 5 <= lim2; p++) {
        if (base[p] == 0xE8) { call = p; break; }
      }
      if (call == 0) continue;
      if (out->sites == 0) {
        out->callRva = call;
        out->dSin = dsin;
        out->dCos = dcos;
      }
      out->sites++;
    }
    if (out->sites != 1) return;

    unsigned wb = out->callRva + 5;
    unsigned we = wb + EK_CUTAWAY_TANG_WINDOW;
    if (we > e) we = e;
    if (wb >= we) { out->faulted = 1; return; }

    // 2 of 3: which xmm register each out-slot came back in, and the snap constant that says this is polymost.cpp:6926.
    out->cosReg = -1;
    out->sinReg = -1;
    for (unsigned p = wb; p < we; p++) {
      int disp = 0;
      const int reg = cw_movss_rsp(base, p, we, &disp);
      if (reg >= 0 && disp == out->dCos) { out->cosReg = reg; out->cosReads++; }
      if (reg >= 0 && disp == out->dSin) { out->sinReg = reg; out->sinReads++; }
      if (cw_movss_snapconst(base, p, we)) out->snapConsts++;
    }
    if (out->cosReads != 1 || out->sinReads != 1) return;

    // 3 of 3: every global stored FROM one of those registers, paired with the most recent .refptr load of its base
    // register. A stale pairing names a wrong global (here a bool, inpreparemirror, stored at polymost.cpp:6933), which
    // the four-bytes-apart rule rejects.
    unsigned last[16];
    for (int i = 0; i < 16; i++) last[i] = 0;
    for (unsigned p = wb; p < we; p++) {
      unsigned slot = 0;
      const int ld = cw_refptr_load(base, p, we, &slot);
      if (ld >= 0 && ld < 16) last[ld] = slot;
      int baseReg = 0;
      const int src = cw_movss_store(base, p, we, &baseReg);
      if (src < 0 || baseReg < 0 || baseReg >= 16) continue;
      if (last[baseReg] == 0) continue;
      unsigned sb = 0, se = 0;
      const bool slotInData = ek_image_data_extent(base, last[baseReg], &sb, &se);
      if (!slotInData) continue;
      const size_t global = slot_target_rva(base, last[baseReg]);
      if (global == 0) continue;
      unsigned gb = 0, ge = 0;
      const bool globalInData =
          ek_image_data_extent(base, (unsigned)global, &gb, &ge);
      if (!globalInData) continue;
      if (src == out->cosReg) {
        if (out->cCount < EK_CW_TANG_MAX) out->cCand[out->cCount] = (unsigned)global;
        else out->capped = 1;
        out->cCount++;
      }
      if (src == out->sinReg) {
        if (out->sCount < EK_CW_TANG_MAX) out->sCand[out->sCount] = (unsigned)global;
        else out->capped = 1;
        out->sCount++;
      }
    }

    const int nc = out->cCount < EK_CW_TANG_MAX ? out->cCount : EK_CW_TANG_MAX;
    const int ns = out->sCount < EK_CW_TANG_MAX ? out->sCount : EK_CW_TANG_MAX;
    for (int i = 0; i < nc; i++) {
      for (int j = 0; j < ns; j++) {
        const unsigned c = out->cCand[i], s = out->sCand[j];
        const unsigned hiRva = c > s ? c : s;
        const unsigned loRva = c > s ? s : c;
        if (hiRva - loRva != 4u) continue;
        if (out->pairs == 0) { out->cRva = c; out->sRva = s; }
        out->pairs++;
      }
    }

    if (out->cRva != 0 && out->sRva != 0) {
      const unsigned hiRva = out->cRva > out->sRva ? out->cRva : out->sRva;
      unsigned hb = 0, he = 0;
      const bool haveExtent = ek_image_data_extent(base, hiRva, &hb, &he);
      out->dataEnd = haveExtent ? he : 0u;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// Non-gating: a refusal costs stage C its viewport clip; the wall quads keep painting as today.
static void cw_locate_tangs(const unsigned char* base, unsigned roomsB,
                            unsigned roomsE) {
  if (roomsB == 0 || roomsE <= roomsB + 16) {
    ek_log("cutaway tangs: no extent for polymost_drawrooms, so gctang and "
           "gstang are not looked for and stage C's wall quads are painted "
           "unclipped against the viewport, exactly as they are today");
    return;
  }

  CwTangs t;
  memset(&t, 0, sizeof t);
  if (!cw_readable(base + roomsB, (size_t)(roomsE - roomsB))) {
    ek_log("cutaway tangs: polymost_drawrooms' own bytes at rva "
           "0x%07X..0x%07X were not readable, so the tilt pair is not located",
           roomsB, roomsE);
    return;
  }
  cw_scan_tangs(base, roomsB, roomsE, &t);

  ek_log("cutaway tangs: %d sincos-shaped call site(s) in polymost_drawrooms "
         "(rva 0x%07X..0x%07X); call at 0x%07X with &sin at rsp+0x%X and &cos "
         "at rsp+0x%X, read back in xmm%d and xmm%d (%d and %d read(s)); %d "
         ".001f snap constant(s) in the 0x%X-byte window; %d cos-side and %d "
         "sin-side candidate global(s)%s giving %d pair(s) four bytes apart -- "
         "gctang 0x%07X, gstang 0x%07X, section ends at 0x%07X%s",
         t.sites, roomsB, roomsE, t.callRva, (unsigned)t.dSin, (unsigned)t.dCos,
         t.sinReg, t.cosReg, t.sinReads, t.cosReads, t.snapConsts,
         EK_CUTAWAY_TANG_WINDOW, t.cCount, t.sCount, t.capped ? " (CAPPED)" : "",
         t.pairs, t.cRva, t.sRva, t.dataEnd,
         t.faulted ? " -- A SCAN FAULTED" : "");

  if (t.faulted || t.capped ||
      !ekm_cutaway_tangs_ok(t.sites, t.pairs, t.snapConsts, t.cRva, t.sRva,
                            t.dataEnd)) {
    ek_log("cutaway tangs: REFUSED -- the tilt pair needs EXACTLY one sincos "
           "call site, exactly one .001f snap constant in its window and "
           "exactly one pairing of two floats FOUR bytes apart, which is what "
           "polymost.cpp:95's `float gchang, gshang, gctang, gstang` and "
           ":6923-6929 look like together. Stage C paints its wall quads "
           "unclipped against the viewport, exactly as it does today: the "
           "stretching is cosmetic, and a clip rotated by the wrong one of "
           "these two would remove geometry the player should see");
    return;
  }

  InterlockedExchange(&s_tangC, (LONG)t.cRva);
  InterlockedExchange(&s_tangS, (LONG)t.sRva);
  ek_log("cutaway tangs: gctang is the float at rva 0x%07X and gstang the one "
         "at rva 0x%07X. WHICH IS WHICH IS NOT A GUESS: gcc compiles "
         "polymost.cpp:6923-6924 as one sincosf(gtang, &sin, &cos), so the "
         "value from the r8 out-slot is the COSINE and the one from the rdx "
         "slot is the SINE, and :6926's own `Bfabsf(gstang) < .001f` is the "
         "compare in the same window that the sine feeds",
         t.cRva, t.sRva);
  ek_log("cutaway tangs: (the two rvas above are 0x1123B8C and 0x1123B88 in the "
         "synthesis this was written against, with the sincos call at 0x0157473 "
         "-- an EXPECTED value for this line and never a constant anything "
         "reads)");
}

// The tilt pair's read, render thread: cutaway_xbounds' split (one VirtualQuery pair per session, values read every
// call; the engine rewrites them at polymost.cpp:6923-6933 and :7450-7455).
static const volatile float* s_tangPtrC = NULL;
static const volatile float* s_tangPtrS = NULL;
static bool s_tangSettled = false;
static bool s_saidTangInert = false;
static bool s_saidTangUnreadable = false;

bool cutaway_tangs(float* ctang, float* stang) {
  if (ctang == NULL || stang == NULL) return false;

  if (!s_tangSettled) {
    const unsigned char* const base = s_base;
    if (base == NULL) return false;
    const unsigned c = cutaway_tang_c_rva();
    const unsigned s = cutaway_tang_s_rva();
    if (c == 0 || s == 0) {
      if (!cutaway_located()) return false;
      s_tangSettled = true;
      if (!s_saidTangInert) {
        s_saidTangInert = true;
        ek_log("cutaway tangs: INERT -- the locate published no tilt pair, so "
               "stage C paints every wall quad unclipped against the viewport, "
               "exactly as it did before this existed");
      }
      return false;
    }
    s_tangSettled = true;
    if (!cw_readable((const void*)(base + c), 4) ||
        !cw_readable((const void*)(base + s), 4)) {
      if (!s_saidTangUnreadable) {
        s_saidTangUnreadable = true;
        ek_log("cutaway tangs: the floats at rva 0x%07X and 0x%07X are not "
               "readable from the render thread, so no wall quad is clipped "
               "against the viewport for the rest of this session and "
               "everything else keeps running", c, s);
      }
      return false;
    }
    s_tangPtrC = (const volatile float*)(base + c);
    s_tangPtrS = (const volatile float*)(base + s);
  }

  if (s_tangPtrC == NULL || s_tangPtrS == NULL) return false;

  const float vc = *s_tangPtrC;
  const float vs = *s_tangPtrS;
  // The pair checks itself: cosine and sine of ONE angle, so the sum of squares is one. A positive comparison, so a NaN
  // refuses.
  const double unit = (double)vc * (double)vc + (double)vs * (double)vs;
  if (!(unit > 0.999 && unit < 1.001)) return false;
  *ctang = vc;
  *stang = vs;
  return true;
}

// The spike's write. The engine writes this global itself (yax_drawrooms clears it at engine.cpp:1131 and restores it
// at :1251) around exactly what the spike does.
static bool s_zcDead = false;
static bool s_zcSaidDead = false;

bool cutaway_zclear_suspend(int* saved) {
  if (saved == NULL) return false;
  *saved = 1;
  if (s_zcDead) return false;

  const unsigned char* const base = s_base;
  const unsigned rva = cutaway_zclear_rva();
  if (base == NULL || rva == 0) return false;

  // Re-validated here, not trusted from the locate (sampler thread, possibly minutes ago): one VirtualQuery on an armed
  // frame.
  volatile int* const flag = (volatile int*)(base + rva);
  if (!cw_readable((const void*)flag, 4) || !cw_writable((const void*)flag, 4)) {
    s_zcDead = true;
    if (!s_zcSaidDead) {
      s_zcSaidDead = true;
      ek_log("cutaway zspike: yax_polymostclearzbuffer at rva 0x%07X is not "
             "readable and writable, so no second drawrooms pass runs for the "
             "rest of this session and nothing was written. Everything else "
             "keeps running", rva);
    }
    return false;
  }

  *saved = *flag;
  *flag = 0;
  return true;
}

// Unguarded on purpose: suspend proved these bytes writable microseconds ago, and a refusing VirtualQuery would leave
// the depth clear off for every later draw. A restore must not be able to decline.
void cutaway_zclear_restore(int saved) {
  const unsigned char* const base = s_base;
  const unsigned rva = cutaway_zclear_rva();
  if (base == NULL || rva == 0) return;
  *(volatile int*)(base + rva) = saved;
}

// g_nodraw's locate: two independent sites must name one .refptr slot (0x05A2050 -> rva 0x103E1C0) and domost must load
// it too. drawrooms: store of 1 at 0x015874D (:6993), call at 0x01587D2 (:6999); drawalls: store 1 at 0x01B0E14, store
// 0 at 0x01B0E44 (:5870-5873); domost: 8 loads, one per `if (g_nodraw)` (:4140-4318).

// `mov dword ptr [r64], imm32` (C7 /0, optional REX.B); returns the base register or -1 and the length in *len.
static int cw_store_imm(const unsigned char* base, unsigned p, unsigned e,
                        int imm, unsigned* len) {
  unsigned at = p;
  int rexb = 0;
  if (at < e && base[at] == 0x41) { rexb = 1; at++; }
  if (at + 6 > e || base[at] != 0xC7) return -1;
  const unsigned char m = base[at + 1];
  if ((m >> 6) != 0 || ((m >> 3) & 7) != 0) return -1;
  if ((m & 7) == 4 || (m & 7) == 5) return -1;
  if (*(const int*)(base + at + 2) != imm) return -1;
  *len = (at + 6) - p;
  return (int)((m & 7) | (rexb << 3));
}

// The slot of the nearest .refptr load into `reg` ending before `store`, within EK_CUTAWAY_ND_LOAD_GAP and above `lo`.
// A stale pairing is possible in a byte scan; the three-way agreement refuses it.
static unsigned cw_slot_loaded_before(const unsigned char* base, unsigned lo,
                                      unsigned store, int reg) {
  if (store < lo + 7) return 0;
  const unsigned floor = store - lo > EK_CUTAWAY_ND_LOAD_GAP
                             ? store - EK_CUTAWAY_ND_LOAD_GAP : lo;
  for (unsigned q = store - 7; ; q--) {
    unsigned slot = 0;
    if (cw_refptr_load(base, q, store, &slot) == reg) return slot;
    if (q <= floor) break;
  }
  return 0;
}

struct CwNodraw {
  unsigned roomsCall;
  int roomsStores;
  unsigned roomsStore, roomsSlot;
  int allsCalls;
  int allsBrackets;
  int allsDistinct;
  unsigned allsSlot, allsCall, allsSet, allsClear, allsCall2;
  int domostReads;
  int faulted;
};

static void cw_scan_nodraw_rooms(const unsigned char* base, unsigned roomsB,
                                 unsigned roomsE, unsigned domostB,
                                 CwNodraw* out) {
  CwCalls calls;
  cw_count_calls_in(base, roomsB, roomsE, domostB, &calls);
  if (calls.faulted) { out->faulted = 1; return; }
  if (calls.sites < 1) return;
  const unsigned c = calls.first;
  const unsigned lo = c - roomsB > EK_CUTAWAY_ND_ROOMS_WINDOW
                          ? c - EK_CUTAWAY_ND_ROOMS_WINDOW : roomsB;
  out->roomsCall = c;
  __try {
    for (unsigned p = lo; p < c; p++) {
      unsigned len = 0;
      const int reg = cw_store_imm(base, p, c, 1, &len);
      if (reg < 0) continue;
      out->roomsStores++;
      out->roomsStore = p;
      out->roomsSlot = cw_slot_loaded_before(base, roomsB, p, reg);
      p += len - 1;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
  if (out->roomsStores != 1) out->roomsSlot = 0;
}

static void cw_scan_nodraw_alls(const unsigned char* base, unsigned allsB,
                                unsigned allsE, unsigned domostB,
                                CwNodraw* out) {
  if (allsE <= allsB + 5) return;
  if (!cw_readable(base + allsB, (size_t)(allsE - allsB))) {
    out->faulted = 1;
    return;
  }
  __try {
    for (unsigned c = allsB; c + 5 <= allsE; c++) {
      if (base[c] != 0xE8) continue;
      if (ekm_us_rel32_target(c, *(const int*)(base + c + 1)) != domostB) continue;
      out->allsCalls++;

      // The LAST store of 1 before the call and the FIRST store of 0 after it: the nearest pair is the bracket.
      const unsigned lo = c - allsB > EK_CUTAWAY_ND_ALLS_WINDOW
                              ? c - EK_CUTAWAY_ND_ALLS_WINDOW : allsB;
      unsigned setAt = 0;
      int setReg = -1;
      for (unsigned p = lo; p < c; p++) {
        unsigned len = 0;
        const int reg = cw_store_imm(base, p, c, 1, &len);
        if (reg < 0) continue;
        setAt = p;
        setReg = reg;
        p += len - 1;
      }
      if (setReg < 0) continue;
      unsigned hi = c + 5 + EK_CUTAWAY_ND_ALLS_WINDOW;
      if (hi > allsE) hi = allsE;
      unsigned clrAt = 0;
      int clrReg = -1;
      for (unsigned p = c + 5; p < hi; p++) {
        unsigned len = 0;
        const int reg = cw_store_imm(base, p, hi, 0, &len);
        if (reg >= 0) { clrAt = p; clrReg = reg; break; }
      }
      if (clrReg < 0) continue;

      const unsigned setSlot = cw_slot_loaded_before(base, allsB, setAt, setReg);
      const unsigned clrSlot = cw_slot_loaded_before(base, c + 5, clrAt, clrReg);
      if (setSlot == 0 || setSlot != clrSlot) continue;
      out->allsBrackets++;
      if (out->allsBrackets == 1) {
        out->allsSlot = setSlot;
        out->allsCall = c;
        out->allsSet = setAt;
        out->allsClear = clrAt;
        out->allsDistinct = 1;
      } else {
        if (out->allsBrackets == 2) out->allsCall2 = c;
        if (setSlot != out->allsSlot) out->allsDistinct = 2;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

static void cw_count_nodraw_reads(const unsigned char* base, unsigned domostB,
                                  unsigned domostE, unsigned slot,
                                  CwNodraw* out) {
  if (domostE <= domostB + 7 ||
      !cw_readable(base + domostB, (size_t)(domostE - domostB))) {
    out->faulted = 1;
    return;
  }
  __try {
    for (unsigned q = domostB; q + 7 <= domostE; q++) {
      unsigned s = 0;
      if (cw_refptr_load(base, q, domostE, &s) >= 0 && s == slot)
        out->domostReads++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// Non-gating: a refusal costs the no-draw forward only.
static void cw_locate_nodraw(const unsigned char* base, unsigned domostB,
                             unsigned domostE, unsigned roomsB, unsigned roomsE,
                             unsigned allsB, unsigned allsE) {
  CwNodraw n;
  memset(&n, 0, sizeof n);
  if (roomsB != 0) cw_scan_nodraw_rooms(base, roomsB, roomsE, domostB, &n);
  if (!n.faulted && allsB != 0) cw_scan_nodraw_alls(base, allsB, allsE, domostB, &n);
  const unsigned cand = n.roomsSlot != 0 ? n.roomsSlot : n.allsSlot;
  if (!n.faulted && cand != 0) cw_count_nodraw_reads(base, domostB, domostE, cand, &n);

  const unsigned slot =
      n.faulted ? 0u
                : ekm_cutaway_nodraw_slot(n.roomsSlot, n.allsSlot,
                                          n.allsDistinct, n.domostReads);

  unsigned rva = 0, dataB = 0, dataE = 0;
  if (slot != 0) {
    unsigned sb = 0, se = 0;
    const bool slotInData = ek_image_data_extent(base, slot, &sb, &se);
    if (slotInData) rva = (unsigned)slot_target_rva(base, slot);
  }
  if (rva != 0) {
    const bool haveExtent = ek_image_data_extent(base, rva, &dataB, &dataE);
    if (!haveExtent) { dataB = 0; dataE = 0; }
  }
  int value = 0;
  bool canRead = false;
  if (rva != 0 && dataE > rva && dataE - rva >= 4u && cw_readable(base + rva, 4)) {
    value = *(const volatile int*)(base + rva);
    canRead = true;
  }

  if (!canRead) {
    ek_log("cutaway nodraw: REFUSED -- g_nodraw needs polymost_drawrooms' one "
           "store of 1 before its first domost call and polymost_drawalls' 1/0 "
           "brackets around domost to name ONE .refptr slot, which domost's own "
           "body loads, resolving to a readable int32 in a data section. This "
           "build gives: drawrooms %d store(s) before the call at 0x%07X, slot "
           "0x%07X; drawalls %d of %d domost call(s) bracketed, %d distinct "
           "slot(s), slot 0x%07X; %d domost load(s); global rva 0x%07X%s. Every "
           "domost call behaves exactly as before",
           n.roomsStores, n.roomsCall, n.roomsSlot, n.allsBrackets, n.allsCalls,
           n.allsDistinct, n.allsSlot, n.domostReads, rva,
           n.faulted ? " -- A SCAN FAULTED" : "");
    return;
  }

  InterlockedExchange(&s_nodrawStoreRva, (LONG)n.roomsStore);
  InterlockedExchange(&s_nodrawRva, (LONG)rva);
  ek_log("cutaway nodraw: g_nodraw is the int32 at rva 0x%07X (reads %d now), "
         "named by the .refptr slot at rva 0x%07X. Site 1, polymost_drawrooms: "
         "%d store of 1 at rva 0x%07X before its first domost call at 0x%07X "
         "(polymost.cpp:6993). Site 2, polymost_drawalls: %d of %d domost "
         "call(s) bracketed by a store of 1 and of 0 through %d slot, first at "
         "0x%07X (set 0x%07X, clear 0x%07X), second at 0x%07X "
         "(:5870-5873, :6290-6293). polymost_domost's own body loads the slot "
         "%d time(s), one per `if (g_nodraw)`",
         rva, value, slot, n.roomsStores, n.roomsStore, n.roomsCall,
         n.allsBrackets, n.allsCalls, n.allsDistinct, n.allsCall, n.allsSet,
         n.allsClear, n.allsCall2, n.domostReads);
}

static CwVerdict locate_now(const unsigned char* base) {
  unsigned domostB = 0, domostE = 0, polyB = 0, polyE = 0;
  unsigned allsB = 0, allsE = 0, roomsB = 0, roomsE = 0;

  // The two this stage publishes gate it.
  CwVerdict v = cw_locate_one(base, CW_DOMOST, &domostB, &domostE);
  if (v != CW_OK) return v;
  v = cw_locate_one(base, CW_DRAWPOLY, &polyB, &polyE);
  if (v != CW_OK) return v;

  // The other two do not gate: nothing is published for drawalls or drawrooms here, so a refusal costs only its check,
  // and taking domost down would be the over-wide refusal nocull.cpp argues against. A FAULT still spends the budget.
  v = cw_locate_one(base, CW_DRAWALLS, &allsB, &allsE);
  if (v == CW_FAULTED) return v;
  v = cw_locate_one(base, CW_DRAWROOMS, &roomsB, &roomsE);
  if (v == CW_FAULTED) return v;

  // Stage B's anchor, non-gating: without it the cutaway cannot name the sector a flat call paints and forwards every
  // call, today's picture.
  unsigned nonparB = 0, nonparE = 0;
  v = cw_locate_one(base, CW_NONPARALLAX, &nonparB, &nonparE);
  if (v == CW_FAULTED) return v;
  if (nonparB != 0) {
    InterlockedExchange(&s_nonparRva, (LONG)nonparB);
    InterlockedExchange(&s_nonparEnd, (LONG)nonparE);
  }

  // polymost_drawalls, published for ONE measurement: how many times the engine calls it, the unmeasured link between
  // 325 sectors swept / 652 bunches and ~300 domost calls / 24 planes painted.
  if (allsB != 0) {
    InterlockedExchange(&s_allsRva, (LONG)allsB);
    InterlockedExchange(&s_allsEnd, (LONG)allsE);
  }

  // The masked-wall path, non-gating: without it nothing is counted or suppressed.
  unsigned maskB = 0, maskE = 0;
  v = cw_locate_one(base, CW_MASKWALL, &maskB, &maskE);
  if (v == CW_FAULTED) return v;
  if (maskB != 0) {
    InterlockedExchange(&s_maskRva, (LONG)maskB);
    InterlockedExchange(&s_maskEnd, (LONG)maskE);
  }

  cw_corroborate_with_nocull(base, roomsB, roomsE, allsB, allsE);

  // Published before the spike's locate and even if the spike then refuses: the extent is a corroborated fact, 0
  // exactly when the anchor was ambiguous.
  cw_locate_zclear(base, roomsB, roomsE);

  // Needs only the two gated extents, so an ambiguous drawrooms cannot starve it.
  cw_locate_dpm(base, domostB, domostE, polyB);

  // Stage C's x-clamp bounds, non-gating: without them the wall quads paint unclamped, today's picture.
  cw_locate_xbounds(base, domostB, domostE);

  // Stage C's viewport clip needs the tilt pair, which polymost_drawrooms ASSIGNS (drawpoly does not read it here,
  // SOFTROTMAT is 0, cutaway.h). Non-gating.
  cw_locate_tangs(base, roomsB, roomsE);

  // g_nodraw, so a TROR no-draw pass reaches the engine's own body. Non-gating.
  cw_locate_nodraw(base, domostB, domostE, roomsB, roomsE, allsB, allsE);

  // The third corroboration: polymost_drawrooms calls polymost_domost exactly twice.
  if (roomsB == 0) {
    ek_log("cutaway: the domost call-count corroboration cannot run -- this "
           "locate has no extent for polymost_drawrooms to count inside");
  } else {
    CwCalls calls;
    cw_count_calls_in(base, roomsB, roomsE, domostB, &calls);
    if (calls.faulted)
      ek_log("cutaway: the domost call-count corroboration is unavailable -- "
             "polymost_drawrooms' own bytes at rva 0x%07X..0x%07X were not "
             "readable or faulted mid-scan", roomsB, roomsE);
    else if (calls.sites == 2)
      ek_log("cutaway: CORROBORATED -- polymost_drawrooms calls polymost_domost "
             "at rva 0x%07X exactly 2 time(s), from rva 0x%07X and 0x%07X, "
             "which is the TROR claim replay at polymost.cpp:6999 and :7006",
             domostB, calls.first, calls.second);
    else
      ek_log("cutaway: !! the domost call-count corroboration DISAGREES -- "
             "polymost_drawrooms at rva 0x%07X..0x%07X calls the rva 0x%07X "
             "this locate names domost %d time(s), where polymost.cpp:6999 and "
             ":7006 are exactly 2. Not a refusal: a synthesis may inline or "
             "fold one of them, but a count far from 2 means one of the two "
             "anchors named the wrong function",
             roomsB, roomsE, domostB, calls.sites);
  }

  // The span list and what a reopen would cost. Non-gating, as the locate above; a FAULT still spends the budget.
  CwVsp vsp;
  cw_find_vsp(base, domostB, domostE, &vsp);
  if (vsp.faulted) {
    ek_log("cutaway: the vsp[] scan over polymost_domost at rva "
           "0x%07X..0x%07X faulted; the span list is not located this pass",
           domostB, domostE);
    return CW_FAULTED;
  }

  ek_log("cutaway: vsp[] scan -- %d `shl reg,5` site(s) (x32 is sizeof(vsptyp)), "
         "%d 16-bit access(es) at [base+idx+0x%X] across %d base register(s), "
         "%d rip-relative load(s) into it (first at rva 0x%07X), slot rva "
         "0x%07X, array rva 0x%07X, .bss ends at rva 0x%07X",
         vsp.scaleSites, vsp.linkSites, EK_CUTAWAY_VSP_LINK, vsp.baseRegs,
         vsp.loads, vsp.loadRva, vsp.slot, vsp.rva, vsp.bssEnd);

  if (!ekm_cutaway_vsp_ok(vsp.rva, vsp.bssEnd, vsp.scaleSites, vsp.linkSites,
                          vsp.baseRegs, vsp.loads)) {
    ek_log("cutaway: vsp[] REFUSED -- it needs a base in uninitialised data with "
           "room for all %u bytes, at least one x32 index, at least one 16-bit "
           "+0x%X access and exactly one base register set by exactly one load; "
           "this build gives rva 0x%07X, .bss end 0x%07X, %d, %d, %d and %d. "
           "Nothing downstream may price a reopen and nothing picks a winner",
           EK_CUTAWAY_VSP_BYTES, EK_CUTAWAY_VSP_LINK, vsp.rva, vsp.bssEnd,
           vsp.scaleSites, vsp.linkSites, vsp.baseRegs, vsp.loads);
  } else {
    ek_log("cutaway: vsp[] is the %u-byte array at rva 0x%07X..0x%07X -- %d "
           "entr(ies) of %u bytes -- reached from polymost_domost's one "
           "rip-relative load at rva 0x%07X, %s rva 0x%07X, and confirmed by %d "
           "16-bit access(es) at +0x%X, which is where vsptyp's n/p/ctag/ftag "
           "begin (polymost.cpp:60-62)",
           EK_CUTAWAY_VSP_BYTES, vsp.rva, vsp.rva + EK_CUTAWAY_VSP_BYTES,
           EK_CUTAWAY_VSPMAX, EK_CUTAWAY_VSP_STRIDE, vsp.loadRva,
           vsp.slot != 0 ? "through the mingw .refptr slot at"
                         : "which named the array outright at",
           vsp.slot != 0 ? vsp.slot : vsp.rva,
           vsp.linkSites, EK_CUTAWAY_VSP_LINK);
    InterlockedExchange(&s_vspRva, (LONG)vsp.rva);
    InterlockedExchange(&s_vspSlot, (LONG)vsp.slot);
    cw_initmosts_verdict(base, vsp.rva, vsp.slot, roomsB, roomsE);
  }

  InterlockedExchange(&s_domostRva, (LONG)domostB);
  InterlockedExchange(&s_domostEnd, (LONG)domostE);
  InterlockedExchange(&s_drawpolyRva, (LONG)polyB);
  InterlockedExchange(&s_drawpolyEnd, (LONG)polyE);
  return CW_OK;
}

void cutaway_locate(void) {
  if (InterlockedCompareExchange(&s_state, 0, 0) != 0) return;

  // s_base is written by the game thread; a NULL here costs nothing (the sampler returns a second later) and no attempt
  // is charged.
  const unsigned char* const base = s_base;
  if (base == NULL) return;
  if (host_image_size(base) == 0) {
    ek_log("cutaway: the host image's PE headers did not parse; the cutaway "
           "stays unavailable");
    InterlockedExchange(&s_state, 2);
    cutaway_mark_unavailable();
    return;
  }

  const CwVerdict v = locate_now(base);
  if (v == CW_OK) {
    InterlockedExchange(&s_state, 1);
    ek_log("cutaway: LOCATED. Nothing is hooked, nothing is redirected and no "
           "engine memory is written -- pass 1 only publishes what pass 3 "
           "will need");
    return;
  }
  // A non-fault refusal is permanent: the image does not change while the game runs.
  if (v == CW_REFUSED) {
    InterlockedExchange(&s_state, 2);
    cutaway_mark_unavailable();
    return;
  }
  const LONG n = InterlockedIncrement(&s_attempts);
  if (n >= EK_CUTAWAY_ATTEMPTS) {
    ek_log("cutaway: giving up after %d faulted attempt(s); the cutaway stays "
           "unavailable for the life of this process", (int)n);
    InterlockedExchange(&s_state, 2);
    cutaway_mark_unavailable();
  }
}

// Step 3: price a reopen. GAME THREAD ONLY. polymost_initmosts runs at polymost.cpp:6986 and the seed
// polymost_scansector at :7078 with nothing between touching vsp[] on the non-TROR path, so nocull.cpp's seed thunk
// reads the list as initialisation left it. Reads and logs only.

static unsigned s_measFrames = 0;
static unsigned s_measAgreed = 0;
static unsigned s_measDisagreed = 0;
static int s_liveMin = 0;
static int s_liveMax = 0;

static bool s_saidFirst = false;
static bool s_saidDisagreed = false;
static bool s_saidUnreadable = false;

static bool s_measDead = false;

static unsigned s_lastMeasMs = 0;
static bool s_measFirstTick = true;

static bool cw_meas_due(void) {
  const unsigned now = wallclock_ms();
  if (!s_measFirstTick && (now - s_lastMeasMs) < 1000u) return false;
  s_measFirstTick = false;
  s_lastMeasMs = now;
  return true;
}

static void cw_arm_reopen(const unsigned char* vsp, int vcnt);
static void cw_disarm_reopen(void);
static void cw_reopen_frame(bool due);

void cutaway_measure_spans(void) {
  if (s_measDead) return;

  const unsigned char* const base = s_base;
  if (base == NULL) return;
  const unsigned vspRva = cutaway_vsp_rva();
  if (vspRva == 0) return;

  // Re-validated here, not trusted from the publish: one VirtualQuery on an armed frame.
  const unsigned char* const vsp = base + vspRva;
  // Writable too, asked before either is used: the reopen stores into this array. One VirtualQuery answers for the
  // whole 65536 bytes and arms the reopen's pointer.
  if (!cw_readable(vsp, EK_CUTAWAY_VSP_BYTES) ||
      !cw_writable(vsp, EK_CUTAWAY_VSP_BYTES)) {
    s_measDead = true;
    cw_disarm_reopen();
    if (!s_saidUnreadable) {
      s_saidUnreadable = true;
      ek_log("cutaway: vsp[] at rva 0x%07X is not readable and writable for all "
             "%u bytes, so the span measurement gives up for this session and "
             "no reopen is armed. Everything else keeps running",
             vspRva, EK_CUTAWAY_VSP_BYTES);
    }
    return;
  }

  const bool due = g_cfg.debuglog != 0 && cw_meas_due();

  // The per-frame report is also the frame boundary: this seam runs once per armed drawrooms before any domost, so
  // counters are the PREVIOUS frame's totals. It runs before any early return so a refused frame still closes the
  // books.
  cw_reopen_frame(due);

  // The walk is bounded by construction: indices are range-checked and the step count capped at VSPMAX, so a corrupt or
  // mid-paint list ends the walk. vsp[0] is the engine's dummy solid node and the head.
  int walked = 0;
  int maxIndex = 0;
  int i = 0;
  bool closed = false;
  for (int step = 0; step <= EK_CUTAWAY_VSPMAX; step++) {
    walked++;
    if (i > maxIndex) maxIndex = i;
    const int nxt = (int)*(const short*)(
        vsp + (unsigned)i * EK_CUTAWAY_VSP_STRIDE + EK_CUTAWAY_VSP_LINK);
    if (nxt == 0) { closed = true; break; }
    if (nxt < 0 || nxt >= EK_CUTAWAY_VSPMAX) break;
    i = nxt;
  }

  // Two independent readings of one number: vsp[0].p (polymost.cpp:3750 sets vcnt-1) and the dummy node's free-list
  // head (:3754 sets vcnt).
  const int headPrev = (int)*(const short*)(vsp + EK_CUTAWAY_VSP_LINK + 2);
  const int freeHead = (int)*(const short*)(
      vsp + (unsigned)(EK_CUTAWAY_VSPMAX - 1) * EK_CUTAWAY_VSP_STRIDE
      + EK_CUTAWAY_VSP_LINK);

  s_measFrames++;
  const bool agreed =
      closed && ekm_cutaway_spans_agree(walked, maxIndex, headPrev, freeHead);

  if (!agreed) {
    s_measDisagreed++;
    if (!s_saidDisagreed) {
      s_saidDisagreed = true;
      ek_log("cutaway spans: the three readings DISAGREE -- the walk from node 0 "
             "%s after %d node(s) with highest index %d, vsp[0].p is %d and the "
             "free head vsp[%d].n is %d. Expected vcnt, vcnt-1, vcnt-1 and vcnt. "
             "A room-over-room pass paints at polymost.cpp:6999/:7006 BEFORE the "
             "seed call, so this is what such a frame looks like; nothing is "
             "published from it", closed ? "closed" : "ran off",
             walked, maxIndex, headPrev, EK_CUTAWAY_VSPMAX - 1, freeHead);
    }
    return;
  }

  s_measAgreed++;
  if (s_liveMin == 0 || walked < s_liveMin) s_liveMin = walked;
  if (walked > s_liveMax) s_liveMax = walked;
  InterlockedExchange(&s_liveSpans, (LONG)walked);

  // The snapshot only on an agreed reading: this is the one instant vsp[] is pristine, and the agreement says so. A
  // frame that painted before the seed fails it and leaves the previous snapshot, which the reopen's freshness check
  // refuses.
  cw_arm_reopen(vsp, walked);

  const unsigned restoreBytes = (unsigned)walked * EK_CUTAWAY_VSP_STRIDE;

  // The one line this task exists to print, so not behind the debug row: once per session.
  if (!s_saidFirst) {
    s_saidFirst = true;
    ek_log("cutaway spans: %d live entr(ies) after the engine's own "
           "polymost_initmosts, agreed by three readings (walk from node 0, "
           "vsp[0].p = %d, free head vsp[%d].n = %d). A restore of that live "
           "prefix copies %u byte(s), against %u for the whole array -- a factor "
           "of %u. vsp[] is at rva 0x%07X",
           walked, headPrev, EK_CUTAWAY_VSPMAX - 1, freeHead, restoreBytes,
           EK_CUTAWAY_VSP_BYTES, EK_CUTAWAY_VSP_BYTES / restoreBytes, vspRva);
    ek_log("cutaway spans: NOT ESTABLISHED, and deliberately not guessed -- how "
           "many polymost_domost calls a frame makes. Nothing outside domost "
           "counts them and this task hooks nothing, so the per-frame price of a "
           "reopen is %u byte(s) TIMES a number the cutaway's own hook is the first "
           "thing that can supply", restoreBytes);
  }

  if (due)
    ek_log("cutaway spans: %d live this frame (%d..%d over the session); "
           "cumulative %u reading(s), %u agreed, %u disagreed; a live-prefix "
           "restore is %u byte(s) and the whole array is %u",
           walked, s_liveMin, s_liveMax, s_measFrames, s_measAgreed,
           s_measDisagreed, restoreBytes, EK_CUTAWAY_VSP_BYTES);
}

// The reopen hook. GAME THREAD ONLY, and the first thing here that WRITES engine memory (vsp[]), so a mistake is a
// HANG: indices range-checked, walks bounded by VSPMAX+1. The engine never frees an index below vcnt
// (polymost.cpp:6824, :4357), so a reopen frees nodes at or above it (vsdel, :3775-3787) and restores the snapshot.

// The vcnt viewport nodes as initmosts left them; 64 KB of our .bss. Written at the seed seam, read in the domost hook,
// both game thread.
static unsigned char s_snap[EK_CUTAWAY_VSP_BYTES];
static int s_snapVcnt = 0;
static unsigned char* s_vspWrite = NULL;

// Static so the hot path clears only vcnt bytes per domost call.
static unsigned char s_seen[EK_CUTAWAY_VSPMAX];

// Cumulative, never sampled. `s_calls` counts EVERY domost call, gated or not; `s_gated` only those the four-fact gate
// let through.
static unsigned s_calls = 0;
static unsigned s_gated = 0;
static unsigned s_flatSkips = 0;
static unsigned s_skySuppressed = 0;
static unsigned s_painted = 0;
static unsigned s_paintRefused = 0;
static unsigned s_planeNotFlat = 0;
static unsigned s_partsFlatDropped = 0;
static unsigned s_partsWallDropped = 0;
static unsigned s_partsWallForwarded = 0;
// Stage C's two, also in cutpaint.cpp. The arithmetic does NOT close: s_paintRefused rises only inside `flatCall &&
// s_planeSector >= 0 && dpm != NULL`, so sky and drawalls flat spans (no plane) land in s_flatSkips and in NEITHER
// painted nor refused. An invented invariant in a diagnostic turns a correct run into a suspected bug.
static unsigned s_wallPainted = 0;
static unsigned s_wallForwarded = 0;
// The widened tall-wall gate's three, distinct from cutpaint.cpp's s_wallTallRefused (armed only): these count every
// wall call about to be FORWARDED while the camera is active.
static unsigned s_engineTallChecked = 0;
static unsigned s_engineTallRefused = 0;
static unsigned s_engineTallNoProj = 0;
static unsigned s_engineTallNoQuad = 0;
// The degenerate-quad refusal's counters (inversion-only; the enormous term measured dead). The two shape counters are
// DISJOINT (ekm_cut_wall_degenerate_kind returns one), so checked - xEqual - crossed is the well-formed population and
// xEqual + crossed == refused whenever the row is on.
static unsigned s_degenerateChecked = 0;
static unsigned s_degenerateXEqual = 0;
static unsigned s_degenerateCrossed = 0;
static unsigned s_degenerateRefused = 0;
// Gated calls that arrived with g_nodraw set, how many the row forwarded, and which painter each would have reached
// with the row off.
static unsigned s_ndSeen = 0;
static unsigned s_ndForwarded = 0;
static unsigned s_ndWouldB = 0;
static unsigned s_ndWouldC = 0;
static unsigned s_ndSeenAtSeam = 0;
static unsigned s_ndFrameLast = 0;
static unsigned s_ndFrameMax = 0;
static unsigned s_ceilHidden = 0;
static unsigned s_reopens = 0;
static unsigned s_freedNodes = 0;
static unsigned s_refusedStale = 0;
static unsigned s_refusedBroken = 0;
static unsigned s_armedFrames = 0;
static int s_walkMax = 0;
static int s_freedMax = 0;

static unsigned s_callsAtSeam = 0;
static unsigned s_gatedAtSeam = 0;
static unsigned s_reopensAtSeam = 0;
static unsigned s_freedAtSeam = 0;

// A failure logs once and latches. The STALE refusal is deliberately not one: it declines a snapshot that is not this
// list's, and latching off for a transient would be silent death.
static bool s_reopenDead = false;
static bool s_saidNoOrig = false;
static bool s_saidBroken = false;
static bool s_saidStale = false;
static bool s_saidFirstReopen = false;
static bool s_saidFirstFrame = false;
static bool s_saidDpmInert = false;
static bool s_saidNoNonPar = false;

// Backface spike read, render thread (throwaway). ONE VirtualQuery per session: the hottest seam the port has touched,
// and a .bss page does not become unreadable. Inert, the hot path is one pointer test.
static const volatile int* s_dpmPtr = NULL;
static bool s_dpmSettled = false;

static const volatile int* cw_dpm_ptr(void) {
  if (s_dpmSettled) return s_dpmPtr;

  const unsigned char* const base = s_base;
  if (base == NULL) return NULL;
  const unsigned rva = cutaway_dpm_rva();
  if (rva == 0) {
    // Not settled yet is not refused: the locate runs on the sampler and a gated frame can arrive first. Once answered
    // with nothing it settles, keeping the interlocked read off the hot path.
    if (!cutaway_located()) return NULL;
    s_dpmSettled = true;
    if (!s_saidDpmInert) {
      s_saidDpmInert = true;
      ek_log("cutaway bfspike: INERT -- the locate published no "
             "domostpolymethod, so every gated polymost_domost call forwards "
             "to the engine exactly as before and nothing is skipped");
    }
    return NULL;
  }

  s_dpmSettled = true;
  if (!cw_readable((const void*)(base + rva), 4)) {
    ek_log("cutaway bfspike: domostpolymethod at rva 0x%07X is not readable "
           "from the render thread, so nothing is skipped for the rest of this "
           "session and everything else keeps running", rva);
    return NULL;
  }
  s_dpmPtr = (const volatile int*)(base + rva);
  return s_dpmPtr;
}

static const volatile int* s_ndPtr = NULL;
static bool s_ndSettled = false;
static bool s_saidNdInert = false;

static const volatile int* cw_nodraw_ptr(void) {
  if (s_ndSettled) return s_ndPtr;

  const unsigned char* const base = s_base;
  if (base == NULL) return NULL;
  const unsigned rva = cutaway_nodraw_rva();
  if (rva == 0) {
    if (!cutaway_located()) return NULL;
    s_ndSettled = true;
    if (!s_saidNdInert) {
      s_saidNdInert = true;
      ek_log("cutaway nodraw: INERT -- the locate published no g_nodraw, so a "
             "TROR no-draw pass is handled exactly as before this existed");
    }
    return NULL;
  }

  s_ndSettled = true;
  if (!cw_readable((const void*)(base + rva), 4)) {
    ek_log("cutaway nodraw: g_nodraw at rva 0x%07X is not readable from the "
           "render thread, so no no-draw pass is forwarded for the rest of this "
           "session and everything else keeps running", rva);
    return NULL;
  }
  s_ndPtr = (const volatile int*)(base + rva);
  return s_ndPtr;
}

// g_nodraw now, or -1 if unreadable; the rooms detour uses it to tell a TROR draw pass from a no-draw one.
int cutaway_nodraw_value(void) {
  const volatile int* const nd = cw_nodraw_ptr();
  return nd != NULL ? *nd : -1;
}

// &vsp[i].n; [1] is the `p` beside it (polymost.cpp:60-62: n, p, ctag, ftag at +24, +26, +28, +30 of a 32-byte entry).
static short* cw_link(unsigned char* vsp, int i) {
  return (short*)(vsp + (unsigned)i * EK_CUTAWAY_VSP_STRIDE
                  + EK_CUTAWAY_VSP_LINK);
}

static bool cw_index_ok(int i) { return i >= 0 && i < EK_CUTAWAY_VSPMAX; }

// vsdel (polymost.cpp:3775-3787) matched, not reinvented: unlink i from the ring, push it on the free head and fix that
// head's `p` (the doubly-linked ring vsinsaft pops, :3819-3830). Range-checked: the one function here that stores into
// the engine.
static bool cw_vsdel(unsigned char* vsp, int i) {
  short* const li = cw_link(vsp, i);
  const int pi = (int)li[1];
  const int ni = (int)li[0];
  const int head = (int)cw_link(vsp, EK_CUTAWAY_VSPMAX - 1)[0];
  if (!cw_index_ok(pi) || !cw_index_ok(ni) || !cw_index_ok(head)) return false;

  cw_link(vsp, ni)[1] = (short)pi;
  cw_link(vsp, pi)[0] = (short)ni;

  li[0] = (short)head;
  li[1] = (short)(EK_CUTAWAY_VSPMAX - 1);
  cw_link(vsp, head)[1] = (short)i;
  cw_link(vsp, EK_CUTAWAY_VSPMAX - 1)[0] = (short)i;
  return true;
}

enum CwReopen { CW_REOPEN_DONE = 0, CW_REOPEN_STALE = 1, CW_REOPEN_BROKEN = 2 };

static CwReopen cw_reopen_spans(unsigned char* vsp, int vcnt, int* walkedOut,
                                int* freedOut) {
  *walkedOut = 0;
  *freedOut = 0;

  // 1 of 4: is the snapshot still this list's? Compare the viewport nodes' x as raw bits (index 0 skipped, :6749).
  // initmosts can re-run between snapshot and use (:6936-6947), and a node's x is NOT written once (:4391-4398), so a
  // false STALE is reachable. It fails closed; if the counter fires often, compare vcnt directly.
  for (int i = 1; i < vcnt; i++) {
    const unsigned off = (unsigned)i * EK_CUTAWAY_VSP_STRIDE;
    if (*(const unsigned*)(vsp + off) != *(const unsigned*)(s_snap + off))
      return CW_REOPEN_STALE;
  }

  // 2 of 4: the walk, bounded by construction (indices range-checked, steps capped at VSPMAX+1).
  for (int k = 0; k < vcnt; k++) s_seen[k] = 0;
  int walked = 0, prefixSeen = 0, i = 0;
  bool closed = false, linksOk = true;
  bool sawSentinel = false;
  for (int step = 0; step <= EK_CUTAWAY_VSPMAX; step++) {
    short* const li = cw_link(vsp, i);
    const int nxt = (int)li[0];
    const int prv = (int)li[1];
    walked++;
    if (i < vcnt && !s_seen[i]) { s_seen[i] = 1; prefixSeen++; }
    if (i == EK_CUTAWAY_VSPMAX - 1) sawSentinel = true;
    if (!cw_index_ok(prv)) { linksOk = false; break; }
    if (nxt == 0) { closed = true; break; }
    if (!cw_index_ok(nxt)) { linksOk = false; break; }
    i = nxt;
  }
  *walkedOut = walked;
  if (!linksOk) return CW_REOPEN_BROKEN;
  if (!ekm_cutaway_reopen_ok(closed, walked, prefixSeen, vcnt, sawSentinel))
    return CW_REOPEN_BROKEN;

  // 3 of 4: free every node the engine could have freed. `nxt` is read BEFORE the delete (vsdel rewrites `n`). `ok`
  // starts false and only reaching the head sets it, so running the step cap out is a refusal (unreachable today, and
  // an unreachable success is what an edit turns into a corrupt list).
  int freed = 0;
  int cur = 0;
  bool ok = false;
  for (int step = 0; step <= EK_CUTAWAY_VSPMAX; step++) {
    const int nxt = (int)cw_link(vsp, cur)[0];
    if (!cw_index_ok(nxt)) break;
    if (ekm_cutaway_freeable(cur, vcnt)) {
      if (!cw_vsdel(vsp, cur)) break;
      freed++;
    }
    if (nxt == 0) { ok = true; break; }
    cur = nxt;
  }
  *freedOut = freed;

  // 4 of 4: the restore runs even when the pass above gave up: a ring rebuilt from the snapshot beats one
  // half-unlinked. The verdict is still BROKEN, so it latches off.
  memcpy(vsp, s_snap, (size_t)vcnt * EK_CUTAWAY_VSP_STRIDE);
  return ok ? CW_REOPEN_DONE : CW_REOPEN_BROKEN;
}

static void cw_arm_reopen(const unsigned char* vsp, int vcnt) {
  if (vcnt < 2 || vcnt > EK_CUTAWAY_VSPMAX) { cw_disarm_reopen(); return; }
  memcpy(s_snap, vsp, (size_t)vcnt * EK_CUTAWAY_VSP_STRIDE);
  s_snapVcnt = vcnt;
  s_vspWrite = (unsigned char*)vsp;
}

static void cw_disarm_reopen(void) {
  s_snapVcnt = 0;
  s_vspWrite = NULL;
}

static void cw_reopen_frame(bool due) {
  // The spike's line sits ABOVE the early return: with the reopen off `reopens` is 0 and s_armedFrames never rises.
  // Cumulative; `due` only throttles printing. It carries DOMOST CALLS PER FRAME, the number stage C's prediction (71
  // toward thousands) is stated in.
  const unsigned calls = s_calls - s_callsAtSeam;
  const unsigned gated = s_gated - s_gatedAtSeam;
  const unsigned reopens = s_reopens - s_reopensAtSeam;
  const unsigned freed = s_freedNodes - s_freedAtSeam;
  s_callsAtSeam = s_calls;
  s_gatedAtSeam = s_gated;
  s_reopensAtSeam = s_reopens;
  s_freedAtSeam = s_freedNodes;
  s_ndFrameLast = s_ndSeen - s_ndSeenAtSeam;
  s_ndSeenAtSeam = s_ndSeen;
  if (s_ndFrameLast > s_ndFrameMax) s_ndFrameMax = s_ndFrameLast;

  if (due && s_gated != 0) {
    ek_log("cutaway domost: %u gated call(s) of %u in all this session, %u of "
           "them flats; STAGE B painted %u of those ourselves and forwarded %u "
           "refused, %u DROPPED by the Cutaway parts row; STAGE C painted %u "
           "wall(s) and forwarded %u refused, %u DROPPED by the Cutaway parts "
           "row, %u FORWARDED by the Cutaway parts row; %u call(s) carried a "
           "plane but were not flat (expected 0); the Cutaway sky row "
           "suppressed %u no-plane flat call(s); the Ceilings row (%s) HID %u "
           "ceiling call(s); camera frames %u inside, %u above, %u below, %u "
           "mixed (between TROR levels, treated as inside), %u sector(s) "
           "unreadable; classification %u us last frame, most %u us",
           s_gated, s_calls, s_flatSkips, s_painted, s_paintRefused,
           s_partsFlatDropped, s_wallPainted, s_wallForwarded,
           s_partsWallDropped, s_partsWallForwarded, s_planeNotFlat,
           s_skySuppressed,
           freecam_hideceilings_name(g_cfg.freecamhideceilings), s_ceilHidden,
           camera_zclass_frames(FREECAM_Z_INSIDE),
           camera_zclass_frames(FREECAM_Z_ABOVE),
           camera_zclass_frames(FREECAM_Z_BELOW),
           camera_zclass_frames(FREECAM_Z_MIXED), camera_zclass_unreadable(),
           camera_zclass_us_last(), camera_zclass_us_max());
    ek_log("cutaway domost: THE ARMED FRAME JUST DRAWN made %u polymost_domost "
           "call(s), %u of them gated. That count is what stage C is checked "
           "against: with nothing claiming the screen, every bunch's walls "
           "should emit, so it rises rather than staying where the claims held "
           "it", calls, gated);
    ek_log("cutaway nodraw (D11): %u gated domost call(s) arrived with "
           "g_nodraw set this session (last armed frame %u, most in one %u); "
           "%u FORWARDED to the engine's own body by the Honour TROR no-draw "
           "row; with the row off %u would have gone to stage B's flat painter "
           "and %u to stage C's wall painter%s",
           s_ndSeen, s_ndFrameLast, s_ndFrameMax, s_ndForwarded, s_ndWouldB,
           s_ndWouldC,
           cutaway_nodraw_rva() == 0 ? " -- g_nodraw NOT LOCATED, so none of "
                                       "this can count" : "");
  }
  if (s_armedFrames == 0 && reopens == 0) return;
  s_armedFrames++;

  if (!s_saidFirstFrame && reopens > 0) {
    s_saidFirstFrame = true;
    ek_log("cutaway reopen: the armed frame just drawn made %u polymost_domost "
           "call(s) in all, %u of them through the gate, and reopened the span "
           "list %u time(s), freeing %u node(s). At %d live entr(ies) that is "
           "%u byte(s) restored per reopen and %u byte(s) for the frame",
           calls, gated, reopens, freed, s_snapVcnt,
           (unsigned)s_snapVcnt * EK_CUTAWAY_VSP_STRIDE,
           reopens * (unsigned)s_snapVcnt * EK_CUTAWAY_VSP_STRIDE);
  }

  if (due)
    ek_log("cutaway reopen: last armed frame %u domost call(s), %u gated, %u "
           "reopen(s), %u node(s) freed; cumulative %u call(s), %u gated, %u "
           "reopen(s), %u freed over %u armed frame(s); worst ring %d node(s) "
           "and worst free %d node(s); %u stale refusal(s), %u broken "
           "refusal(s)%s; SPIKE walls-only: %u of %u gated call(s) were FLATS "
           "and were left to the engine's own claims",
           calls, gated, reopens, freed, s_calls, s_gated, s_reopens,
           s_freedNodes, s_armedFrames, s_walkMax, s_freedMax, s_refusedStale,
           s_refusedBroken, s_reopenDead ? " -- LATCHED OFF" : "",
           s_flatSkips, s_gated);
}

// The hook: polymost_domost(float x0, y0, x1, y1, y0top, y0bot, y1top, y1bot) (polymost.cpp:3925): four floats in
// xmm0-xmm3, four on the stack (Microsoft x64), read from the image (the entry loads arguments five to eight from
// [entry rsp + 0x28..0x40]); it is `static`, so not assumed.
typedef void (*DomostFn)(float, float, float, float, float, float, float, float);
static DomostFn s_origDomost = NULL;
static EkHook s_domostHook;

// Stage B: polymost_internal_nonparallaxed(..., int32_t sectnum) (polymost.cpp:4801); the ninth argument is at entry
// rsp + 0x48 per the image, the vec2f_t as opaque rcx/rdx words. It makes exactly ONE domost call (:5008, :5014), so a
// captured plane belongs to the next flat call; cleared on exit so the sky is never mistaken for one.
typedef void (*NonParFn)(unsigned long long, unsigned long long, float, float,
                         float, float, float, float, int);
static NonParFn s_origNonPar = NULL;
static EkHook s_nonparHook;

// polymost_drawalls, COUNTED ONLY: `static void polymost_drawalls(int32_t bunch)` (polymost.cpp:5351). One increment
// and forward, ungated. ABI from the image: entry rva 0x01B0570 reaches `movsxd rbp, ecx` at 0x01B05FD, so the bunch is
// in ecx. :7100 (mirror) and :7153 (`!bunchnodraw`) can only ADD calls.
typedef void (*DrawAllsFn)(int);
static DrawAllsFn s_origDrawAlls = NULL;
static EkHook s_allsHook;
static volatile LONG s_allsCalls = 0;

unsigned cutaway_drawalls_calls(void) { return read_pub(&s_allsCalls); }

static void EK_DrawAllsThunk(int bunch) {
  DrawAllsFn const orig = s_origDrawAlls;
  InterlockedIncrement(&s_allsCalls);
  if (g_cfg.perfzones == 0) {
    if (orig) orig(bunch);
    return;
  }
  const long long thunkIn = perf_stamp();
  const int kindZone = ekm_perf_kind_zone(nocull_pass_kind());
  camera_q16_drawalls();
  const long long in = perf_stamp();
  if (orig) orig(bunch);
  const long long out = perf_stamp();
  camera_q16_alls(in, out);
  const long long d = ekm_perf_span(in, out);
  perf_zone_add(EK_PZ_CUT_DRAWALLS, d, 1);
  for (int z = EK_PZ_CUT_DRAWALLS_PREPARE; z <= EK_PZ_CUT_DRAWALLS_OTHER; z++)
    perf_zone_add(z, z == kindZone ? d : 0, z == kindZone ? 1 : 0);
  const long long thunkOut = perf_stamp();
  perf_zone_add(EK_PZ_CUT_DRAWALLS_THUNK, ekm_perf_span(thunkIn, thunkOut), 1);
}

// The masked-wall path, which nothing here could see: polymost_drawmaskwallinternal (polymost.cpp:7175) makes ZERO
// domost calls, so the bisection row, clamps and tall-wall refusal never touched fences, grates or window panels: a
// candidate for "SOME walls". ABI from the image (docs/re/engine-facts.md); a patch on the entry catches both callers.
typedef void (*MaskWallFn)(int);
static MaskWallFn s_origMaskWall = NULL;
static EkHook s_maskHook;
static volatile LONG s_maskHooked = 0;
static volatile LONG s_maskCalls = 0;
static bool s_saidNoMaskWall = false;

unsigned cutaway_maskwall_calls(void)  { return read_pub(&s_maskCalls); }
bool cutaway_maskwall_hooked(void)     { return read_pub(&s_maskHooked) != 0; }

// Render clock only (the thunk runs in renderDrawMasks, the report in renderDrawRoomsQ16: one draw, one thread). WINDOW
// figures, not session: s_mwWindow* and the tables below clear at each print; the two cumulative totals stay.
static unsigned s_mwTotalGated = 0;
static unsigned s_mwTotalSuppressed = 0;
static unsigned s_mwWindowCalls = 0;
static unsigned s_mwWindowSuppressed = 0;
static unsigned s_mwWindowFrames = 0;
static unsigned s_mwBadIndex = 0;

// A handful with an "other" bucket and no per-value allocation (cutpaint.cpp's method histogram's rule).
static const int CW_MW_HIST_CAP = 8;
static int s_mwHistIndex[CW_MW_HIST_CAP];
static unsigned s_mwHistCount[CW_MW_HIST_CAP];
static int s_mwHistN = 0;
// Sightings past the cap, not distinct indexes.
static unsigned s_mwHistOverflowSightings = 0;

// Fields for the first few DISTINCT walls of the window only; `have*` is per source.
static const int CW_MW_DETAIL_CAP = 4;
struct CwMwDetail {
  int index, picnum, overpicnum, cstat, yrepeat, ypanning;
  int nextsector, ceilz, florz;
  int haveWall, havePanel, haveSector;
};
static CwMwDetail s_mwDetail[CW_MW_DETAIL_CAP];
static int s_mwDetailN = 0;

// The sector is wal->nextsector: polymost.cpp:7239 takes cz[1]/fz[1] from it and :7322 builds the quad from
// csy[1]/fsy[1], so the neighbour's ceiling and floor bound the polygon. wallstate.h exposes it; the wall's OWN sector
// would need a new locate.
static void cw_mw_capture(int wallIndex) {
  if (s_mwDetailN >= CW_MW_DETAIL_CAP) return;
  CwMwDetail* const d = &s_mwDetail[s_mwDetailN];
  memset(d, 0, sizeof *d);
  d->index = wallIndex;
  d->nextsector = -1;

  EkWallFace face;
  const bool haveFace = wallstate_face(wallIndex, &face);
  if (haveFace) {
    d->picnum = face.picnum;
    d->overpicnum = face.overpicnum;
    d->cstat = face.cstat;
    d->nextsector = face.nextsector;
    d->haveWall = 1;
  }

  EkWallPanel panel;
  const bool havePanel = wallstate_panel(wallIndex, &panel);
  if (havePanel) {
    d->yrepeat = panel.yrepeat;
    d->ypanning = panel.ypanning;
    d->havePanel = 1;
  }

  if (haveFace && face.nextsector >= 0 && face.nextsector < EK_MAXSECTORS) {
    EkSectorInfo si;
    const bool haveSect = sectorstate_info(face.nextsector, &si);
    if (haveSect) {
      d->ceilz = si.ceilz;
      d->florz = si.florz;
      d->haveSector = 1;
    }
  }
  s_mwDetailN++;
}

static void cw_mw_tally(int wallIndex) {
  for (int i = 0; i < s_mwHistN; i++) {
    if (s_mwHistIndex[i] == wallIndex) { s_mwHistCount[i]++; return; }
  }
  if (s_mwHistN < CW_MW_HIST_CAP) {
    s_mwHistIndex[s_mwHistN] = wallIndex;
    s_mwHistCount[s_mwHistN] = 1;
    s_mwHistN++;
  } else {
    s_mwHistOverflowSightings++;
  }
  cw_mw_capture(wallIndex);
}

static void EK_MaskWallThunk(int wallIndex) {
  MaskWallFn const orig = s_origMaskWall;
  InterlockedIncrement(&s_maskCalls);
  if (!orig) {
    // hook_install patches the entry (:3339) and s_origMaskWall is assigned only after hook_trampoline succeeds
    // (:3358): a real window if the install ran while the game renders, as for the other three hooks.
    if (!s_saidNoMaskWall) {
      s_saidNoMaskWall = true;
      ek_log("cutaway: the maskwall thunk ran with no original to forward to, "
             "which nothing in this build can produce -- please report it. The "
             "frame draws no masked walls while this lasts");
    }
    return;
  }

  // Deliberately NOT nocull_should_scan's gate: camera_render_armed() is false here, since masked walls are drawn by
  // renderDrawMasks() (game.cpp:1152) AFTER the substituted renderDrawRoomsQ16 (camera.cpp:2362, :2380).
  // camera_active() alone gates. A flying frame whose bracket did not substitute still counts.
  const bool gated = ekm_cutaway_maskwall_should_count(camera_active());
  if (!gated) {
    orig(wallIndex);
    return;
  }

  s_mwTotalGated++;
  s_mwWindowCalls++;
  if (wallIndex < 0 || wallIndex >= EK_WALL_SLOTS) s_mwBadIndex++;
  else cw_mw_tally(wallIndex);

  if (ekm_cutaway_maskwall_should_suppress(gated, g_cfg.freecamcutawaymaskwall != 0)) {
    // Dropped, not forwarded: nothing else draws this wall, so the result is not confounded by the engine redrawing it.
    s_mwTotalSuppressed++;
    s_mwWindowSuppressed++;
    return;
  }
  orig(wallIndex);
}

static unsigned s_mwLastMs = 0;
static bool s_mwFirstTick = true;

static bool cw_mw_due(void) {
  const unsigned now = wallclock_ms();
  if (!s_mwFirstTick && (now - s_mwLastMs) < 1000u) return false;
  s_mwFirstTick = false;
  s_mwLastMs = now;
  return true;
}

// Not behind g_cfg.debuglog, and prints a window of ZERO: zero masked walls drawn while the picture stretches would
// kill this candidate, so the absence must reach the log. Called from nocull.cpp above nocull_should_scan's gate, so it
// checks camera_active() itself.
void cutaway_maskwall_report(void) {
  if (read_pub(&s_maskHooked) == 0) return;
  if (!camera_active()) return;
  s_mwWindowFrames++;
  if (!cw_mw_due()) return;

  char histbuf[224];
  histbuf[0] = '\0';
  int hoff = 0;
  for (int i = 0; i < s_mwHistN; i++) {
    const int room = (int)sizeof(histbuf) - hoff;
    if (room <= 24) break;
    // _TRUNCATE, not sprintf_s: sprintf_s does not truncate, it calls the invalid-parameter handler and aborts.
    const int wrote = _snprintf_s(histbuf + hoff, (size_t)room, _TRUNCATE,
                                  "%s%d:%u", i ? "," : "", s_mwHistIndex[i],
                                  s_mwHistCount[i]);
    if (wrote < 0) break;
    hoff += wrote;
  }
  if (hoff == 0)
    _snprintf_s(histbuf, sizeof histbuf, _TRUNCATE, "none");

  char detbuf[512];
  detbuf[0] = '\0';
  int doff = 0;
  for (int i = 0; i < s_mwDetailN; i++) {
    const CwMwDetail* const d = &s_mwDetail[i];
    const int room = (int)sizeof(detbuf) - doff;
    if (room <= 32) break;
    // Each source says whether it read; 0 for an unread field is the worst of three.
    char panbuf[48], secbuf[64];
    if (d->havePanel)
      _snprintf_s(panbuf, sizeof panbuf, _TRUNCATE, "yrepeat %d ypanning %d",
                  d->yrepeat, d->ypanning);
    else
      _snprintf_s(panbuf, sizeof panbuf, _TRUNCATE,
                  "yrepeat/ypanning unreadable");
    if (d->haveSector)
      _snprintf_s(secbuf, sizeof secbuf, _TRUNCATE, "ceilz %d florz %d",
                  d->ceilz, d->florz);
    else
      _snprintf_s(secbuf, sizeof secbuf, _TRUNCATE, "ceilz/florz unreadable");

    int wrote;
    if (!d->haveWall)
      wrote = _snprintf_s(detbuf + doff, (size_t)room, _TRUNCATE,
                          "%swall %d unreadable, %s", i ? "; " : "", d->index,
                          panbuf);
    else
      wrote = _snprintf_s(detbuf + doff, (size_t)room, _TRUNCATE,
                          "%swall %d picnum %d over %d cstat 0x%X %s, "
                          "nextsector %d %s",
                          i ? "; " : "", d->index, d->picnum, d->overpicnum,
                          (unsigned)d->cstat, panbuf, d->nextsector, secbuf);
    if (wrote < 0) break;
    doff += wrote;
  }
  if (doff == 0)
    _snprintf_s(detbuf, sizeof detbuf, _TRUNCATE, "none");

  ek_log("cutaway maskwall: %u masked wall(s) drawn over the last %u armed "
         "frame(s), %u of them SUPPRESSED by the row; %u distinct wall "
         "index(es) this window -- %s%s; %u index(es) outside wall[]'s "
         "allocation. First few: %s. CUMULATIVE %u gated call(s), %u "
         "suppressed, %u call(s) in all including ungated frames. This path "
         "makes NO polymost_domost call, so nothing else this file measures "
         "has ever seen it",
         s_mwWindowCalls, s_mwWindowFrames, s_mwWindowSuppressed, s_mwHistN,
         histbuf, s_mwHistOverflowSightings != 0 ? " (+other)" : "", s_mwBadIndex, detbuf,
         s_mwTotalGated, s_mwTotalSuppressed, cutaway_maskwall_calls());

  // The window is cleared here (a figure that never resets described a period nobody could name); the cumulative totals
  // are not.
  s_mwWindowCalls = 0;
  s_mwWindowSuppressed = 0;
  s_mwWindowFrames = 0;
  s_mwBadIndex = 0;
  s_mwHistN = 0;
  s_mwHistOverflowSightings = 0;
  s_mwDetailN = 0;
}

// A throttle of its own (about a second): this report is called from a seam nocull_should_scan can refuse before
// cutaway_measure_spans's clock.
static unsigned s_etwLastMs = 0;
static bool s_etwFirstTick = true;

static bool cw_etw_due(void) {
  const unsigned now = wallclock_ms();
  if (!s_etwFirstTick && (now - s_etwLastMs) < 1000u) return false;
  s_etwFirstTick = false;
  s_etwLastMs = now;
  return true;
}

// The widened tall-wall gate's report, called from nocull.cpp above nocull_should_scan's gate: the refusal's gate is
// camera_active() alone, so a report behind the four-fact gate would print nothing exactly when "Draw the whole map" is
// off. Not behind debuglog: a zero window belongs in the log.
void cutaway_engine_tall_report(void) {
  if (!camera_active()) return;
  if (!cw_etw_due()) return;
  if (s_engineTallChecked == 0) return;

  ek_log("cutaway engine tall-wall (brief-tallwall-engine.md): the SAME rule "
         "and row as STAGE C's own refusal (ekm_cut_tall_wall_refuse, "
         "freecamcutawaytallwall), gated on FREE CAMERA ACTIVE ALONE so it "
         "also reaches calls the cutaway never sees -- %u wall call(s) "
         "checked in all, %u refused before the engine's own domost ran, %u "
         "skipped for no projection that frame, %u forwarded for no quad "
         "(brief-tallwall-fixes.md, 2026-09-20). DISTINCT population from "
         "cutpaint.cpp's s_wallTallRefused, which only ever counts while the "
         "cutaway is armed",
         s_engineTallChecked, s_engineTallRefused, s_engineTallNoProj,
         s_engineTallNoQuad);
}

// Its own throttle: sharing cw_etw_due let the tall-wall report win each second so the degenerate report never printed.
static unsigned s_degLastMs = 0;
static bool s_degFirstTick = true;

static bool cw_deg_due(void) {
  const unsigned now = wallclock_ms();
  if (!s_degFirstTick && (now - s_degLastMs) < 1000u) return false;
  s_degFirstTick = false;
  s_degLastMs = now;
  return true;
}

// The degenerate-quad refusal's report. Its old close ("only the enormous ones are the columns") is FALSE: in the
// flight it rests on, all 64,070 inverted spans sat under the tightest (8x) threshold and 0 were refused. Same gate and
// reason as the reports above; own throttle (cw_deg_due).
void cutaway_degenerate_report(void) {
  if (!camera_active()) return;
  if (!cw_deg_due()) return;
  if (s_degenerateChecked == 0) return;

  ek_log("cutaway degenerate (brief-retire-rows.md): a wall call is "
         "refused whenever its clip box is DEGENERATE, no magnitude test -- "
         "UNCONDITIONAL now, gated on FREE CAMERA ACTIVE ALONE at the same "
         "two sites as the tall-wall refusal -- %u wall call(s) checked, "
         "%u with x0 == x1, %u with a crossed/pinched end, %u actually "
         "refused. 57%% of wall calls invert and most are "
         "a correctly-projected edge-on wall seen from above; every inverted "
         "span measured sat under 8x the screen height, so extent does NOT "
         "separate the columns from those legitimate ones -- refusing all of "
         "them is what removes the columns, at the cost of a black triangle "
         "at some angles",
         s_degenerateChecked, s_degenerateXEqual, s_degenerateCrossed,
         s_degenerateRefused);
}

// Game/render thread, one clock, one drawrooms: written by the nonparallaxed thunk, read by the domost thunk a few
// instructions later, so no interlock. -1 is "no plane" (the parallax sky and every wall call).
static int s_planeSector = -1;
static bool s_planeIsFloor = false;

// polymost.cpp:4802-4803: the caller ORs MAXSECTORS into sectnum for "floor side" and the function masks it off (4096,
// build.h:48,59 = EK_MAXSECTORS). `orig` is the trampoline or cw_nonpar_fwd_timed.
static void cw_nonpar_body(NonParFn const orig, unsigned long long n0,
                           unsigned long long n1, float ryp0, float ryp1,
                           float x0, float x1, float y0, float y1, int sectnum) {
  const bool gated =
      nocull_should_scan(g_cfg.freecamwholemap != 0, camera_active(),
                         camera_render_armed(), false);
  if (gated) {
    const int sect = sectnum & ~EK_MAXSECTORS;
    s_planeIsFloor = (sectnum & EK_MAXSECTORS) != 0;
    s_planeSector = (sect >= 0 && sect < EK_MAXSECTORS) ? sect : -1;
    if (s_planeSector >= 0)
      cutpaint_flip_note_np(s_planeSector, s_planeIsFloor);
  }

  orig(n0, n1, ryp0, ryp1, x0, x1, y0, y1, sectnum);

  // Cleared on every path out: a plane left standing would be read by the next sky call or another bunch's wall.
  s_planeSector = -1;
}

static NonParFn s_npFwdOrig = NULL;
static long long s_npFwdTicks = 0;
static int s_npFwdCount = 0;

static void cw_nonpar_fwd_timed(unsigned long long n0, unsigned long long n1,
                                float ryp0, float ryp1, float x0, float x1,
                                float y0, float y1, int sectnum) {
  const long long in = perf_stamp();
  s_npFwdOrig(n0, n1, ryp0, ryp1, x0, x1, y0, y1, sectnum);
  const long long out = perf_stamp();
  const long long d = ekm_perf_span(in, out);
  if (d > 0) s_npFwdTicks += d;
  s_npFwdCount++;
}

static void EK_NonParallaxThunk(unsigned long long n0, unsigned long long n1,
                                float ryp0, float ryp1, float x0, float x1,
                                float y0, float y1, int sectnum) {
  NonParFn const orig = s_origNonPar;
  if (!orig) {
    // Unreachable: the trampoline is published before the patch and cleared after it comes out, and the install runs
    // inside SwapBuffers where the engine is not in renderDrawRoomsQ16.
    if (!s_saidNoNonPar) {
      s_saidNoNonPar = true;
      ek_log("cutaway: the nonparallaxed thunk ran with no original to forward "
             "to, which nothing in this build can produce -- please report it. "
             "The frame paints no flats while this lasts");
    }
    return;
  }

  if (g_cfg.perfzones == 0) {
    cw_nonpar_body(orig, n0, n1, ryp0, ryp1, x0, x1, y0, y1, sectnum);
    return;
  }
  s_npFwdOrig = orig;
  s_npFwdTicks = 0;
  s_npFwdCount = 0;
  const long long in = perf_stamp();
  cw_nonpar_body(&cw_nonpar_fwd_timed, n0, n1, ryp0, ryp1, x0, x1, y0, y1,
                 sectnum);
  const long long out = perf_stamp();
  perf_zone_add(EK_PZ_CUT_NONPAR, ekm_perf_span(in, out), 1);
  perf_zone_add(EK_PZ_CUT_NONPAR_FWD, s_npFwdTicks, s_npFwdCount);
}

// The widened tall-wall refusal: cutpaint_wall's rule, row (freecamcutawaytallwall) and threshold, applied to a call
// about to be FORWARDED. Only the gate differs: free camera active alone (columns appear with "Draw the whole map"
// OFF). True means the call must NOT reach `orig`. Flats untouched.
static bool EK_EngineTallWallRefuse(bool flatCall, int tallMultiple, double x0,
                                    double y0top, double y0bot, double x1,
                                    double y1top, double y1bot) {
  if (flatCall) return false;

  s_engineTallChecked++;
  const CpWallRatio r = cutpaint_wall_ratio(x0, y0top, y0bot, x1, y1top, y1bot);
  if (!r.projOk) {
    // No projection this frame: forward, as cutpaint_wall's identical early-out.
    s_engineTallNoProj++;
    return false;
  }
  if (!r.haveQuad) {
    // No quad: cutpaint_wall's no-quad path forwards, and this must agree (the defect that withheld 43% of gated wall
    // calls).
    s_engineTallNoQuad++;
    return false;
  }

  if (!ekm_cut_engine_tall_wall_refuse(r.projOk, r.haveQuad, r.ratioOk,
                                       r.ratio, tallMultiple))
    return false;

  s_engineTallRefused++;
  return true;
}

// The degenerate-quad refusal (inversion-only; the enormous term measured dead, see cutpaint.h). Separate from the
// tall-wall wrapper, whose job is to keep FORWARDING a degenerate quad, so the two stay independently switchable. Flats
// untouched; the classifier alone decides.
static bool EK_DegenerateWallRefuse(bool flatCall, double x0,
                                    double y0top, double y0bot, double x1,
                                    double y1top, double y1bot) {
  if (flatCall) return false;

  s_degenerateChecked++;
  const EkCutDegenerateKind kind =
      ekm_cut_wall_degenerate_kind(x0, y0top, y0bot, x1, y1top, y1bot);
  if (kind == EK_CUT_DEGENERATE_X_EQUAL) s_degenerateXEqual++;
  else if (kind == EK_CUT_DEGENERATE_QUAD) s_degenerateCrossed++;

  if (!ekm_cut_wall_degenerate_refuse(kind)) return false;

  s_degenerateRefused++;
  return true;
}

// camera_active() alone is the whole gate, unconditional (the row is gone).
static bool EK_DegenerateGateOpen(void) {
  return camera_active();
}

// The gate hoisted above both call sites: camera_active() and `multiple <= 0` make the widened refusal a no-op, and
// both must be known before any ratio or flat-call work, which keeps "with the row off the whole thunk is four scalar
// reads and one forward" true.
static bool EK_EngineTallWallGateOpen(int* multipleOut) {
  if (!ekm_cutaway_engine_tall_wall_should_gate(camera_active())) return false;
  const int m = ekm_cutaway_tall_wall_multiple(g_cfg.freecamcutawaytallwall);
  if (m <= 0) return false;
  *multipleOut = m;
  return true;
}

unsigned cutaway_domost_calls(void)          { return s_calls; }
unsigned cutaway_domost_gated_calls(void)    { return s_gated; }
unsigned cutaway_domost_painted_calls(void)  { return s_painted + s_wallPainted; }
unsigned cutaway_domost_forwarded_calls(void) {
  return s_ndForwarded + s_wallForwarded + s_partsWallForwarded;
}
unsigned cutaway_domost_hole_calls(void)     { return s_ndSeen; }

static void cw_domost_body(DomostFn const orig, float x0, float y0, float x1,
                           float y1, float y0top, float y0bot, float y1top,
                           float y1bot) {
  s_calls++;

  // The gate first and nothing above it: domost runs dozens of times per wall. The mirror term is false for
  // nocull.cpp's reason: camera_render_armed() is raised around one substituted renderDrawRoomsQ16 (and a TROR base
  // level), lowered on every other.
  if (nocull_should_scan(g_cfg.freecamwholemap != 0, camera_active(),
                         camera_render_armed(), false)) {
    s_gated++;

    // Walls only. polymost_clipmost returns at once when y0bot < y0top (polymost.cpp:3853-3854), domost's defaults, so
    // a WALL call is clipped to its quad and a FLAT call is not clipped. Read at ENTRY, before the +/- DOMOST_OFFSET
    // adjustment (:3932-3935); the pure rule (cutpaint.h) is pinned against :3925's defaults.
    const bool flatCall =
        ekm_cut_flat_call((double)y0top, (double)y0bot,
                          (double)y1top, (double)y1bot) != 0;
    if (flatCall) s_flatSkips++;

    // The bisection row: a suppressed category is DROPPED, not painted and not forwarded, or the engine's geometry
    // would muddy the question. Only known-plane flats are counted; a no-plane flat is the sky ("Cutaway sky").
    const int cutParts = ekm_cutaway_parts_clamped(g_cfg.freecamcutawayparts);

    // A no-draw pass belongs to the engine: with g_nodraw set its body records a TROR hole in yax_holecf and paints
    // nothing (polymost.cpp:4139-4150); painting put footprints into passes meant to draw nothing.
    const volatile int* const nd = cw_nodraw_ptr();
    const int ndValue = nd != NULL ? *nd : 0;
    if (ndValue != 0) {
      s_ndSeen++;
      const bool painter = cw_dpm_ptr() != NULL;
      if (painter && flatCall && s_planeSector >= 0 &&
          cutParts != CUTAWAY_PARTS_WALLS_ONLY)
        s_ndWouldB++;
      if (painter && !flatCall && cutParts != CUTAWAY_PARTS_FLATS_ONLY &&
          cutParts != CUTAWAY_PARTS_WALLS_FORWARDED)
        s_ndWouldC++;
    }
    // Both locates or no forward: forwarding with yax_drawrooms unlocated leaves no armed pass that draws.
    if (ekm_cutaway_forward_nodraw(
            ekm_tror_support_located(nd != NULL, camera_tror_base_located()),
            freecam_tror_on(g_cfg.freecamtrornodraw), ndValue)) {
      s_ndForwarded++;
      orig(x0, y0, x1, y1, y0top, y0bot, y1top, y1bot);
      return;
    }

    if (flatCall && s_planeSector >= 0 && cutParts == CUTAWAY_PARTS_WALLS_ONLY) {
      s_partsFlatDropped++;
      return;
    }
    if (!flatCall && cutParts == CUTAWAY_PARTS_FLATS_ONLY) {
      s_partsWallDropped++;
      return;
    }

    // From above the map a ceiling is seen only from under its extended slope and pops; below it ceilings stay. Dropped
    // like stage B's paint exit; a no-draw pass keeps its hole records; the sky is untouched.
    if (flatCall && s_planeSector >= 0 && !s_planeIsFloor && ndValue == 0 &&
        freecam_hideceilings_clamped(g_cfg.freecamhideceilings) ==
            FREECAM_CEILINGS_HIDE &&
        camera_above_map()) {
      s_ceilHidden++;
      return;
    }

    // Stage B: paint the flat ourselves and claim nothing. cutpaint_flat paints the sector's WHOLE FOOTPRINT, not this
    // call's span (a wall-driven renderer never submits a sector with no front-facing wall). Two facts must agree: the
    // clip-box defaults say FLAT and the nonparallaxed thunk names the sector; a disagreement is counted.
    if (flatCall && s_planeSector >= 0) {
      const volatile int* const dpm = cw_dpm_ptr();
      if (dpm != NULL) {
        // The method is the engine's domostpolymethod (what every drawpoly in domost passes, polymost.cpp:4107..:4380);
        // without the locate the call forwards.
        const int method = *dpm;
        if (cutpaint_flat(s_planeSector, s_planeIsFloor, method)) {
          s_painted++;
          return;
        }
        s_paintRefused++;
        // cutpaint_flat refused (no footprint, sector info or projection): falls through to the forward.
        cutpaint_flip_note_dm_forward(s_planeSector, s_planeIsFloor);
      } else {
        cutpaint_flip_note_dm_forward(s_planeSector, s_planeIsFloor);
      }
    } else if (!flatCall && s_planeSector >= 0) {
      // Should stay zero: the nonparallaxed function makes one domost call and it is a flat one; non-zero means the
      // ninth argument is not read where assumed.
      s_planeNotFlat++;
    }

    // Paint the wall too, so nothing claims. After the flats, 652 bunches gave 71 domost calls a frame: a bunch emits
    // nothing when its span is CLAIMED (polymost.cpp:7110-7166), so the WALLS closed the screen. The quad is free:
    // domost's last four arguments are the clip box only the four wall call sites fill (:6409, :6466, :6536, :6539).
    if (!flatCall) {
      // The row's own choice, not a painting refusal: stage C never calls cutpaint_wall in this mode; identical to a
      // refusal but for the counter.
      if (cutParts == CUTAWAY_PARTS_WALLS_FORWARDED) {
        s_partsWallForwarded++;
      } else {
        const volatile int* const dpm = cw_dpm_ptr();
        if (dpm != NULL) {
          const int method = *dpm;
          if (cutpaint_wall((double)x0, (double)y0top, (double)y0bot,
                            (double)x1, (double)y1top, (double)y1bot, method)) {
            s_wallPainted++;
            return;
          }
        }
        s_wallForwarded++;
      }
    }

    // The backface skip is GONE, its condition unreachable: drawpoly returns at once on DAMETH_BACKFACECULL (-1,
    // polymost.h:232; polymost.cpp:3184), but 0 of 1,629,048 gated calls hit it. The guards are exact complements
    // (:5511 vs :4997; :5931 vs :5007). The domostpolymethod locate stays: painting needs it.

    // The sky's calls, gated by "Cutaway sky". A flat carrying no plane is the parallax sky's (it never enters
    // nonparallaxed). With the row OFF the call never reaches the engine: its fake plane 32 m up paints into everything
    // open (dollhouse.cpp's gate). Without the nonparallax hook every flat looks like the sky; forward.
    if (flatCall && s_planeSector < 0 && s_origNonPar != NULL && g_cfg.freecamcutawaysky == 0) {
      s_skySuppressed++;
      return;
    }

    // The reopen is off, the approach dead. vsp[] is SHARED SEQUENTIAL state: for a flat it SUPPLIES THE NEAR EDGE, so
    // reopening wipes what every LATER call depends on (70% of gated calls are flats). Neither reference reopened
    // (dollhouse.cpp:527). The locate, snapshot and rebuild work (69525 reopens, 0 broken); only calling them is off.
    const bool reopenApproachDead = true;
    if (reopenApproachDead) {
      // The widened gate, right before the forward it decides (a wall cutpaint_wall declined to paint). Checked first,
      // so with the row off or the camera inactive the ratio work never runs.
      int tallMultiple = 0;
      if (EK_EngineTallWallGateOpen(&tallMultiple) &&
          EK_EngineTallWallRefuse(flatCall, tallMultiple, (double)x0,
                                  (double)y0top, (double)y0bot, (double)x1,
                                  (double)y1top, (double)y1bot))
        return;
      if (EK_DegenerateGateOpen() &&
          EK_DegenerateWallRefuse(flatCall, (double)x0,
                                  (double)y0top, (double)y0bot, (double)x1,
                                  (double)y1top, (double)y1bot))
        return;
      orig(x0, y0, x1, y1, y0top, y0bot, y1top, y1bot);
      return;
    }

    // Fail closed: the latch, then the armed snapshot. No snapshot means no agreed pristine list, so nothing safe can
    // be reopened.
    if (!flatCall && !s_reopenDead && s_vspWrite != NULL && s_snapVcnt >= 2) {
      int walked = 0, freed = 0;
      const CwReopen v = cw_reopen_spans(s_vspWrite, s_snapVcnt, &walked, &freed);
      if (walked > s_walkMax) s_walkMax = walked;
      if (v == CW_REOPEN_DONE) {
        s_reopens++;
        s_freedNodes += (unsigned)freed;
        if (freed > s_freedMax) s_freedMax = freed;
        if (!s_saidFirstReopen) {
          s_saidFirstReopen = true;
          ek_log("cutaway reopen: the span list is being REOPENED before every "
                 "gated polymost_domost call -- %d live entr(ies) restored from "
                 "the seed seam's snapshot (%u byte(s)), %d node(s) freed on the "
                 "first one, walk %d node(s). The apron getting worse, a black "
                 "sky and more of the map showing through are PREDICTED here",
                 s_snapVcnt,
                 (unsigned)s_snapVcnt * EK_CUTAWAY_VSP_STRIDE, freed, walked);
        }
      } else if (v == CW_REOPEN_STALE) {
        // Not a latch: the snapshot is not this list's (initmosts re-ran with a different clip polygon; a
        // room-over-room pass paints at polymost.cpp:6989-7010 before the seed at :7073, unhooked). The list is left
        // untouched; the next armed base pass re-arms.
        s_refusedStale++;
        if (!s_saidStale) {
          s_saidStale = true;
          ek_log("cutaway reopen: the snapshot is NOT this list's -- a viewport "
                 "node's x disagrees with the %d entr(ies) taken at the seed "
                 "seam, so polymost_initmosts has re-run since. No reopen this "
                 "call and nothing is written; this is a refusal, not a "
                 "failure, and it is counted rather than latched", s_snapVcnt);
        }
      } else {
        // This one IS: an inconsistent ring must never be written through twice.
        s_refusedBroken++;
        s_reopenDead = true;
        if (!s_saidBroken) {
          s_saidBroken = true;
          ek_log("cutaway reopen: the live span ring is INCONSISTENT -- the walk "
                 "from node 0 covered %d node(s) against %d viewport entr(ies), "
                 "or a link left the array. Reopening is latched off for this "
                 "session and the engine keeps its own list; nothing else "
                 "changes", walked, s_snapVcnt);
        }
      }
    }
  }

  // The engine last, on every path: an open list is open only until the call that claims against it. Unguarded on
  // purpose (the engine's own arguments). Reached only when the four-fact gate was false, a wall the cutaway never
  // looked at, which the widened gate exists for. flatCall is recomputed only when that gate is open.
  int tallMultiple = 0;
  const bool tallGateOpen = EK_EngineTallWallGateOpen(&tallMultiple);
  const bool degenerateGateOpen = EK_DegenerateGateOpen();
  if (tallGateOpen || degenerateGateOpen) {
    const bool flatCallUngated =
        ekm_cut_flat_call((double)y0top, (double)y0bot, (double)y1top,
                          (double)y1bot) != 0;
    if (tallGateOpen &&
        EK_EngineTallWallRefuse(flatCallUngated, tallMultiple, (double)x0,
                                (double)y0top, (double)y0bot, (double)x1,
                                (double)y1top, (double)y1bot))
      return;
    if (degenerateGateOpen &&
        EK_DegenerateWallRefuse(flatCallUngated, (double)x0,
                                (double)y0top, (double)y0bot, (double)x1,
                                (double)y1top, (double)y1bot))
      return;
  }
  orig(x0, y0, x1, y1, y0top, y0bot, y1top, y1bot);
}

static DomostFn s_dmFwdOrig = NULL;
static long long s_dmFwdTicks = 0;
static int s_dmFwdCount = 0;

static void cw_domost_fwd_timed(float x0, float y0, float x1, float y1,
                                float y0top, float y0bot, float y1top,
                                float y1bot) {
  const long long in = perf_stamp();
  s_dmFwdOrig(x0, y0, x1, y1, y0top, y0bot, y1top, y1bot);
  const long long out = perf_stamp();
  const long long d = ekm_perf_span(in, out);
  if (d > 0) s_dmFwdTicks += d;
  s_dmFwdCount++;
}

static void EK_DomostThunk(float x0, float y0, float x1, float y1,
                           float y0top, float y0bot, float y1top, float y1bot) {
  DomostFn const orig = s_origDomost;
  if (!orig) {
    // Unreachable: the trampoline is published before the patch and cleared after it comes out.
    if (!s_saidNoOrig) {
      s_saidNoOrig = true;
      ek_log("cutaway: the domost thunk ran with no original to forward to, "
             "which nothing in this build can produce -- please report it. The "
             "frame paints no world while this lasts");
    }
    return;
  }

  // One branch with Frame timings off; ~50k calls a frame on the whole map.
  if (g_cfg.perfzones == 0) {
    cw_domost_body(orig, x0, y0, x1, y1, y0top, y0bot, y1top, y1bot);
    return;
  }
  s_dmFwdOrig = orig;
  s_dmFwdTicks = 0;
  s_dmFwdCount = 0;
  const long long in = perf_stamp();
  cw_domost_body(&cw_domost_fwd_timed, x0, y0, x1, y1, y0top, y0bot, y1top,
                 y1bot);
  const long long out = perf_stamp();
  perf_zone_add(EK_PZ_CUT_DOMOST, ekm_perf_span(in, out), 1);
  perf_zone_add(EK_PZ_CUT_DOMOST_FWD, s_dmFwdTicks, s_dmFwdCount);
}

// Deliberately NOT wired into any aggregate verification flag (nocull_install's ruling): a refusal here says nothing
// about the camera's bracket, the firing hooks or any cvar. A failure logs once, marks the row unavailable and leaves
// the game running.
void cutaway_install(void) {
  static bool tried = false;
  if (tried) return;

  const unsigned char* const base = s_base;
  const unsigned entry = cutaway_domost_rva();
  const unsigned end = cutaway_domost_end_rva();
  const int verdict = ekm_cutaway_install_verdict(base != NULL, cutaway_located(),
                                                  cutaway_available(), entry, end);
  // Not yet rather than refused: the locate runs on the sampler; claiming the attempt now would spend it on a verdict
  // nobody published.
  if (verdict == EK_CUTAWAY_INSTALL_WAIT) return;

  tried = true;

  if (verdict != EK_CUTAWAY_INSTALL_GO) {
    cutaway_mark_unavailable();
    ek_log("cutaway: no hook -- the locate did not reach a usable LOCATED state "
           "(polymost_domost rva 0x%07X..0x%07X). The cutaway stays unavailable "
           "for this session and everything else keeps running", entry, end);
    return;
  }

  if (!hook_install((void*)(base + entry), (void*)&EK_DomostThunk,
                    &s_domostHook)) {
    cutaway_mark_unavailable();
    // hook.cpp has said which stage refused and guarantees nothing was left written.
    ek_log("cutaway: polymost_domost at rva 0x%07X was NOT hooked; the cutaway "
           "stays unavailable for this session and everything else keeps "
           "running", entry);
    return;
  }

  DomostFn const tramp = (DomostFn)hook_trampoline(&s_domostHook);
  if (!tramp) {
    // Cannot happen after a successful install, and a wrong guess is an entry jumping into this module with nowhere to
    // return: unpatch rather than trust.
    if (!hook_remove(&s_domostHook))
      ek_log("cutaway: polymost_domost has no trampoline AND could not be "
             "unpatched -- its entry still jumps into this module with nowhere "
             "to return to");
    cutaway_mark_unavailable();
    ek_log("cutaway: polymost_domost was patched but has no trampoline; "
           "unpatched again and the cutaway stays unavailable");
    return;
  }
  s_origDomost = tramp;

  ek_log("cutaway: polymost_domost at rva 0x%07X..0x%07X is HOOKED -- its entry "
         "now runs our thunk, which on a gated flat call paints that sector's "
         "whole footprint itself and does NOT run the engine's body, and "
         "forwards straight through on every other. The row is off by default "
         "and the paint is inert until the free camera flies with it on",
         entry, end);

  // The counting hook goes before the one that can return, or a measurement would vanish on the same conditions as the
  // feature. Failing costs the number only.
  const unsigned daEntry = cutaway_drawalls_rva();
  const unsigned daEnd = cutaway_drawalls_end_rva();
  if (!ekm_cutaway_extent_ok(daEntry, daEnd)) {
    ek_log("cutaway: polymost_drawalls has no usable extent (0x%07X..0x%07X), "
           "so its calls per frame stay unmeasured; everything else keeps "
           "running", daEntry, daEnd);
  } else if (!hook_install((void*)(base + daEntry), (void*)&EK_DrawAllsThunk,
                           &s_allsHook)) {
    ek_log("cutaway: polymost_drawalls at rva 0x%07X was NOT hooked; its calls "
           "per frame stay unmeasured", daEntry);
  } else {
    DrawAllsFn const daTramp = (DrawAllsFn)hook_trampoline(&s_allsHook);
    if (!daTramp) {
      // A patched entry with nowhere to return to is worse than no measurement.
      if (!hook_remove(&s_allsHook))
        ek_log("cutaway: polymost_drawalls has no trampoline AND could not be "
               "unpatched -- its entry still jumps into this module with "
               "nowhere to return to");
      else
        ek_log("cutaway: polymost_drawalls was patched but has no trampoline; "
               "unpatched again and its calls stay unmeasured");
    } else {
      s_origDrawAlls = daTramp;
      ek_log("cutaway: polymost_drawalls at rva 0x%07X..0x%07X is HOOKED FOR "
             "COUNTING ONLY -- the thunk increments and forwards, decides "
             "nothing and is not gated, so the count is the ENGINE's call "
             "rate and not ours", daEntry, daEnd);
    }
  }

  // The masked-wall hook, also before the one that can return; a refusal costs the masked-wall count and suppress row
  // only. Its locate is independent (another anchor and function), so it neither depends on cutaway_available() nor
  // clears it.
  const unsigned mwEntry = cutaway_maskwall_rva();
  const unsigned mwEnd = cutaway_maskwall_end_rva();
  if (!ekm_cutaway_extent_ok(mwEntry, mwEnd)) {
    ek_log("cutaway: polymost_drawmaskwallinternal has no usable extent "
           "(0x%07X..0x%07X), so masked walls stay uncounted and the suppress "
           "row stays inert; everything else keeps running", mwEntry, mwEnd);
  } else if (!hook_install((void*)(base + mwEntry), (void*)&EK_MaskWallThunk,
                           &s_maskHook)) {
    ek_log("cutaway: polymost_drawmaskwallinternal at rva 0x%07X was NOT "
           "hooked; masked walls stay uncounted and the suppress row stays "
           "inert", mwEntry);
  } else {
    MaskWallFn const mwTramp = (MaskWallFn)hook_trampoline(&s_maskHook);
    if (!mwTramp) {
      if (!hook_remove(&s_maskHook))
        ek_log("cutaway: polymost_drawmaskwallinternal has no trampoline AND "
               "could not be unpatched -- its entry still jumps into this "
               "module with nowhere to return to");
      else
        ek_log("cutaway: polymost_drawmaskwallinternal was patched but has no "
               "trampoline; unpatched again and masked walls stay uncounted");
    } else {
      s_origMaskWall = mwTramp;
      InterlockedExchange(&s_maskHooked, 1);
      ek_log("cutaway: polymost_drawmaskwallinternal at rva 0x%07X..0x%07X is "
             "HOOKED -- its one int argument is the WALL INDEX and the image "
             "says it survives: the entry's first touch of rcx is a READ, and "
             "both reachable sites load ecx immediately before transferring "
             "control. The thunk counts and forwards; it drops the wall only "
             "while the suppress row is on, and that row is off by default. "
             "This path makes no polymost_domost call at all, which is why no "
             "instrument in this file has ever seen it", mwEntry, mwEnd);
    }
  }

  // Stage B's second hook is deliberately not fatal: without it s_planeSector stays -1 and every flat forwards, today's
  // picture. The domost hook stays either way.
  const unsigned npEntry = cutaway_nonpar_rva();
  const unsigned npEnd = cutaway_nonpar_end_rva();
  if (!ekm_cutaway_extent_ok(npEntry, npEnd)) {
    ek_log("cutaway: polymost_internal_nonparallaxed was not located, so no "
           "flat call can be named to a sector and the cutaway paints nothing; "
           "everything else keeps running");
    return;
  }
  if (!hook_install((void*)(base + npEntry), (void*)&EK_NonParallaxThunk,
                    &s_nonparHook)) {
    ek_log("cutaway: polymost_internal_nonparallaxed at rva 0x%07X was NOT "
           "hooked; the cutaway paints nothing and the engine keeps drawing "
           "its own flats", npEntry);
    return;
  }
  NonParFn const npTramp = (NonParFn)hook_trampoline(&s_nonparHook);
  if (!npTramp) {
    if (!hook_remove(&s_nonparHook))
      ek_log("cutaway: polymost_internal_nonparallaxed has no trampoline AND "
             "could not be unpatched -- its entry still jumps into this module "
             "with nowhere to return to");
    ek_log("cutaway: polymost_internal_nonparallaxed was patched but has no "
           "trampoline; unpatched again and the cutaway paints nothing");
    return;
  }
  s_origNonPar = npTramp;
  ek_log("cutaway: polymost_internal_nonparallaxed at rva 0x%07X..0x%07X is "
         "HOOKED -- its ninth argument names the sector of every non-parallax "
         "floor and ceiling call, which is how the cutaway knows whose "
         "footprint to paint. The parallax sky never enters it, so the "
         "engine's own sky is untouched", npEntry, npEnd);
}

void cutaway_remove(void) {
  // A failed restore leaves everything where it was (hook.cpp's guarantee): the entry still jumps to our thunk, so the
  // pointer it forwards through must stay valid.
  if (s_nonparHook.target != NULL && hook_remove(&s_nonparHook))
    s_origNonPar = NULL;
  if (s_maskHook.target != NULL && hook_remove(&s_maskHook)) {
    s_origMaskWall = NULL;
    InterlockedExchange(&s_maskHooked, 0);
  }
  if (s_allsHook.target != NULL && hook_remove(&s_allsHook))
    s_origDrawAlls = NULL;
  if (s_domostHook.target == NULL) return;
  if (hook_remove(&s_domostHook)) s_origDomost = NULL;
}
