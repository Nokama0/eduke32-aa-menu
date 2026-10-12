// Pickup range, the live half. The rules are in pickuprange.h. Once AA's compiled script is located this scans it
// once for every `ifpdistl 844`, attributes each site to its actor through g_tile[].execPtr, and only if the 97/68/7
// counts hold writes R into the 75 verified operand slots (or 844 back when the row is Off).

#include <windows.h>
#include <stdio.h>         // _snprintf_s
#include <stdlib.h>        // qsort
#include <string.h>
#include "pickuprange.h"
#include "config.h"
#include "log.h"
#include "gamestate.h"     // gamestate_module_base
#include "imagescan.h"     // ek_image_sections
#include "badguy.h"        // badguy_ready, badguy_tile_record: execPtr at offset 0
#include "tiles.h"         // EK_MAXTILES_ARR
#include "actorstate.h"    // actorstate_script_span

// Game thread only: every touch is inside the SwapBuffers hook, as is the menu
// that reads the status.
enum { PR_WAITING = 0, PR_READY, PR_REFUSED, PR_STOPPED };
static int s_state = PR_WAITING;
static const char* s_status = "Waiting for AA's compiled script.";
static char s_statusBuf[192];

struct PrSite {
  volatile long long* word;   // the ifpdistl instruction word; the operand is word[1]
  long long inst;             // the instruction word as read at verify
  long long opLast;           // the operand as we last wrote it (844 at verify)
};
static PrSite s_site[EK_PR_GATES + EK_PR_RESETS];
static int s_sites = 0;
static long long s_written = EK_PR_AA_RETRIEVE;

static void refuse(const char* status) {
  s_state = PR_REFUSED;
  s_status = status;
}

// vm_keywords[] is `{ const char* token; int32 val; }`. A keyword's string may be pooled as the tail of a longer
// one, so every "name\0" is a candidate and exactly ONE 8-aligned pointer to any may exist in the data: "state" has
// two and would refuse.
enum { PR_KW_IFPDISTL = 0, PR_KW_RESETCOUNT, PR_KW_ENDA, PR_KW_COUNT };
static const char* const kKwName[PR_KW_COUNT] = { "ifpdistl", "resetcount", "enda" };
const int PR_KW_MAX_STRINGS = 8;

static bool find_opcodes(const unsigned char* base, int* op) {
  EkSpan spans[16];
  int nspan = 0;
  if (!ek_image_sections(base, false, spans, 16, &nspan)) {
    ek_log("pickuprange: the image's section headers did not parse; refusing");
    return false;
  }
  uintptr_t addr[PR_KW_COUNT][PR_KW_MAX_STRINGS];
  volatile int strings[PR_KW_COUNT] = { 0, 0, 0 };
  volatile int entries[PR_KW_COUNT] = { 0, 0, 0 };
  volatile int val[PR_KW_COUNT] = { -1, -1, -1 };
  volatile bool faulted = false;

  for (int k = 0; k < PR_KW_COUNT; k++) {
    const size_t len = strlen(kKwName[k]) + 1;   // with its terminator
    for (int i = 0; i < nspan; i++) {
      if (spans[i].size < len) continue;
      __try {
        const DWORD last = spans[i].size - (DWORD)len;
        for (DWORD off = 0; off <= last; off++) {
          if (spans[i].base[off] != (unsigned char)kKwName[k][0]) continue;
          if (memcmp(spans[i].base + off, kKwName[k], len) != 0) continue;
          if (strings[k] < PR_KW_MAX_STRINGS) addr[k][strings[k]] = (uintptr_t)(spans[i].base + off);
          strings[k]++;
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
      }
    }
  }
  for (int i = 0; i < nspan; i++) {
    if (spans[i].size < 16) continue;
    __try {
      const DWORD last = spans[i].size - 16;
      for (DWORD off = 0; off <= last; off += 8) {
        const uintptr_t q = *(const uintptr_t*)(spans[i].base + off);
        for (int k = 0; k < PR_KW_COUNT; k++) {
          const int m = strings[k] < PR_KW_MAX_STRINGS ? strings[k] : PR_KW_MAX_STRINGS;
          for (int j = 0; j < m; j++) {
            if (q != addr[k][j]) continue;
            entries[k]++;
            val[k] = *(const int*)(spans[i].base + off + 8);
          }
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      faulted = true;
    }
  }

  bool ok = !faulted;
  for (int k = 0; k < PR_KW_COUNT; k++) {
    const bool one = strings[k] >= 1 && strings[k] <= PR_KW_MAX_STRINGS && entries[k] == 1 &&
                     val[k] >= 0 && val[k] <= 0xFFF;
    if (!one) {
      ek_log("pickuprange: keyword \"%s\": %d string(s), %d table entr%s, value %d%s -- "
             "exactly one entry is required; refusing", kKwName[k], strings[k], entries[k],
             entries[k] == 1 ? "y" : "ies", val[k], faulted ? " (a section faulted)" : "");
      ok = false;
    }
    op[k] = val[k];
  }
  return ok;
}

// One guarded validation of the whole range; the words are then read directly. Writable too, because the only thing
// this feature does is write there.
static bool range_writable(const unsigned char* b, const unsigned char* e) {
  const unsigned char* p = b;
  for (int guard = 0; p < e && guard < 4096; guard++) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof(m)) || m.RegionSize == 0) return false;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD prot = m.Protect & 0xFF;
    if (prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READWRITE) return false;
    p = (const unsigned char*)m.BaseAddress + m.RegionSize;
  }
  return p >= e;
}

