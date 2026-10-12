// aGameArrays[], the CON gamearray registry, located by content (layout: gamearrays.h). tilesizx/y hold
// &tilesiz[0].x/.y, and tiles.cpp locates tilesiz independently, so the scan defers until that answer exists. A
// cross-check failure prints both rvas and the delta, since the engine's registration beats tiles.cpp's pick.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "config.h"
#include "tiles.h"
#include "gamearrays.h"

static const unsigned char* volatile s_arrays = NULL;
// Worker thread only: the scan walks every writable section, so without a terminal state it would burn a core on an
// image that never has aGameArrays.
static int s_attempts = 0;
static bool s_budgetSpent = false;
static bool s_deferSaid = false;
static int s_deferCount = 0;
static volatile LONG s_rva = 0;
static volatile LONG s_walked = 0;

// AA's five upgrade arrays (AADEFS.CON:4381-4385), resolved once by the label walk; the index and "said once" latch are
// per array so one failure cannot silence another.
static const char* const kUpgrades[EK_GA_UP_COUNT] = {
  "dukeupgrades", "shellyupgrades", "wesupgrades", "mandoupgrades",
  "novaupgrades",
};
static volatile LONG s_upIdx[EK_GA_UP_COUNT] = { -1, -1, -1, -1, -1 };
static volatile LONG s_upSaid[EK_GA_UP_COUNT] = { 0, 0, 0, 0, 0 };
static volatile LONG s_upResolved = 0;

