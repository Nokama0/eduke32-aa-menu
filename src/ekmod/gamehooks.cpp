// Locates P_PreFireHitscan, A_ShootWithZvel, A_IncurDamage and A_InsertSprite by signature scan (no COFF symbols) and
// hooks the first two with pass-through detours. Windows sit deep in each body, unique across .text, entries resolved
// via the exception directory; any ambiguity means no hook. Evidence and rvas: docs/re/gamehooks.md.

#include <windows.h>
#include <stdio.h>
#include "log.h"
#include "hook.h"
#include "gamehooks.h"
#include "config.h"
#include "tracer.h"
#include "playerstate.h"
#include "aim.h"
#include "aimsolve.h"
#include "shotray.h"
#include "tbot.h"
#include "wallclock.h"
#include "imagescan.h"
#include "dmgnum.h"
#include "gamestate.h"

// Windows x64 has one calling convention, but EVERY ARGUMENT MUST BE RE-PASSED: both targets read arguments five to
// eight off the stack relative to their own entry rsp. vec3_t stays an opaque void*.
typedef void (*PreFireFn)(int spriteNum, int playerNum, int projecTile, void* srcVect,
                          int* zvel, int* shootAng, int accurateAim, int doSpread);
typedef int  (*ShootFn)(int spriteNum, int projecTile, int forceZvel);

// player.cpp:500-501, the hitscan() call (+512 / &2047 sintable pair, CLIPMASK1 0x1000040).
static const unsigned char EK_PREFIRE_SIG[] = {
  0x41, 0x89, 0xD0,
  0x81, 0xC2, 0x00, 0x02, 0x00, 0x00,
  0x48, 0x03, 0x1E,
  0x41, 0x81, 0xE0, 0xFF, 0x07, 0x00, 0x00,
  0x81, 0xE2, 0xFF, 0x07, 0x00, 0x00,
  0x46, 0x0F, 0xBF, 0x0C, 0x41,
  0x44, 0x0F, 0xBF, 0x04, 0x51,
  0x48, 0x8D, 0x4C, 0x24, 0x50,
  0x0F, 0xBF, 0x53, 0x18,
  0x48, 0x89, 0x4C, 0x24, 0x28,
  0x48, 0x89, 0xF9,
  0xC7, 0x44, 0x24, 0x30, 0x40, 0x00, 0x00, 0x01,
  0x89, 0x44, 0x24, 0x20
};

// player.cpp:1710-1714, the player arm (0x309 PCRACKTIME and 0x400 ZOFFSET6 as immediates).
static const unsigned char EK_SHOOT_SIG[] = {
  0x41, 0x0F, 0xBF, 0x96, 0xE8, 0x01, 0x00, 0x00,
  0x41, 0xB8, 0x09, 0x03, 0x00, 0x00,
  0x41, 0x8B, 0x46, 0x08,
  0x66, 0x45, 0x89, 0x86, 0x34, 0x02, 0x00, 0x00,
  0x45, 0x8B, 0x26,
  0x45, 0x8B, 0x6E, 0x04,
  0xF3, 0x41, 0x0F, 0x7E, 0x36,
  0x8D, 0xAC, 0x02, 0x00, 0x04, 0x00, 0x00
};

// actors.cpp:1327-1328, the actor arm's single exit (htextra 0x4C, htpicnum 0x48, stride 96). rsi is spriteNum, rdi is
// &actor[0]; bytes read from the image, as MSVC would spell it differently.
static const unsigned char EK_INCURDAMAGE_SIG[] = {
  0xBA, 0xFF, 0xFF, 0xFF, 0xFF,
  0x48, 0x8D, 0x04, 0x76,
  0x48, 0xC1, 0xE0, 0x05,
  0x66, 0x89, 0x54, 0x07, 0x4C,
  0x0F, 0xBF, 0x44, 0x07, 0x48
};

// actors.cpp:1319, the VM_OnEvent call: a different statement from the window above. Stops before the `call` (rel32
// moves).
static const unsigned char EK_INCURDAMAGE_SIG2[] = {
  0xB9, 0xA0, 0x00, 0x00, 0x00,
  0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00
};

