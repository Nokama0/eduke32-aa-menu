// Locating G_DrawRooms by a string anchor (game.cpp:803, inside its cursectnum == -1 early return): source text
// survives any optimisation level. The pose goes in BELOW it, into renderDrawRoomsQ16 with polymer_drawrooms inlined
// (G_DrawRooms clamps camera z twice, game.cpp:1074/:1097); set_globalpos/ang run in the detoured call (docs/re/freecam.md).

#include <windows.h>
#include <intrin.h>
#include <string.h>
#include "log.h"
#include "camera.h"
#include "camerascan.h"
#include "imagescan.h"
#include "hitscan.h"
#include "sectorstate.h"
#include "tsprites.h"
#include "frustum.h"
#include "gamestate.h"
#include "hook.h"
#include "playerstate.h"
#include "aimsolve.h"
#include "config.h"
#include "input.h"
#include "udstate.h"
#include "cvars.h"
#include "shot.h"

#pragma intrinsic(_ReturnAddress)

// game.cpp:803: the anchor, source text rather than compiler output.
static const char EK_CAMERA_ANCHOR[] =
    "Unable to capture screenshot for savegame because player cursectnum is -1!";

// polymer.cpp:1180 and :1223, inside polymer_drawrooms (:1094), which GCC inlined into renderDrawRoomsQ16. Two anchors
// landing in the SAME RUNTIME_FUNCTION corroborate what one string cannot; a disagreement refuses.
static const char EK_POLY_ANCHOR_A[] = "got sector %d after update!";
static const char EK_POLY_ANCHOR_B[] = "EXT sec=%d  z=%d (%d, %d)";

static volatile LONG s_rva = 0;

// renderDrawRoomsQ16, where the pose goes (named for its polymer half), and the EXTENT of G_DrawRooms, which the detour
// tests its return address against. Taken from the live locate; rvas in comments are search results, not inputs.
static volatile LONG s_roomsQ16Rva = 0;
volatile LONG s_drawBegin = 0;
volatile LONG s_drawEnd = 0;

// getzsofslopeptr, the engine's band reader, or 0 when refused; zeroed if the call ever faults.
volatile LONG s_getzRva = 0;

// The one `call rel32` inside the bracket that reaches it, which the sky redirect rewrites, or 0 if not unique.
// Separate from the function's rva: no function costs the sloped band, no unique site costs the sky.
volatile LONG s_getzSiteRva = 0;

// The host base, written once on the game thread by camera_scan (the thread that later runs the patched code).
const unsigned char* s_base = NULL;

// A refusal is permanent: the image does not change while the game runs.
static bool s_refused = false;

// The corroboration: read-only, non-gating. The walk is ek_image_count_calls (imagescan.cpp); `owner` is the list so
// the ototalclock scan walks exactly those functions.

// Run at most once; returns the entry rva or 0 having logged why. Every refusal carries its counts ("not found" and
// "found four times" are different failures).
static unsigned locate_once(const unsigned char* base, const char* anchor,
                            size_t anchorLen, const char* what,
                            unsigned* beginOut, unsigned* endOut) {
  if (beginOut) *beginOut = 0;
  if (endOut) *endOut = 0;
  EkStrHit s;
  ek_image_find_string(base, anchor, anchorLen, &s);
  if (s.faulted) {
    ek_log("camera: a data section faulted mid-scan, so the %s anchor "
           "cannot be claimed unique (%d occurrence(s) before the fault); "
           "refusing", what, s.count);
    return 0;
  }
  if (s.count == 0) {
    ek_log("camera: the %s anchor string is not in this build; the "
           "free camera has no draw call to bracket and stays unavailable", what);
    return 0;
  }
  if (s.count != 1) {
    ek_log("camera: the %s anchor occurs %d times, first at rva "
           "0x%07X; refusing to guess", what, s.count, s.rva);
    return 0;
  }

  EkRefHit h;
  ek_image_find_lea_refs(base, s.rva, &h);
  if (h.faulted) {
    ek_log("camera: a code section faulted mid-scan, so the %s anchor's "
           "uniqueness cannot be claimed (%d reference(s) before the fault); "
           "refusing", what, h.count);
    return 0;
  }
  if (h.count == 0) {
    ek_log("camera: the %s anchor at rva 0x%07X is referenced by no "
           "rip-relative lea (%d raw displacement match(es)); refusing",
           what, s.rva, h.raw);
    return 0;
  }
  if (h.count != 1) {
    ek_log("camera: the %s anchor at rva 0x%07X has %d references, first at "
           "rva 0x%07X; refusing to guess", what, s.rva, h.count, h.rva);
    return 0;
  }

  unsigned begin = 0, end = 0;
  const unsigned entry =
      ek_image_extent_containing(base, h.rva, &begin, &end) ? begin : 0;
  if (entry == 0 || entry > h.rva) {
    // Zero means "no exception directory", "no entry covers this rva" or "the walk faulted" alike, so the line says
    // only that.
    ek_log("camera: the %s reference at rva 0x%07X is not inside any function "
           "the exception directory names, or the image has no usable exception "
           "directory; refusing", what, h.rva);
    return 0;
  }

  if (beginOut) *beginOut = begin;
  if (endOut) *endOut = end;
  ek_log("camera: %s anchor once at rva 0x%07X, referenced once at rva "
         "0x%07X (%d raw displacement match(es)), inside the function at rva "
         "0x%07X..0x%07X (%+d)",
         what, s.rva, h.rva, h.raw, entry, end, (int)(h.rva - entry));
  return entry;
}

// The engine's own band reader, getzsofslopeptr (build.h:1417, engine.cpp:14288): out of line though getzsofslope is
// inlined, and polymer.cpp:1211 calls it, so a band read through it IS the renderer's test. Identified by order (a
// `call rel32` target inside the located drawrooms) and shape (its four sector reads); exactly one must pass.
static const int EK_GETZ_WINDOW = 96;
static const int EK_GETZ_MAX_TARGETS = 128;

// `opc` followed by a modrm with mod=01 and rm=001 ([rcx+disp8]) and that disp8; any reg field (the compiler's choice).
static bool getz_reads(const unsigned char* p, int len, const unsigned char* opc,
                       int opcLen, unsigned char disp) {
  for (int i = 0; i + opcLen + 1 < len; i++) {
    int k = 0;
    while (k < opcLen && p[i + k] == opc[k]) k++;
    if (k != opcLen) continue;
    if ((p[i + opcLen] & 0xC7) != 0x41) continue;
    if (p[i + opcLen + 1] == disp) return true;
  }
  return false;
}

