// The call-site table for G_DrawRooms, and the one site it names: the renderDrawMasks call the player's own view goes
// through, which the world-quad layer flushes on top of. Rules and lifetime in maskscan.h.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "maskscan.h"
#include "camerascan.h"
#include "camera.h"
#include "imagescan.h"
#include "hitscan.h"
#include "hook.h"
#include "worldquad.h"
#include "log.h"

// Raised from 96 to 160 because the cap stopped being cosmetic: mask_locate refuses on an incomplete table and this
// image lists 88. ekm_mask_table_is_tight logs the margin either way.
static const int EK_MASK_MAX_SITES = 160;

struct MaskSite {
  unsigned site;
  unsigned target;
  unsigned begin;
  unsigned end;
  int isJump;
  int inImage;
  int hasExtent;
  int prevOk;
  unsigned char prev[16];
};

struct MaskCache { unsigned target; int sites; int owners; int capped; int faulted; };

// A target outside the image is the shape an 0xE8/0xE9 operand byte of another instruction takes, so it must not be
// called "no.pdata entry"; the column exists to tell those from real calls.
static const char* site_kind(const MaskSite* r) {
  if (!r->inImage) return "TARGET OUTSIDE THE IMAGE";
  if (!r->hasExtent) return "in the image, NO .pdata entry";
  return r->begin == r->target ? "entry" : "MID-FUNCTION";
}

static void hex16(const unsigned char* b, char* out) {
  static const char digits[] = "0123456789ABCDEF";
  int o = 0;
  for (int i = 0; i < 16; i++) {
    out[o++] = digits[(b[i] >> 4) & 0xF];
    out[o++] = digits[b[i] & 0xF];
    out[o++] = ' ';
  }
  out[o - 1] = 0;
}


// Published by the locate on the sampler thread, read by the install on the game thread: interlocked.
static volatile LONG s_masksTargetRva = 0;
static volatile LONG s_masksSiteRva = 0;
static volatile LONG s_playerQ16SiteRva = 0;
unsigned maskscan_player_q16_site_rva(void) {
  return (unsigned)InterlockedCompareExchange(&s_playerQ16SiteRva, 0, 0);
}
static volatile LONG s_probeAnswered = 0;
bool maskscan_answered(void) {
  return InterlockedCompareExchange(&s_probeAnswered, 0, 0) != 0;
}

// Step (c) over one site, from the sixteen bytes the walk captured. The window is cut at the nearest preceding call,
// taken from the table (not by hunting an 0xE8); only a row whose target has a.pdata entry at its entry anchors.
static bool mask_site_takes_args(const MaskSite* row, int nrow, unsigned site) {
  int at = -1;
  for (int i = 0; i < nrow; i++)
    if (row[i].site == site) { at = i; break; }
  if (at < 0 || !row[at].prevOk) return true;

  unsigned sites[EK_MASK_MAX_SITES];
  unsigned char anchors[EK_MASK_MAX_SITES];
  for (int i = 0; i < nrow; i++) {
    sites[i] = row[i].site;
    anchors[i] = (!row[i].isJump && row[i].hasExtent &&
                  row[i].begin == row[i].target) ? (unsigned char)1 : (unsigned char)0;
  }

  const unsigned prevEnd = ekm_mask_prev_call_end(sites, anchors, nrow, site);
  const int n = ekm_mask_window_len(site, prevEnd);
  return ekm_mask_touches_arg(row[at].prev + (16 - n), n);
}

// Both calls to one target inside the bracket, or false unless called exactly twice. Jumps are ignored: a tail jump
// is listed so an inlined chain cannot look absent, but it is not a call site to patch.
static bool mask_pair_of(const MaskSite* row, int nrow, unsigned target,
                         unsigned* a, unsigned* b) {
  int n = 0;
  unsigned first = 0, second = 0;
  for (int i = 0; i < nrow; i++) {
    if (row[i].isJump || row[i].target != target) continue;
    if (++n == 1) first = row[i].site;
    else if (n == 2) second = row[i].site;
  }
  if (n != 2) return false;
  *a = first;
  *b = second;
  return true;
}