// game.cpp:1377-1381, the new sprite's record and actor[].
static const unsigned char EK_INSERT_SIG[] = {
  0x66, 0x89, 0x78, 0x0E,
  0x0F, 0xB6, 0x7C, 0x24, 0x68,
  0x66, 0x44, 0x0F, 0xD6, 0x08,
  0x44, 0x89, 0x60, 0x08,
  0x66, 0x44, 0x89, 0x58, 0x0C,
  0x40, 0x88, 0x78, 0x10,
  0x0F, 0xB6, 0x7C, 0x24, 0x6C,
  0x66, 0x44, 0x0F, 0x7E, 0x50, 0x11,
  0x0F, 0x11, 0x70, 0x18,
  0xC7, 0x40, 0x28, 0x00, 0x00, 0x00, 0x00,
  0x40, 0x88, 0x78, 0x15,
  0x31, 0xFF,
  0x66, 0x89, 0x78, 0x16,
  0x4A, 0x8D, 0x3C, 0x02,
  0x31, 0xC0,
  0x41, 0x81, 0xFA, 0xFF, 0x3F, 0x00, 0x00,
  0xF3, 0x48, 0xAB,
  0x66, 0x46, 0x0F, 0xD6, 0x4C, 0x02, 0x2C,
  0x46, 0x89, 0x64, 0x02, 0x34
};

// game.cpp:1403-1407, EVENT_EGS: the corroboration.
static const unsigned char EK_INSERT_SIG2[] = {
  0x89, 0xF2,
  0xB9, 0x49, 0x00, 0x00, 0x00,
  0x44, 0x8B, 0x8C, 0x24, 0x8C, 0x00, 0x00, 0x00,
  0x41, 0x89, 0xC0,
  0x83, 0x03, 0x01
};

static EkHook s_prefireHook;
static EkHook s_shootHook;

// Interlocked like every cross-thread value here: the install writes once on the game thread, detours read.
static PreFireFn volatile s_prefireOrig = NULL;
static ShootFn   volatile s_shootOrig   = NULL;
static volatile LONG s_prefireRva = 0;
static volatile LONG s_shootRva   = 0;

// overlay.cpp reads this on the RENDER thread while the install writes it once on the game thread.
static volatile LONG s_incurRva = 0;

// The gate lives here, not in the caller (the overlay calls every frame). A refusal is permanent: the image does not
// change.
static bool s_attempted = false;

struct SigHit { unsigned rva; int count; int faulted; };

// Guarded: a section's VirtualSize can run past what is committed, and a scan must not crash the game.
static bool find_pattern(const unsigned char* base, const unsigned char* sig,
                         size_t len, SigHit* out) {
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = (int)nt->FileHeader.NumberOfSections;

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
        if (start[off] != sig[0]) continue;
        size_t k = 1;
        for (; k < len; k++) if (start[off + k] != sig[k]) break;
        if (k != len) continue;
        if (out->count == 0) out->rva = (unsigned)(sec[i].VirtualAddress + off);
        out->count++;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A section that faulted part-way was NOT searched: its partial count must not pass as complete, since a second
      // match could sit unlooked-at. The flag forces a refusal.
      out->faulted = 1;
    }
  }
  return true;
}