static bool getz_shape(const unsigned char* base, unsigned begin, unsigned end) {
  if (end <= begin) return false;
  int len = (int)(end - begin);
  if (len > EK_GETZ_WINDOW) len = EK_GETZ_WINDOW;
  const unsigned char* const p = base + begin;
  static const unsigned char MOV[1] = { 0x8B };
  static const unsigned char MOVZX[2] = { 0x0F, 0xB7 };
  bool ok = false;
  __try {
    ok = getz_reads(p, len, MOV, 1, 0x04) && getz_reads(p, len, MOV, 1, 0x08) &&
         getz_reads(p, len, MOVZX, 2, 0x0C) && getz_reads(p, len, MOVZX, 2, 0x0E);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  return ok;
}

// `sites` counts only 0xE8 positions whose target is a real function entry, so an operand byte is not read as an
// opcode.
struct GetzHit { int sites; int targets; int shaped; unsigned rva; int capped; int faulted; };

static void find_getz(const unsigned char* base, unsigned polyBegin,
                      unsigned polyEnd, GetzHit* out) {
  out->sites = 0; out->targets = 0; out->shaped = 0;
  out->rva = 0; out->capped = 0; out->faulted = 0;
  if (polyEnd <= polyBegin + 5) { out->faulted = 1; return; }

  unsigned seen[EK_GETZ_MAX_TARGETS];
  int nseen = 0;
  __try {
    for (unsigned q = polyBegin; q + 5 <= polyEnd; q++) {
      if (base[q] != 0xE8) continue;
      const int disp = *(const int*)(base + q + 1);
      const unsigned t = ekm_us_rel32_target(q, disp);
      unsigned b = 0, e = 0;
      if (!ek_image_extent_containing(base, t, &b, &e) || b != t) continue;
      out->sites++;
      int dup = 0;
      for (int k = 0; k < nseen; k++) if (seen[k] == t) { dup = 1; break; }
      if (dup) continue;
      if (nseen < EK_GETZ_MAX_TARGETS) seen[nseen++] = t;
      else out->capped = 1;
      out->targets++;
      if (!getz_shape(base, b, e)) continue;
      out->shaped++;
      if (out->rva == 0) out->rva = t;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

static unsigned locate_getz(const unsigned char* base, unsigned polyBegin,
                            unsigned polyEnd) {
  GetzHit g;
  find_getz(base, polyBegin, polyEnd, &g);
  if (g.faulted) {
    ek_log("camera: the polymer_drawrooms body faulted mid-scan, so "
           "getzsofslopeptr cannot be claimed unique (%d call site(s), %d "
           "distinct target(s), %d of the shape before the fault); refusing",
           g.sites, g.targets, g.shaped);
    return 0;
  }
  if (g.shaped != 1) {
    ek_log("camera: getzsofslopeptr not identified -- %d `call rel32` site(s) "
           "inside polymer_drawrooms reach %d distinct function(s), %d of which "
           "open with the four sector reads engine.cpp:14288-14293 makes%s; "
           "refusing (r10669-ec5824db8 has 1 of 45). Sloped sectors keep the "
           "flat read and the pick declines to substitute on one",
           g.sites, g.targets, g.shaped, g.capped ? " (target list capped)" : "");
    return 0;
  }
  ek_log("camera: getzsofslopeptr at rva 0x%07X -- the only one of %d distinct "
         "target(s) of polymer_drawrooms' %d `call rel32` site(s) whose body "
         "opens with dword [sec+4], dword [sec+8], word [sec+0xC], word "
         "[sec+0xE]", g.rva, g.targets, g.sites);
  return g.rva;
}

// The site the sky redirect rewrites: find_getz was already standing on it; counted in its own pass because that loop
// skips targets it has seen.
struct GetzSite { int count; unsigned rva; int faulted; };

static void find_getz_site(const unsigned char* base, unsigned polyBegin,
                           unsigned polyEnd, unsigned target, GetzSite* out) {
  out->count = 0; out->rva = 0; out->faulted = 0;
  if (target == 0 || polyEnd <= polyBegin + 5) { out->faulted = 1; return; }
  __try {
    for (unsigned q = polyBegin; q + 5 <= polyEnd; q++) {
      if (base[q] != 0xE8) continue;
      const int disp = *(const int*)(base + q + 1);
      if (ekm_us_rel32_target(q, disp) != target) continue;
      if (out->count++ == 0) out->rva = q;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// Exactly one or nothing (freecam_redirect_locate_ok).
static unsigned locate_getz_site(const unsigned char* base, unsigned polyBegin,
                                 unsigned polyEnd, unsigned target) {
  GetzSite s;
  find_getz_site(base, polyBegin, polyEnd, target, &s);
  if (!freecam_redirect_locate_ok(s.count, s.faulted)) {
    ek_log("camera: the band test's own call to getzsofslopeptr is not unique "
           "inside the bracketed drawrooms -- %d site(s)%s; refusing to patch "
           "any of them (r10669-ec5824db8 has exactly 1). The sky keeps coming "
           "from the band pick, which finds one where a reachable sector "
           "contains the camera and nowhere else",
           s.count, s.faulted ? " (the body faulted mid-scan, so that count is "
                                "partial)" : "");
    return 0;
  }
  ek_log("camera: the band test's call to getzsofslopeptr is at rva 0x%07X -- "
         "the ONLY site inside the bracketed drawrooms reaching 0x%07X, which "
         "is what makes polymer.cpp:1211's two locals ours to widen",
         s.rva, target);
  return s.rva;
}

// The four calls the whole-map sweep makes: polymer.cpp:1232-1242's external-arm calls after the normal arm returns
// (the sweep PAYS for the sky). Each has VLOG_F anchors located by two witnesses, then cross-checked as four `call
// rel32` sites in polymer_drawrooms, one each, in source order; any disagreement refuses the sweep only.
static const char EK_US_ANCHOR_A[] = "Can't update uninitialized sector %i.";
static const char EK_US_ANCHOR_B[] = "Updated sector %i.";
static const char EK_DS_ANCHOR_A[] = "Drawing sector %i";
static const char EK_DS_ANCHOR_B[] = "Finished drawing sector %i.";
static const char EK_UW_ANCHOR_A[] = "Skipped wall %i.";
static const char EK_UW_ANCHOR_B[] = "Updated wall %i.";
static const char EK_DW_ANCHOR_A[] = "Drawing wall %i";
static const char EK_DW_ANCHOR_B[] = "Finished drawing wall %i.";

// The four entries in polymer.cpp:1232's order, or 0; s_sweepOk is the verdict: all four located, distinct, in
// call-site order.
volatile LONG s_usRva = 0;
volatile LONG s_dsRva = 0;
volatile LONG s_uwRva = 0;
volatile LONG s_dwRva = 0;
// The .refptr SLOT holding &prsectors[0], not the array: one more load per frame, nothing stale after a reload.
volatile LONG s_prsRva = 0;
// And the walls' own: polymer_drawwall dereferences prwalls[wallnum] unchecked (sectorstate.h).
volatile LONG s_prwRva = 0;
// drawingstate, the renderer's bit-per-sector record of what the portal walk drew; the ADDRESS, as a static array
// reached by lea.
volatile LONG s_dstateRva = 0;
// The three .refptr slots the sprite half reads: &sprite (only to cross-check the content scan), &tsprite and
// &spritesortcnt.
volatile LONG s_sprSlotRva = 0;
volatile LONG s_tsprSlotRva = 0;
volatile LONG s_ssCntSlotRva = 0;
// headspritesect[] and nextspritesect[], found in the same window: they replaced a walk of all 16384 sprite[] records
// per frame.
volatile LONG s_headSlotRva = 0;
volatile LONG s_nextSlotRva = 0;
volatile LONG s_sweepOk = 0;
// The sprite half's own verdict: it gates ONLY whether the sweep appends the swept sectors' sprites. It once gated
// s_sweepOk and then the sky redirect; both were reversed (a missing distant actor was never reported, a black sky
// three times).
volatile LONG s_spritesOk = 0;

// g_visibility, "Remove fog"'s one global, or 0: the ADDRESS, as a scalar written twice per drawn frame.
volatile LONGLONG s_gvisAddr = 0;

// ototalclock, the engine's tic counter, or 0 (which takes the whole camera: it cannot tell one tic from the next). The
// ADDRESS.
volatile LONGLONG s_otclkAddr = 0;

// `count`: `call rel32` sites inside polymer_drawrooms reaching each entry; `site` is the first.
struct SweepSites { int count[4]; unsigned site[4]; int faulted; };

static void find_sweep_sites(const unsigned char* base, unsigned polyBegin,
                             unsigned polyEnd, const unsigned* fn,
                             SweepSites* out) {
  for (int k = 0; k < 4; k++) { out->count[k] = 0; out->site[k] = 0; }
  out->faulted = 0;
  if (polyEnd <= polyBegin + 5) { out->faulted = 1; return; }
  __try {
    for (unsigned q = polyBegin; q + 5 <= polyEnd; q++) {
      if (base[q] != 0xE8) continue;
      const int disp = *(const int*)(base + q + 1);
      const unsigned t = ekm_us_rel32_target(q, disp);
      for (int k = 0; k < 4; k++) {
        if (t != fn[k]) continue;
        if (out->count[k]++ == 0) out->site[k] = q;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// Both witnesses for one function; 0 having logged which step refused.
static unsigned locate_pair(const unsigned char* base, const char* a, size_t alen,
                            const char* whatA, const char* b, size_t blen,
                            const char* whatB, const char* what) {
  const unsigned x = locate_once(base, a, alen, whatA, NULL, NULL);
  const unsigned y = locate_once(base, b, blen, whatB, NULL, NULL);
  if (x == 0 || y == 0) {
    ek_log("camera: %s could not be located by both of its anchors (witness 1 "
           "%s, witness 2 %s)", what, x ? "found" : "REFUSED",
           y ? "found" : "REFUSED");
    return 0;
  }
  if (x != y) {
    ek_log("camera: the two %s anchors name DIFFERENT functions (0x%07X and "
           "0x%07X); refusing to guess", what, x, y);
    return 0;
  }
  return x;
}

// prsectors[]: polymer_drawsector dereferences prsectors[sectnum] unchecked (polymer.cpp:3043), NULL past the loaded
// map, and the tiling heuristic can accept a stale record. Reached through the `.refptr` slot in polymer_updatesector's
// head; ekm_prs_* in sectorstate.h are the rules. The head window is a constraint, not slack.
static const unsigned EK_PRS_HEAD = 0x100;

struct PrsScan { unsigned slot; int loads; int refptrs; int faulted; };

static void find_prsectors_slot(const unsigned char* base, unsigned usBegin,
                                unsigned usEnd, PrsScan* out) {
  out->slot = 0; out->loads = 0; out->refptrs = 0; out->faulted = 0;
  if (usEnd <= usBegin + 9) { out->faulted = 1; return; }
  const unsigned end =
      (usEnd - usBegin > EK_PRS_HEAD) ? usBegin + EK_PRS_HEAD : usEnd;
  unsigned loadAt = 0;
  int baseReg = -1;
  __try {
    // `mov r64,[base+index*8]` / `test r,r` / `je`: REX, 0x8B, ModRM mod=00 rm=100 (SIB), SIB scale 8 and a real base.
    for (unsigned q = usBegin; q + 9 <= end; q++) {
      const unsigned char rex = base[q], op = base[q + 1];
      const unsigned char modrm = base[q + 2], sib = base[q + 3];
      if (rex < 0x48 || rex > 0x4F || op != 0x8B) continue;
      if ((modrm & 0xC7) != 0x04) continue;
      if ((sib & 0xC0) != 0xC0) continue;
      if ((sib & 0x07) == 0x05) continue;
      const int dst = ((rex & 4) << 1) | ((modrm >> 3) & 7);
      const unsigned char trex = base[q + 4], top = base[q + 5];
      const unsigned char tmod = base[q + 6];
      if (trex < 0x48 || trex > 0x4F || top != 0x85) continue;
      if ((tmod & 0xC0) != 0xC0) continue;
      if ((((trex & 4) << 1) | ((tmod >> 3) & 7)) != dst) continue;
      if ((((trex & 1) << 3) | (tmod & 7)) != dst) continue;
      const unsigned char j = base[q + 7];
      if (!(j == 0x74 || (j == 0x0F && base[q + 8] == 0x84))) continue;
      if (out->loads++ == 0) {
        loadAt = q;
        baseReg = ((rex & 1) << 3) | (sib & 7);
      }
    }
    // And the one `mov <that register>, qword [rip+disp32]` before it.
    if (out->loads == 1) {
      for (unsigned p = usBegin; p + 7 <= loadAt; p++) {
        const unsigned char rex = base[p];
        if (rex < 0x48 || rex > 0x4F || (rex & 8) == 0) continue;
        if (base[p + 1] != 0x8B) continue;
        const unsigned char modrm = base[p + 2];
        if ((modrm & 0xC7) != 0x05) continue;
        if ((((rex & 4) << 1) | ((modrm >> 3) & 7)) != baseReg) continue;
        const int disp = *(const int*)(base + p + 3);
        if (out->refptrs++ == 0) out->slot = (unsigned)((int)(p + 7) + disp);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

static int count_refptr_loads(const unsigned char* base, unsigned begin,
                              unsigned end, unsigned slot, int* faulted) {
  int n = 0;
  if (end <= begin + 7) { *faulted = 1; return 0; }
  __try {
    for (unsigned p = begin; p + 7 <= end; p++) {
      const unsigned char rex = base[p];
      if (rex < 0x48 || rex > 0x4F || (rex & 8) == 0) continue;
      if (base[p + 1] != 0x8B) continue;
      if ((base[p + 2] & 0xC7) != 0x05) continue;
      const int disp = *(const int*)(base + p + 3);
      if ((unsigned)((int)(p + 7) + disp) == slot) n++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = 1;
  }
  return n;
}

// Read from the headers HERE, not from gamestate.cpp: camera_scan runs on the render thread and can beat the scan
// thread.
size_t host_image_size(const unsigned char* base) {
  size_t n = 0;
  __try {
    const IMAGE_DOS_HEADER* const dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS64* const nt =
        (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    n = (size_t)nt->OptionalHeader.SizeOfImage;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
  return n;
}

static bool rva_in_data_section(const unsigned char* base, unsigned rva) {
  EkSpan spans[16];
  int n = 0;
  if (!ek_image_sections(base, false, spans, 16, &n)) return false;
  for (int i = 0; i < n; i++) {
    const unsigned b = ek_image_rva(base, spans[i].base);
    if (rva >= b && rva < b + spans[i].size) return true;
  }
  return false;
}

static unsigned locate_prsectors(const unsigned char* base, const unsigned* fn) {
  unsigned b[4] = { 0, 0, 0, 0 }, e[4] = { 0, 0, 0, 0 };
  for (int k = 0; k < 4; k++)
    if (!ek_image_extent_containing(base, fn[k], &b[k], &e[k])) {
      ek_log("camera: the whole-map sweep cannot bound polymer function %d "
             "(rva 0x%07X) in the exception directory, so prsectors[] cannot "
             "be reached; the sweep stays unavailable", k, fn[k]);
      return 0;
    }

  PrsScan s;
  find_prsectors_slot(base, b[0], e[0], &s);
  if (s.faulted || !ekm_prs_shape_ok(s.loads, s.refptrs)) {
    ek_log("camera: prsectors[] is not where polymer.cpp:2624 puts it -- %d "
           "candidate indexed load(s) and %d refptr load(s) in the first 0x%X "
           "bytes of polymer_updatesector (0x%07X)%s; r10669-ec5824db8 has one "
           "of each. Refusing: the whole-map sweep stays unavailable and the "
           "camera is unaffected",
           s.loads, s.refptrs, EK_PRS_HEAD, b[0],
           s.faulted ? " (the head faulted mid-scan, so these counts are "
                       "partial)" : "");
    return 0;
  }

  // The witnesses, from polymer.cpp's own uses rather than from this build.
  int faulted = 0;
  const int us = count_refptr_loads(base, b[0], e[0], s.slot, &faulted);
  const int ds = count_refptr_loads(base, b[1], e[1], s.slot, &faulted);
  const int uw = count_refptr_loads(base, b[2], e[2], s.slot, &faulted);
  const int dw = count_refptr_loads(base, b[3], e[3], s.slot, &faulted);
  if (faulted || !ekm_prs_witness_ok(us, ds, uw, dw)) {
    ek_log("camera: the slot at rva 0x%07X is not read the way polymer.cpp "
           "reads prsectors[] -- updatesector x%d, drawsector x%d, updatewall "
           "x%d, drawwall x%d%s; the source has drawsector :3043 and updatewall "
           ":3258/:3262 reading it and drawwall never. Refusing: the whole-map "
           "sweep stays unavailable",
           s.slot, us, ds, uw, dw,
           faulted ? " (a body faulted mid-scan, so these counts are partial)"
                   : "");
    return 0;
  }

  // And the span: the slot is image data; what it holds is an image address with room for the whole array.
  const size_t image = host_image_size(base);
  const int inData = rva_in_data_section(base, s.slot) ? 1 : 0;
  size_t arr = 0;
  __try {
    const unsigned char* const* const slotp =
        (const unsigned char* const*)(base + s.slot);
    const unsigned char* const a = *slotp;
    if (a > base) arr = (size_t)(a - base);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    arr = 0;
  }
  if (!ekm_prs_span_ok((size_t)s.slot, arr, image, inData)) {
    ek_log("camera: the prsectors[] candidate does not lie in the host image -- "
           "slot rva 0x%07X (in data: %s), array rva 0x%llX, image 0x%llX bytes, "
           "and %d pointers must fit. Refusing: the whole-map sweep stays "
           "unavailable",
           s.slot, inData ? "yes" : "NO", (unsigned long long)arr,
           (unsigned long long)image, EK_MAXSECTORS);
    return 0;
  }

  ek_log("camera: prsectors[] reached through the refptr slot at rva 0x%07X -- "
         "one indexed load at 0x%07X in polymer_updatesector's head with one "
         "refptr before it, read %d/%d/%d/%d times by updatesector/drawsector/"
         "updatewall/drawwall, naming array rva 0x%llX inside a 0x%llX-byte "
         "image",
         s.slot, b[0], us, ds, uw, dw, (unsigned long long)arr,
         (unsigned long long)image);
  return s.slot;
}

// prwalls[]: same bound for the wall half (sectorstate.h's `ACCEPTING prwalls[]'s refptr SLOT`). Read out of
// polymer_drawwall, which is short (0x397 bytes) and indexes exactly one pointer array, so "one scale-8 `mov` in the
// body" needs no window. A refusal costs the sweep only and does not fall back to the tiling chain.

struct PrwScan { unsigned slot; int loads; int refptrs; int faulted; };

static void find_prwalls_slot(const unsigned char* base, unsigned dwBegin,
                              unsigned dwEnd, PrwScan* out) {
  out->slot = 0; out->loads = 0; out->refptrs = 0; out->faulted = 0;
  if (dwEnd <= dwBegin + 4) { out->faulted = 1; return; }
  unsigned loadAt = 0;
  int baseReg = -1;
  __try {
    // `mov r64,[base+index*8]` with NO test/je after it, unlike prsectors[]: neither reader null-checks this array,
    // which is why the sweep has to.
    for (unsigned q = dwBegin; q + 4 <= dwEnd; q++) {
      const unsigned char rex = base[q], op = base[q + 1];
      const unsigned char modrm = base[q + 2], sib = base[q + 3];
      if (rex < 0x48 || rex > 0x4F || op != 0x8B) continue;
      if ((modrm & 0xC7) != 0x04) continue;
      if ((sib & 0xC0) != 0xC0) continue;
      if ((sib & 0x07) == 0x05) continue;
      if (out->loads++ == 0) {
        loadAt = q;
        baseReg = ((rex & 1) << 3) | (sib & 7);
      }
    }
    // And the one `mov <that register>, qword [rip+disp32]` before it.
    if (out->loads == 1) {
      for (unsigned p = dwBegin; p + 7 <= loadAt; p++) {
        const unsigned char rex = base[p];
        if (rex < 0x48 || rex > 0x4F || (rex & 8) == 0) continue;
        if (base[p + 1] != 0x8B) continue;
        const unsigned char modrm = base[p + 2];
        if ((modrm & 0xC7) != 0x05) continue;
        if ((((rex & 4) << 1) | ((modrm >> 3) & 7)) != baseReg) continue;
        const int disp = *(const int*)(base + p + 3);
        if (out->refptrs++ == 0) out->slot = (unsigned)((int)(p + 7) + disp);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// `prsSlot` is the sector array's own accepted slot, the one answer this may not return.
static unsigned locate_prwalls(const unsigned char* base, const unsigned* fn,
                               unsigned prsSlot) {
  unsigned b[4] = { 0, 0, 0, 0 }, e[4] = { 0, 0, 0, 0 };
  for (int k = 0; k < 4; k++)
    if (!ek_image_extent_containing(base, fn[k], &b[k], &e[k])) {
      ek_log("camera: the whole-map sweep cannot bound polymer function %d "
             "(rva 0x%07X) in the exception directory, so prwalls[] cannot be "
             "reached; the sweep stays unavailable", k, fn[k]);
      return 0;
    }

  PrwScan s;
  find_prwalls_slot(base, b[3], e[3], &s);
  if (s.faulted || !ekm_prw_shape_ok(s.loads, s.refptrs)) {
    ek_log("camera: prwalls[] is not where polymer.cpp:3652 puts it -- %d "
           "candidate indexed load(s) and %d refptr load(s) in the whole of "
           "polymer_drawwall (0x%07X..0x%07X)%s; r10669-ec5824db8 has one of "
           "each. Refusing: the whole-map sweep stays unavailable and the "
           "camera is unaffected",
           s.loads, s.refptrs, b[3], e[3],
           s.faulted ? " (the body faulted mid-scan, so these counts are "
                       "partial)" : "");
    return 0;
  }

  // The witnesses, from polymer.cpp's own uses and NOT from this build.
  int faulted = 0;
  const int ds = count_refptr_loads(base, b[1], e[1], s.slot, &faulted);
  const int uw = count_refptr_loads(base, b[2], e[2], s.slot, &faulted);
  const int dw = count_refptr_loads(base, b[3], e[3], s.slot, &faulted);
  const int same = (prsSlot != 0 && s.slot == prsSlot) ? 1 : 0;
  if (faulted || !ekm_prw_witness_ok(ds, uw, dw, same)) {
    ek_log("camera: the slot at rva 0x%07X is not read the way polymer.cpp "
           "reads prwalls[] -- drawsector x%d, updatewall x%d, drawwall x%d, "
           "and prsectors[] named 0x%07X%s; the source has updatewall :3257 "
           "and drawwall :3652 reading it and drawsector never. Refusing: the "
           "whole-map sweep stays unavailable",
           s.slot, ds, uw, dw, prsSlot,
           faulted ? " (a body faulted mid-scan, so these counts are partial)"
                   : "");
    return 0;
  }

  // And the span: MAXWALLS pointers, four times prsectors[]'s bound.
  const size_t image = host_image_size(base);
  const int inData = rva_in_data_section(base, s.slot) ? 1 : 0;
  size_t arr = 0;
  __try {
    const unsigned char* const* const slotp =
        (const unsigned char* const*)(base + s.slot);
    const unsigned char* const a = *slotp;
    if (a > base) arr = (size_t)(a - base);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    arr = 0;
  }
  if (!ekm_prw_span_ok((size_t)s.slot, arr, image, inData)) {
    ek_log("camera: the prwalls[] candidate does not lie in the host image -- "
           "slot rva 0x%07X (in data: %s), array rva 0x%llX, image 0x%llX "
           "bytes, and %d pointers must fit. Refusing: the whole-map sweep "
           "stays unavailable",
           s.slot, inData ? "yes" : "NO", (unsigned long long)arr,
           (unsigned long long)image, EK_MAXWALLS);
    return 0;
  }

  ek_log("camera: prwalls[] reached through the refptr slot at rva 0x%07X -- "
         "one indexed load in the whole of polymer_drawwall (0x%07X..0x%07X) "
         "with one refptr before it, read %d/%d/%d times by drawsector/"
         "updatewall/drawwall, naming array rva 0x%llX inside a 0x%llX-byte "
         "image and distinct from prsectors[] at 0x%07X",
         s.slot, b[3], e[3], ds, uw, dw, (unsigned long long)arr,
         (unsigned long long)image, prsSlot);
  return s.slot;
}

// drawingstate[]: what the portal walk drew, so the sweep adds only the rest (drawing every sector ran at 30-40 fps).
// No name or string, so it is found by call graph: the one callee of the located drawrooms calling drawsector and
// drawwall twice each (ekm_dstate_* in sectorstate.h). A mirror in view disturbs it (polymer.cpp:2072-2129): disclosed.
static const unsigned EK_DSTATE_BACK  = 0x100;
static const unsigned EK_DSTATE_AHEAD = 0x20;
static const int EK_DSTATE_CALLEES = 96;

// `lea r64,[rip+disp32]` and what it reaches; only REX 0x48 and 0x4C can carry it (X and B mean nothing without SIB or
// base).
static bool lea_rip_target(const unsigned char* base, unsigned q, unsigned* out) {
  const unsigned char rex = base[q];
  if (rex != 0x48 && rex != 0x4C) return false;
  if (base[q + 1] != 0x8D) return false;
  if ((base[q + 2] & 0xC7) != 0x05) return false;
  const int disp = *(const int*)(base + q + 3);
  *out = (unsigned)((int)(q + 7) + disp);
  return true;
}

static void count_two_targets(const unsigned char* base, unsigned begin,
                              unsigned end, unsigned a, unsigned b, int* na,
                              int* nb, int* faulted) {
  *na = 0; *nb = 0;
  if (end <= begin + 5) { *faulted = 1; return; }
  __try {
    for (unsigned q = begin; q + 5 <= end; q++) {
      if (base[q] != 0xE8) continue;
      const unsigned t = ekm_us_rel32_target(q, *(const int*)(base + q + 1));
      if (t == a) (*na)++;
      if (t == b) (*nb)++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = 1;
  }
}

struct DstateScan {
  unsigned rva; unsigned stosAt;
  int stos; int counts; int leas; int confirms; int uses; int faulted;
};

static void find_drawingstate(const unsigned char* base, unsigned begin,
                              unsigned end, DstateScan* out) {
  out->rva = 0; out->stosAt = 0; out->stos = 0; out->counts = 0;
  out->leas = 0; out->confirms = 0; out->uses = 0; out->faulted = 0;
  if (end <= begin + 8) { out->faulted = 1; return; }
  __try {
    // polymer.cpp:1827's 512-byte zero as one `rep stosq`: the only fixed-size Bmemset in this function.
    for (unsigned q = begin; q + 3 <= end; q++)
      if (base[q] == 0xF3 && base[q + 1] == 0x48 && base[q + 2] == 0xAB)
        if (out->stos++ == 0) out->stosAt = q;
    if (out->stos != 1) return;

    const unsigned w0 = (out->stosAt - begin > EK_DSTATE_BACK)
                            ? out->stosAt - EK_DSTATE_BACK : begin;
    // `mov ecx, 0x40`: 64 qwords, which IS sizeof(drawingstate).
    for (unsigned q = w0; q + 5 <= out->stosAt; q++)
      if (base[q] == 0xB9 && base[q + 1] == 0x40 && base[q + 2] == 0 &&
          base[q + 3] == 0 && base[q + 4] == 0)
        out->counts++;
    // And the one `lea rdi,[rip+disp32]` that gave the stosq its destination.
    unsigned leaAt = 0;
    for (unsigned q = w0; q + 7 <= out->stosAt; q++)
      if (base[q] == 0x48 && base[q + 1] == 0x8D && base[q + 2] == 0x3D)
        if (out->leas++ == 0) leaAt = q;
    if (out->leas != 1) return;
    unsigned cand = 0;
    if (!lea_rip_target(base, leaAt, &cand)) { out->leas = 0; return; }

    // polymer.cpp:1832's bitmap_set on the next line names the same address again: this says the 512 bytes are the
    // sector bitmap.
    const unsigned w1 = (out->stosAt + EK_DSTATE_AHEAD < end)
                            ? out->stosAt + EK_DSTATE_AHEAD : end;
    for (unsigned q = out->stosAt + 3; q + 7 <= w1; q++) {
      unsigned t = 0;
      if (lea_rip_target(base, q, &t) && t == cand) out->confirms++;
    }
    for (unsigned q = begin; q + 7 <= end; q++) {
      unsigned t = 0;
      if (lea_rip_target(base, q, &t) && t == cand) out->uses++;
    }
    out->rva = cand;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

static unsigned locate_drawingstate(const unsigned char* base,
                                    unsigned polyBegin, unsigned polyEnd,
                                    unsigned dsRva, unsigned dwRva) {
  unsigned seen[EK_DSTATE_CALLEES];
  int nseen = 0, capped = 0, faulted = 0;
  if (polyEnd <= polyBegin + 5) {
    ek_log("camera: drawrooms has no body to read call sites from; the "
           "whole-map sweep stays unavailable");
    return 0;
  }
  __try {
    for (unsigned q = polyBegin; q + 5 <= polyEnd; q++) {
      if (base[q] != 0xE8) continue;
      const unsigned t = ekm_us_rel32_target(q, *(const int*)(base + q + 1));
      int have = 0;
      for (int i = 0; i < nseen; i++) if (seen[i] == t) { have = 1; break; }
      if (have) continue;
      if (nseen >= EK_DSTATE_CALLEES) { capped = 1; break; }
      seen[nseen++] = t;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    faulted = 1;
  }

  int cands = 0, bestDs = 0, bestDw = 0;
  unsigned cb = 0, ce = 0;
  for (int i = 0; i < nseen && !faulted; i++) {
    unsigned b = 0, e = 0;
    if (!ek_image_extent_containing(base, seen[i], &b, &e)) continue;
    int nds = 0, ndw = 0;
    count_two_targets(base, b, e, dsRva, dwRva, &nds, &ndw, &faulted);
    if (nds < 1 || ndw < 1) continue;
    if (cands++ == 0) { cb = b; ce = e; bestDs = nds; bestDw = ndw; }
  }
  if (faulted || capped || !ekm_dstate_owner_ok(cands, bestDs, bestDw)) {
    ek_log("camera: polymer_displayrooms is not the one callee of drawrooms "
           "that draws both sectors and walls -- %d of %d distinct callee(s) "
           "call both, the first with drawsector x%d and drawwall x%d%s%s; "
           "polymer.cpp:1859/:2058 and :1978/:2063 make it two each. Refusing: "
           "the whole-map sweep stays unavailable and the camera is unaffected",
           cands, nseen, bestDs, bestDw,
           capped ? " (the callee list filled, so this count is partial)" : "",
           faulted ? " (a body faulted mid-scan, so these counts are partial)"
                   : "");
    return 0;
  }

  DstateScan s;
  find_drawingstate(base, cb, ce, &s);
  if (s.faulted || !ekm_dstate_shape_ok(s.stos, s.counts, s.leas, s.confirms)) {
    ek_log("camera: drawingstate is not where polymer.cpp:1827 puts it -- %d "
           "`rep stosq` site(s) in polymer_displayrooms (0x%07X..0x%07X), with "
           "%d `mov ecx,0x40` and %d `lea rdi,[rip]` in the 0x%X before it and "
           "%d confirming lea(s) in the 0x%X after it%s; r10669-ec5824db8 has "
           "one of each. Refusing: the whole-map sweep stays unavailable",
           s.stos, cb, ce, s.counts, s.leas, EK_DSTATE_BACK, s.confirms,
           EK_DSTATE_AHEAD,
           s.faulted ? " (the body faulted mid-scan, so these counts are "
                       "partial)" : "");
    return 0;
  }
  if (!ekm_dstate_use_ok(s.uses)) {
    ek_log("camera: the array at rva 0x%07X is used %d time(s) inside "
           "polymer_displayrooms; polymer.cpp uses drawingstate nine times "
           "there, five of them outside the TROR block. Refusing: the "
           "whole-map sweep stays unavailable", s.rva, s.uses);
    return 0;
  }

  // The section, not the image: rva_in_data_section uses the search filter, which drops .bss, where a static array
  // lives, so it refused every time and the sweep never ran. The extent walk sees uninitialised sections.
  unsigned secBegin = 0, secEnd = 0;
  const int inData =
      ek_image_data_extent(base, s.rva, &secBegin, &secEnd) ? 1 : 0;
  if (!ekm_dstate_span_ok((size_t)s.rva, (size_t)secBegin, (size_t)secEnd,
                          inData)) {
    // Both what it looked for and what it saw: the two refusals are different faults.
    if (!inData)
      ek_log("camera: the drawingstate candidate at rva 0x%07X is in no "
             "readable non-executable section of the host -- looked for the "
             "one section holding all %d byte(s) of the bitmap, and no section "
             "covers that rva (an executable or unreadable one, or an address "
             "past every section). Refusing: the whole-map sweep stays "
             "unavailable", s.rva, EK_SECTOR_BITMAP_BYTES);
    else
      ek_log("camera: the drawingstate candidate at rva 0x%07X has no room for "
             "the whole bitmap -- its section runs 0x%07X..0x%07X and leaves "
             "%u byte(s) after the candidate, where %d are needed. Refusing: "
             "the whole-map sweep stays unavailable",
             s.rva, secBegin, secEnd, (unsigned)(secEnd - s.rva),
             EK_SECTOR_BITMAP_BYTES);
    return 0;
  }

  ek_log("camera: drawingstate at rva 0x%07X -- the 512-byte zero at 0x%07X "
         "inside the one callee of drawrooms that draws both sectors (x%d) and "
         "walls (x%d), 0x%07X..0x%07X, confirmed by the bitmap_set that follows "
         "it and used %d time(s) in that body; it sits in the non-executable "
         "section at 0x%07X..0x%07X, which is .bss on r10669-ec5824db8",
         s.rva, s.stosAt, bestDs, bestDw, cb, ce, s.uses, secBegin, secEnd);
  return s.rva;
}

// tsprite[] and spritesortcnt for the swept sectors' actors. polymer_scansprites (polymer.cpp:1234) is `static inline`,
// so the loop is written out; tsprites.h holds the rules. The window is two proved call sites (drawsector's and
// updatewall's); inside, one `imul 44`, one `add`, two pointer loads, one compare against 4095. Witness: gamestate's content scan.
static const unsigned EK_SS_ADD  = 0x10;
static const unsigned EK_SS_CMP  = 0x10;
static const unsigned EK_SS_BACK = 0x80;

struct SsScan {
  int imuls, adds, derefs, spriteDeref, counters;
  int spriteLoads, tspriteLoads, countLoads;
  unsigned spriteSlot, tspriteSlot, countSlot;
  int heads, nexts, headLoads, nextLoads;
  unsigned headSlot, nextSlot;
  unsigned imulAt;
  int faulted;
};

static bool rip_mov_target(const unsigned char* base, unsigned q, int* dst,
                           unsigned* target) {
  const unsigned char rex = base[q];
  if (rex < 0x48 || rex > 0x4F) return false;
  if (base[q + 1] != 0x8B) return false;
  if ((base[q + 2] & 0xC7) != 0x05) return false;
  *dst = ((rex & 4) << 1) | ((base[q + 2] >> 3) & 7);
  *target = (unsigned)((int)(q + 7) + *(const int*)(base + q + 3));
  return true;
}

// How many such loads into `reg` lie before the USE that reads it, bounded at the use so the wall loop's own later load
// cannot count.
static int rip_mov_into(const unsigned char* base, unsigned lo, unsigned use,
                        int reg, unsigned* slot) {
  int n = 0;
  *slot = 0;
  if (use <= lo + 6) return 0;
  for (unsigned q = lo; q + 7 <= use; q++) {
    int dst = -1;
    unsigned t = 0;
    if (!rip_mov_target(base, q, &dst, &t)) continue;
    if (dst != reg) continue;
    if (n++ == 0) *slot = t;
  }
  return n;
}

static void find_scansprites(const unsigned char* base, unsigned polyBegin,
                             unsigned w0, unsigned w1, SsScan* out) {
  out->imuls = 0; out->adds = 0; out->derefs = 0; out->spriteDeref = 0;
  out->counters = 0; out->spriteLoads = 0; out->tspriteLoads = 0;
  out->countLoads = 0; out->spriteSlot = 0; out->tspriteSlot = 0;
  out->countSlot = 0; out->heads = 0; out->nexts = 0; out->headLoads = 0;
  out->nextLoads = 0; out->headSlot = 0; out->nextSlot = 0;
  out->imulAt = 0; out->faulted = 0;
  if (w1 <= w0 + 16) { out->faulted = 1; return; }
  __try {
    // &sprite[i] is `imul r64,r64,44`, the stride this port reads every sprite through; its SOURCE register is the
    // sprite index, which tells the two chain arrays apart below.
    unsigned imulAt = 0;
    int imulDst = -1;
    int imulSrc = -1;
    for (unsigned q = w0; q + 4 <= w1; q++) {
      const unsigned char rex = base[q];
      if (rex < 0x48 || rex > 0x4F) continue;
      if (base[q + 1] != 0x6B) continue;
      if ((base[q + 2] & 0xC0) != 0xC0) continue;
      if (base[q + 3] != (unsigned char)EK_SPR_STRIDE) continue;
      if (out->imuls++ == 0) {
        imulAt = q;
        imulDst = ((rex & 4) << 1) | ((base[q + 2] >> 3) & 7);
        imulSrc = ((rex & 1) << 3) | (base[q + 2] & 7);
      }
    }
    if (out->imuls != 1) return;
    out->imulAt = imulAt;

    // Then `add <that register>, <array base>`: the register added holds sprite[], so the other pointer load is
    // tsprite[] by elimination.
    int sprReg = -1;
    const unsigned addEnd = (imulAt + EK_SS_ADD < w1) ? imulAt + EK_SS_ADD : w1;
    for (unsigned q = imulAt + 4; q + 3 <= addEnd; q++) {
      const unsigned char rex = base[q];
      if (rex < 0x48 || rex > 0x4F) continue;
      if (base[q + 1] != 0x01) continue;
      const unsigned char m = base[q + 2];
      if ((m & 0xC0) != 0xC0) continue;
      if ((((rex & 1) << 3) | (m & 7)) != imulDst) continue;
      if (out->adds++ == 0) sprReg = ((rex & 4) << 1) | ((m >> 3) & 7);
    }
    if (out->adds != 1) return;

    // The two `mov r64, qword [reg]`: mingw loads each .refptr slot once and dereferences it here, so these are the
    // arrays themselves.
    unsigned useAt[2] = { 0, 0 };
    int dstReg[2] = { -1, -1 };
    int baseReg[2] = { -1, -1 };
    for (unsigned q = w0; q + 3 <= w1; q++) {
      const unsigned char rex = base[q];
      if (rex < 0x48 || rex > 0x4F) continue;
      if (base[q + 1] != 0x8B) continue;
      const unsigned char m = base[q + 2];
      if ((m & 0xC0) != 0x00) continue;
      const int rm = m & 7;
      if (rm == 4 || rm == 5) continue;
      if (out->derefs++ < 2) {
        useAt[out->derefs - 1] = q;
        dstReg[out->derefs - 1] = ((rex & 4) << 1) | ((m >> 3) & 7);
        baseReg[out->derefs - 1] = ((rex & 1) << 3) | rm;
      }
    }
    if (out->derefs != 2) return;
    const int si = (dstReg[0] == sprReg) ? 0 : (dstReg[1] == sprReg ? 1 : -1);
    if (si < 0) return;
    out->spriteDeref = 1;

    // The counter and its bound: polymer.cpp:3898's `< MAXSPRITESONSCREEN` compiles to a compare with 4095, the
    // engine's own statement of tsprite[]'s size.
    unsigned cntAt = 0;
    int cntBase = -1;
    for (unsigned q = w0; q + 3 <= w1; q++) {
      const unsigned char rex = base[q];
      if (rex < 0x48 || rex > 0x4F) continue;
      if (base[q + 1] != 0x63) continue;
      const unsigned char m = base[q + 2];
      if ((m & 0xC0) != 0x00) continue;
      const int rm = m & 7;
      if (rm == 4 || rm == 5) continue;
      const int dst = ((rex & 4) << 1) | ((m >> 3) & 7);
      const unsigned cmpEnd = (q + EK_SS_CMP < w1) ? q + EK_SS_CMP : w1;
      bool matched = false;
      for (unsigned p = q + 3; p + 6 <= cmpEnd && !matched; p++) {
        unsigned r = p;
        int rexB = 0;
        const unsigned char pre = base[r];
        if (pre >= 0x40 && pre <= 0x4F) {
          if (pre & 8) continue;
          rexB = (pre & 1) << 3;
          r++;
        }
        if (r + 6 > cmpEnd) break;
        if (base[r] != 0x81) continue;
        const unsigned char cm = base[r + 1];
        if ((cm & 0xC0) != 0xC0 || ((cm >> 3) & 7) != 7) continue;
        if ((rexB | (cm & 7)) != dst) continue;
        if (*(const int*)(base + r + 2) != EK_MAXSPRITESONSCREEN - 1) continue;
        matched = true;
      }
      if (!matched) continue;
      if (out->counters++ == 0) {
        cntAt = q;
        cntBase = ((rex & 1) << 3) | rm;
      }
    }
    if (out->counters != 1) return;

    // The chain's two arrays are the loop header and increment, two `movsx r32, word [base+idx*2]`; the one indexed by
    // the imul'd (sprite) register is nextspritesect[]. Anchored on the `0F BF` pair so each instruction is visited
    // once; the prefix is read from the byte before.
    unsigned headAt = 0, nextAt = 0;
    int headBase = -1, nextBase = -1;
    for (unsigned p = w0; p + 5 <= w1; p++) {
      if (base[p] != 0x0F || base[p + 1] != 0xBF) continue;
      int rexX = 0, rexB = 0;
      if (p > w0) {
        const unsigned char pre = base[p - 1];
        if (pre >= 0x40 && pre <= 0x4F) { rexX = (pre >> 1) & 1; rexB = pre & 1; }
      }
      const unsigned char m = base[p + 2];
      if ((m & 0xC0) == 0xC0) continue;
      if ((m & 7) != 4) continue;
      const unsigned char sib = base[p + 3];
      if ((sib >> 6) != 1) continue;
      const int idx = (rexX << 3) | ((sib >> 3) & 7);
      if (idx == 4) continue;
      if ((m & 0xC0) == 0 && (sib & 7) == 5) continue;
      const int bse = (rexB << 3) | (sib & 7);
      if (idx == imulSrc) {
        if (out->nexts++ == 0) { nextAt = p; nextBase = bse; }
      } else {
        if (out->heads++ == 0) { headAt = p; headBase = bse; }
      }
    }
    if (!ekm_chain_shape_ok(out->heads, out->nexts)) return;

    // And the hoisted slot load each of the five uses reads its base from.
    const unsigned lo =
        (w0 > polyBegin + EK_SS_BACK) ? w0 - EK_SS_BACK : polyBegin;
    out->spriteLoads =
        rip_mov_into(base, lo, useAt[si], baseReg[si], &out->spriteSlot);
    out->tspriteLoads = rip_mov_into(base, lo, useAt[1 - si], baseReg[1 - si],
                                     &out->tspriteSlot);
    out->countLoads = rip_mov_into(base, lo, cntAt, cntBase, &out->countSlot);
    out->headLoads = rip_mov_into(base, lo, headAt, headBase, &out->headSlot);
    out->nextLoads = rip_mov_into(base, lo, nextAt, nextBase, &out->nextSlot);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// The image rva a .refptr slot holds, or 0 when unreadable or below the module; callers hand it to a rule that bounds
// it.
size_t slot_target_rva(const unsigned char* base, unsigned slot) {
  size_t rva = 0;
  __try {
    const unsigned char* const* const p =
        (const unsigned char* const*)(base + slot);
    const unsigned char* const a = *p;
    if (a > base) rva = (size_t)(a - base);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    rva = 0;
  }
  return rva;
}

// True having published the five slots. The geometry sweep and the camera run either way; a refusal costs the swept
// sectors' actors.
static bool locate_scansprites(const unsigned char* base, unsigned polyBegin,
                               unsigned dsSite, unsigned uwSite) {
  SsScan s;
  find_scansprites(base, polyBegin, dsSite + 5, uwSite, &s);
  if (s.faulted || !ekm_scansprites_shape_ok(s.imuls, s.adds, s.derefs,
                                             s.spriteDeref, s.counters)) {
    ek_log("camera: polymer_scansprites is not inlined the way polymer.cpp:1234 "
           "writes it -- between the drawsector call at 0x%07X and the "
           "updatewall call at 0x%07X there are %d record multiply(s) by %d, %d "
           "add(s) naming an array base, %d pointer load(s) of which %d names "
           "that base, and %d counter(s) compared with %d%s; r10669-ec5824db8 "
           "has one, one, two, one and one. Refusing: the swept sectors draw "
           "without their actors. The sky redirect, the geometry sweep and the "
           "camera are unaffected",
           dsSite, uwSite, s.imuls, EK_SPR_STRIDE, s.adds, s.derefs,
           s.spriteDeref, s.counters, EK_MAXSPRITESONSCREEN - 1,
           s.faulted ? " (the window faulted mid-scan, so these counts are "
                       "partial)" : "");
    return false;
  }
  if (!ekm_chain_shape_ok(s.heads, s.nexts)) {
    ek_log("camera: the inlined scansprites does not walk a sector's sprite "
           "chain the way polymer.cpp:3893 writes it -- %d sector-indexed and "
           "%d sprite-indexed `movsx r32, word [base+idx*2]` in the window; one "
           "of each is what headspritesect[sectnum] and nextspritesect[i] emit. "
           "Refusing: the swept sectors draw without their actors and the sky "
           "redirect stays out",
           s.heads, s.nexts);
    return false;
  }
  if (!ekm_scansprites_slots_ok(s.spriteLoads, s.tspriteLoads, s.countLoads,
                                s.spriteSlot, s.tspriteSlot, s.countSlot)) {
    ek_log("camera: the inlined scansprites does not reach its three arrays "
           "through one refptr slot each -- sprite x%d (0x%07X), tsprite x%d "
           "(0x%07X), spritesortcnt x%d (0x%07X) in the 0x%X bytes before their "
           "uses; one apiece and all three distinct is what mingw emits. "
           "Refusing: the swept sectors draw without their actors and the sky "
           "redirect stays out",
           s.spriteLoads, s.spriteSlot, s.tspriteLoads, s.tspriteSlot,
           s.countLoads, s.countSlot, EK_SS_BACK);
    return false;
  }
  if (!ekm_chain_slots_ok(s.headLoads, s.nextLoads, s.headSlot, s.nextSlot,
                          s.spriteSlot, s.tspriteSlot, s.countSlot)) {
    ek_log("camera: the chain arrays are not reached through one refptr slot "
           "each -- headspritesect x%d (0x%07X), nextspritesect x%d (0x%07X) in "
           "the 0x%X bytes before their uses, against sprite 0x%07X, tsprite "
           "0x%07X and spritesortcnt 0x%07X; one apiece and five distinct slots "
           "is what mingw emits. Refusing: the swept sectors draw without their "
           "actors; the sky redirect and the geometry sweep are unaffected",
           s.headLoads, s.headSlot, s.nextLoads, s.nextSlot, EK_SS_BACK,
           s.spriteSlot, s.tspriteSlot, s.countSlot);
    return false;
  }

  // The spans: the slots are image data and hold .bss statics (tsprite_s, engine.cpp:9160, and spritesortcnt), so the
  // image is the extent.
  const size_t image = host_image_size(base);
  const size_t sprTarget = slot_target_rva(base, s.spriteSlot);
  const size_t tsprTarget = slot_target_rva(base, s.tspriteSlot);
  const size_t cntTarget = slot_target_rva(base, s.countSlot);
  const int sprIn = rva_in_data_section(base, s.spriteSlot) ? 1 : 0;
  const int tsprIn = rva_in_data_section(base, s.tspriteSlot) ? 1 : 0;
  const int cntIn = rva_in_data_section(base, s.countSlot) ? 1 : 0;
  if (!ekm_tsprite_slot_ok(s.spriteSlot, sprTarget, image, sprIn, sizeof(void*)) ||
      !ekm_tsprite_slot_ok(s.tspriteSlot, tsprTarget, image, tsprIn, sizeof(void*)) ||
      !ekm_tsprite_slot_ok(s.countSlot, cntTarget, image, cntIn, sizeof(int))) {
    ek_log("camera: one of the scansprites slots does not lie in the host image "
           "-- sprite slot 0x%07X (in data: %s) -> 0x%llX, tsprite slot 0x%07X "
           "(%s) -> 0x%llX, spritesortcnt slot 0x%07X (%s) -> 0x%llX, image "
           "0x%llX bytes. Refusing: the swept sectors draw without their "
           "actors; the sky redirect and the geometry sweep are unaffected",
           s.spriteSlot, sprIn ? "yes" : "NO", (unsigned long long)sprTarget,
           s.tspriteSlot, tsprIn ? "yes" : "NO", (unsigned long long)tsprTarget,
           s.countSlot, cntIn ? "yes" : "NO", (unsigned long long)cntTarget,
           (unsigned long long)image);
    return false;
  }

  // The chain arrays' spans are the arrays themselves (the slot holds &headspritesect[0] and the load indexes straight
  // off it): MAXSECTORS+1 and MAXSPRITES shorts (build.h:885, :887).
  const size_t headTarget = slot_target_rva(base, s.headSlot);
  const size_t nextTarget = slot_target_rva(base, s.nextSlot);
  const int headIn = rva_in_data_section(base, s.headSlot) ? 1 : 0;
  const int nextIn = rva_in_data_section(base, s.nextSlot) ? 1 : 0;
  if (!ekm_tsprite_slot_ok(s.headSlot, headTarget, image, headIn,
                           (size_t)EK_HEADSPRITESECT_SLOTS * sizeof(short)) ||
      !ekm_tsprite_slot_ok(s.nextSlot, nextTarget, image, nextIn,
                           (size_t)EK_NEXTSPRITESECT_SLOTS * sizeof(short))) {
    ek_log("camera: a chain array does not lie in the host image with room for "
           "itself -- headspritesect slot 0x%07X (in data: %s) -> 0x%llX needs "
           "%d shorts, nextspritesect slot 0x%07X (%s) -> 0x%llX needs %d, "
           "image 0x%llX bytes. Refusing: the swept sectors draw without their "
           "actors; the sky redirect and the geometry sweep are unaffected",
           s.headSlot, headIn ? "yes" : "NO", (unsigned long long)headTarget,
           EK_HEADSPRITESECT_SLOTS, s.nextSlot, nextIn ? "yes" : "NO",
           (unsigned long long)nextTarget, EK_NEXTSPRITESECT_SLOTS,
           (unsigned long long)image);
    return false;
  }

  InterlockedExchange(&s_sprSlotRva, (LONG)s.spriteSlot);
  InterlockedExchange(&s_tsprSlotRva, (LONG)s.tspriteSlot);
  InterlockedExchange(&s_ssCntSlotRva, (LONG)s.countSlot);
  InterlockedExchange(&s_headSlotRva, (LONG)s.headSlot);
  InterlockedExchange(&s_nextSlotRva, (LONG)s.nextSlot);
  ek_log("camera: the swept sectors' sprites are reachable -- one record "
         "multiply at rva 0x%07X inside the inlined polymer_scansprites, with "
         "the refptr slots at 0x%07X (sprite -> 0x%07X), 0x%07X (tsprite -> "
         "0x%07X), 0x%07X (spritesortcnt -> 0x%07X), 0x%07X (headspritesect -> "
         "0x%07X) and 0x%07X (nextspritesect -> 0x%07X), and the list bounded "
         "at %d by the engine's own compare. The sprite pointer is checked "
         "against the content scan's answer, and every chain link against its "
         "own record's sectnum, on the frames that append",
         s.imulAt, s.spriteSlot, (unsigned)sprTarget, s.tspriteSlot,
         (unsigned)tsprTarget, s.countSlot, (unsigned)cntTarget, s.headSlot,
         (unsigned)headTarget, s.nextSlot, (unsigned)nextTarget,
         EK_MAXSPRITESONSCREEN);
  return true;
}

static bool locate_sweep(const unsigned char* base, unsigned polyBegin,
                         unsigned polyEnd) {
  unsigned fn[4];
  fn[0] = locate_pair(base, EK_US_ANCHOR_A, sizeof(EK_US_ANCHOR_A) - 1,
                      "polymer_updatesector (witness 1)",
                      EK_US_ANCHOR_B, sizeof(EK_US_ANCHOR_B) - 1,
                      "polymer_updatesector (witness 2)",
                      "polymer_updatesector");
  fn[1] = locate_pair(base, EK_DS_ANCHOR_A, sizeof(EK_DS_ANCHOR_A) - 1,
                      "polymer_drawsector (witness 1)",
                      EK_DS_ANCHOR_B, sizeof(EK_DS_ANCHOR_B) - 1,
                      "polymer_drawsector (witness 2)",
                      "polymer_drawsector");
  fn[2] = locate_pair(base, EK_UW_ANCHOR_A, sizeof(EK_UW_ANCHOR_A) - 1,
                      "polymer_updatewall (witness 1)",
                      EK_UW_ANCHOR_B, sizeof(EK_UW_ANCHOR_B) - 1,
                      "polymer_updatewall (witness 2)",
                      "polymer_updatewall");
  fn[3] = locate_pair(base, EK_DW_ANCHOR_A, sizeof(EK_DW_ANCHOR_A) - 1,
                      "polymer_drawwall (witness 1)",
                      EK_DW_ANCHOR_B, sizeof(EK_DW_ANCHOR_B) - 1,
                      "polymer_drawwall (witness 2)",
                      "polymer_drawwall");
  if (!fn[0] || !fn[1] || !fn[2] || !fn[3]) {
    ek_log("camera: the whole-map sweep has no draw calls to make and stays "
           "unavailable -- the free camera and the game are unaffected");
    return false;
  }
  // Four DISTINCT functions: two names on one entry would be called with the wrong arguments (a sector index that is a
  // wall's).
  for (int i = 0; i < 4; i++)
    for (int j = i + 1; j < 4; j++)
      if (fn[i] == fn[j]) {
        ek_log("camera: two of the whole-map sweep's four anchors name the SAME "
               "function (0x%07X); refusing, and the sweep stays unavailable",
               fn[i]);
        return false;
      }

  SweepSites s;
  find_sweep_sites(base, polyBegin, polyEnd, fn, &s);
  const bool ordered = s.site[0] < s.site[1] && s.site[1] < s.site[2] &&
                       s.site[2] < s.site[3];
  const bool once = s.count[0] == 1 && s.count[1] == 1 && s.count[2] == 1 &&
                    s.count[3] == 1;
  if (s.faulted || !once || !ordered) {
    ek_log("camera: the whole-map sweep's four functions are not called from "
           "polymer_drawrooms the way polymer.cpp:1232-1242 calls them -- "
           "updatesector x%d at 0x%07X, drawsector x%d at 0x%07X, updatewall "
           "x%d at 0x%07X, drawwall x%d at 0x%07X%s; r10669-ec5824db8 has one "
           "site each in ascending order. Refusing: the sweep stays "
           "unavailable and nothing else changes",
           s.count[0], s.site[0], s.count[1], s.site[1], s.count[2], s.site[2],
           s.count[3], s.site[3],
           s.faulted ? " (the body faulted mid-scan, so these counts are "
                       "partial)" : "");
    return false;
  }

  // And prsectors[], the sweep's own bound; without it only a heuristic stands between a stale record and an unchecked
  // dereference.
  const unsigned prs = locate_prsectors(base, fn);
  if (prs == 0) {
    ek_log("camera: the whole-map sweep has no way to tell a live sector from "
           "the last map's and stays unavailable -- the free camera and the "
           "game are unaffected");
    return false;
  }

  // And prwalls[], the same bound for the wall half. A refusal refuses the SWEEP, not a fall back to the tiling chain.
  const unsigned prw = locate_prwalls(base, fn, prs);
  if (prw == 0) {
    ek_log("camera: the whole-map sweep has no way to tell a live wall from "
           "the last map's and stays unavailable -- the free camera and the "
           "game are unaffected");
    return false;
  }

  // And drawingstate, which keeps the sweep from drawing the map twice (30-40 fps): a feature that silently becomes the
  // slow version is worse than one reading Unavailable.
  const unsigned dstate = locate_drawingstate(base, polyBegin, polyEnd, fn[1],
                                              fn[3]);
  if (dstate == 0) {
    ek_log("camera: the whole-map sweep cannot tell which sectors the portal "
           "walk already drew and stays unavailable -- the free camera and the "
           "game are unaffected");
    return false;
  }

  // The sprites gate neither this row nor the sky (s_spritesOk): a refusal costs the actors standing in sweep-only
  // sectors.
  const bool spritesOk = locate_scansprites(base, polyBegin, s.site[1], s.site[2]);
  if (!spritesOk)
    ek_log("camera: the whole-map sweep cannot collect the swept sectors' "
           "sprites, so the sectors only the sweep reaches draw without their "
           "actors. The sky redirect, the geometry sweep, the free camera and "
           "the game are unaffected");
  InterlockedExchange(&s_spritesOk, spritesOk ? 1 : 0);

  InterlockedExchange(&s_prsRva, (LONG)prs);
  InterlockedExchange(&s_prwRva, (LONG)prw);
  InterlockedExchange(&s_dstateRva, (LONG)dstate);
  InterlockedExchange(&s_usRva, (LONG)fn[0]);
  InterlockedExchange(&s_dsRva, (LONG)fn[1]);
  InterlockedExchange(&s_uwRva, (LONG)fn[2]);
  InterlockedExchange(&s_dwRva, (LONG)fn[3]);
  InterlockedExchange(&s_sweepOk, 1);
  ek_log("camera: the whole-map sweep is available -- polymer_updatesector at "
         "rva 0x%07X, drawsector 0x%07X, updatewall 0x%07X, drawwall 0x%07X, "
         "each named by two anchors that agree and each called ONCE from "
         "polymer_drawrooms (0x%07X, 0x%07X, 0x%07X, 0x%07X) in "
         "polymer.cpp:1232-1242's own order",
         fn[0], fn[1], fn[2], fn[3], s.site[0], s.site[1], s.site[2], s.site[3]);
  return true;
}

// g_visibility, the one field "Remove fog" substitutes (freecam.h): globalvisibility is DERIVED at the head of the
// bracketed call (engine.cpp:9460-9482), so the field to write is that computation's input. Identified by `imul
// r32,r32,150` (engine.cpp:9480's `* 150`, once in the located drawrooms) and a three-step trace to the .refptr load.

// Each step looks back a narrow window (too wide refuses on a count, too narrow on zero; neither mislocates).
static const unsigned EK_GVIS_WINDOW = 0x20;

struct GvisScan {
  int imuls, shls, loads, refptrs, faulted;
  unsigned imulAt, shlAt, loadAt, slot;
  int reg, baseReg;
};

// An optional REX prefix and what its low bits extend; -1 for REX.W, which none of the three traced instructions
// carries.
static int gvis_rex(const unsigned char* base, unsigned p, int* rexB, int* rexR) {
  *rexB = 0; *rexR = 0;
  const unsigned char c = base[p];
  if (c < 0x40 || c > 0x4F) return 0;
  if (c & 8) return -1;
  *rexB = (c & 1) << 3;
  *rexR = (c & 4) << 1;
  return 1;
}

// `imul r32, r32, 150`: 0x69 /r id, mod=11, the same register both sides (`x = x * 150`).
static void gvis_find_imul(const unsigned char* base, unsigned begin,
                           unsigned end, GvisScan* out) {
  for (unsigned p = begin; p + 7 <= end; p++) {
    int rexB = 0, rexR = 0;
    const int skip = gvis_rex(base, p, &rexB, &rexR);
    if (skip < 0) continue;
    const unsigned q = p + (unsigned)skip;
    if (base[q] != 0x69) continue;
    const unsigned char modrm = base[q + 1];
    if ((modrm & 0xC0) != 0xC0) continue;
    if (*(const int*)(base + q + 2) != 150) continue;
    const int rm = rexB | (modrm & 7);
    if ((rexR | ((modrm >> 3) & 7)) != rm) continue;
    if (out->imuls++ == 0) { out->imulAt = p; out->reg = rm; }
  }
}

static void gvis_find_shl(const unsigned char* base, unsigned begin,
                          unsigned end, int reg, GvisScan* out) {
  for (unsigned p = begin; p + 4 <= end; p++) {
    int rexB = 0, rexR = 0;
    const int skip = gvis_rex(base, p, &rexB, &rexR);
    if (skip < 0) continue;
    const unsigned q = p + (unsigned)skip;
    if (base[q] != 0xC1) continue;
    if ((base[q + 1] & 0xF8) != 0xE0) continue;
    if ((rexB | (base[q + 1] & 7)) != reg) continue;
    if (base[q + 2] != 2) continue;
    if (out->shls++ == 0) out->shlAt = p;
  }
}

// `mov reg32, dword [rB]`: 0x8B /r, mod=00, a plain base (rm 100 is a SIB, 101 rip-relative).
static void gvis_find_load(const unsigned char* base, unsigned begin,
                           unsigned end, int reg, GvisScan* out) {
  for (unsigned p = begin; p + 3 <= end; p++) {
    int rexB = 0, rexR = 0;
    const int skip = gvis_rex(base, p, &rexB, &rexR);
    if (skip < 0) continue;
    const unsigned q = p + (unsigned)skip;
    if (base[q] != 0x8B) continue;
    const unsigned char modrm = base[q + 1];
    if ((modrm & 0xC0) != 0x00) continue;
    const int rm = modrm & 7;
    if (rm == 4 || rm == 5) continue;
    if ((rexR | ((modrm >> 3) & 7)) != reg) continue;
    if (out->loads++ == 0) { out->loadAt = p; out->baseReg = rexB | rm; }
  }
}

// `mov r64, qword [rip+disp32]`, the .refptr load; the LAST one wins (freecam.h's rule 1).
static void gvis_find_refptr(const unsigned char* base, unsigned begin,
                             unsigned end, int reg, GvisScan* out) {
  for (unsigned p = begin; p + 7 <= end; p++) {
    const unsigned char rex = base[p];
    if (rex < 0x48 || rex > 0x4F || (rex & 8) == 0) continue;
    if (base[p + 1] != 0x8B) continue;
    if ((base[p + 2] & 0xC7) != 0x05) continue;
    if ((((rex & 4) << 1) | ((base[p + 2] >> 3) & 7)) != reg) continue;
    const int disp = *(const int*)(base + p + 3);
    out->refptrs++;
    out->slot = (unsigned)((int)(p + 7) + disp);
  }
}

static int gvis_count_slot_loads(const unsigned char* base, unsigned begin,
                                 unsigned end, unsigned slot, int* faulted) {
  int n = 0;
  if (end <= begin + 7) { *faulted = 1; return 0; }
  __try {
    for (unsigned p = begin; p + 7 <= end; p++) {
      const unsigned char rex = base[p];
      if (rex < 0x48 || rex > 0x4F || (rex & 8) == 0) continue;
      if (base[p + 1] != 0x8B) continue;
      if ((base[p + 2] & 0xC7) != 0x05) continue;
      const int disp = *(const int*)(base + p + 3);
      if ((unsigned)((int)(p + 7) + disp) == slot) n++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = 1;
  }
  return n;
}

// `mov dword [rip+disp32], r32` is game.cpp:817's own instruction form, the second witness.
static int gvis_count_stores(const unsigned char* base, unsigned begin,
                             unsigned end, unsigned addr, int* faulted) {
  int n = 0;
  if (end <= begin + 6) { *faulted = 1; return 0; }
  __try {
    for (unsigned p = begin; p + 6 <= end; p++) {
      int rexB = 0, rexR = 0;
      const int skip = gvis_rex(base, p, &rexB, &rexR);
      if (skip < 0) continue;
      const unsigned q = p + (unsigned)skip;
      if (base[q] != 0x89) continue;
      if ((base[q + 1] & 0xC7) != 0x05) continue;
      const int disp = *(const int*)(base + q + 2);
      if ((unsigned)((int)(q + 6) + disp) == addr) n++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = 1;
  }
  return n;
}

// The whole trace, guarded: a faulting step leaves counts partial and the caller refuses on them.
static void find_gvisibility(const unsigned char* base, unsigned polyBegin,
                             unsigned polyEnd, GvisScan* s) {
  s->imuls = s->shls = s->loads = s->refptrs = s->faulted = 0;
  s->imulAt = s->shlAt = s->loadAt = s->slot = 0;
  s->reg = s->baseReg = -1;
  if (polyEnd <= polyBegin + 7) { s->faulted = 1; return; }
  __try {
    gvis_find_imul(base, polyBegin, polyEnd, s);
    if (s->imuls != 1) return;
    const unsigned lo1 = s->imulAt > polyBegin + EK_GVIS_WINDOW
                             ? s->imulAt - EK_GVIS_WINDOW : polyBegin;
    gvis_find_shl(base, lo1, s->imulAt, s->reg, s);
    if (s->shls != 1) return;
    const unsigned lo2 = s->shlAt > polyBegin + EK_GVIS_WINDOW
                             ? s->shlAt - EK_GVIS_WINDOW : polyBegin;
    gvis_find_load(base, lo2, s->shlAt, s->reg, s);
    if (s->loads != 1) return;
    const unsigned lo3 = s->loadAt > polyBegin + EK_GVIS_WINDOW
                             ? s->loadAt - EK_GVIS_WINDOW : polyBegin;
    gvis_find_refptr(base, lo3, s->loadAt, s->baseReg, s);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    s->faulted = 1;
  }
}

static unsigned long long locate_gvisibility(const unsigned char* base,
                                             unsigned polyBegin,
                                             unsigned polyEnd,
                                             unsigned drawBegin,
                                             unsigned drawEnd) {
  GvisScan s;
  find_gvisibility(base, polyBegin, polyEnd, &s);
  if (s.faulted || !ekm_gvis_shape_ok(s.imuls, s.shls, s.loads, s.refptrs)) {
    ek_log("camera: g_visibility is not where engine.cpp:9480 puts it -- %d "
           "`imul r32,r32,150` site(s) in drawrooms (0x%07X), then %d `shl r,2`, "
           "%d dword read(s) and %d refptr load(s) in the 0x%X bytes before each "
           "step%s; r10669-ec5824db8 has 1, 1, 1 and 2. Refusing: Remove fog "
           "stays unavailable and the camera is unaffected",
           s.imuls, polyBegin, s.shls, s.loads, s.refptrs, EK_GVIS_WINDOW,
           s.faulted ? " (the body faulted mid-scan, so these counts are "
                       "partial)" : "");
    return 0;
  }

  // The slot is image data; what it holds is an image address with room for the int32 after it.
  const size_t image = host_image_size(base);
  const int inData = rva_in_data_section(base, s.slot) ? 1 : 0;
  size_t addr = 0;
  __try {
    const unsigned char* const* const slotp =
        (const unsigned char* const*)(base + s.slot);
    const unsigned char* const a = *slotp;
    if (a > base) addr = (size_t)(a - base);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    addr = 0;
  }
  if (!ekm_gvis_span_ok((size_t)s.slot, addr, image, inData != 0)) {
    ek_log("camera: the g_visibility candidate does not lie in the host image "
           "-- slot rva 0x%07X (in data: %s), address rva 0x%llX, image 0x%llX "
           "bytes. Refusing: Remove fog stays unavailable",
           s.slot, inData ? "yes" : "NO", (unsigned long long)addr,
           (unsigned long long)image);
    return 0;
  }

  // The witnesses, from the source: every arm of engine.cpp:9460-9482's switch reads it and game.cpp:817 writes it once
  // inside G_DrawRooms.
  int faulted = 0;
  const int loads = gvis_count_slot_loads(base, polyBegin, polyEnd, s.slot,
                                          &faulted);
  const int stores = gvis_count_stores(base, drawBegin, drawEnd,
                                       (unsigned)addr, &faulted);
  if (faulted || !ekm_gvis_witness_ok(loads, stores)) {
    ek_log("camera: the slot at rva 0x%07X is not read and written the way the "
           "engine reads and writes g_visibility -- %d refptr load(s) inside "
           "drawrooms and %d direct store(s) inside G_DrawRooms (0x%07X..0x%07X)"
           "%s; the source has three switch arms reading it (engine.cpp:9460-"
           "9482) and one write (game.cpp:817). Refusing: Remove fog stays "
           "unavailable",
           s.slot, loads, stores, drawBegin, drawEnd,
           faulted ? " (a body faulted mid-scan, so these counts are partial)"
                   : "");
    return 0;
  }

  ek_log("camera: g_visibility at rva 0x%llX, reached from the `* 150` at rva "
         "0x%07X through the shl at 0x%07X, the dword read at 0x%07X and the "
         "refptr slot at 0x%07X (%d candidate load(s), nearest taken) -- read "
         "%d time(s) inside drawrooms and written %d time(s) inside G_DrawRooms",
         (unsigned long long)addr, s.imulAt, s.shlAt, s.loadAt, s.slot,
         s.refptrs, loads, stores);
  return (unsigned long long)(base + addr);
}

// totalclock/ototalclock: the clock the camera steps on (smoothRatio clamps to 65536 below 30 fps, freecam.h).
// totalclock is located only as the anchor, and what the game copies INTO it is ototalclock. A refusal costs the whole
// camera: one that cannot tell one tic from the next does not move.
static const unsigned EK_TCLK_BACK = 0x10;
static const unsigned EK_TCLK_SLOT = 0x40;
static const unsigned EK_OTCLK_BACK = 0x20;

struct TclkScan {
  unsigned slot; unsigned xorAt; unsigned loadAt;
  int xors; int loads; int refptrs; int faulted;
};

static void find_totalclock(const unsigned char* base, unsigned begin,
                            unsigned end, TclkScan* out) {
  out->slot = 0; out->xorAt = 0; out->loadAt = 0;
  out->xors = 0; out->loads = 0; out->refptrs = 0; out->faulted = 0;
  if (end <= begin + 8) { out->faulted = 1; return; }
  __try {
    // ClockTicks::update()'s `ticksS32 &= VALUE_MASK` is a 16-bit clear of the low half (`xor ax,ax`); nothing else in
    // this body does.
    for (unsigned q = begin; q + 3 <= end; q++)
      if (base[q] == 0x66 && base[q + 1] == 0x31 && base[q + 2] == 0xC0)
        if (out->xors++ == 0) out->xorAt = q;
    if (out->xors != 1) return;

    // `mov rax, [reg+8]`: ticksS32, the class's second member, read into the register the mask clears.
    const unsigned w0 = (out->xorAt - begin > EK_TCLK_BACK)
                            ? out->xorAt - EK_TCLK_BACK : begin;
    int baseReg = -1;
    for (unsigned q = w0; q + 4 <= out->xorAt; q++) {
      const unsigned char rex = base[q], op = base[q + 1], modrm = base[q + 2];
      if ((rex != 0x48 && rex != 0x49) || op != 0x8B) continue;
      if ((modrm & 0xC0) != 0x40 || ((modrm >> 3) & 7) != 0) continue;
      if ((modrm & 7) == 4 || base[q + 3] != 0x08) continue;
      if (out->loads++ == 0) {
        out->loadAt = q;
        baseReg = (int)(((rex & 1) << 3) | (modrm & 7));
      }
    }
    if (out->loads != 1 || baseReg < 0) return;

    // And the one refptr load that gave that register the object's address.
    const unsigned w1 = (out->loadAt - begin > EK_TCLK_SLOT)
                            ? out->loadAt - EK_TCLK_SLOT : begin;
    for (unsigned q = w1; q + 7 <= out->loadAt; q++) {
      const unsigned char rex = base[q], op = base[q + 1], modrm = base[q + 2];
      if ((rex != 0x48 && rex != 0x4C) || op != 0x8B) continue;
      if ((modrm & 0xC7) != 0x05) continue;
      if ((int)(((rex & 4) << 1) | ((modrm >> 3) & 7)) != baseReg) continue;
      const int disp = *(const int*)(base + q + 3);
      if (out->refptrs++ == 0) out->slot = (unsigned)((int)(q + 7) + disp);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

static int count_slot_loads(const unsigned char* base, unsigned begin,
                            unsigned end, unsigned slot, int* faulted) {
  int n = 0;
  if (end <= begin + 7) { *faulted = 1; return 0; }
  __try {
    for (unsigned q = begin; q + 7 <= end; q++) {
      const unsigned char rex = base[q];
      if (rex != 0x48 && rex != 0x4C) continue;
      if (base[q + 1] != 0x8B || (base[q + 2] & 0xC7) != 0x05) continue;
      const int disp = *(const int*)(base + q + 3);
      if ((unsigned)((int)(q + 7) + disp) == slot) n++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = 1;
  }
  return n;
}

static size_t slot_address(const unsigned char* base, unsigned slot) {
  size_t a = 0;
  __try {
    const unsigned char* const* const p =
        (const unsigned char* const*)(base + slot);
    const unsigned char* const v = *p;
    if (v > base) a = (size_t)(v - base);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    a = 0;
  }
  return a;
}

static unsigned locate_totalclock(const unsigned char* base, unsigned polyBegin,
                                  unsigned polyEnd, unsigned drawBegin,
                                  unsigned drawEnd) {
  TclkScan s;
  find_totalclock(base, polyBegin, polyEnd, &s);
  if (s.faulted || !ekm_tclk_shape_ok(s.xors, s.loads, s.refptrs)) {
    ek_log("camera: totalclock is not where engine.cpp:9497 puts it -- %d "
           "`xor ax,ax` site(s) in drawrooms (0x%07X..0x%07X), %d "
           "`mov rax,[reg+8]` in the 0x%X before it and %d refptr load(s) in "
           "the 0x%X before that%s; r10669-ec5824db8 has one of each. "
           "Refusing: the free camera has no tic clock and stays unavailable",
           s.xors, polyBegin, polyEnd, s.loads, EK_TCLK_BACK, s.refptrs,
           EK_TCLK_SLOT,
           s.faulted ? " (the body faulted mid-scan, so these counts are "
                       "partial)" : "");
    return 0;
  }
  int faulted = 0;
  const int witness = count_slot_loads(base, drawBegin, drawEnd, s.slot,
                                       &faulted);
  if (faulted || !ekm_tclk_witness_ok(witness)) {
    ek_log("camera: the slot at rva 0x%07X is not read inside G_DrawRooms "
           "(0x%07X..0x%07X) -- %d load(s)%s; game.cpp:786 carries the same "
           "`totalclocklock = totalclock` that engine.cpp:9497 does. Refusing: "
           "the free camera stays unavailable",
           s.slot, drawBegin, drawEnd, witness,
           faulted ? " (a body faulted mid-scan, so this count is partial)"
                   : "");
    return 0;
  }
  const size_t image = host_image_size(base);
  const size_t addr = slot_address(base, s.slot);
  const int inData = rva_in_data_section(base, s.slot) ? 1 : 0;
  if (!ekm_tclk_span_ok((size_t)s.slot, addr, image, inData)) {
    ek_log("camera: the totalclock candidate does not lie in the host image -- "
           "slot rva 0x%07X (in data: %s), clock rva 0x%llX, image 0x%llX bytes."
           " Refusing: the free camera stays unavailable",
           s.slot, inData ? "yes" : "NO", (unsigned long long)addr,
           (unsigned long long)image);
    return 0;
  }
  ek_log("camera: totalclock at rva 0x%llX, through the refptr slot 0x%07X -- "
         "the ClockTicks mask at 0x%07X in drawrooms reads it at 0x%07X, and "
         "G_DrawRooms names the same slot %d time(s)",
         (unsigned long long)addr, s.slot, s.xorAt, s.loadAt, witness);
  return s.slot;
}

// Walks only the owners of the G_DrawRooms call sites, where the game's own pauses live.
static unsigned long long locate_ototalclock(const unsigned char* base,
                                             const unsigned* owners,
                                             int nowners, unsigned tcSlot) {
  unsigned src = 0;
  int sites = 0, distinct = 0, faulted = 0;
  for (int k = 0; k < nowners; k++) {
    unsigned b = 0, e = 0;
    if (!ek_image_extent_containing(base, owners[k], &b, &e)) continue;
    if (e <= b + 9) continue;
    unsigned slotOf[16];
    for (int i = 0; i < 16; i++) slotOf[i] = 0;
    __try {
      for (unsigned q = b; q + 9 <= e; q++) {
        const unsigned char rex = base[q], op = base[q + 1];
        const unsigned char modrm = base[q + 2];
        if ((rex == 0x48 || rex == 0x4C) && op == 0x8B &&
            (modrm & 0xC7) == 0x05) {
          const int dst = (int)(((rex & 4) << 1) | ((modrm >> 3) & 7));
          const int disp = *(const int*)(base + q + 3);
          slotOf[dst] = (unsigned)((int)(q + 7) + disp);
          continue;
        }
        // `mov [reg+8], r64`: a ClockTicks' ticksS32 being written.
        if (rex < 0x48 || rex > 0x4D || op != 0x89) continue;
        if ((modrm & 0xC0) != 0x40 || (modrm & 7) == 4) continue;
        if (base[q + 3] != 0x08) continue;
        if (slotOf[((rex & 1) << 3) | (modrm & 7)] != tcSlot) continue;
        // And the read it came from, a few instructions above.
        const unsigned w = (q - b > EK_OTCLK_BACK) ? q - EK_OTCLK_BACK : b;
        for (unsigned p = q; p-- > w; ) {
          const unsigned char r2 = base[p], o2 = base[p + 1];
          const unsigned char m2 = base[p + 2];
          if (r2 < 0x48 || r2 > 0x4D || o2 != 0x8B) continue;
          if ((m2 & 0xC0) != 0x40 || (m2 & 7) == 4) continue;
          if (base[p + 3] != 0x08) continue;
          const unsigned from = slotOf[((r2 & 1) << 3) | (m2 & 7)];
          if (from != 0 && from != tcSlot) {
            sites++;
            if (src == 0) { src = from; distinct = 1; }
            else if (src != from) distinct = 2;
          }
          break;
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      faulted = 1;
    }
  }
  if (faulted || !ekm_otclk_witness_ok(sites, distinct)) {
    ek_log("camera: ototalclock is not what this build copies into totalclock "
           "-- %d ClockTicks assignment(s) into the slot at rva 0x%07X across "
           "%d G_DrawRooms owner(s), naming %d distinct source(s)%s; the source "
           "has six of them in those functions and they all name ototalclock. "
           "Refusing: the free camera has no tic clock and stays unavailable",
           sites, tcSlot, nowners, distinct,
           faulted ? " (a body faulted mid-scan, so these counts are partial)"
                   : "");
    return 0;
  }
  const size_t image = host_image_size(base);
  const size_t addr = slot_address(base, src);
  const int inData = rva_in_data_section(base, src) ? 1 : 0;
  if (!ekm_tclk_span_ok((size_t)src, addr, image, inData)) {
    ek_log("camera: the ototalclock candidate does not lie in the host image "
           "-- slot rva 0x%07X (in data: %s), clock rva 0x%llX, image 0x%llX "
           "bytes. Refusing: the free camera stays unavailable",
           src, inData ? "yes" : "NO", (unsigned long long)addr,
           (unsigned long long)image);
    return 0;
  }
  ek_log("camera: ototalclock at rva 0x%llX, through the refptr slot 0x%07X -- "
         "%d assignment(s) into totalclock across %d G_DrawRooms owner(s), all "
         "naming one source",
         (unsigned long long)addr, src, sites, nowners);
  return (unsigned long long)(base + addr);
}

// Logged, not gating. The expected pair is IN the line: a bare "7/6" in a pasted bug report tells the reader nothing.
static void log_call_probe(const unsigned char* base, unsigned entry,
                           const char* what, const char* expected,
                           EkCallCount* out) {
  EkCallCount c;
  ek_image_count_calls(base, entry, &c);
  if (out) *out = c;
  ek_log("camera:   %s probe (not gating): %d `call rel32` site(s) reach it, in "
         "%d distinct function(s)%s%s; r10669-ec5824db8 has %s",
         what, c.sites, c.owners, c.capped ? " (caller list capped)" : "",
         c.faulted ? " (a code section faulted, so these counts are partial)" : "",
         expected);
}

bool camera_scan(void) {
  if (s_rva && s_roomsQ16Rva) return true;
  if (s_refused) return false;

  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  if (!base) {
    s_refused = true;
    ek_log("camera: no host module; the free camera stays unavailable");
    return false;
  }

  // One attempt: the image does not change while the game runs.
  s_refused = true;

  // G_DrawRooms first: still hooked, and its EXTENT is half of the gate deciding which drawrooms call the detour stands
  // in.
  unsigned drawBegin = 0, drawEnd = 0;
  const unsigned entry = locate_once(base, EK_CAMERA_ANCHOR,
                                     sizeof(EK_CAMERA_ANCHOR) - 1, "G_DrawRooms",
                                     &drawBegin, &drawEnd);
  if (entry == 0 || drawEnd <= drawBegin) {
    ek_log("camera: the free camera has no draw call to bracket and stays "
           "unavailable -- nothing else is affected");
    return false;
  }

  // The draw itself, twice, by two different strings that must land in the same function: the only target found through
  // a callee, so it pays for a second witness.
  unsigned polyBegin = 0, polyEnd = 0;
  const unsigned pa = locate_once(base, EK_POLY_ANCHOR_A,
                                  sizeof(EK_POLY_ANCHOR_A) - 1,
                                  "polymer_drawrooms (witness 1)",
                                  &polyBegin, &polyEnd);
  const unsigned pb = locate_once(base, EK_POLY_ANCHOR_B,
                                  sizeof(EK_POLY_ANCHOR_B) - 1,
                                  "polymer_drawrooms (witness 2)", NULL, NULL);
  if (pa == 0 || pb == 0) {
    ek_log("camera: the bracketed drawrooms could not be located by both of "
           "its anchors (witness 1 %s, witness 2 %s); the free camera stays "
           "unavailable -- nothing else is affected",
           pa ? "found" : "REFUSED", pb ? "found" : "REFUSED");
    return false;
  }
  if (pa != pb) {
    // Two witnesses that disagree are worse than one: both strings are in one function in the source, so the image is
    // not the one this reads.
    ek_log("camera: the two drawrooms anchors name DIFFERENT functions "
           "(0x%07X and 0x%07X); refusing to guess, and the free camera stays "
           "unavailable", pa, pb);
    return false;
  }

  InterlockedExchange(&s_drawBegin, (LONG)drawBegin);
  InterlockedExchange(&s_drawEnd, (LONG)drawEnd);
  InterlockedExchange(&s_roomsQ16Rva, (LONG)pa);
  InterlockedExchange(&s_rva, (LONG)entry);
  s_base = base;

  ek_log("camera: G_DrawRooms at rva 0x%07X..0x%07X, and the drawrooms it "
         "calls -- renderDrawRoomsQ16, with polymer_drawrooms inlined into it "
         "-- at rva 0x%07X (both anchors agree)", drawBegin, drawEnd, pa);
  EkCallCount gdr;
  log_call_probe(base, entry, "G_DrawRooms", "7 in 6", &gdr);
  log_call_probe(base, pa, "renderDrawRoomsQ16", "10 in 6", NULL);

  // The tic clock DOES gate the whole feature: the camera steps on ototalclock and blends by the smoothRatio derived
  // from it.
  const unsigned tcSlot = locate_totalclock(base, polyBegin, polyEnd,
                                            drawBegin, drawEnd);
  if (tcSlot != 0)
    InterlockedExchange64(&s_otclkAddr,
                          (LONGLONG)locate_ototalclock(base, gdr.owner,
                                                       gdr.owners, tcSlot));
  if (InterlockedCompareExchange64(&s_otclkAddr, 0, 0) == 0)
    ek_log("camera: without the engine's tic clock the free camera would "
           "not step at all, so it stays unavailable -- the game is "
           "unaffected");

  // Last and NOT gating: a refusal costs the sky over a sloped sector, never the feature.
  InterlockedExchange(&s_getzRva, (LONG)locate_getz(base, polyBegin, polyEnd));

  // And the one call site that fills the band test, which the sky redirect rewrites; not gating, and only asked if the
  // function above was identified.
  if (InterlockedCompareExchange(&s_getzRva, 0, 0) != 0)
    InterlockedExchange(&s_getzSiteRva,
                        (LONG)locate_getz_site(base, polyBegin, polyEnd,
                                               (unsigned)s_getzRva));

  // Not gating: a refused sweep costs the whole-map row only.
  locate_sweep(base, polyBegin, polyEnd);

  // The walk's answer for sprite[] goes to gamestate either way: decoding the engine's own sprite load names the
  // variable the renderer reads, while a content scan can latch any global holding that pointer. So the proof decides
  // and the scan witnesses; a refusal is published too. This couples the ESP's sprite array to s_spritesOk.
  gamestate_sprite_authority(
      InterlockedCompareExchange(&s_spritesOk, 0, 0)
          ? (unsigned)slot_target_rva(
                base, (unsigned)InterlockedCompareExchange(&s_sprSlotRva, 0, 0))
          : 0u);

  // Not gating: a refused g_visibility costs the Remove fog row only.
  InterlockedExchange64(&s_gvisAddr,
                        (LONGLONG)locate_gvisibility(base, polyBegin, polyEnd,
                                                     drawBegin, drawEnd));
  return true;
}

bool camera_ready(void) { return s_rva != 0 && s_roomsQ16Rva != 0; }

bool camera_wholemap_available(void) {
  return camera_available() && InterlockedCompareExchange(&s_sweepOk, 0, 0) != 0;
}

bool camera_sprites_available(void) {
  return camera_wholemap_available() &&
         InterlockedCompareExchange(&s_spritesOk, 0, 0) != 0;
}

bool camera_fog_available(void) {
  return camera_available() &&
         InterlockedCompareExchange64(&s_gvisAddr, 0, 0) != 0;
}

unsigned camera_rva(void) { return (unsigned)s_rva; }

unsigned camera_roomsQ16_rva(void) { return (unsigned)s_roomsQ16Rva; }