// The locate: steps (a) to (e) over the table, no second image walk or byte pattern. Every refusal says what it
// counted and needed, and none falls back to a best candidate: a wrong site redirects an engine function.
static void mask_locate(const unsigned char* base, unsigned begin,
                        const MaskSite* row, int nrow, bool complete) {
  const unsigned q16 = camera_roomsQ16_rva();
  if (!complete) {
    ek_log("maskscan: the draw point is REFUSED -- the call table is not "
           "complete, so a rule over it could name a site whose pair was never "
           "seen. Nothing is patched");
    return;
  }
  if (q16 == 0) {
    ek_log("maskscan: the draw point is REFUSED at step (a) -- "
           "renderDrawRoomsQ16 was never located, so the two chains have no "
           "heads to be told apart by. Nothing is patched");
    return;
  }
  unsigned d1 = 0, d2 = 0;
  if (!mask_pair_of(row, nrow, q16, &d1, &d2) || d1 >= d2) {
    int heads = 0;
    for (int i = 0; i < nrow; i++)
      if (!row[i].isJump && row[i].target == q16) heads++;
    ek_log("maskscan: the draw point is REFUSED at step (a) -- "
           "renderDrawRoomsQ16 (0x%07X) heads %d chain(s) in the bracket and "
           "the rule needs exactly 2. Nothing is patched", q16, heads);
    return;
  }

  unsigned survTarget = 0, survSite = 0;
  int nb = 0, nc = 0;
  for (int i = 0; i < nrow; i++) {
    if (row[i].isJump || !row[i].hasExtent) continue;
    if (row[i].begin != row[i].target) continue;
    bool seen = false;
    for (int k = 0; k < i && !seen; k++)
      seen = !row[k].isJump && row[k].target == row[i].target;
    if (seen) continue;

    unsigned a = 0, b = 0;
    if (!mask_pair_of(row, nrow, row[i].target, &a, &b)) continue;
    if (!ekm_mask_straddles(a, b, d1, d2)) continue;
    if (row[i].end - row[i].begin < EK_MASK_MIN_EXTENT) continue;
    nb++;

    if (mask_site_takes_args(row, nrow, a)) continue;
    if (mask_site_takes_args(row, nrow, b)) continue;
    const unsigned pick = ekm_mask_player_site(a, b, d2);
    if (pick == 0) continue;
    nc++;
    survTarget = row[i].target;
    survSite = pick;
  }

  if (nc != 1) {
    ek_log("maskscan: the draw point is REFUSED at step (b)/(c) -- %d target(s) "
           "are called once between the two chain heads and once after the "
           "second with a .pdata extent of at least %u bytes, and %d of those "
           "have neither call preceded by argument setup; the rule needs "
           "exactly 1. Nothing is patched", nb, EK_MASK_MIN_EXTENT, nc);
    return;
  }

  EkCallCount c;
  ek_image_count_calls(base, survTarget, &c);
  if (c.faulted || !ekm_mask_corroborated(c.sites, c.owners)) {
    ek_log("maskscan: the draw point is REFUSED at step (e) -- 0x%07X is "
           "reached image-wide by %d site(s) in %d fn(s)%s, and the rule needs "
           "at least %d site(s) in at least %d fn(s). Nothing is patched",
           survTarget, c.sites, c.owners,
           c.faulted ? " (THE COUNT FAULTED, so it is a floor and not a count)"
                     : "",
           EK_MASK_MIN_SITES, EK_MASK_MIN_OWNERS);
    return;
  }

  InterlockedExchange(&s_playerQ16SiteRva, (LONG)d2);
  InterlockedExchange(&s_masksTargetRva, (LONG)survTarget);
  InterlockedExchange(&s_masksSiteRva, (LONG)survSite);
  ek_log("maskscan: the draw point is NAMED -- renderDrawMasks 0x%07X, called "
         "from the player's own view at 0x%07X +0x%04X, the chain after the "
         "second renderDrawRoomsQ16 at +0x%04X; %d candidate(s) survived step "
         "(b) and %d step (c); reached image-wide by %d site(s) in %d fn(s). "
         "Nothing is patched until the frame path installs it",
         survTarget, survSite, survSite - begin, d2 - begin, nb, nc,
         c.sites, c.owners);
}

