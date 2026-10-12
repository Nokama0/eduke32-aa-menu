// aGameVars[]: EDuke32's CON gamevar registry, located by content (gamevars.h has the record layout and AA's
// friend/foe rule). This file is the locate, the label walk and the one guarded per-actor read. The anchor is not the
// one the design first named; see the locate below.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "gamestate.h"
#include "tiles.h"
#include "playerstate.h"
#include "weapondata.h"
#include "udstate.h"
#include "gamevars.h"

static const unsigned char* volatile s_vars = NULL;
// Worker thread only, like badguy.cpp's pair: the scan walks every writable section, so riding the 1 Hz sampler
// without a terminal state would burn a core for the life of the process where aGameVars is never found.
static int s_attempts = 0;
static bool s_budgetSpent = false;
static volatile LONG s_rva = 0;
static volatile LONG s_walked = 0;

// The named per-actor vars, resolved once by the label walk and read through one guarded accessor (gv_peractor_read).
// Each is `gamevar <name> <default> 2` in AADEFS.CON, so they differ only in name; a fifth is a row, not a second
// read.
enum { EK_GV_PA_TEAM = 0, EK_GV_PA_MTYPE = 1, EK_GV_PA_INITHP = 2,
       EK_GV_PA_MLEVEL = 3, EK_GV_PA_DROPTILE = 4, EK_GV_PA_MONSTATUS = 5,
       EK_GV_PA_BOTTARGET = 6, EK_GV_PA_COUNTVARC = 7, EK_GV_PA_COUNTVAR = 8,
       EK_GV_PA_DASHTIME = 9, EK_GV_PA_MYSPAWNER = 10, EK_GV_PA_INITFLAGS = 11,
       EK_GV_PA_MONSTFLAGS = 12, EK_GV_PA_COUNT = 13 };
static const char* const kPerActor[EK_GV_PA_COUNT] =
    { "team", "mtype", "inithp", "mlevel", "droptile", "monstatus",
      "bottarget", "countvarc", "countvar", "dashtime", "myspawner", "initflags",
      "monstflags" };
