// The DLL half of the per-storey flush: yax_globallev's two-site locate, and the flush driven from
// EK_RoomsQ16Detour's entry. The rules are in storeyflush.h; this file reads the image and three engine globals and
// writes none.

#include <windows.h>
#include <string.h>

#include "storeyflush.h"
#include "camerascan.h"   // s_base, slot_target_rva
#include "camera.h"       // the yax span, renderDrawRoomsQ16's rva, its answer
#include "cutaway.h"      // g_nodraw: its value, and the store site A sits before
#include "maskscan.h"     // d2: the player view's renderDrawRoomsQ16 call
#include "nocull.h"       // gotsector
#include "imagescan.h"    // ek_image_data_extent
#include "hitscan.h"      // ekm_us_rel32_target
#include "gamestate.h"    // EK_MAXSECTORS
#include "worldquad.h"
#include "config.h"       // g_cfg.debuglog
#include "log.h"
#include "ui/wallclock.h"

static bool sf_readable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const unsigned char* const end = (const unsigned char*)p + bytes;
  const unsigned char* const regionEnd =
      (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  return end <= regionEnd;
}

// THE LOCATE (sampler thread). Measured in AA's eduke32.exe; nothing reads these:
// site A 0x015871D mov rax,[rip+d] (slot 0x05A4C70); 0x0158724 cmp dword [rax],8, 0x29 before cutaway's g_nodraw
// store at 0x015874D. Site B 0x012E7FA mov dword [rip+d],8 -> 0x04D50D8, 0x43 before the base call at 0x012E83D.
static volatile LONG s_levRva = 0;
static volatile LONG s_levAnswered = 0;

// A site's global: its direct target, or its .refptr slot's, in a data section.
static unsigned sf_resolve(const unsigned char* base, const EkSfSite& s) {
  if (s.count != 1) return 0;
  unsigned rva = s.target;
  if (rva == 0 && s.slot != 0) {
    unsigned sb = 0, se = 0;
    const bool slotInData = ek_image_data_extent(base, s.slot, &sb, &se);
    if (!slotInData || se - s.slot < 8u) return 0;
    const size_t t = slot_target_rva(base, s.slot);
    rva = t < 0x80000000u ? (unsigned)t : 0u;   // never a truncated address
  }
  if (rva == 0) return 0;
  unsigned db = 0, de = 0;
  const bool inData = ek_image_data_extent(base, rva, &db, &de);
  return (inData && de > rva && de - rva >= 4u) ? rva : 0u;
}