struct PrExec { uintptr_t p; int pic; };
static PrExec s_exec[EK_MAXTILES_ARR];
static uintptr_t s_execSorted[EK_MAXTILES_ARR];

static int cmp_exec(const void* a, const void* b) {
  const uintptr_t x = ((const PrExec*)a)->p, y = ((const PrExec*)b)->p;
  return x < y ? -1 : (x > y ? 1 : 0);
}

// Every tile whose execPtr lands on a word of the script, sorted once. -1 on
// a faulted read of g_tile.
static int collect_exec(const unsigned char* sb, const unsigned char* se) {
  volatile int n = 0;
  __try {
    for (int pic = 0; pic < EK_MAXTILES_ARR; pic++) {
      const unsigned char* const rec = badguy_tile_record(pic);
      if (!rec) return -1;
      const uintptr_t e = *(const uintptr_t*)rec;
      if (e < (uintptr_t)sb || e >= (uintptr_t)se || ((e - (uintptr_t)sb) & 7)) continue;
      s_exec[n].p = e;
      s_exec[n].pic = pic;
      n++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
  qsort(s_exec, (size_t)n, sizeof(PrExec), cmp_exec);
  for (int i = 0; i < n; i++) s_execSorted[i] = s_exec[i].p;
  return n;
}

enum { PR_KIND_LEFT = 0, PR_KIND_GATE, PR_KIND_SLIDE };
struct PrFound { size_t s; int line; int pic; int kind; };
const int PR_MAX_FOUND = 160;   // well over 97; a count past it refuses anyway
static PrFound s_found[PR_MAX_FOUND];

static const char* kind_name(int kind) {
  return kind == PR_KIND_GATE ? "gate" : (kind == PR_KIND_SLIDE ? "resetcount" : "left at 844");
}

static void verify(void) {
  const unsigned char* const base = gamestate_module_base();
  const unsigned char* sb = NULL;
  const unsigned char* se = NULL;
  if (!base || !actorstate_script_span(&sb, &se)) return;   // still waiting

  int op[PR_KW_COUNT] = { -1, -1, -1 };
  const bool opsOk = find_opcodes(base, op);
  const bool same = op[PR_KW_IFPDISTL] == EK_PR_R10669_IFPDISTL &&
                    op[PR_KW_RESETCOUNT] == EK_PR_R10669_RESETCOUNT &&
                    op[PR_KW_ENDA] == EK_PR_R10669_ENDA;
  ek_log("pickuprange: opcodes ifpdistl=%d resetcount=%d enda=%d from the keyword table "
         "(r10669: %d/%d/%d, %s)", op[PR_KW_IFPDISTL], op[PR_KW_RESETCOUNT], op[PR_KW_ENDA],
         EK_PR_R10669_IFPDISTL, EK_PR_R10669_RESETCOUNT, EK_PR_R10669_ENDA,
         same ? "the same" : "DIFFERENT");
  if (!opsOk) {
    refuse("The engine's keyword table did not answer; see ekmod.log.");
    return;
  }
  if (!range_writable(sb, se)) {
    ek_log("pickuprange: the script block %p-%p is not all committed read-write; refusing",
           (const void*)sb, (const void*)se);
    refuse("AA's script memory is not writable; see ekmod.log.");
    return;
  }
  const int nexec = collect_exec(sb, se);
  int dups = 0;
  for (int i = 1; i < nexec; i++)
    if (s_execSorted[i] == s_execSorted[i - 1]) dups++;
  if (nexec <= 0 || dups) {
    ek_log("pickuprange: %d actor block(s) in g_tile, %d sharing an execPtr%s; refusing",
           nexec, dups, nexec < 0 ? " (the read faulted)" : "");
    refuse("AA's actors could not be read from g_tile; see ekmod.log.");
    return;
  }

  // The scan runs to the END OF THE ALLOCATION (9437184 bytes on AA), not the compiled size. Still safe: past the last
  // ENDA no site is owned, and any extra site breaks the 97 count, which refuses.
  const long long* const w = (const long long*)sb;
  const size_t n = (size_t)(se - sb) / 8;
  const uintptr_t wb = (uintptr_t)sb;
  const int opIf = op[PR_KW_IFPDISTL], opRc = op[PR_KW_RESETCOUNT], opEnda = op[PR_KW_ENDA];
  volatile int total = 0, gates = 0, resets = 0;   // written in __try, read after it
  int gatesPerRow[EKM_PR_AA_COUNT], resetsPerRow[EKM_PR_AA_COUNT];
  memset(gatesPerRow, 0, sizeof(gatesPerRow));
  memset(resetsPerRow, 0, sizeof(resetsPerRow));
  const EkPickupAaRow* const rows = pickuprange_aa_rows();
  bool faulted = false;
  __try {
    for (size_t s = 0; s + 2 < n; s++) {
      if (!pr_site_match(w, n, s, opIf, wb)) continue;
      if (total < PR_MAX_FOUND) {
        PrFound* const f = &s_found[total];
        f->s = s;
        f->line = (int)((w[s] >> 12) & 0x7FFFF);
        f->pic = -1;
        f->kind = PR_KIND_LEFT;
        const int own = pr_owner(s_execSorted, nexec, (uintptr_t)&w[s]);
        if (own >= 0 && pr_owned(w, n, (size_t)((s_execSorted[own] - wb) / 8), s, opEnda))
          f->pic = s_exec[own].pic;
        const int row = f->pic >= 0 ? pickuprange_aa_row(f->pic) : -1;
        if (row >= 0) {
          if (pr_is_resetcount(w, n, s, opRc, wb)) { f->kind = PR_KIND_SLIDE; resets++; resetsPerRow[row]++; }
          else { f->kind = PR_KIND_GATE; gates++; gatesPerRow[row]++; }
        }
      }
      total++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    faulted = true;
  }

  const bool ok = !faulted && total <= PR_MAX_FOUND &&
                  pr_counts_ok(total, gates, resets, gatesPerRow, resetsPerRow);
  ek_log("pickuprange: sites %d (gates %d, resetcount %d, left at 844 %d) in %llu script "
         "words, %d actor block(s); expected %d (%d, %d, %d)%s -- %s",
         total, gates, resets, total - gates - resets, (unsigned long long)n, nexec,
         EK_PR_SITES, EK_PR_GATES, EK_PR_RESETS, EK_PR_SITES - EK_PR_GATES - EK_PR_RESETS,
         faulted ? " (the scan faulted)" : "", ok ? "verified" : "REFUSED, nothing is patched");
  const int listed = total < PR_MAX_FOUND ? total : PR_MAX_FOUND;
  for (int k = 0; k < listed; k++) {
    const PrFound& f = s_found[k];
    if (ok && (f.kind == PR_KIND_LEFT || !g_cfg.debuglog)) continue;
    const int row = f.pic >= 0 ? pickuprange_aa_row(f.pic) : -1;
    ek_log("pickuprange:   word %llu line %d owner %d%s%s: %s", (unsigned long long)f.s, f.line,
           f.pic, row >= 0 ? " " : "", row >= 0 ? rows[row].name : "", kind_name(f.kind));
  }
  if (!ok) {
    _snprintf_s(s_statusBuf, sizeof(s_statusBuf), _TRUNCATE,
                "AA's script did not match: %d sites, %d gates, %d resetcounts "
                "(expected %d, %d, %d). See ekmod.log.",
                total, gates, resets, EK_PR_SITES, EK_PR_GATES, EK_PR_RESETS);
    refuse(s_statusBuf);
    return;
  }

  s_sites = 0;
  for (int k = 0; k < total; k++) {
    if (s_found[k].kind == PR_KIND_LEFT) continue;
    PrSite* const p = &s_site[s_sites++];
    p->word = (volatile long long*)(w + s_found[k].s);
    p->inst = w[s_found[k].s];
    p->opLast = EK_PR_AA_RETRIEVE;
  }
  s_written = EK_PR_AA_RETRIEVE;
  s_state = PR_READY;
  s_status = "Ready.";
}

// NEVER A CORRECTION: a slot that no longer holds what we left there is not ours any more, so the feature stops and
// writes nothing else this session. `done` > 0 only after a partial-write fault: sites [0, done) hold `want`.
static void stop(const char* why, int site, int done, long long want) {
  s_state = PR_STOPPED;
  s_status = "Stopped: AA's script changed under the mod. The reach stays at the last "
             "value written until the game restarts. See ekmod.log.";
  if (done > 0)
    ek_log("pickuprange: %s (site %d of %d) -- stopped for this session, nothing more is "
           "written; the sites are MIXED: 0..%d hold %lld, the rest %lld", why, site, s_sites,
           done - 1, want, s_written);
  else
    ek_log("pickuprange: %s (site %d of %d) -- stopped for this session, nothing more is "
           "written; every site still holds %lld", why, site, s_sites, s_written);
}

static void write_all(long long want) {
  volatile int bad = -1, at = 0;
  volatile long long instNow = 0, opNow = 0;
  __try {
    for (int k = 0; k < s_sites; k++) {
      at = k;
      instNow = s_site[k].word[0];
      opNow = s_site[k].word[1];
      if (!pr_write_ok(instNow, s_site[k].inst, opNow, s_site[k].opLast)) { bad = k; break; }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    stop("reading an operand faulted", at, 0, want);
    return;
  }
  if (bad >= 0) {
    ek_log("pickuprange: site %d reads instruction 0x%llX (recorded 0x%llX), operand %lld "
           "(last written %lld)", (int)bad, (unsigned long long)instNow,
           (unsigned long long)s_site[bad].inst, (long long)opNow, s_site[bad].opLast);
    stop("an operand or instruction word is not what was left there", bad, 0, want);
    return;
  }
  volatile int done = 0;
  __try {
    for (int k = 0; k < s_sites; k++) {
      s_site[k].word[1] = want;
      s_site[k].opLast = want;
      done = k + 1;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    stop("writing an operand faulted", done, done, want);
    return;
  }
  s_written = want;
  ek_log("pickuprange: %d operands now %lld", s_sites, want);
}

void pickuprange_frame(void) {
  if (s_state == PR_WAITING) {
    // A one-off read, row on or off: the row stays greyed until it has run.
    if (!actorstate_script_ready() || !badguy_ready()) return;
    verify();
  }
  if (s_state != PR_READY) return;
  const long long want = pickuprange_operand(g_cfg.pickuprange);
  if (want == s_written) return;
  write_all(want);
}

bool pickuprange_ready(void) { return s_state == PR_READY; }

const char* pickuprange_status(void) { return s_status; }