// Occurrences in [begin, end) to corroborate a locate inside its resolved function; a fault is reported, not folded in.
static void find_within(const unsigned char* base, unsigned begin, unsigned end,
                        const unsigned char* sig, size_t len, SigHit* out) {
  out->rva = 0;
  out->count = 0;
  out->faulted = 0;
  if (end <= begin || (size_t)(end - begin) < len) return;

  __try {
    const unsigned char* const p = base + begin;
    const size_t last = (size_t)(end - begin) - len;
    for (size_t off = 0; off <= last; off++) {
      if (p[off] != sig[0]) continue;
      size_t k = 1;
      for (; k < len; k++) if (p[off + k] != sig[k]) break;
      if (k != len) continue;
      if (out->count == 0) out->rva = (unsigned)(begin + off);
      out->count++;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->faulted = 1;
  }
}

// The function containing `rva`, from the exception directory; 0 means no RUNTIME_FUNCTION covers it, not something to
// patch.
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

// The entry rva of the function containing the one match, or 0 having logged the refused stage. `sig2` is a second fact
// from a different statement: unique is not correct, and a build that folded the body into another function would match
// once in the wrong place. NULL skips it (the two hooked targets rest on facts in docs/re/gamehooks.md).
static unsigned locate(const unsigned char* base, const char* what,
                       const unsigned char* sig, size_t len,
                       const unsigned char* sig2, size_t len2) {
  SigHit hit;
  if (!find_pattern(base, sig, len, &hit)) {
    ek_log("gamehooks: %s -- the host module has no PE headers; refusing", what);
    return 0;
  }
  if (hit.count == 0 && !hit.faulted) {
    ek_log("gamehooks: %s -- signature not found in this build; not hooking", what);
    return 0;
  }
  if (hit.count != 1 || hit.faulted) {
    if (hit.faulted)
      ek_log("gamehooks: %s -- a code section faulted mid-scan, so uniqueness "
             "cannot be claimed (%d match(es) before the fault); refusing",
             what, hit.count);
    else
      ek_log("gamehooks: %s -- signature is ambiguous, %d matches, first at rva "
             "0x%07X; refusing to guess", what, hit.count, hit.rva);
    return 0;
  }
  const unsigned entry = entry_containing(base, hit.rva);
  if (entry == 0 || entry > hit.rva) {
    ek_log("gamehooks: %s -- match at rva 0x%07X is not inside any function in "
           "the exception directory; refusing", what, hit.rva);
    return 0;
  }
  if (sig2 == NULL) {
    ek_log("gamehooks: %s -- signature matched once at rva 0x%07X, inside the "
           "function at rva 0x%07X (%+d)", what, hit.rva, entry,
           (int)(hit.rva - entry));
    return entry;
  }

  unsigned begin = 0, end = 0;
  if (!ek_image_extent_containing(base, entry, &begin, &end) || end <= begin) {
    ek_log("gamehooks: %s -- fact 1 matched once at rva 0x%07X but the function "
           "at rva 0x%07X has no extent to corroborate inside; refusing",
           what, hit.rva, entry);
    return 0;
  }
  SigHit second;
  find_within(base, begin, end, sig2, len2, &second);
  if (second.count == 0 || second.faulted) {
    ek_log("gamehooks: %s -- fact 1 matched once at rva 0x%07X inside the "
           "function at rva 0x%07X, but FACT 2 IS ABSENT from its %u bytes%s; "
           "refusing, because one window occurring once does not make it this "
           "function", what, hit.rva, entry, end - begin,
           second.faulted ? " (the range also faulted mid-scan)" : "");
    return 0;
  }
  ek_log("gamehooks: %s -- signature matched once at rva 0x%07X, inside the "
         "function at rva 0x%07X (%+d); corroborated by fact 2, %d time(s) in "
         "that function, first at rva 0x%07X (%+d)", what, hit.rva, entry,
         (int)(hit.rva - entry), second.count, second.rva,
         (int)(second.rva - entry));
  return entry;
}

// Pure delegation; every argument re-passed in position so the compiler rebuilds the frame the original reads. Perfect
// Accuracy: doSpread becomes 0 for a weapon whose row is on (keyed on curr_weapon); a failed read delegates untouched.
// Instrumentation reads the out-parameters before and after (GetAutoAimAng can move both).
static const unsigned EK_PREFIRE_LOG_MIN_MS = 250;
static unsigned s_prefireLogMs = 0;
static int s_prefireLogHave = 0;

// The staleness diff: the triggerbot validates its ray in the SwapBuffers hook but the weapon fires in the tic before,
// so every approved ray gates the NEXT tic's shot; this prints ONE diff of the two instants, calling no hook-restricted
// engine helper (range from the record, pitch from shotray.h). Own rate limit: a 45-round burst runs ~4 s.
static const unsigned EK_STALE_LOG_MIN_MS = 300;
static unsigned s_staleLogMs = 0;
static int s_staleLogHave = 0;

static void ek_log_shot_staleness(void) {
  TbotCastRecord rec;
  if (!tbot_cast_record(&rec)) {
    ek_log("stale: NO CAST ON RECORD -- nothing gated this shot. Either the "
           "trigger was the player's own finger or the triggerbot has not cast "
           "a ray yet this session");
    return;
  }

  // ekm_shot_fresh's own clock on both ends; unsigned, so it survives the wrap.
  const unsigned age = aim_clock_ms() - rec.ms;
  if (!ekm_shot_fresh(age)) {
    ek_log("stale: the cast on record is STALE -- age %u ms, past the port's "
           "%u ms ceiling. No delta printed: it would be a diff against a cast "
           "from before some gap, not against the shot's own frame",
           age, EKM_SHOT_STALE_MS);
    return;
  }

  PlayerSnap now;
  if (!player_snap(&now)) {
    ek_log("stale: a cast is on record (age %u ms) but the player struct did "
           "not read at fire time, so there is nothing to diff it against", age);
    return;
  }

  // Whole tics by tbot_held_tics' integer arithmetic (EKM_MS_PER_TIC is inexact).
  const int ageTics = (int)((age * (unsigned)TBOT_TICS_PER_SECOND) / 1000u);

  int pyoff = 0;
  const bool pyoffOk = player_read_pyoff(&pyoff);
  // aim_muzzle_z, as tbot.cpp's ray: the slot alone answers wrong for AA.
  const int nowMuzzleZ = pyoffOk ? aim_muzzle_z(now.z, pyoff) : now.z;

  // What the pitch drift is worth at the target: hitscan advances by (vx,vy,vz) with a horizontal magnitude of 16384
  // (shotray.h), so over range2d the impact's z moves range2d * dvz / 16384; EKM_Z_PER_XY restates it in an enemy's
  // units.
  const EkShotRay castRay = ekm_shot_ray(rec.q16ang, rec.q16horiz, rec.horizoff);
  const EkShotRay nowRay  = ekm_shot_ray(rec.q16ang, now.horiz, now.horizoff);
  const int dvz = nowRay.vz - castRay.vz;

  char worth[192];
  if (rec.range2d > 0 && rec.targetSprite >= 0) {
    const long long dz = (long long)rec.range2d * (long long)dvz / 16384;
    _snprintf_s(worth, sizeof(worth), _TRUNCATE,
                "worth at range: sprite %d is %d units out, so that pitch moves "
                "the impact %d z units (%d x/y units)",
                rec.targetSprite, rec.range2d, (int)dz,
                (int)(dz / (long long)(int)EKM_Z_PER_XY));
  } else {
    _snprintf_s(worth, sizeof(worth), _TRUNCATE,
                "worth at range: UNKNOWN -- the cast named no target sprite, so "
                "there is no range to price the pitch delta over");
  }

  ek_log("stale: the cast %s | age %u ms (%d tic(s)) | fire minus cast: "
         "dx %+d dy %+d dz %+d dmuzzlez %+d%s dsector %+d (%d -> %d) "
         "dq16ang %+d dq16horiz %+d dhorizoff %+d | %s",
         rec.fired ? (rec.usedSolution
                        ? "was the bot's own fire, cast on SILENT AIM'S SOLUTION"
                        : "was the bot's own fire, cast on the player's view")
                   : "happened but the bot did NOT fire on it (so this shot is "
                     "probably the player's own trigger)",
         age, ageTics,
         now.x - rec.ox, now.y - rec.oy, now.z - rec.posZ,
         nowMuzzleZ - rec.oz,
         (pyoffOk && rec.pyoffOk) ? "" : " (pyoff unreadable on one side, so "
                                         "dmuzzlez is not a muzzle delta)",
         now.sect - rec.osect, rec.osect, now.sect,
         now.ang - rec.q16ang, now.horiz - rec.q16horiz,
         now.horizoff - rec.horizoff, worth);
}

static volatile LONG s_bulletsFired = 0;

static void EK_PreFireDetour(int spriteNum, int playerNum, int projecTile, void* srcVect,
                             int* zvel, int* shootAng, int accurateAim, int doSpread) {
  PreFireFn const orig = s_prefireOrig;
  if (!orig) return;

  int weapon = 0;
  if (player_read_weapon(&weapon) &&
      ekm_weapon_wants_accuracy(weapon, g_cfg.accpistol, g_cfg.accshotgun,
                                g_cfg.accchaingun, g_cfg.accexpander,
                                g_cfg.accincinerator))
    doSpread = 0;

  // Every round, not every logged round: the engine calls this once per bullet, the only honest shot count (the
  // triggerbot's tally counts presented frames; the prefire line is rate limited). Unconditional so it compares against
  // a burst fired with the log off.
  s_bulletsFired++;

  // Decided BEFORE the call, so the values going in are the values that went in.
  const unsigned now = g_cfg.debuglog ? wallclock_ms() : 0;
  const bool say = g_cfg.debuglog && zvel != NULL && shootAng != NULL &&
                   (!s_prefireLogHave ||
                    (unsigned)(now - s_prefireLogMs) >= EK_PREFIRE_LOG_MIN_MS);
  const int inZvel = say ? *zvel : 0;
  const int inAng  = say ? *shootAng : 0;

  // Called BEFORE the original so "current" is the state the shot leaves on, not what GetAutoAimAng has done to the
  // out-parameters.
  if (g_cfg.debuglog &&
      (!s_staleLogHave || (unsigned)(now - s_staleLogMs) >= EK_STALE_LOG_MIN_MS)) {
    s_staleLogHave = 1;
    s_staleLogMs = now;
    ek_log_shot_staleness();
  }

  orig(spriteNum, playerNum, projecTile, srcVect, zvel, shootAng, accurateAim, doSpread);

  // Tracers record AFTER the original: it ends with `srcVect->z -= ZOFFSET6` (player.cpp:531), and auto-aim,
  // EVENT_GETSHOTRANGE and the spread have written shootAng and zvel by then, so these are the values Proj_DoHitscan
  // hands hitscan. Nothing is cast here (ek_hitscan is SwapBuffers-hook only).
  if (g_cfg.tracers != 0 && srcVect != NULL && zvel != NULL && shootAng != NULL) {
    const int* const sv = (const int*)srcVect;
    tracer_note_shot(sv[0], sv[1], sv[2], *shootAng, *zvel);
  }

  if (say) {
    s_prefireLogHave = 1;
    s_prefireLogMs = now;
    ek_log("prefire: sprite %d player %d tile %d weapon %d accurateAim %d "
           "doSpread %d | zvel %d -> %d (%+d), shootAng %d -> %d (%+d)",
           spriteNum, playerNum, projecTile, weapon, accurateAim, doSpread,
           inZvel, *zvel, *zvel - inZvel, inAng, *shootAng, *shootAng - inAng);
  }
}

// Silent aim: the shot is redirected, the view is not; the substitution lives inside ONE engine call, between two
// frames. Re-entrancy is a depth counter, not a tile list: World Tour's tile check matched nothing after its fork
// renumbered tiles (silentwt.cpp:73).
static int s_silentDepth = 0;

// Once each: this is the firing path, and the second occurrence says nothing the first did not.
static bool s_silentWriteLogged = false;
static bool s_silentRestoreLogged = false;

// D29's accurate-shot scope, read by EK_InsertDetour. A stack: a CON-fired shot can nest inside another (AA's
// SWITCHBULLET helper), so each level saves the tile it replaces and restores it. Game thread only.
typedef int (*InsertFn)(short whatsect, int s_x, int s_y, int s_z, short s_pn,
                        signed char s_s, unsigned char s_xr, unsigned char s_yr,
                        short s_a, short s_ve, short s_zv, short s_ow, short s_ss);
static InsertFn volatile s_insertOrig = NULL;
static int s_accDepth = 0;
static int s_accTile = 0;

// Independent of silent aim, which wraps this call: __finally restores on every path the process survives. A
// non-accurate shot nested inside an accurate one CLOSES the scope for its duration.
static int ek_shoot_scoped(ShootFn orig, int spriteNum, int projecTile, int forceZvel,
                           bool accurate) {
  if (!accurate && s_accDepth == 0) return orig(spriteNum, projecTile, forceZvel);
  const int prevDepth = s_accDepth;
  const int prevTile = s_accTile;
  int spawned = -1;
  s_accDepth = accurate ? prevDepth + 1 : 0;
  s_accTile = projecTile;
  __try {
    spawned = orig(spriteNum, projecTile, forceZvel);
  } __finally {
    s_accDepth = prevDepth;
    s_accTile = prevTile;
  }
  return spawned;
}

static int EK_ShootDetour(int spriteNum, int projecTile, int forceZvel) {
  ShootFn const orig = s_shootOrig;
  if (!orig) return -1;   // "nothing spawned", the engine's own dead-end value.

  // Cost order: hook and any accuracy row before any read, so with every row off no shot reads the engine.
  const bool anyAccRow = g_cfg.accpistol != 0 || g_cfg.accshotgun != 0 ||
                         g_cfg.accchaingun != 0 || g_cfg.accexpander != 0 ||
                         g_cfg.accincinerator != 0;
  const bool isPlayer = s_insertOrig != NULL && anyAccRow && player_sprite_matches(spriteNum);
  int accWeapon = 0;
  const bool accWeaponOk = isPlayer && player_read_weapon(&accWeapon);
  const bool accWants = accWeaponOk &&
      ekm_weapon_wants_accuracy(accWeapon, g_cfg.accpistol, g_cfg.accshotgun,
                                g_cfg.accchaingun, g_cfg.accexpander,
                                g_cfg.accincinerator);
  const bool accurate = ekm_accurate_shot_scope(isPlayer, accWeaponOk, accWants);

  // Every term must hold, in cost order (every actor comes through here). player_sprite_matches, not the latched index:
  // ps->i changes when the player respawns, and a stale index could match an ACTOR's shot and write our yaw into its
  // sprite ang, which the non-player arm reads for shootAng (player.cpp:1719).
  int q16ang = 0, q16horiz = 0, curAng = 0;
  EkShotSave save;
  const bool wants =
      g_cfg.silenton != 0 &&
      s_silentDepth == 0 &&
      player_sprite_matches(spriteNum) &&
      player_q16ang(&curAng) &&
      aim_solution_for_shot(&q16ang, &q16horiz) &&
      ekm_redirect_within((curAng >> 16) & (EKM_YAW_UNITS - 1),
                          (q16ang >> 16) & (EKM_YAW_UNITS - 1),
                          g_cfg.silentmaxangle);

  // The substitution is its own step so its failure can be reported apart from the ordinary reasons not to redirect.
  bool redirect = false;
  if (wants) {
    redirect = player_write_shot_view(q16ang, q16horiz, EK_SILENT_AUTO_AIM, &save);
    if (!redirect && !s_silentWriteLogged) {
      s_silentWriteLogged = true;
      ek_log("gamehooks: silent aim could not write the shot view; the shot is "
             "the player's own and stays that way");
    }
  }

  if (!redirect) return ek_shoot_scoped(orig, spriteNum, projecTile, forceZvel, accurate);

  // Restored on every path the process survives (__finally): a normal return, an early return in the original, a fault
  // some handler up-stack handles. An unhandled fault skips termination handlers, but that is process death. No
  // destructors, so it compiles under /EHsc.
  int spawned = -1;
  s_silentDepth++;
  __try {
    spawned = ek_shoot_scoped(orig, spriteNum, projecTile, forceZvel, accurate);
  } __finally {
    s_silentDepth--;
    if (!player_restore_shot_view(&save) && !s_silentRestoreLogged) {
      s_silentRestoreLogged = true;
      // Worth a line though nothing can be done: a failed restore is the one state this feature must never leave
      // behind.
      ek_log("gamehooks: silent aim could not restore the shot view after a "
             "shot; the player's aim may be left where the shot went");
    }
  }
  return spawned;
}

// EVENT_EGS has run by the time the original returns (game.cpp:1403), so randtraj's ang and zvel are in the record; the
// call's s_a and s_zv are A_ShootCustom's shootAng and zvel (player.cpp:995). Writing them back is limited to the three
// randtraj-only tiles (ekm_egs_randtraj_tile), so no deliberate AA edit is undone.
static EkHook s_insertHook;
static unsigned s_egsAng = 0, s_egsZvel = 0, s_egsUntouched = 0, s_egsRefused = 0;
static unsigned s_egsLogMs = 0, s_egsLogSum = 0;
static int s_egsLogHave = 0;

static void ek_egs_restore_after(int newSprite, short pn, short argStat, short argAng,
                                 short argZvel, short argXvel) {
  // Not a candidate: not read, not counted. ekm_egs_restore repeats these tests; this copy keeps the counters honest.
  if (s_accDepth <= 0 || pn != s_accTile || !ekm_egs_randtraj_tile(pn) || argStat != 4)
    return;
  short pic = 0, ang = 0, zvel = 0, xvel = 0;
  bool read = false;
  const unsigned char* const r = gamestate_sprite(newSprite);
  if (r) {
    __try {
      pic  = *(const short*)(r + EK_SPR_PICNUM);
      ang  = *(const short*)(r + EK_SPR_ANG);
      zvel = *(const short*)(r + EK_SPR_ZVEL);
      xvel = *(const short*)(r + EK_SPR_XVEL);
      read = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      read = false;
    }
  }
  const int w = ekm_egs_restore(s_accDepth > 0, s_accTile, pn, argStat, argAng, argZvel,
                                argXvel, ang, zvel, xvel);
  // The record must still be the projectile the call made; EGS may retile it.
  if (!read || pic != pn) {
    s_egsRefused++;
  } else if (w == 0) {
    s_egsUntouched++;
  } else {
    bool ok = true;
    if (w & EKM_EGS_RESTORE_ANG) {
      ok = gamestate_sprite_write(newSprite, EK_SPR_ANG, &argAng, sizeof(argAng));
      if (ok) s_egsAng++;
    }
    if (ok && (w & EKM_EGS_RESTORE_ZVEL)) {
      ok = gamestate_sprite_write(newSprite, EK_SPR_ZVEL, &argZvel, sizeof(argZvel));
      if (ok) s_egsZvel++;
    }
    if (!ok) s_egsRefused++;
  }

  // Cumulative on the prefire cadence: a Nova shot is nine bolts.
  if (!g_cfg.debuglog) return;
  const unsigned sum = s_egsAng + s_egsZvel + s_egsUntouched + s_egsRefused;
  const unsigned now = wallclock_ms();
  if (sum == s_egsLogSum ||
      (s_egsLogHave && (unsigned)(now - s_egsLogMs) < EK_PREFIRE_LOG_MIN_MS))
    return;
  s_egsLogHave = 1;
  s_egsLogMs = now;
  s_egsLogSum = sum;
  ek_log("egs restore: accurate projectiles (cumulative) -- restored ang %u, zvel %u, "
         "untouched %u, refused %u", s_egsAng, s_egsZvel, s_egsUntouched, s_egsRefused);
}

// Pure delegation outside the scope. The original never fails to return a sprite (fatal_exits instead, game.cpp:1363),
// so a missing trampoline waits.
static int EK_InsertDetour(short whatsect, int s_x, int s_y, int s_z, short s_pn,
                           signed char s_s, unsigned char s_xr, unsigned char s_yr,
                           short s_a, short s_ve, short s_zv, short s_ow, short s_ss) {
  InsertFn orig = s_insertOrig;
  while (!orig) { YieldProcessor(); orig = s_insertOrig; }
  const int newSprite = orig(whatsect, s_x, s_y, s_z, s_pn, s_s, s_xr, s_yr,
                             s_a, s_ve, s_zv, s_ow, s_ss);
  if (s_accDepth > 0) ek_egs_restore_after(newSprite, s_pn, s_ss, s_a, s_zv, s_ve);
  return newSprite;
}

typedef int (*IncurDamageFn)(int spriteNum);
static IncurDamageFn volatile s_incurOrig = 0;
static EkHook s_incurHook;

// Read before, decide after: A_IncurDamage subtracts the damage and sets htextra to -1 (actors.cpp:1322, :1327), but
// four paths return -1 having applied nothing (including a CON veto of EVENT_PREACTORDAMAGE, :1319). World Tour reports
// all four; this waits.
static int ek_incur_damage(int spriteNum) {
  IncurDamageFn orig = s_incurOrig;
  while (!orig) { YieldProcessor(); orig = s_incurOrig; }
  EkDmgPending p = { 0, 0, 0, 0, 0, 0, 0 };
  const int have = dmgnum_pre(spriteNum, &p);
  const int r = orig(spriteNum);
  if (have && r >= 0) dmgnum_post(&p);
  return r;
}

static bool install_one(const unsigned char* base, const char* what,
                        const unsigned char* sig, size_t len,
                        void* detour, EkHook* hook, volatile LONG* rvaOut) {
  const unsigned entry = locate(base, what, sig, len, NULL, 0);
  if (entry == 0) return false;

  if (!hook_install((void*)(base + entry), detour, hook)) {
    // hook.cpp logged its refusing stage. rvaOut is published only on success, so _rva() cannot be nonzero while
    // _ready() is false.
    ek_log("gamehooks: %s at rva 0x%07X was NOT hooked; the game keeps its own",
           what, entry);
    return false;
  }
  InterlockedExchange(rvaOut, (LONG)entry);
  ek_log("gamehooks: %s hooked at rva 0x%07X", what, entry);
  return true;
}

bool gamehooks_install(void) {
  if (s_attempted) return s_prefireOrig != NULL && s_shootOrig != NULL;
  s_attempted = true;

  const unsigned char* const base = (const unsigned char*)GetModuleHandleA(NULL);
  if (!base) { ek_log("gamehooks: no host module; not hooking"); return false; }

  if (install_one(base, "P_PreFireHitscan", EK_PREFIRE_SIG, sizeof(EK_PREFIRE_SIG),
                  (void*)&EK_PreFireDetour, &s_prefireHook, &s_prefireRva))
    s_prefireOrig = (PreFireFn)hook_trampoline(&s_prefireHook);

  if (install_one(base, "A_ShootWithZvel", EK_SHOOT_SIG, sizeof(EK_SHOOT_SIG),
                  (void*)&EK_ShootDetour, &s_shootHook, &s_shootRva))
    s_shootOrig = (ShootFn)hook_trampoline(&s_shootHook);

  // A_IncurDamage (actors.cpp:1299): a 0 is a refusal already logged; the damage numbers stay off and the two hooks are
  // left alone.
  const unsigned incurEntry = locate(base, "A_IncurDamage",
                                     EK_INCURDAMAGE_SIG,  sizeof(EK_INCURDAMAGE_SIG),
                                     EK_INCURDAMAGE_SIG2, sizeof(EK_INCURDAMAGE_SIG2));
  if (incurEntry != 0) InterlockedExchange(&s_incurRva, (LONG)incurEntry);

  // A refusal here leaves the other two hooks alone: the damage numbers are an independent feature.
  if (s_incurRva != 0) {
    if (hook_install((void*)(base + incurEntry), (void*)ek_incur_damage, &s_incurHook)) {
      s_incurOrig = (IncurDamageFn)hook_trampoline(&s_incurHook);
      ek_log("gamehooks: A_IncurDamage hooked at rva 0x%07X; the damage numbers "
             "read htextra before the original consumes it and keep the event "
             "only when the original reports the damage landed", incurEntry);
    } else {
      InterlockedExchange(&s_incurRva, 0);
      ek_log("gamehooks: the A_IncurDamage hook did not take; damage numbers stay off");
    }
  }

  // A_InsertSprite (game.cpp:1348), the EVENT_EGS restore; a refusal leaves Perfect Accuracy's hitscan half working.
  const unsigned insertEntry = locate(base, "A_InsertSprite",
                                      EK_INSERT_SIG,  sizeof(EK_INSERT_SIG),
                                      EK_INSERT_SIG2, sizeof(EK_INSERT_SIG2));
  if (insertEntry != 0) {
    if (hook_install((void*)(base + insertEntry), (void*)&EK_InsertDetour, &s_insertHook)) {
      s_insertOrig = (InsertFn)hook_trampoline(&s_insertHook);
      ek_log("gamehooks: A_InsertSprite hooked at rva 0x%07X; an accurate player "
             "shot's projectile gets its own ang and zvel back after EVENT_EGS",
             insertEntry);
    } else {
      ek_log("gamehooks: the A_InsertSprite hook did not take; projectile shotguns "
             "keep AA's EVENT_EGS spread");
    }
  }

  ek_log("gamehooks: install finished -- P_PreFireHitscan=%s A_ShootWithZvel=%s "
         "A_InsertSprite=%s", s_prefireOrig ? "hooked" : "stock",
         s_shootOrig ? "hooked" : "stock", s_insertOrig ? "hooked" : "stock");
  return s_prefireOrig != NULL && s_shootOrig != NULL;
}

void gamehooks_remove(void) {
  // The pointers go first: this NARROWS the unload window (a thread already inside the original still holds its return
  // address into the trampoline hook_remove frees), it cannot close it.
  s_prefireOrig = NULL;
  s_shootOrig = NULL;
  if (s_prefireHook.target && !hook_remove(&s_prefireHook))
    ek_log("gamehooks: P_PreFireHitscan could not be unpatched");
  if (s_shootHook.target && !hook_remove(&s_shootHook))
    ek_log("gamehooks: A_ShootWithZvel could not be unpatched");
  // The opposite order, on purpose: ek_incur_damage SPINS on a null original and hook.cpp:186 leaves the target patched
  // when a restore fails, so clearing first would hang the next damaged actor.
  if (s_incurHook.target && !hook_remove(&s_incurHook))
    ek_log("gamehooks: A_IncurDamage could not be unpatched");
  else
    s_incurOrig = 0;
  // Same order and reason: EK_InsertDetour spins on a null original too.
  if (s_insertHook.target && !hook_remove(&s_insertHook))
    ek_log("gamehooks: A_InsertSprite could not be unpatched");
  else
    s_insertOrig = NULL;
}

bool gamehooks_shoot_ready(void)  { return s_shootOrig != NULL; }
bool gamehooks_spread_ready(void) { return s_prefireOrig != NULL; }

unsigned gamehooks_bullets_fired(void) {
  return (unsigned)InterlockedCompareExchange(&s_bulletsFired, 0, 0);
}

unsigned gamehooks_shoot_rva(void)  { return (unsigned)s_shootRva; }
unsigned gamehooks_spread_rva(void) { return (unsigned)s_prefireRva; }
unsigned gamehooks_incurdamage_rva(void) { return (unsigned)s_incurRva; }