static volatile LONG s_paIdx[EK_GV_PA_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static volatile LONG s_paSaid[EK_GV_PA_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static_assert(EK_GV_PA_COUNT == 13,
              "s_paIdx's -1 initialiser has one entry per row; a missing one is 0, "
              "which is the index of `sprite`");

// The per-player side is the same record with a different slot: GAMEVAR_PERPLAYER (0x1) instead of GAMEVAR_PERACTOR
// (0x2), and pValues has MAXPLAYERS entries instead of MAXSPRITES (Gv_NewVar), so gv_indexed_read serves both.
static_assert(EK_WD_FIELD_COUNT == 10,
              "the -1 initialiser below has one entry per field; add a column "
              "here when an eleventh field joins EK_WD_FIELD_COUNT");
static_assert(EK_WD_MAX_WEAPONS == 13,
              "the -1 initialiser below has one row per weapon");
static volatile LONG s_wIdx[EK_WD_MAX_WEAPONS][EK_WD_FIELD_COUNT] = {
  { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
  { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
  { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
  { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
  { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
  { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }, { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
  { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
};
// One latch per weapon, not one overall: a weapon whose vars stopped reading must not silence another that also
// stopped.
static volatile LONG s_wSaid[EK_WD_MAX_WEAPONS] =
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
// AA's five zoom vars: two PERPLAYER, three GLOBAL (AADEFS.CON), so `want` is a column here rather than a second
// list.
enum { EK_GV_Z_ZOOMIN = 0, EK_GV_Z_ZOOMLEVEL = 1, EK_GV_Z_VR = 2,
       EK_GV_Z_VRHI = 3, EK_GV_Z_VRLO = 4, EK_GV_Z_COUNT = 5 };
static_assert(EK_GV_Z_COUNT == EK_GV_ZOOM_COUNT,
              "the count the trace prints is the length of this list");
static const char* const kZoom[EK_GV_Z_COUNT] =
    { "zoomin", "zoomlevel", "vr", "vrhi", "vrlo" };
static const uintptr_t kZoomWant[EK_GV_Z_COUNT] = {
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  EK_GV_WANT_GLOBAL, EK_GV_WANT_GLOBAL, EK_GV_WANT_GLOBAL,
};
static volatile LONG s_zIdx[EK_GV_Z_COUNT] = { -1, -1, -1, -1, -1 };
static volatile LONG s_zSaid[EK_GV_Z_COUNT] = { 0, 0, 0, 0, 0 };
static volatile LONG s_zResolved = 0;

enum { EK_GV_E_VIEWINGRANGE = 0, EK_GV_E_YXASPECT = 1, EK_GV_E_COUNT = 2 };
static_assert(EK_GV_E_COUNT == EK_GV_ENGINE_COUNT,
              "the count the trace prints is the length of this list");
static const char* const kEngine[EK_GV_E_COUNT] = { "viewingrange", "yxaspect" };
static volatile LONG s_eIdx[EK_GV_E_COUNT] = { -1, -1 };
static volatile LONG s_eResolved = 0;

static_assert(EK_GV_CO_COUNT == EK_GV_COLLECT_COUNT,
              "the count the report line prints is the length of this list");
static const char* const kCollect[EK_GV_CO_COUNT] = { "collectibles1", "collectibles2",
                                                       "collectinlevel", "collectfound",
                                                       "startguns_perma", "mcguffins" };
static volatile LONG s_coIdx[EK_GV_CO_COUNT] = { -1, -1, -1, -1, -1, -1 };
static volatile LONG s_coResolved = 0;
static volatile LONG s_pcharIdx = -1;
static volatile LONG s_pcharSaid = 0;
// AA's `gotshock`, the Executioner/chaingun bitfield. Its own latch, as `pchar` has.
static volatile LONG s_gotshockIdx = -1;
static volatile LONG s_gotshockSaid = 0;
static volatile LONG s_aimbotOnIdx = -1;
static volatile LONG s_aimbotOnSaid = 0;

// THISCLIP, fastfire and perks, one row each rather than three copies of pchar's shape; `burstfired` joined because
// AAPLAYER.CON gates the same refill block on it before the clip test.
enum { EK_GV_D12_THISCLIP = 0, EK_GV_D12_FASTFIRE = 1, EK_GV_D12_PERKS = 2,
       EK_GV_D12_BURSTFIRED = 3, EK_GV_D12_GOTDB = 4, EK_GV_D12_DBAMMO = 5,
       EK_GV_D12_RIGHTDB = 6, EK_GV_D12_STEROIDSTACK = 7,
       EK_GV_D12_INVENUPG = 8, EK_GV_D12_COUNT = 9 };
static const char* const kD12[EK_GV_D12_COUNT] =
    { "THISCLIP", "fastfire", "perks", "burstfired", "gotDB", "dbammo",
      "rightDB_weapcount", "steroidstack", "inven_upgrades" };
static volatile LONG s_d12Idx[EK_GV_D12_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static volatile LONG s_d12Said[EK_GV_D12_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static_assert(EK_GV_D12_COUNT == 9,
              "s_d12Idx's -1 initialiser has one entry per row; a missing one is 0");
static volatile LONG s_wResolved = 0;

// AA's counter-driven reloads: one more row list of pchar's shape. s_rcWriteDead is the write's give-up latch, set on
// the first failure and never cleared, so a refused write is said once and never retried.
static const char* const kReloadCtr[EK_GV_RC_COUNT] =
    { "m4reload", "uzireload", "m4clip", "uziclip" };
static volatile LONG s_rcIdx[EK_GV_RC_COUNT] = { -1, -1, -1, -1 };
static volatile LONG s_rcSaid[EK_GV_RC_COUNT] = { 0, 0, 0, 0 };
static volatile LONG s_rcWriteDead = 0;

static const char* const kRecoil[EK_GV_RV_COUNT] = { "horizfall", "savedhoriz" };
static volatile LONG s_rvIdx[EK_GV_RV_COUNT] = { -1, -1 };
static volatile LONG s_rvSaid[EK_GV_RV_COUNT] = { 0, 0 };
static volatile LONG s_rvWriteDead = 0;

// AA's item-take vars, shot-guard shape (AADEFS.CON): most are 131073 (NORESET|PERPLAYER); charsel (131072),
// startguns and gametype are GLOBAL.
static const char* const kTake[EK_GV_TK_COUNT] =
    { "LASERMODE", "pistolammo", "wristammo", "nukeselect", "weap4ammo",
      "gotplasma", "plasmammo", "gotrailgun", "railgunammo", "gotbow", "arrows",
      "sawammo", "charsel", "startguns", "gametype",
      "stakes", "nukeamount", "LASERAMMO", "COMMAMMO", "gotsaw", "gotsaber",
      "attmode",
      "es_type", "es_battery", "es_batterymax" };
static const uintptr_t kTakeWant[EK_GV_TK_COUNT] = {
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  EK_GV_WANT_GLOBAL, EK_GV_WANT_GLOBAL, EK_GV_WANT_GLOBAL,
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  EK_GV_WANT_GLOBAL,
  (uintptr_t)EK_GAMEVAR_PERPLAYER, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  (uintptr_t)EK_GAMEVAR_PERPLAYER,
};
static volatile LONG s_tkIdx[EK_GV_TK_COUNT] =
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static volatile LONG s_tkSaid[EK_GV_TK_COUNT] = { 0 };
static_assert(EK_GV_TK_COUNT == 25,
              "s_tkIdx's -1 initialiser has one entry per row; a missing one is 0");

// Screenshot mode's ten guards, zoom's shape: `want` is a column because vendor_screen and matchmenu are PERPLAYER
// and the rest GLOBAL.
static const char* const kShotGuard[EK_GV_SG_COUNT] =
    { "brutalcount", "crawlclock", "vendor_screen", "vrmenu", "startmode",
      "bonus_screen", "epselect", "matchmenu", "vrscreen_time", "thiscam" };
static const uintptr_t kShotGuardWant[EK_GV_SG_COUNT] = {
  EK_GV_WANT_GLOBAL, EK_GV_WANT_GLOBAL, (uintptr_t)EK_GAMEVAR_PERPLAYER,
  EK_GV_WANT_GLOBAL, EK_GV_WANT_GLOBAL, EK_GV_WANT_GLOBAL,
  EK_GV_WANT_GLOBAL, (uintptr_t)EK_GAMEVAR_PERPLAYER, EK_GV_WANT_GLOBAL,
  EK_GV_WANT_GLOBAL,
};
static volatile LONG s_sgIdx[EK_GV_SG_COUNT] =
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static volatile LONG s_sgSaid[EK_GV_SG_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// The nineteen, in Gv_AddSystemVars' own order: an ordered nineteen-string signature is not something a stretch of
// memory produces.
static const char* const kSpecial[EK_GV_SPECIAL_COUNT] = {
  "sprite", "__sprite__", "__actor__", "__spriteext__", "sector", "__sector__",
  "wall", "__wall__", "player", "__player__", "actorvar", "playervar", "tspr",
  "projectile", "thisprojectile", "userdef", "input", "tiledata", "paldata",
};

// A committed, readable page. Unlike cvars.cpp's readable this is NOT bounded to the host image: a szLabel is a
// 26-byte heap Xcalloc and a per-actor pValues a 128 KB aligned allocation, so an in-image test would refuse every
// pointer here.
static volatile LONG s_vqCalls = 0;

static bool readable(const void* p, size_t bytes) {
  if (!p) return false;
  MEMORY_BASIC_INFORMATION mbi;
  InterlockedIncrement(&s_vqCalls);
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const unsigned char* end = (const unsigned char*)p + bytes;
  const unsigned char* regionEnd =
      (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
  return end <= regionEnd;
}

static intptr_t gv_word(const unsigned char* rec, int off) {
  return *(const intptr_t*)(rec + off);
}
static const char* gv_label(const unsigned char* rec) {
  return *(const char* const*)(rec + EK_GV_LABEL);
}

// Exact, never a prefix: AADEFS.CON declares `teamspawned`, also per-actor, and a prefix match on "team" takes it.
static bool label_is(const char* s, const char* want) {
  if (!s) return false;
  for (int i = 0; i < EK_MAXVARLABEL; i++) {
    if (!readable(s + i, 1)) return false;
    const char c = s[i];
    if (c != want[i]) return false;
    if (c == 0) return true;
  }
  return false;
}

// A label copied through the guard once so the parser never touches engine memory: label_is asks one name at a time,
// which suits four fixed names but not a walk asking every record which of 91 names it might be. One VirtualQuery for
// the 26 bytes.
static bool label_copy(const char* s, char* out, int cap) {
  if (!s || !out || cap < EK_MAXVARLABEL) return false;
  out[0] = 0;
  if (readable(s, EK_MAXVARLABEL)) {
    for (int i = 0; i < EK_MAXVARLABEL; i++) {
      out[i] = s[i];
      if (s[i] == 0) return i > 0;
    }
    return false;
  }
  for (int i = 0; i < EK_MAXVARLABEL; i++) {
    if (!readable(s + i, 1)) return false;
    out[i] = s[i];
    if (s[i] == 0) return i > 0;
  }
  return false;
}

static bool label_readable(const char* s) {
  if (!s) return false;
  for (int i = 0; i < EK_MAXVARLABEL; i++) {
    if (!readable(s + i, 1)) return false;
    if (s[i] == 0) return i > 0;
  }
  return false;
}

// The cheap half of the anchor: three known words per record, nineteen times, nothing dereferenced; almost every
// candidate dies on the first compare.
static bool shape_ok(const unsigned char* cand) {
  for (int i = 0; i < EK_GV_SPECIAL_COUNT; i++) {
    const unsigned char* const rec = cand + (size_t)i * EK_GV_STRIDE;
    if (gv_word(rec, EK_GV_GLOBAL) != -1) return false;
    if (gv_word(rec, EK_GV_DEFAULT) != -1) return false;
    // GAMEVAR_RESET is masked out rather than matched: Gv_Free sets it on every var during a CON re-parse and
    // Gv_NewVar clears it again, so the bit comes and goes under a live scan.
    const uintptr_t f = (uintptr_t)gv_word(rec, EK_GV_FLAGS);
    if ((f & ~(uintptr_t)EK_GAMEVAR_RESET) != (uintptr_t)EK_GV_SPECIAL_FLAGS)
      return false;
  }
  return true;
}

// The half that identifies: all nineteen labels in order at stride 32; anything short of nineteen is a refusal.
static bool labels_ok(const unsigned char* cand) {
  for (int i = 0; i < EK_GV_SPECIAL_COUNT; i++)
    if (!label_is(gv_label(cand + (size_t)i * EK_GV_STRIDE), kSpecial[i]))
      return false;
  return true;
}

struct GvSweep {
  const unsigned char* hit;
  unsigned rva, secondRva;
  int shape, verified, faulted;
};

// A section's VirtualSize can run past what is committed, and a partial sweep must never be reported complete (unseen
// bytes could hold the second match). imagescan.cpp's contract, kept here because this walk needs.bss.
static void sweep_section(const unsigned char* start, DWORD vaddr, DWORD span,
                          GvSweep* s) {
  const size_t bytes = (size_t)EK_GV_SPECIAL_COUNT * EK_GV_STRIDE;
  if (span < bytes) return;
  __try {
    for (DWORD off = 0; (size_t)off + bytes <= (size_t)span; off += 8) {
      const unsigned char* const cand = start + off;
      if (!shape_ok(cand)) continue;
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

struct GvWalk {
  int walked, pa[EK_GV_PA_COUNT], coop, multimode;
  int perPlayer, resolved, pchar, gotshock, aimbotOn;
  int d12[EK_GV_D12_COUNT];
  int rc[EK_GV_RC_COUNT], rcResolved;
  int rv[EK_GV_RV_COUNT], rvResolved;
  int w[EK_WD_MAX_WEAPONS][EK_WD_FIELD_COUNT];
  int z[EK_GV_Z_COUNT], zResolved;
  int e[EK_GV_E_COUNT], eResolved;
  int sg[EK_GV_SG_COUNT], sgResolved;
  int co[EK_GV_CO_COUNT], coResolved;
  int tk[EK_GV_TK_COUNT], tkResolved;
};

// Index 0 upward, stopping at the first szLabel that is null or unreadable: Gv_NewVar fills the array contiguously.
static void walk_labels(const unsigned char* base, GvWalk* w) {
  w->walked = 0; w->coop = -1; w->multimode = -1;
  w->perPlayer = 0; w->resolved = 0; w->pchar = -1; w->gotshock = -1;
  w->aimbotOn = -1;
  for (int k = 0; k < EK_GV_D12_COUNT; k++) w->d12[k] = -1;
  for (int k = 0; k < EK_GV_RC_COUNT; k++) w->rc[k] = -1;
  w->rcResolved = 0;
  for (int k = 0; k < EK_GV_RV_COUNT; k++) w->rv[k] = -1;
  w->rvResolved = 0;
  w->zResolved = 0; w->eResolved = 0; w->sgResolved = 0; w->coResolved = 0;
  for (int k = 0; k < EK_GV_SG_COUNT; k++) w->sg[k] = -1;
  w->tkResolved = 0;
  for (int k = 0; k < EK_GV_TK_COUNT; k++) w->tk[k] = -1;
  for (int k = 0; k < EK_GV_PA_COUNT; k++) w->pa[k] = -1;
  for (int k = 0; k < EK_GV_Z_COUNT; k++) w->z[k] = -1;
  for (int k = 0; k < EK_GV_E_COUNT; k++) w->e[k] = -1;
  for (int k = 0; k < EK_GV_CO_COUNT; k++) w->co[k] = -1;
  for (int a = 0; a < EK_WD_MAX_WEAPONS; a++)
    for (int b = 0; b < EK_WD_FIELD_COUNT; b++) w->w[a][b] = -1;
  for (int i = 0; i < EK_MAXGAMEVARS; i++) {
    const unsigned char* const rec = base + (size_t)i * EK_GV_STRIDE;
    if (!readable(rec, EK_GV_STRIDE)) break;
    const char* const lbl = gv_label(rec);
    if (!label_readable(lbl)) break;
    w->walked++;
    const uintptr_t f = (uintptr_t)gv_word(rec, EK_GV_FLAGS);
    if (f & EK_GAMEVAR_PERACTOR)
      for (int k = 0; k < EK_GV_PA_COUNT; k++)
        if (w->pa[k] < 0 && label_is(lbl, kPerActor[k])) w->pa[k] = i;
    // The per-player arm costs one label copy per per-player record rather than a walk per name; AA's `pchar`
    // (`gamevar pchar 0 131073`, NORESET|PERPLAYER) is matched off the same copy.
    if (f & EK_GAMEVAR_PERPLAYER) {
      w->perPlayer++;
      char lbuf[EK_MAXVARLABEL + 2];
      if (label_copy(lbl, lbuf, (int)sizeof(lbuf))) {
        int weapon = -1;
        const int field = ekm_weapon_var_parse(lbuf, &weapon);
        // Every index bounded at the use, not only at the parse: the parse refuses a weapon outside the space, and
        // this is what a later reader cites when tempted to skip a bound that matters.
        if (field >= 0 && field < EK_WD_FIELD_COUNT &&
            weapon >= 0 && weapon < EK_WD_MAX_WEAPONS &&
            w->w[weapon][field] < 0) {
          w->w[weapon][field] = i;
          w->resolved++;
        }
        if (w->pchar < 0 && strcmp(lbuf, "pchar") == 0) w->pchar = i;
        // A row in the list, not a second walk: `gamevar gotshock 0 131073` (NORESET|PERPLAYER), whose bit 2 is AA's
        // "executioner is on top (not chaingun)".
        if (w->gotshock < 0 && strcmp(lbuf, "gotshock") == 0) w->gotshock = i;
        if (w->aimbotOn < 0 && strcmp(lbuf, "aimbot_on") == 0) w->aimbotOn = i;
        for (int k = 0; k < EK_GV_D12_COUNT; k++)
          if (w->d12[k] < 0 && strcmp(lbuf, kD12[k]) == 0) w->d12[k] = i;
        for (int k = 0; k < EK_GV_RC_COUNT; k++)
          if (w->rc[k] < 0 && strcmp(lbuf, kReloadCtr[k]) == 0) {
            w->rc[k] = i; w->rcResolved++;
          }
        for (int k = 0; k < EK_GV_RV_COUNT; k++)
          if (w->rv[k] < 0 && strcmp(lbuf, kRecoil[k]) == 0) {
            w->rv[k] = i; w->rvResolved++;
          }
        for (int k = 0; k < EK_GV_Z_COUNT; k++)
          if (kZoomWant[k] == (uintptr_t)EK_GAMEVAR_PERPLAYER && w->z[k] < 0 &&
              strcmp(lbuf, kZoom[k]) == 0) { w->z[k] = i; w->zResolved++; }
        for (int k = 0; k < EK_GV_SG_COUNT; k++)
          if (kShotGuardWant[k] == (uintptr_t)EK_GAMEVAR_PERPLAYER && w->sg[k] < 0 &&
              strcmp(lbuf, kShotGuard[k]) == 0) { w->sg[k] = i; w->sgResolved++; }
        for (int k = 0; k < EK_GV_TK_COUNT; k++)
          if (kTakeWant[k] == (uintptr_t)EK_GAMEVAR_PERPLAYER && w->tk[k] < 0 &&
              strcmp(lbuf, kTake[k]) == 0) { w->tk[k] = i; w->tkResolved++; }
      }
    }
    // The global arm asks the same predicate the read will: a record whose +0 is a pointer (COOP and its INT32PTR
    // siblings) is not a global this file will read a value from, so the walk must not name it either.
    if (ekm_gv_flags_accept(f, EK_GV_WANT_GLOBAL)) {
      char lbuf[EK_MAXVARLABEL + 2];
      if (label_copy(lbl, lbuf, (int)sizeof(lbuf))) {
        for (int k = 0; k < EK_GV_Z_COUNT; k++)
          if (kZoomWant[k] == EK_GV_WANT_GLOBAL && w->z[k] < 0 &&
              strcmp(lbuf, kZoom[k]) == 0) { w->z[k] = i; w->zResolved++; }
        for (int k = 0; k < EK_GV_SG_COUNT; k++)
          if (kShotGuardWant[k] == EK_GV_WANT_GLOBAL && w->sg[k] < 0 &&
              strcmp(lbuf, kShotGuard[k]) == 0) { w->sg[k] = i; w->sgResolved++; }
        for (int k = 0; k < EK_GV_TK_COUNT; k++)
          if (kTakeWant[k] == EK_GV_WANT_GLOBAL && w->tk[k] < 0 &&
              strcmp(lbuf, kTake[k]) == 0) { w->tk[k] = i; w->tkResolved++; }
        for (int k = 0; k < EK_GV_CO_COUNT; k++)
          if (w->co[k] < 0 && strcmp(lbuf, kCollect[k]) == 0) {
            w->co[k] = i; w->coResolved++;
          }
      }
    }
    // The pointer-typed arm asks the same predicate its read will: a name matched here but read through
    // gv_global_read would return the ADDRESS of the engine's global as its value.
    if (ekm_gv_flags_accept(f, EK_GV_WANT_INT32PTR)) {
      char lbuf[EK_MAXVARLABEL + 2];
      if (label_copy(lbl, lbuf, (int)sizeof(lbuf)))
        for (int k = 0; k < EK_GV_E_COUNT; k++)
          if (w->e[k] < 0 && strcmp(lbuf, kEngine[k]) == 0) {
            w->e[k] = i; w->eResolved++;
          }
    }
    if (w->coop < 0 && label_is(lbl, "COOP")) w->coop = i;
    if (w->multimode < 0 && label_is(lbl, "MULTIMODE")) w->multimode = i;
  }
}

// Deliberately a containment window and not a byte-exact offsetof: user_defs may gain a field in any build
// (udstate.h), and an exact check would reintroduce the size assumption ud_base avoids.
static const long long EK_GV_UD_WINDOW = 8192;

enum { EK_GV_XC_SKIPPED = 0, EK_GV_XC_PASSED = 1, EK_GV_XC_FAILED = 2 };

// A value slot that cannot be mistaken for a value: a failed read used to print 0 beside a real one. _TRUNCATE, never
// sprintf_s, which aborts the process over a log line.
static void value_text(bool got, int v, char* out, size_t bytes) {
  if (got) _snprintf_s(out, bytes, _TRUNCATE, "%d", v);
  else     _snprintf_s(out, bytes, _TRUNCATE, "unread");
}

static int crosscheck(const unsigned char* base, const GvWalk& w) {
  const unsigned char* const ud = ud_base();
  if (!ud) {
    // A skipped cross-check is not a passed one, and the line says so: the locate then stands on the nineteen labels
    // alone.
    ek_log("gamevars: &ud cross-check SKIPPED -- ud_base() is NULL, so COOP and "
           "MULTIMODE cannot be checked against it; this is not a pass");
    return EK_GV_XC_SKIPPED;
  }
  if (w.coop < 0 || w.multimode < 0) {
    ek_log("gamevars: &ud cross-check FAILED -- COOP=%d MULTIMODE=%d, and both "
           "are Gv_AddSystemVars' own (gamevars.cpp:1783, :1790)",
           w.coop, w.multimode);
    return EK_GV_XC_FAILED;
  }

  const unsigned char* const cRec = base + (size_t)w.coop * EK_GV_STRIDE;
  const unsigned char* const mRec = base + (size_t)w.multimode * EK_GV_STRIDE;
  const uintptr_t cF = (uintptr_t)gv_word(cRec, EK_GV_FLAGS);
  const uintptr_t mF = (uintptr_t)gv_word(mRec, EK_GV_FLAGS);
  const void* const cP = (const void*)gv_word(cRec, EK_GV_GLOBAL);
  const void* const mP = (const void*)gv_word(mRec, EK_GV_GLOBAL);
  const long long cOff = (long long)((intptr_t)cP - (intptr_t)ud);
  const long long mOff = (long long)((intptr_t)mP - (intptr_t)ud);

  const bool typed = (cF & EK_GAMEVAR_INT32PTR) != 0 &&
                     (mF & EK_GAMEVAR_INT32PTR) != 0;
  const bool inWindow = cOff >= 0 && cOff < EK_GV_UD_WINDOW &&
                        mOff >= 0 && mOff < EK_GV_UD_WINDOW;
  const bool got = inWindow && readable(cP, sizeof(int)) && readable(mP, sizeof(int));
  const int cv = got ? *(const int*)cP : 0;
  const int mv = got ? *(const int*)mP : 0;
  const bool sane = got && cv >= 0 && cv <= 2 && mv >= 1 && mv <= 16;
  const bool pass = typed && sane;

  char cText[24], mText[24];
  value_text(got, cv, cText, sizeof(cText));
  value_text(got, mv, mText, sizeof(mText));

  ek_log("gamevars: &ud cross-check COOP[%d] = &ud+%lld = %s, MULTIMODE[%d] = "
         "&ud+%lld = %s, delta %lld bytes, INT32PTR %d/%d, window %d, read %d -- %s",
         w.coop, cOff, cText, w.multimode, mOff, mText, mOff - cOff,
         (cF & EK_GAMEVAR_INT32PTR) ? 1 : 0, (mF & EK_GAMEVAR_INT32PTR) ? 1 : 0,
         inWindow ? 1 : 0, got ? 1 : 0, pass ? "PASS" : "FAIL");
  return pass ? EK_GV_XC_PASSED : EK_GV_XC_FAILED;
}

// Charged on the expensive walk only, as in badguy.cpp: aGameVars is filled while the CON is parsed, so an early
// attempt can legitimately miss; after the budget the image will not start containing it.
static bool give_up(const char* why) {
  if (++s_attempts >= EK_TILE_SCAN_BUDGET) {
    s_budgetSpent = true;
    ek_log("gamevars: giving up on aGameVars after %d attempts (%s); friend/foe "
           "is unavailable and nothing will retry", s_attempts, why);
  }
  return false;
}

bool gamevars_scan(void) {
  if (s_vars) return true;
  if (s_budgetSpent) return false;

  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  const IMAGE_DOS_HEADER* const dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const IMAGE_NT_HEADERS* const nt =
      (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  const IMAGE_SECTION_HEADER* const sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = (int)nt->FileHeader.NumberOfSections;

  GvSweep s;
  s.hit = NULL; s.rva = 0; s.secondRva = 0;
  s.shape = 0; s.verified = 0; s.faulted = 0;

  for (int i = 0; i < nsec; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
    // VirtualSize, not SizeOfRawData: aGameVars is a 64 KB static in.bss with raw size zero, whose bytes exist once
    // the loader commits them. The wrong extent is what kept Polymer's drawingstate refused for a whole branch.
    sweep_section(base + sec[i].VirtualAddress, sec[i].VirtualAddress,
                  sec[i].Misc.VirtualSize, &s);
  }

  if (s.faulted) {
    ek_log("gamevars: a writable section faulted part-way; %d shape candidate(s) "
           "and %d verified so far are NOT a complete count, so this pass refuses",
           s.shape, s.verified);
    return give_up("a faulted sweep");
  }
  if (!s.hit) {
    ek_log("gamevars: aGameVars not found -- %d shape candidate(s), 0 passing the "
           "19-label check; friend/foe unavailable and every actor stays hostile",
           s.shape);
    return give_up("never found");
  }
  if (s.verified > 1) {
    // Ambiguity is reported, not resolved: two arrays passing an ordered nineteen-label check means the signature
    // identifies nothing, and picking either would hand consumers a team read nobody verified.
    ek_log("gamevars: AMBIGUOUS -- %d candidates pass the 19-label check "
           "(rva 0x%07X and 0x%07X); refusing to guess",
           s.verified, s.rva, s.secondRva);
    return give_up("ambiguous");
  }

  GvWalk w;
  walk_labels(s.hit, &w);

  const int xc = crosscheck(s.hit, w);
  if (xc == EK_GV_XC_FAILED) {
    ek_log("gamevars: rva 0x%07X passed 19 labels but FAILED the &ud "
           "cross-check; the cross-check is the evidence, so this is refused",
           s.rva);
    return give_up("a failed cross-check");
  }

  ek_log("gamevars: aGameVars at rva 0x%07X -- %d shape candidate(s), %d passing "
         "the 19-label check at stride %d", s.rva, s.shape, s.verified,
         EK_GV_STRIDE);

  // A -1 here is not a failure: stock Duke and World Tour declare none of these vars (no friendly actors, damaged
  // gunships or rolled levels), and every consumer keeps today's behaviour on -1.
  const int kPaRowMax = 64;
  char paText[EK_GV_PA_COUNT * kPaRowMax];
  paText[0] = 0;
  for (int k = 0; k < EK_GV_PA_COUNT; k++) {
    char one[kPaRowMax];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s'%s' at index %d",
                k ? ", " : "", kPerActor[k], w.pa[k]);
    strcat_s(paText, sizeof(paText), one);
  }
  ek_log("gamevars: %d entr(ies) walked; per-actor %s -- -1 means this CON "
         "declares no such var, which is correct for stock Duke and leaves "
         "every consumer's behaviour unchanged", w.walked, paText);

  // The count beside the enumeration on one line: per-player records seen and how many MAX_WEAPONS x
  // EK_WD_FIELD_COUNT WEAPONx_* slots resolved (130 today), printed so the two cannot drift.
  ek_log("gamevars: %d per-player entr(ies); %d of %d WEAPONx_* field slots "
         "resolved (%d weapons x %d fields), 'pchar' at index %d, 'gotshock' "
         "at index %d, 'aimbot_on' at index %d -- -1 means this CON declares "
         "no such var", w.perPlayer, w.resolved,
         EK_WD_MAX_WEAPONS * EK_WD_FIELD_COUNT, EK_WD_MAX_WEAPONS,
         EK_WD_FIELD_COUNT, w.pchar, w.gotshock, w.aimbotOn);

  char d12Text[384];
  d12Text[0] = 0;
  for (int k = 0; k < EK_GV_D12_COUNT; k++) {
    char one[48];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s'%s' at %d", k ? ", " : "",
                kD12[k], w.d12[k]);
    strcat_s(d12Text, sizeof(d12Text), one);
  }
  ek_log("gamevars: D12b clip vars, D15's gotDB, D32's DB barrel and AA32's item-take pair (%d) -- %s -- "
         "-1 means this CON declares no such var", (int)EK_GV_D12_COUNT, d12Text);

  char rcText[128];
  rcText[0] = 0;
  for (int k = 0; k < EK_GV_RC_COUNT; k++) {
    char one[48];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s'%s' at %d", k ? ", " : "",
                kReloadCtr[k], w.rc[k]);
    strcat_s(rcText, sizeof(rcText), one);
  }
  ek_log("gamevars: AA reload counters %d of %d resolved -- %s -- -1 means this "
         "CON declares no such var", w.rcResolved, EK_GV_RC_COUNT, rcText);
  ek_log("gamevars: AA aim-kick vars %d of %d resolved -- '%s' at %d, '%s' at %d "
         "-- -1 means this CON declares no such var", w.rvResolved, EK_GV_RV_COUNT,
         kRecoil[EK_GV_RV_HORIZFALL], w.rv[EK_GV_RV_HORIZFALL],
         kRecoil[EK_GV_RV_SAVEDHORIZ], w.rv[EK_GV_RV_SAVEDHORIZ]);

  // The five zoom vars, count beside enumeration, built by a loop so a sixth row cannot be added without reaching the
  // log. The shape is printed per index (it decides which read serves it); a 0/5 on AA would mean the zoom trace sees
  // nothing.
  char zText[256];
  zText[0] = 0;
  for (int k = 0; k < EK_GV_Z_COUNT; k++) {
    char one[64];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s'%s' at %d (%s)", k ? ", " : "",
                kZoom[k], w.z[k],
                kZoomWant[k] == EK_GV_WANT_GLOBAL ? "global" : "per-player");
    strcat_s(zText, sizeof(zText), one);
  }
  ek_log("gamevars: AA zoom vars %d of %d resolved -- %s; -1 means this CON "
         "declares no such var, which is correct for stock Duke and leaves the "
         "projection reading fov alone", w.zResolved, EK_GV_Z_COUNT, zText);

  // The engine's own pair, count beside enumeration: a -1 here means something different from above, since these two
  // are Gv_AddSystemVars' own and exist in every EDuke32, so -1 is a locate or shape gone wrong.
  char eText[192];
  eText[0] = 0;
  for (int k = 0; k < EK_GV_E_COUNT; k++) {
    char one[64];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s'%s' at %d", k ? ", " : "",
                kEngine[k], w.e[k]);
    strcat_s(eText, sizeof(eText), one);
  }
  ek_log("gamevars: engine INT32PTR vars %d of %d resolved -- %s; these are the "
         "engine's own system vars, so a -1 is a locate or a shape problem "
         "rather than a CON that lacks them", w.eResolved, EK_GV_E_COUNT, eText);

  char sgText[512];
  sgText[0] = 0;
  for (int k = 0; k < EK_GV_SG_COUNT; k++) {
    char one[64];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s'%s' at %d", k ? ", " : "",
                kShotGuard[k], w.sg[k]);
    strcat_s(sgText, sizeof(sgText), one);
  }
  ek_log("gamevars: screenshot-mode HUD guards %d of %d resolved -- %s; a -1 "
         "means that event is never skipped", w.sgResolved, EK_GV_SG_COUNT, sgText);

  char coText[256];
  coText[0] = 0;
  for (int k = 0; k < EK_GV_CO_COUNT; k++) {
    char one[48];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s'%s' at %d", k ? ", " : "",
                kCollect[k], w.co[k]);
    strcat_s(coText, sizeof(coText), one);
  }
  ek_log("gamevars: AA25/AA30 collectible vars %d of %d resolved -- %s; a -1 on "
         "either collectibles means the figurine counter never shows; a -1 on "
         "startguns_perma or mcguffins marks every chest of that kind and hides "
         "the chest counter; collectinlevel/collectfound are only logged", w.coResolved,
         EK_GV_CO_COUNT, coText);

  char tkText[EK_GV_TK_COUNT * 48 + 1];
  tkText[0] = 0;
  for (int k = 0; k < EK_GV_TK_COUNT; k++) {
    char one[48];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s'%s' at %d", k ? ", " : "",
                kTake[k], w.tk[k]);
    strcat_s(tkText, sizeof(tkText), one);
  }
  ek_log("gamevars: AA32b/AA32c item-take vars %d of %d resolved -- %s; a -1 "
         "shows every ammo box or weapon whose rule reads it", w.tkResolved,
         EK_GV_TK_COUNT, tkText);

  InterlockedExchange(&s_rva, (LONG)s.rva);
  InterlockedExchange(&s_walked, (LONG)w.walked);
  for (int k = 0; k < EK_GV_PA_COUNT; k++)
    InterlockedExchange(&s_paIdx[k], (LONG)w.pa[k]);
  // Published before s_vars, like s_paIdx: every read refuses while s_vars is NULL, so no caller sees a half-filled
  // index table.
  for (int a = 0; a < EK_WD_MAX_WEAPONS; a++)
    for (int b = 0; b < EK_WD_FIELD_COUNT; b++)
      InterlockedExchange(&s_wIdx[a][b], (LONG)w.w[a][b]);
  InterlockedExchange(&s_pcharIdx, (LONG)w.pchar);
  InterlockedExchange(&s_gotshockIdx, (LONG)w.gotshock);
  InterlockedExchange(&s_aimbotOnIdx, (LONG)w.aimbotOn);
  for (int k = 0; k < EK_GV_D12_COUNT; k++)
    InterlockedExchange(&s_d12Idx[k], (LONG)w.d12[k]);
  for (int k = 0; k < EK_GV_RC_COUNT; k++)
    InterlockedExchange(&s_rcIdx[k], (LONG)w.rc[k]);
  for (int k = 0; k < EK_GV_RV_COUNT; k++)
    InterlockedExchange(&s_rvIdx[k], (LONG)w.rv[k]);
  for (int k = 0; k < EK_GV_SG_COUNT; k++)
    InterlockedExchange(&s_sgIdx[k], (LONG)w.sg[k]);
  for (int k = 0; k < EK_GV_TK_COUNT; k++)
    InterlockedExchange(&s_tkIdx[k], (LONG)w.tk[k]);
  InterlockedExchange(&s_wResolved, (LONG)w.resolved);
  for (int k = 0; k < EK_GV_Z_COUNT; k++)
    InterlockedExchange(&s_zIdx[k], (LONG)w.z[k]);
  InterlockedExchange(&s_zResolved, (LONG)w.zResolved);
  for (int k = 0; k < EK_GV_E_COUNT; k++)
    InterlockedExchange(&s_eIdx[k], (LONG)w.e[k]);
  InterlockedExchange(&s_eResolved, (LONG)w.eResolved);
  for (int k = 0; k < EK_GV_CO_COUNT; k++)
    InterlockedExchange(&s_coIdx[k], (LONG)w.co[k]);
  InterlockedExchange(&s_coResolved, (LONG)w.coResolved);
  s_vars = s.hit;
  return true;
}

bool gamevars_ready(void) { return s_vars != NULL; }
unsigned gamevars_rva(void) {
  return (unsigned)InterlockedCompareExchange(&s_rva, 0, 0);
}
int gamevars_walked(void) {
  return (int)InterlockedCompareExchange(&s_walked, 0, 0);
}
int gamevars_vq_calls(void) {
  return (int)InterlockedCompareExchange(&s_vqCalls, 0, 0);
}
int gamevars_team_index(void) {
  return (int)InterlockedCompareExchange(&s_paIdx[EK_GV_PA_TEAM], -1, -1);
}

// The guards all three reads share: a published array, a bounded index, a readable record, and flags that still say
// what the cached index was resolved for. The cached index is not a cached variable.
static const unsigned char* gv_record(int idx, uintptr_t want) {
  const unsigned char* const v = s_vars;
  if (!v || idx < 0 || idx >= EK_MAXGAMEVARS) return NULL;
  const unsigned char* const rec = v + (size_t)idx * EK_GV_STRIDE;
  if (!readable(rec, EK_GV_STRIDE)) return NULL;
  if (!ekm_gv_flags_accept((uintptr_t)gv_word(rec, EK_GV_FLAGS), want))
    return NULL;
  return rec;
}

// Both kinds of indexed var go through here: the record is bounds-checked by gv_record, and pValues here because it
// is a heap allocation the engine frees and remakes.
static bool gv_indexed_read(int idx, unsigned flagWant, int slot,
                            volatile LONG* said, const char* name, int* out) {
  if (!out) return false;
  const unsigned char* const rec = gv_record(idx, (uintptr_t)flagWant);
  if (!rec) return false;
  // pValues is a heap allocation, not image: 128 KB for a per-actor var, MAXPLAYERS pointers for a per-player one, so
  // it is bounds-checked at read time like every engine pointer here.
  const intptr_t* const pv = (const intptr_t*)gv_word(rec, EK_GV_GLOBAL);
  if (!readable(pv + slot, sizeof(intptr_t))) {
    // Said once per var but not given up on, because the condition is transient: Gv_Free frees and NULLs pValues
    // during a CON re-parse and Gv_NewVar reallocates it a moment later.
    if (said && InterlockedCompareExchange(said, 1, 0) == 0)
      ek_log("gamevars: '%s' pValues at %p will not read; every caller takes "
             "today's no-answer behaviour for now and this is not logged "
             "again. The read retries every call and resumes on its own -- a "
             "CON re-parse frees this array and reallocates it",
             name ? name : "?", (const void*)pv);
    return false;
  }
  *out = (int)pv[slot];
  return true;
}

// The third read: for a global the union at +0 is the value, not a pointer to an array of them, so nothing to index
// or bound.
static bool gv_global_read(int idx, int* out) {
  if (!out) return false;
  const unsigned char* const rec = gv_record(idx, EK_GV_WANT_GLOBAL);
  if (!rec) return false;
  *out = (int)gv_word(rec, EK_GV_GLOBAL);
  return true;
}

// The fourth read: the third with the indirection put back. A SYSTEM var flagged GAMEVAR_INT32PTR holds &viewingrange
// at +0, the only read whose +0 is a pointer (what gv_global_read exists to refuse).
static bool gv_int32ptr_read(int idx, int* out) {
  if (!out) return false;
  const unsigned char* const rec = gv_record(idx, EK_GV_WANT_INT32PTR);
  if (!rec) return false;
  const int* const p = (const int*)gv_word(rec, EK_GV_GLOBAL);
  if (!readable(p, sizeof(int))) return false;
  *out = *p;
  return true;
}

static bool gv_peractor_read(int which, int sprite, int* out) {
  // Defence against a future caller, not a live hazard: every caller passes an enum constant.
  if (which < 0 || which >= EK_GV_PA_COUNT) return false;
  if (sprite < 0 || sprite >= EK_MAXSPRITES) return false;
  const int idx = (int)InterlockedCompareExchange(&s_paIdx[which], -1, -1);
  return gv_indexed_read(idx, EK_GAMEVAR_PERACTOR, sprite & (EK_MAXSPRITES - 1),
                         &s_paSaid[which], kPerActor[which], out);
}

// gv_peractor_read's checks taken once for a whole walk: the record and PERACTOR flag, then the full MAXSPRITES-entry
// array in one VirtualQuery. Same latch and message, since the failure is the same transient freed-by-Gv_Free state.
static bool gv_peractor_view(int which, EkGvView* out) {
  if (!out) return false;
  out->values = NULL;
  out->ok = false;
  if (which < 0 || which >= EK_GV_PA_COUNT) return false;
  const int idx = (int)InterlockedCompareExchange(&s_paIdx[which], -1, -1);
  const unsigned char* const rec = gv_record(idx, (uintptr_t)EK_GAMEVAR_PERACTOR);
  if (!rec) return false;
  const intptr_t* const pv = (const intptr_t*)gv_word(rec, EK_GV_GLOBAL);
  if (!readable(pv, (size_t)EK_MAXSPRITES * sizeof(intptr_t))) {
    if (InterlockedCompareExchange(&s_paSaid[which], 1, 0) == 0)
      ek_log("gamevars: '%s' pValues at %p will not read; every caller takes "
             "today's no-answer behaviour for now and this is not logged "
             "again. The read retries every call and resumes on its own -- a "
             "CON re-parse frees this array and reallocates it",
             kPerActor[which], (const void*)pv);
    return false;
  }
  out->values = pv;
  out->ok = true;
  return true;
}

// Per-player wrapper: bounded, not masked. The engine does mask (`pValues[playerNum & (MAXPLAYERS-1)]`); this caller
// is a diagnostic.
static bool gv_perplayer_read(int idx, int playerNum, volatile LONG* said,
                              const char* name, int* out) {
  if (playerNum < 0 || playerNum >= EK_MAXPLAYERS) return false;
  return gv_indexed_read(idx, EK_GAMEVAR_PERPLAYER, playerNum, said, name, out);
}

bool gamevars_weapon_field(int weapon, int field, int playerNum, int* out) {
  if (weapon < 0 || weapon >= EK_WD_MAX_WEAPONS) return false;
  if (field < 0 || field >= EK_WD_FIELD_COUNT) return false;
  const int idx = (int)InterlockedCompareExchange(&s_wIdx[weapon][field], -1, -1);
  char name[32];
  _snprintf_s(name, sizeof(name), _TRUNCATE, "WEAPON%d_%s", weapon,
              ekm_weapon_field_name(field));
  return gv_perplayer_read(idx, playerNum, &s_wSaid[weapon], name, out);
}

// Restored for the TotalTime spike: readable plus a second VirtualQuery for write permission, and the record re-check
// before the store, because a CON re-parse can reassign the slot to a var of another shape between calls.
static bool writable(const void* p, size_t bytes) {
  if (!readable(p, bytes)) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  const DWORD w = PAGE_READWRITE | PAGE_WRITECOPY |
                  PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return (mbi.Protect & w) != 0;
}

bool gamevars_weapon_field_set(int weapon, int field, int playerNum, int value) {
  if (weapon < 0 || weapon >= EK_WD_MAX_WEAPONS) return false;
  if (field < 0 || field >= EK_WD_FIELD_COUNT) return false;
  if (playerNum < 0 || playerNum >= EK_MAXPLAYERS) return false;
  const int idx = (int)InterlockedCompareExchange(&s_wIdx[weapon][field], -1, -1);
  const unsigned char* const rec = gv_record(idx, (uintptr_t)EK_GAMEVAR_PERPLAYER);
  if (!rec) return false;
  intptr_t* const pv = (intptr_t*)gv_word(rec, EK_GV_GLOBAL);
  if (!writable(pv + playerNum, sizeof(intptr_t))) return false;
  pv[playerNum] = (intptr_t)value;
  return true;
}

bool gamevars_pchar(int playerNum, int* out) {
  const int idx = (int)InterlockedCompareExchange(&s_pcharIdx, -1, -1);
  return gv_perplayer_read(idx, playerNum, &s_pcharSaid, "pchar", out);
}

bool gamevars_gotshock_present(void) {
  if (!gamevars_ready()) return false;
  return (int)InterlockedCompareExchange(&s_gotshockIdx, -1, -1) >= 0;
}

bool gamevars_gotshock(int playerNum, int* out) {
  const int idx = (int)InterlockedCompareExchange(&s_gotshockIdx, -1, -1);
  return gv_perplayer_read(idx, playerNum, &s_gotshockSaid, "gotshock", out);
}

bool gamevars_aimbot_on(int playerNum, int* out) {
  const int idx = (int)InterlockedCompareExchange(&s_aimbotOnIdx, -1, -1);
  return gv_perplayer_read(idx, playerNum, &s_aimbotOnSaid, "aimbot_on", out);
}

int gamevars_weapon_resolved(void) {
  return (int)InterlockedCompareExchange(&s_wResolved, 0, 0);
}

static bool gv_d12_perplayer(int which, int playerNum, int* out) {
  if (which < 0 || which >= EK_GV_D12_COUNT) return false;
  const int idx = (int)InterlockedCompareExchange(&s_d12Idx[which], -1, -1);
  return gv_perplayer_read(idx, playerNum, &s_d12Said[which], kD12[which], out);
}

bool gamevars_thisclip(int playerNum, int* out) {
  return gv_d12_perplayer(EK_GV_D12_THISCLIP, playerNum, out);
}

bool gamevars_fastfire(int playerNum, int* out) {
  return gv_d12_perplayer(EK_GV_D12_FASTFIRE, playerNum, out);
}

bool gamevars_perks(int playerNum, int* out) {
  return gv_d12_perplayer(EK_GV_D12_PERKS, playerNum, out);
}

bool gamevars_burstfired(int playerNum, int* out) {
  return gv_d12_perplayer(EK_GV_D12_BURSTFIRED, playerNum, out);
}

bool gamevars_gotdb(int playerNum, int* out) {
  return gv_d12_perplayer(EK_GV_D12_GOTDB, playerNum, out);
}

bool gamevars_dbammo(int playerNum, int* out) {
  return gv_d12_perplayer(EK_GV_D12_DBAMMO, playerNum, out);
}

bool gamevars_rightdb_weapcount(int playerNum, int* out) {
  return gv_d12_perplayer(EK_GV_D12_RIGHTDB, playerNum, out);
}

bool gamevars_steroidstack(int playerNum, int* out) {
  return gv_d12_perplayer(EK_GV_D12_STEROIDSTACK, playerNum, out);
}

bool gamevars_inven_upgrades(int playerNum, int* out) {
  return gv_d12_perplayer(EK_GV_D12_INVENUPG, playerNum, out);
}

bool gamevars_reload_counter(int which, int playerNum, int* out) {
  if (which < 0 || which >= EK_GV_RC_COUNT) return false;
  const int idx = (int)InterlockedCompareExchange(&s_rcIdx[which], -1, -1);
  return gv_perplayer_read(idx, playerNum, &s_rcSaid[which], kReloadCtr[which], out);
}

// Resolves the element as the read does (flag re-check, player bound, pValues bounded), then write permission. The
// clips are refused: AA's refill owns them. Given up at the first failure; retrying a refused write every frame risks
// a crash.
bool gamevars_reload_counter_set(int which, int playerNum, int value) {
  if (which != EK_GV_RC_M4RELOAD && which != EK_GV_RC_UZIRELOAD) return false;
  if (InterlockedCompareExchange(&s_rcWriteDead, 0, 0)) return false;
  if (playerNum < 0 || playerNum >= EK_MAXPLAYERS) return false;
  const int idx = (int)InterlockedCompareExchange(&s_rcIdx[which], -1, -1);
  const unsigned char* const rec = gv_record(idx, (uintptr_t)EK_GAMEVAR_PERPLAYER);
  intptr_t* const pv = rec ? (intptr_t*)gv_word(rec, EK_GV_GLOBAL) : NULL;
  if (!pv || !writable(pv + playerNum, sizeof(intptr_t))) {
    if (InterlockedExchange(&s_rcWriteDead, 1) == 0)
      ek_log("gamevars: '%s' will not take a write (record %s, pValues %p); the "
             "reload counter advance stops for this session and is not retried",
             kReloadCtr[which], rec ? "ok" : "refused", (const void*)pv);
    return false;
  }
  pv[playerNum] = (intptr_t)value;
  return true;
}

bool gamevars_recoil(int which, int playerNum, int* out) {
  if (which < 0 || which >= EK_GV_RV_COUNT) return false;
  const int idx = (int)InterlockedCompareExchange(&s_rvIdx[which], -1, -1);
  return gv_perplayer_read(idx, playerNum, &s_rvSaid[which], kRecoil[which], out);
}

bool gamevars_recoil_present(void) {
  const bool ready = gamevars_ready();
  return ekm_recoil_vars_present(
      ready, (int)InterlockedCompareExchange(&s_rvIdx[EK_GV_RV_HORIZFALL], -1, -1),
      (int)InterlockedCompareExchange(&s_rvIdx[EK_GV_RV_SAVEDHORIZ], -1, -1));
}

// gamevars_reload_counter_set's guards and give-up latch for the two aim-kick vars; AA's own recovery writes both.
bool gamevars_recoil_set(int which, int playerNum, int value) {
  if (which != EK_GV_RV_HORIZFALL && which != EK_GV_RV_SAVEDHORIZ) return false;
  if (InterlockedCompareExchange(&s_rvWriteDead, 0, 0)) return false;
  if (playerNum < 0 || playerNum >= EK_MAXPLAYERS) return false;
  const int idx = (int)InterlockedCompareExchange(&s_rvIdx[which], -1, -1);
  const unsigned char* const rec = gv_record(idx, (uintptr_t)EK_GAMEVAR_PERPLAYER);
  intptr_t* const pv = rec ? (intptr_t*)gv_word(rec, EK_GV_GLOBAL) : NULL;
  if (!pv || !writable(pv + playerNum, sizeof(intptr_t))) {
    if (InterlockedExchange(&s_rvWriteDead, 1) == 0)
      ek_log("gamevars: '%s' will not take a write (record %s, pValues %p); no "
             "recoil stops for this session and is not retried",
             kRecoil[which], rec ? "ok" : "refused", (const void*)pv);
    return false;
  }
  pv[playerNum] = (intptr_t)value;
  return true;
}

bool gamevars_shot_guard(int which, int playerNum, int* out) {
  if (which < 0 || which >= EK_GV_SG_COUNT) return false;
  const int idx = (int)InterlockedCompareExchange(&s_sgIdx[which], -1, -1);
  if (kShotGuardWant[which] == EK_GV_WANT_GLOBAL) return gv_global_read(idx, out);
  return gv_perplayer_read(idx, playerNum, &s_sgSaid[which], kShotGuard[which], out);
}

bool gamevars_take(int which, int playerNum, int* out) {
  if (which < 0 || which >= EK_GV_TK_COUNT) return false;
  const int idx = (int)InterlockedCompareExchange(&s_tkIdx[which], -1, -1);
  if (kTakeWant[which] == EK_GV_WANT_GLOBAL) return gv_global_read(idx, out);
  return gv_perplayer_read(idx, playerNum, &s_tkSaid[which], kTake[which], out);
}

// Each row goes to the read its own `want` column names, so a wrong pairing is a visible mismatch in this one place
// rather than a dereferenced integer at runtime.
static bool gv_zoom_perplayer(int which, int playerNum, int* out) {
  if (which < 0 || which >= EK_GV_Z_COUNT) return false;
  const int idx = (int)InterlockedCompareExchange(&s_zIdx[which], -1, -1);
  return gv_perplayer_read(idx, playerNum, &s_zSaid[which], kZoom[which], out);
}

static bool gv_zoom_global(int which, int* out) {
  if (which < 0 || which >= EK_GV_Z_COUNT) return false;
  return gv_global_read((int)InterlockedCompareExchange(&s_zIdx[which], -1, -1),
                        out);
}

bool gamevars_zoomin(int playerNum, int* out) {
  return gv_zoom_perplayer(EK_GV_Z_ZOOMIN, playerNum, out);
}

bool gamevars_zoomlevel(int playerNum, int* out) {
  return gv_zoom_perplayer(EK_GV_Z_ZOOMLEVEL, playerNum, out);
}

bool gamevars_vr(int* out)   { return gv_zoom_global(EK_GV_Z_VR, out); }
bool gamevars_vrhi(int* out) { return gv_zoom_global(EK_GV_Z_VRHI, out); }
bool gamevars_vrlo(int* out) { return gv_zoom_global(EK_GV_Z_VRLO, out); }

int gamevars_zoom_resolved(void) {
  return (int)InterlockedCompareExchange(&s_zResolved, 0, 0);
}

// The engine's own pair; `which` bounds three arrays, for gv_peractor_read's reason.
static bool gv_engine_read(int which, int* out) {
  if (which < 0 || which >= EK_GV_E_COUNT) return false;
  return gv_int32ptr_read(
      (int)InterlockedCompareExchange(&s_eIdx[which], -1, -1), out);
}

bool gamevars_viewingrange(int* out) {
  return gv_engine_read(EK_GV_E_VIEWINGRANGE, out);
}

bool gamevars_yxaspect(int* out) {
  return gv_engine_read(EK_GV_E_YXASPECT, out);
}

int gamevars_engine_resolved(void) {
  return (int)InterlockedCompareExchange(&s_eResolved, 0, 0);
}

bool gamevars_collectibles(int which, int* out) {
  if (which < 0 || which >= EK_GV_CO_COUNT) return false;
  return gv_global_read(
      (int)InterlockedCompareExchange(&s_coIdx[which], -1, -1), out);
}

int gamevars_collect_resolved(void) {
  return (int)InterlockedCompareExchange(&s_coResolved, 0, 0);
}

bool gamevars_team(int sprite, int* outTeam) {
  return gv_peractor_read(EK_GV_PA_TEAM, sprite, outTeam);
}

bool gamevars_mtype(int sprite, int* outMtype) {
  return gv_peractor_read(EK_GV_PA_MTYPE, sprite, outMtype);
}

// The value is not judged here: AA's HUD treats inithp 0 as "no answer" (weapons_hud.CON), a display rule that
// belongs with the display (esp_maxhp.h). This layer answers only whether the var read.
bool gamevars_inithp(int sprite, int* outInitHp) {
  return gv_peractor_read(EK_GV_PA_INITHP, sprite, outInitHp);
}

bool gamevars_mlevel(int sprite, int* outLevel) {
  return gv_peractor_read(EK_GV_PA_MLEVEL, sprite, outLevel);
}

// Not judged here, for inithp's reason: 0 is the CON's "drops nothing" and the two cash picnums are a display rule
// (esp_cash_aa.h).
bool gamevars_droptile(int sprite, int* outDropTile) {
  return gv_peractor_read(EK_GV_PA_DROPTILE, sprite, outDropTile);
}

bool gamevars_monstatus(int sprite, int* outMonStatus) {
  return gv_peractor_read(EK_GV_PA_MONSTATUS, sprite, outMonStatus);
}

// `bottarget`, read at the player's own sprite: `sprite` is whatever the caller passes (aim.cpp owns the "player's
// sprite" rule). -1 is AA's sentinel and is not judged here.
bool gamevars_bottarget(int sprite, int* outTarget) {
  return gv_peractor_read(EK_GV_PA_BOTTARGET, sprite, outTarget);
}

bool gamevars_countvarc(int sprite, int* outCountvarc) {
  return gv_peractor_read(EK_GV_PA_COUNTVARC, sprite, outCountvarc);
}

bool gamevars_team_view(EkGvView* out) {
  return gv_peractor_view(EK_GV_PA_TEAM, out);
}

bool gamevars_inithp_view(EkGvView* out) {
  return gv_peractor_view(EK_GV_PA_INITHP, out);
}

bool gamevars_mlevel_view(EkGvView* out) {
  return gv_peractor_view(EK_GV_PA_MLEVEL, out);
}

bool gamevars_monstatus_view(EkGvView* out) {
  return gv_peractor_view(EK_GV_PA_MONSTATUS, out);
}

bool gamevars_countvarc_view(EkGvView* out) {
  return gv_peractor_view(EK_GV_PA_COUNTVARC, out);
}

bool gamevars_mtype_view(EkGvView* out) {
  return gv_peractor_view(EK_GV_PA_MTYPE, out);
}

bool gamevars_countvar(int sprite, int* outCountvar) {
  return gv_peractor_read(EK_GV_PA_COUNTVAR, sprite, outCountvar);
}

bool gamevars_dashtime(int sprite, int* outDashtime) {
  return gv_peractor_read(EK_GV_PA_DASHTIME, sprite, outDashtime);
}

bool gamevars_countvar_view(EkGvView* out) {
  return gv_peractor_view(EK_GV_PA_COUNTVAR, out);
}

bool gamevars_dashtime_view(EkGvView* out) {
  return gv_peractor_view(EK_GV_PA_DASHTIME, out);
}

bool gamevars_myspawner(int sprite, int* outSpawner) {
  return gv_peractor_read(EK_GV_PA_MYSPAWNER, sprite, outSpawner);
}

bool gamevars_initflags(int sprite, int* outInitflags) {
  return gv_peractor_read(EK_GV_PA_INITFLAGS, sprite, outInitflags);
}

bool gamevars_initflags_view(EkGvView* out) {
  return gv_peractor_view(EK_GV_PA_INITFLAGS, out);
}

bool gamevars_monstflags(int sprite, int* outMonstflags) {
  return gv_peractor_read(EK_GV_PA_MONSTFLAGS, sprite, outMonstflags);
}

bool gamevars_monstflags_view(EkGvView* out) {
  return gv_peractor_view(EK_GV_PA_MONSTFLAGS, out);
}