// Committed, readable page. Unlike cvars.cpp's readable() this is NOT bounded to the host image: szLabel and pValues
// are heap allocations (Gv_NewArray:1004, :1024).
static bool readable(const void* p, size_t bytes) {
  if (!p) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const unsigned char* end = (const unsigned char*)p + bytes;
  const unsigned char* regionEnd =
      (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  return end <= regionEnd;
}

static const char* ga_label(const unsigned char* rec) {
  const char* s = NULL;
  memcpy(&s, rec + EK_GA_LABEL, sizeof(s));
  return s;
}

// Exact match, never a prefix: AA declares `dweaplevel` beside `dweapxp`. One 26-byte VirtualQuery first,
// byte-at-a-time only as fallback (512 records would be ~13000 syscalls on the worker).
static bool label_is(const char* s, const char* want) {
  if (!s) return false;
  if (readable(s, EK_MAXARRAYLABEL)) {
    for (int i = 0; i < EK_MAXARRAYLABEL; i++) {
      if (s[i] != want[i]) return false;
      if (s[i] == 0) return true;
    }
    return false;
  }
  for (int i = 0; i < EK_MAXARRAYLABEL; i++) {
    if (!readable(s + i, 1)) return false;
    if (s[i] != want[i]) return false;
    if (s[i] == 0) return true;
  }
  return false;
}

// Copy the label through the guard once, so the walk costs one guarded read per record rather than one per name.
static bool label_copy(const char* s, char* out, int cap) {
  if (!s || !out || cap < EK_MAXARRAYLABEL) return false;
  out[0] = 0;
  if (readable(s, EK_MAXARRAYLABEL)) {
    for (int i = 0; i < EK_MAXARRAYLABEL; i++) {
      out[i] = s[i];
      if (s[i] == 0) return i > 0;
    }
    return false;
  }
  for (int i = 0; i < EK_MAXARRAYLABEL; i++) {
    if (!readable(s + i, 1)) return false;
    out[i] = s[i];
    if (s[i] == 0) return i > 0;
  }
  return false;
}

// All six labels, in order, at stride 32; anything short is a refusal.
static bool labels_ok(const unsigned char* cand) {
  for (int i = 0; i < EK_GA_SYS_COUNT; i++)
    if (!label_is(ga_label(cand + (size_t)i * EK_GA_STRIDE), EK_GA_SYS[i].name))
      return false;
  return true;
}

struct GaSweep {
  const unsigned char* hit;
  unsigned rva, secondRva;
  int shape, verified, faulted;
};

// A section's VirtualSize can run past what is committed, and a partial sweep must not pass as complete (the unseen
// bytes could hold a second match). This walk needs .bss, which ek_image_sections drops.
static void sweep_section(const unsigned char* start, DWORD vaddr, DWORD span,
                          GaSweep* s) {
  const size_t bytes = (size_t)EK_GA_SYS_COUNT * EK_GA_STRIDE;
  if (span < bytes) return;
  __try {
    for (DWORD off = 0; (size_t)off + bytes <= (size_t)span; off += 8) {
      const unsigned char* const cand = start + off;
      if (!ekm_ga_shape_ok(cand)) continue;
      s->shape++;
      if (!labels_ok(cand)) continue;
      s->verified++;
      const unsigned rva = (unsigned)(vaddr + off);
      if (!s->hit) { s->hit = cand; s->rva = rva; }
      else if (!s->secondRva) s->secondRva = rva;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    s->faulted = 1;
  }
}

struct GaWalk {
  int walked, up[EK_GA_UP_COUNT], resolved;
};

// Stops at the first null or unreadable szLabel (Gv_NewArray fills contiguously from 0). May run past g_gameArrayCount
// after a CON re-parse since Gv_Free keeps labels; every read re-checks the label.
static void walk_labels(const unsigned char* base, GaWalk* w) {
  w->walked = 0; w->resolved = 0;
  for (int k = 0; k < EK_GA_UP_COUNT; k++) w->up[k] = -1;
  for (int i = 0; i < EK_MAXGAMEARRAYS; i++) {
    const unsigned char* const rec = base + (size_t)i * EK_GA_STRIDE;
    if (!readable(rec, EK_GA_STRIDE)) break;
    char lbuf[EK_MAXARRAYLABEL + 2];
    if (!label_copy(ga_label(rec), lbuf, (int)sizeof(lbuf))) break;
    w->walked++;
    for (int k = 0; k < EK_GA_UP_COUNT; k++)
      if (w->up[k] < 0 && strcmp(lbuf, kUpgrades[k]) == 0) {
        w->up[k] = i;
        w->resolved++;
      }
  }
}

// Both rvas come back so the log can print them either way: a FAIL is evidence about tilesiz too.
static bool crosscheck(uintptr_t imgBase, size_t imgSize,
                       const unsigned char* cand, unsigned tilesRva,
                       unsigned* outX, unsigned* outY) {
  *outX = 0;
  *outY = 0;
  const unsigned char* const xRec =
      cand + (size_t)EK_GA_SYS_TILESIZX * EK_GA_STRIDE;
  const unsigned char* const yRec =
      cand + (size_t)EK_GA_SYS_TILESIZY * EK_GA_STRIDE;
  const uintptr_t x = (uintptr_t)ekm_ga_word(xRec, EK_GA_PVALUES);
  const uintptr_t y = (uintptr_t)ekm_ga_word(yRec, EK_GA_PVALUES);
  // tilesiz is in .bss, so both must land inside the image; otherwise the rva stays 0 and the predicate refuses.
  if (x >= imgBase && x - imgBase < imgSize) *outX = (unsigned)(x - imgBase);
  if (y >= imgBase && y - imgBase < imgSize) *outY = (unsigned)(y - imgBase);
  const unsigned gotX = *outX;
  const unsigned gotY = *outY;
  return ekm_ga_tilesiz_crosscheck(gotX, gotY, tilesRva);
}

// Charged on the expensive walk only (as in gamevars.cpp): aGameArrays fills while the CON parses, so an early miss is
// legitimate.
static bool give_up(const char* why) {
  if (++s_attempts >= EK_TILE_SCAN_BUDGET) {
    s_budgetSpent = true;
    ek_log("gamearrays: giving up on aGameArrays after %d attempts (%s); AA's "
           "upgrade state is unavailable and nothing will retry", s_attempts,
           why);
  }
  return false;
}

bool gamearrays_scan(void) {
  if (s_arrays) return true;
  if (s_budgetSpent) return false;

  // Deferral comes first and is uncharged: until tiles.cpp answers there is no evidence to stand on.
  const unsigned tilesRva = tiles_ready() ? tiles_rva() : 0u;
  if (!tilesRva) {
    // Repeat every 64 deferrals so a late reader of the log sees a stated refusal rather than silence.
    if (!s_deferSaid || (s_deferCount % 64) == 0) {
      ek_log("gamearrays: deferring%s -- tilesiz is not located yet, and "
             "tilesizx.pValues == &tilesiz[0].x is what proves a candidate. "
             "This is not an attempt and is not charged against the budget "
             "(deferral %d)",
             s_deferSaid ? " still" : "", s_deferCount + 1);
      s_deferSaid = true;
    }
    s_deferCount++;
    return false;
  }

  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  const IMAGE_DOS_HEADER* const dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const IMAGE_NT_HEADERS* const nt =
      (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  const size_t imgSize = (size_t)nt->OptionalHeader.SizeOfImage;

  const IMAGE_SECTION_HEADER* const sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = (int)nt->FileHeader.NumberOfSections;

  GaSweep s;
  s.hit = NULL; s.rva = 0; s.secondRva = 0;
  s.shape = 0; s.verified = 0; s.faulted = 0;

  for (int i = 0; i < nsec; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
    // VirtualSize, not SizeOfRawData: aGameArrays is a 16 KB .bss array with raw size zero (imagescan.h:43-49).
    sweep_section(base + sec[i].VirtualAddress, sec[i].VirtualAddress,
                  sec[i].Misc.VirtualSize, &s);
  }

  if (s.faulted) {
    ek_log("gamearrays: a writable section faulted part-way; %d shape "
           "candidate(s) and %d verified so far are NOT a complete count, so "
           "this pass refuses", s.shape, s.verified);
    return give_up("a faulted sweep");
  }
  if (!s.hit) {
    ek_log("gamearrays: aGameArrays not found -- %d shape candidate(s), 0 "
           "passing the 6-label check; AA's upgrade state is unavailable and "
           "every consumer keeps today's behaviour", s.shape);
    return give_up("never found");
  }
  if (s.verified > 1) {
    // Ambiguity is reported, not resolved: two regions passing the check means the signature identifies nothing.
    ek_log("gamearrays: AMBIGUOUS -- %d candidates pass the 6-label check "
           "(rva 0x%07X and 0x%07X); refusing to guess",
           s.verified, s.rva, s.secondRva);
    return give_up("ambiguous");
  }

  unsigned xRva = 0, yRva = 0;
  const bool xc = crosscheck((uintptr_t)base, imgSize, s.hit, tilesRva,
                             &xRva, &yRva);
  if (!xc) {
    const long long delta = (long long)xRva - (long long)tilesRva;
    ek_log("gamearrays: rva 0x%07X passed the 6-label check but FAILED the "
           "tilesiz cross-check -- tilesizx.pValues at rva 0x%07X, tilesizy at "
           "0x%07X (want 0x%07X and 0x%07X), delta %lld bytes. The cross-check "
           "is the evidence, so this is refused; and since tilesizx.pValues is "
           "the ENGINE'S OWN registration, a non-zero delta says tiles.cpp's "
           "pick is the wrong array rather than this one",
           s.rva, xRva, yRva, tilesRva, tilesRva + 2, delta);
    return give_up("a failed cross-check");
  }

  GaWalk w;
  walk_labels(s.hit, &w);

  InterlockedExchange(&s_rva, (LONG)s.rva);
  InterlockedExchange(&s_walked, (LONG)w.walked);
  // Published before s_arrays: reads refuse while s_arrays is NULL, so no caller sees a half-filled table.
  for (int k = 0; k < EK_GA_UP_COUNT; k++)
    InterlockedExchange(&s_upIdx[k], (LONG)w.up[k]);
  InterlockedExchange(&s_upResolved, (LONG)w.resolved);
  s_arrays = s.hit;

  // Behind the debug row; refusals above are not. A -1 is no failure (stock Duke and World Tour declare none of these
  // arrays).
  if (!g_cfg.debuglog) return true;
  char upText[256];
  upText[0] = 0;
  for (int k = 0; k < EK_GA_UP_COUNT; k++) {
    char one[64];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s'%s' at %d", k ? ", " : "",
                kUpgrades[k], w.up[k]);
    strcat_s(upText, sizeof(upText), one);
  }
  ek_log("gamearrays: aGameArrays at rva 0x%07X -- %d shape candidate(s), %d "
         "passing the 6-label check at stride %d; tilesiz cross-check PASS "
         "(tilesizx.pValues rva 0x%07X == tiles.cpp's 0x%07X, tilesizy 0x%07X "
         "== +2, and tiles.cpp found that array by a different mechanism -- "
         "THAT is the evidence, not this scan's own plausibility); %d entr(ies) "
         "walked; AA upgrade arrays %d of %d resolved -- %s; -1 means this CON "
         "declares no such array, which is correct for stock Duke",
         s.rva, s.shape, s.verified, EK_GA_STRIDE, xRva, tilesRva, yRva,
         w.walked, w.resolved, EK_GA_UP_COUNT, upText);
  return true;
}

bool gamearrays_ready(void) { return s_arrays != NULL; }

unsigned gamearrays_rva(void) {
  return (unsigned)InterlockedCompareExchange(&s_rva, 0, 0);
}

int gamearrays_walked(void) {
  return (int)InterlockedCompareExchange(&s_walked, 0, 0);
}

int gamearrays_upgrades_resolved(void) {
  return (int)InterlockedCompareExchange(&s_upResolved, 0, 0);
}

// The cached index is not a cached array: Gv_Free resets g_gameArrayCount (gamevars.cpp:79) and a re-parse re-assigns
// indices, and no flags bit distinguishes `dukeupgrades` from another untyped array, so the label is re-checked.
static const unsigned char* ga_record(int which) {
  const unsigned char* const v = s_arrays;
  if (!v) return NULL;
  const int idx = (int)InterlockedCompareExchange(&s_upIdx[which], -1, -1);
  if (!ekm_ga_index_ok(idx)) return NULL;
  const unsigned char* const rec = v + (size_t)idx * EK_GA_STRIDE;
  if (!readable(rec, EK_GA_STRIDE)) return NULL;
  if (!label_is(ga_label(rec), kUpgrades[which])) return NULL;
  return rec;
}

// Presence and value are separate questions (as gamevars_gotshock_present). s_arrays is part of the answer: the index
// table is published before it, so ignoring it could report "not AA" off an unfinished table.
bool gamearrays_upgrade_present(int which) {
  if (which < 0 || which >= EK_GA_UP_COUNT) return false;
  if (!gamearrays_ready()) return false;
  return ekm_ga_index_ok((int)InterlockedCompareExchange(&s_upIdx[which], -1, -1));
}

bool gamearrays_upgrade(int which, int slot, int* out) {
  if (!out) return false;
  if (which < 0 || which >= EK_GA_UP_COUNT) return false;
  const unsigned char* const rec = ga_record(which);
  if (!rec) return false;

  // Element size and bound come from the record, not AA's source: a redeclared type or length is still read correctly
  // (bound is CHECK_INDEX on .size).
  const uintptr_t flags = (uintptr_t)ekm_ga_word(rec, EK_GA_FLAGS);
  const intptr_t size = ekm_ga_word(rec, EK_GA_SIZE);
  size_t off = 0, bytes = 0;
  if (!ekm_ga_element_span(flags, size, slot, &off, &bytes)) return false;

  // pValues is a heap allocation the engine frees and remakes (Gv_NewArray:1024), so it is bounds-checked at read time.
  const unsigned char* const pv =
      (const unsigned char*)ekm_ga_word(rec, EK_GA_PVALUES);
  if (!pv) return false;
  if (!readable(pv + off, bytes)) {
    // Said once per array but retried: Gv_Free NULLs pValues during a re-parse and Gv_NewArray refills it, so the
    // condition is transient.
    if (InterlockedCompareExchange(&s_upSaid[which], 1, 0) == 0)
      ek_log("gamearrays: '%s' pValues at %p will not read; every caller takes "
             "today's no-answer behaviour for now and this is not logged "
             "again. The read retries every call and resumes on its own -- a "
             "CON re-parse frees this array and reallocates it",
             kUpgrades[which], (const void*)pv);
    return false;
  }
  *out = ekm_ga_decode(flags, pv + off, slot);
  return true;
}