void storeyflush_locate(void) {
  if (InterlockedCompareExchange(&s_levAnswered, 0, 0) != 0) return;
  const unsigned char* const base = s_base;
  if (base == NULL) return;
  // Every locate the flush needs answers first, so the render side settles
  // once, for good, on what they said.
  if (!cutaway_answered() || !camera_tror_base_answered() ||
      !nocull_answered() || !maskscan_answered())
    return;
  // s_levAnswered goes up only after s_levRva is final, on either path below:
  // the render side settles once and reads the rva when it sees it.

  int faulted = 0;
  // Site A: the one `cmp [g], 8` in the window before `g_nodraw = 1`.
  const unsigned store = cutaway_nodraw_store_rva();
  EkSfSite sa = { 0, 0, 0, 0 };
  if (store > EK_SF_WINDOW && sf_readable(base + store - EK_SF_WINDOW, EK_SF_WINDOW)) {
    __try {
      ekm_sf_scan(base, store - EK_SF_WINDOW, store, true, EK_SF_YAX_MAXDRAWS, &sa);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      faulted = 1;
    }
  }
  const unsigned aRva = faulted ? 0u : sf_resolve(base, sa);

  // Site B: of yax_drawrooms' three renderDrawRoomsQ16 calls, the one store of
  // 8 before exactly one of them.
  unsigned yb = 0, ye = 0;
  const unsigned rooms = camera_roomsQ16_rva();
  const bool haveYax = camera_yax_span(&yb, &ye);
  unsigned sites[3] = { 0, 0, 0 };
  int nsites = 0;
  EkSfSite sb[3] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
  int pick = -1;
  if (haveYax && rooms != 0 && ye > yb + 5 && sf_readable(base + yb, ye - yb)) {
    __try {
      for (unsigned q = yb; q + 5 <= ye; q++) {
        if (base[q] != 0xE8) continue;
        if (ekm_us_rel32_target(q, *(const int*)(base + q + 1)) != rooms) continue;
        if (nsites < 3) sites[nsites] = q;
        nsites++;
      }
      if (nsites == 3)
        for (int i = 0; i < 3; i++) {
          const unsigned lo = sites[i] - yb > EK_SF_WINDOW ? sites[i] - EK_SF_WINDOW : yb;
          ekm_sf_scan(base, lo, sites[i], false, EK_SF_YAX_MAXDRAWS, &sb[i]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      faulted = 1;
    }
    if (!faulted && nsites == 3) pick = ekm_sf_pick_base(sb, 3);
  }
  const unsigned bRva = pick >= 0 ? sf_resolve(base, sb[pick]) : 0u;

  const unsigned lev = faulted ? 0u : ekm_sf_agree(aRva, bRva);
  if (lev == 0 || !sf_readable(base + lev, 4)) {
    ek_log("storeyflush: yax_globallev REFUSED -- site A (polymost_drawrooms' "
           "cmp with 8 before the g_nodraw store at 0x%07X) found %d, naming "
           "0x%07X; site B (yax_drawrooms' store of 8 before its base-level call) "
           "%d drawrooms call(s), pick %d, naming 0x%07X; they must name one "
           "readable int32%s. Storeys are grouped one pass each (fallback): a "
           "room seen through two holes of one storey keeps the first hole's "
           "tint only", store, sa.count, aRva, nsites, pick, bRva,
           faulted ? " -- A SCAN FAULTED" : "");
    InterlockedExchange(&s_levAnswered, 1);
    return;
  }
  InterlockedExchange(&s_levRva, (LONG)lev);
  InterlockedExchange(&s_levAnswered, 1);
  ek_log("storeyflush: yax_globallev at rva 0x%07X (site A 0x%07X via %s, site "
         "B 0x%07X before the base-level call at 0x%07X); reads %d now",
         lev, sa.at, sa.slot != 0 ? ".refptr" : "rip", sb[pick].at, sites[pick],
         *(const volatile int*)(base + lev));
}

// THE FLUSH (render thread only). Settled once from storeyflush_frame_begin, after every locate has answered: ready
// (1), or inert for the session (-1), sending every quad to the final flush as before.
static int s_ready = 0;
static const unsigned char* s_rtBase = NULL;
static unsigned s_d2Ret = 0, s_yaxB = 0, s_yaxE = 0;
static const unsigned char* s_got = NULL;
static int s_gotBytes = 0;
static const volatile int* s_lev = NULL;   // NULL: the per-pass fallback

// One window per player view: opened at its Q16 call, closed at the base.
static int s_open = 0, s_prevReal = 0, s_candBuilt = 0, s_ncand = 0;
static int s_ordinal = 0, s_pendingCount = 0;
static EkSfLevelState s_st = { 0, 0 };
static short s_cand[EK_WORLD_QUAD_MAX];
static unsigned char s_pending[EK_MAXSECTORS / 8];

// Cumulative, never sampled.
static unsigned s_windows = 0, s_passFake = 0, s_passReal = 0, s_passBase = 0;
static unsigned s_levels = 0, s_levelFlushes = 0, s_fallbackPasses = 0;
static unsigned s_multiPass = 0;
static unsigned s_logMs = 0;
static bool s_logFirst = true;

static void sf_settle(void) {
  if (InterlockedCompareExchange(&s_levAnswered, 0, 0) == 0) return;
  const unsigned char* const base = s_base;
  if (base == NULL) return;
  const unsigned d2 = maskscan_player_q16_site_rva();
  unsigned yb = 0, ye = 0;
  const bool haveYax = camera_yax_span(&yb, &ye);
  const unsigned gotRva = nocull_gotsector_rva();
  const int gotBytes = nocull_gotsector_bytes();
  const char* missing = NULL;
  if (d2 == 0) missing = "the player view's renderDrawRoomsQ16 call (maskscan)";
  else if (!haveYax) missing = "yax_drawrooms (camera yax)";
  else if (gotRva == 0 || gotBytes <= 0 || gotBytes > (int)sizeof s_pending)
    missing = "gotsector (nocull)";
  else if (cutaway_nodraw_rva() == 0) missing = "g_nodraw (cutaway nodraw)";
  else if (!sf_readable(base + gotRva, (size_t)gotBytes))
    missing = "a readable gotsector";
  if (missing) {
    s_ready = -1;
    ek_log("storeyflush: INERT -- %s is not located, so every world quad "
           "goes to the final flush exactly as before D33", missing);
    return;
  }
  const unsigned lev = (unsigned)InterlockedCompareExchange(&s_levRva, 0, 0);
  s_lev = (lev != 0 && sf_readable(base + lev, 4))
              ? (const volatile int*)(base + lev) : NULL;
  s_rtBase = base;
  s_d2Ret = d2 + 5u;
  s_yaxB = yb;
  s_yaxE = ye;
  s_got = base + gotRva;
  s_gotBytes = gotBytes;
  s_ready = 1;
  ek_log("storeyflush: READY -- the storey window opens at the call returning "
         "to 0x%07X; storeys grouped by %s; gotsector %d bytes",
         s_d2Ret, s_lev ? "yax_globallev" : "PASS (yax_globallev refused: fallback)",
         s_gotBytes);
}

static void sf_close(void) {
  s_open = 0;
  s_prevReal = 0;
}

static void sf_level_flush(void) {
  worldquad_flush_level(s_pending, (int)sizeof s_pending);
  s_levelFlushes++;
  memset(s_pending, 0, sizeof s_pending);
  s_pendingCount = 0;
}

void storeyflush_frame_begin(void) {
  sf_close();
  if (s_ready == 0) { sf_settle(); return; }
  if (s_ready < 0) return;
  if (g_cfg.debuglog == 0) return;
  const unsigned now = wallclock_ms();
  if (!s_logFirst && now - s_logMs < 1000u) return;
  s_logFirst = false;
  s_logMs = now;
  // Here, between frames: every level-time quad has met its final flush.
  const unsigned q = worldquad_level_drawn(), f = worldquad_final_undrawn();
  const unsigned p = worldquad_final_total();
  ek_log("storeyflush: %u window(s); passes fake %u / real %u / base %u; %u "
         "storey(s), %u level flush(es), %u fallback pass(es), %u multi-pass "
         "sector(s); quads at level time Q %u + at final flush F %u = %u, "
         "pushed P %u -- %s",
         s_windows, s_passFake, s_passReal, s_passBase, s_levels, s_levelFlushes,
         s_fallbackPasses, s_multiPass, q, f, q + f, p,
         q + f == p ? "AGREES" : "DISAGREES");
}

void storeyflush_pass_skipped(void) { s_prevReal = 0; }

void storeyflush_on_q16(const void* ret, int sect) {
  if (s_ready <= 0) return;
  const unsigned retRva = (unsigned)(size_t)((const unsigned char*)ret - s_rtBase);
  if (retRva == s_d2Ret) {
    // The player view's own call: yax_drawrooms follows it (game.cpp:1142-1143).
    s_open = 1;
    s_prevReal = 0;
    s_st.haveLast = 0;
    s_st.lastKey = 0;
    s_ordinal = 0;
    s_candBuilt = 0;
    s_ncand = 0;
    if (s_pendingCount != 0) memset(s_pending, 0, sizeof s_pending);
    s_pendingCount = 0;
    s_windows++;
    return;
  }
  if (!s_open) return;

  // The previous pass reached the engine and returned: gotsector is its scan
  // until this call clears it (engine.cpp:9502).
  if (s_prevReal) {
    int rep = 0;
    s_pendingCount += ekm_sf_fold_pass(s_got, s_gotBytes, s_cand, s_ncand, s_pending,
                                       (int)sizeof s_pending, &rep);
    s_multiPass += (unsigned)rep;
    s_prevReal = 0;
  }

  const bool inYax = retRva >= s_yaxB && retRva < s_yaxE;
  const int cls = ekm_sf_classify(inYax, cutaway_nodraw_value(), sect, EK_MAXSECTORS);
  if (cls == EK_SF_CALL_FAKE) { s_passFake++; return; }
  if (cls == EK_SF_CALL_BASE) {
    s_passBase++;
    if (s_pendingCount > 0) sf_level_flush();
    sf_close();
    return;
  }
  if (cls != EK_SF_CALL_REAL) return;

  s_passReal++;
  if (!s_candBuilt) {
    s_candBuilt = 1;
    s_ncand = worldquad_candidate_sectors(s_cand, EK_WORLD_QUAD_MAX);
  }
  if (s_ncand == 0) return;   // no quad of this frame names a sector
  const bool levRead = s_lev != NULL;
  const int lev = levRead ? *s_lev : 0;
  const int key = ekm_sf_level_key(levRead, lev, s_ordinal++);
  if (ekm_sf_key_is_fallback(key)) s_fallbackPasses++;
  if (!s_st.haveLast || key != s_st.lastKey) s_levels++;
  if (ekm_sf_enter_real(&s_st, key, s_pendingCount)) sf_level_flush();
  s_prevReal = 1;
}