void maskscan_probe(void) {
  static volatile LONG s_done = 0;

  const unsigned begin = (unsigned)InterlockedCompareExchange(&s_drawBegin, 0, 0);
  const unsigned end = (unsigned)InterlockedCompareExchange(&s_drawEnd, 0, 0);
  if (begin == 0 || end <= begin + 5) return;

  // s_base is stored after s_drawBegin in camera_scan, so seeing the bracket does not prove the base is visible: give
  // up without claiming the run; the sampler retries a second later.
  const unsigned char* const base = s_base;
  if (base == NULL) return;

  if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;

  const unsigned imgSize = (unsigned)host_image_size(base);
  if (imgSize == 0 || end > imgSize) {
    ek_log("maskscan: the bracket 0x%07X..0x%07X does not fit the image "
           "(SizeOfImage 0x%X); enumerating nothing", begin, end, imgSize);
    // The refusal says so in its own words: an earlier silent return claimed s_done and left the feature off with no
    // line.
    ek_log("maskscan: the draw point is REFUSED -- there is no call table to "
           "run the rule over. Nothing is patched");
    InterlockedExchange(&s_probeAnswered, 1);
    return;
  }

  const IMAGE_RUNTIME_FUNCTION_ENTRY* fn = NULL;
  DWORD nfn = 0;
  const bool havePdata = ek_image_pdata(base, &fn, &nfn);

  MaskSite row[EK_MASK_MAX_SITES];
  int nrow = 0;
  int found = 0;
  int inner = 0;
  int faulted = 0;
  __try {
    for (unsigned q = begin; q + 5 <= end; q++) {
      const unsigned char op = base[q];
      // E9 too: if a branch's last renderDrawMasks were a tail jump, a call-only walk would list nothing and look
      // like inlining.
      if (op != 0xE8 && op != 0xE9) continue;
      const int disp = *(const int*)(base + q + 1);
      const unsigned t = ekm_us_rel32_target(q, disp);
      if (op == 0xE9 && t >= begin && t < end) { inner++; continue; }

      found++;
      if (nrow >= EK_MASK_MAX_SITES) continue;
      MaskSite* const r = &row[nrow++];
      r->site = q;
      r->target = t;
      r->isJump = (op == 0xE9);
      r->inImage = (t < imgSize);
      r->begin = 0; r->end = 0; r->hasExtent = 0; r->prevOk = 0;
      for (int i = 0; i < 16; i++) r->prev[i] = 0;
      if (q >= 16) {
        for (int i = 0; i < 16; i++) r->prev[i] = base[q - 16 + i];
        r->prevOk = 1;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    faulted = 1;
  }

  for (int i = 0; i < nrow; i++) {
    if (!row[i].inImage) continue;
    unsigned b = 0, e = 0;
    const bool ok = ek_image_extent_containing(base, row[i].target, &b, &e);
    if (!ok) continue;
    row[i].begin = b; row[i].end = e; row[i].hasExtent = 1;
  }

  ek_log("maskscan: G_DrawRooms 0x%07X..0x%07X (%u bytes), %d listed rel32 "
         "site(s)%s of %d row slot(s), %d intra-bracket jmp suppressed; "
         "exception directory %s (%u entries). A 0xE8/0xE9 can be another "
         "instruction's operand byte, which is what the target column "
         "separates. THIS DECIDES NOTHING",
         begin, end, end - begin, found,
         found > nrow ? ", ROW LIST CAPPED" : "", EK_MASK_MAX_SITES, inner,
         havePdata ? "present" : "ABSENT", (unsigned)nfn);
  // Said before the cliff: the verdict needs every row, so a table merely close to the cap is the warning worth
  // having.
  if (ekm_mask_table_is_tight(found, EK_MASK_MAX_SITES))
    ek_log("maskscan: the row table is within a quarter of its cap (%d of %d) "
           "-- the draw point's rule needs a COMPLETE table, so raise "
           "EK_MASK_MAX_SITES before a build crosses it", found,
           EK_MASK_MAX_SITES);
  if (faulted)
    ek_log("maskscan: the bracket faulted mid-walk -- the %d row(s) below are "
           "partial and no count here is complete", nrow);

  MaskCache cache[EK_MASK_MAX_SITES];
  int ncache = 0;

  for (int i = 0; i < nrow; i++) {
    int sites = 0, owners = 0, capped = 0, cfaulted = 0;
    int hit = -1;
    for (int k = 0; k < ncache; k++)
      if (cache[k].target == row[i].target) { hit = k; break; }
    if (hit >= 0) {
      sites = cache[hit].sites; owners = cache[hit].owners;
      capped = cache[hit].capped; cfaulted = cache[hit].faulted;
    } else {
      EkCallCount c;
      ek_image_count_calls(base, row[i].target, &c);
      sites = c.sites; owners = c.owners; capped = c.capped; cfaulted = c.faulted;
      if (ncache < EK_MASK_MAX_SITES) {
        cache[ncache].target = row[i].target;
        cache[ncache].sites = sites; cache[ncache].owners = owners;
        cache[ncache].capped = capped; cache[ncache].faulted = cfaulted;
        ncache++;
      }
    }

    char ext[64];
    if (row[i].hasExtent)
      sprintf_s(ext, sizeof(ext), "0x%07X..0x%07X %u bytes", row[i].begin,
                row[i].end, row[i].end - row[i].begin);
    else
      strcpy_s(ext, sizeof(ext), "none");

    char prev[48];
    if (row[i].prevOk) hex16(row[i].prev, prev);
    else strcpy_s(prev, sizeof(prev), "unavailable");

    ek_log("maskscan: [%02d] %s 0x%07X +0x%04X -> 0x%07X %s; callers %d "
           "site(s) in %d fn(s)%s%s; extent %s; prev16 %s",
           i, row[i].isJump ? "jmp " : "call", row[i].site,
           row[i].site - begin, row[i].target, site_kind(&row[i]), sites,
           owners, capped ? " (owners capped)" : "",
           cfaulted ? " (COUNT FAULTED)" : "", ext, prev);
  }

  // The landmark is not unique: G_DrawRooms holds TWO renderDrawRoomsQ16 -> yax_drawrooms -> G_DoSpriteAnimations ->
  // renderDrawMasks chains (arms of one if/else), and the log must not pretend otherwise.
  const unsigned q16 = camera_roomsQ16_rva();
  int hits = 0;
  if (q16 != 0)
    for (int i = 0; i < nrow; i++)
      if (row[i].target == q16) hits++;

  if (q16 == 0) {
    ek_log("maskscan: no landmark -- renderDrawRoomsQ16 was never located");
  } else if (hits == 0) {
    ek_log("maskscan: landmark -- renderDrawRoomsQ16 (0x%07X) is reached by "
           "NONE of the %d listed site(s); the source has two chains, so zero "
           "is a surprise worth reading the rows over", q16, nrow);
  } else {
    int seen = 0;
    for (int i = 0; i < nrow; i++) {
      if (row[i].target != q16) continue;
      seen++;
      ek_log("maskscan: landmark %d of %d -- renderDrawRoomsQ16 (0x%07X) at "
             "row [%02d], site 0x%07X +0x%04X%s",
             seen, hits, q16, i, row[i].site, row[i].site - begin,
             hits == 2 ? "" : " -- NOT the 2 the source's two chains imply");
    }
  }

  // Only now the verdict, given the whole table's health: a capped list or faulted walk means unseen bytes could hold
  // the second call that makes a pair.
  mask_locate(base, begin, row, nrow, found == nrow && !faulted);
  InterlockedExchange(&s_probeAnswered, 1);
}

// The address the patched site used to reach, captured at install: the thunk must forward as long as the patch is in.
typedef void(*RenderDrawMasksFn)(void);
static RenderDrawMasksFn s_masksOrig = NULL;
static EkCallPatch s_masksPatch;

static bool s_saidNoMasksOrig = false;

// The thunk: renderDrawMasks paints masked geometry and switch sprites are masked geometry, so a tint flushed before
// it would be covered by the very switch it marks.
static void EK_DrawMasksThunk(void) {
  RenderDrawMasksFn const orig = s_masksOrig;
  if (!orig) {
    if (!s_saidNoMasksOrig) {
      s_saidNoMasksOrig = true;
      ek_log("maskscan: the draw-masks thunk ran with no original to forward "
             "to, which nothing in this build can produce -- please report it. "
             "The frame draws no masked geometry while this lasts");
    }
    return;
  }

  // The engine first and unguarded on purpose (camera.cpp's reason): a fault is one the game would take without the
  // mod.
  orig();
  worldquad_flush();
}

void maskscan_install(void) {
  static bool tried = false;
  if (tried) return;

  const unsigned site = (unsigned)InterlockedCompareExchange(&s_masksSiteRva, 0, 0);
  const unsigned target = (unsigned)InterlockedCompareExchange(&s_masksTargetRva, 0, 0);
  if (site == 0 || target == 0) return;

  const unsigned char* const base = s_base;
  if (base == NULL) return;
  tried = true;

  // The address goes in first: the patch is live the instant its four bytes land.
  s_masksOrig = (RenderDrawMasksFn)(const void*)(base + target);
  // The located entry is the `expected`: hook.cpp refuses unless the five bytes decode as a `call rel32` reaching it,
  // a second independent confirmation of the locate.
  if (!callsite_install((void*)(base + site), (void*)(base + target),
                        (void*)&EK_DrawMasksThunk, &s_masksPatch)) {
    s_masksOrig = NULL;
    ek_log("maskscan: the world-quad draw point at rva 0x%07X was NOT "
           "redirected; the game keeps its own call and every quad consumer "
           "draws nothing", site);
    return;
  }

  // The list is told from the transition we own: worldquad.cpp knows only whether it has a draw point.
  worldquad_set_drawpoint(true);
  ek_log("maskscan: the world-quad draw point is IN -- the player view's "
         "renderDrawMasks call at rva 0x%07X now reaches our thunk, which runs "
         "0x%07X first and then flushes the quad list on top of it", site,
         target);
}

void maskscan_remove(void) {
  // The verdict goes down before the bytes, so a producer pushing between the two is refused rather than filling a
  // list nothing flushes.
  worldquad_set_drawpoint(false);
  if (s_masksPatch.site == NULL) return;
  if (callsite_remove(&s_masksPatch)) s_masksOrig = NULL;
}
