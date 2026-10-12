// g_tile[] flags, and so what counts as an enemy: the engine's own A_CheckEnemyTile (actors.h:471-474), so CON mods
// come free. tiledata_t follows #pragma pack(pop) (actors.h:313-323): x64 40 bytes, flags at +32 (+16 is x86). Flags
// cannot be range-checked, so the evidence is content: named enemies flagged AND a small total.

#include <windows.h>
#include "log.h"
#include "gamestate.h"
#include "tiles.h"
#include "badguy.h"

static const unsigned char* volatile s_tile = NULL;
// Worker thread only (tiles.cpp's reason): the scan walks every writable section.
static int s_attempts = 0;
static bool s_budgetSpent = false;
static volatile LONG s_rva = 0;
static volatile LONG s_badguys = 0;

// names.h picnums: Duke's art numbering, the same in every build.
static const int kEnemies[] = {
  1680,  1820, 1880,   1920,
  1960,     2000,    2120,  2420,
  2630,
};
static const int kEnemyCount = (int)(sizeof(kEnemies) / sizeof(kEnemies[0]));
static const int kPlayerPic = 1405;

// Creatures the engine calls enemies but are not worth a box. RAT is in DukeEnemies[] (game.cpp:6134), so this is our
// preference, not a correction of A_CheckEnemyTile.
static const int kHarmless[] = {
  1267,
};
static const int kHarmlessCount = (int)(sizeof(kHarmless) / sizeof(kHarmless[0]));

bool picnum_is_harmless_critter(int picnum) {
  for (int i = 0; i < kHarmlessCount; i++) if (kHarmless[i] == picnum) return true;
  return false;
}

static unsigned flags_at(const unsigned char* arr, int pic) {
  return *(const unsigned*)(arr + (size_t)pic * EK_TILEDATA_STRIDE + EK_TILEDATA_FLAGS);
}

static bool candidate_ok(const unsigned char* arr, int* badguysOut) {
  for (int i = 0; i < kEnemyCount; i++)
    if ((flags_at(arr, kEnemies[i]) & EK_SFLAG_ANY_BADGUY) == 0) return false;
  if ((flags_at(arr, kPlayerPic) & EK_SFLAG_ANY_BADGUY) != 0) return false;

  // Ten named picnums alone pass at about one offset in two thousand, hundreds of false positives over a million
  // candidates; the total-flagged test makes it decisive.
  int badguys = 0;
  for (int pic = 0; pic < EK_MAXTILES_ARR; pic++)
    if (flags_at(arr, pic) & EK_SFLAG_ANY_BADGUY) badguys++;
  if (badguys < kEnemyCount || badguys > 400) return false;

  if (badguysOut) *badguysOut = badguys;
  return true;
}

bool badguy_scan(void) {
  if (s_tile) return true;
  if (s_budgetSpent) return false;

  const unsigned char* base = (const unsigned char*)GetModuleHandleA(NULL);
  const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const int nsec = nt->FileHeader.NumberOfSections;
  const size_t bytes = (size_t)EK_MAXTILES_ARR * EK_TILEDATA_STRIDE;

  const unsigned char* best = NULL;
  unsigned bestRva = 0, firstRva = 0, lastRva = 0;
  int bestBad = 0, matches = 0;

  for (int i = 0; i < nsec; i++) {
    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
    const unsigned char* start = base + sec[i].VirtualAddress;
    const DWORD span = sec[i].Misc.VirtualSize;
    if (span < bytes) continue;

    for (DWORD off = 0; off + bytes <= span; off += 8) {
      const unsigned char* cand = start + off;
      if ((((UINT_PTR)cand) & 15u) != 0) continue;
      int badguys = 0;
      if (!candidate_ok(cand, &badguys)) continue;
      matches++;
      const unsigned thisRva = (unsigned)(sec[i].VirtualAddress + off);
      if (!firstRva) firstRva = thisRva;
      lastRva = thisRva;
      // Fewest flagged wins: a shifted view smears flag bits and only gains spurious hits.
      if (!best || badguys < bestBad) { bestBad = badguys; best = cand; bestRva = thisRva; }
    }
  }

  if (!best) {
    // Charged on the expensive walk only (tiles.cpp); g_tile fills when the CON parses, so an early miss is legitimate.
    if (++s_attempts >= EK_TILE_SCAN_BUDGET) {
      s_budgetSpent = true;
      ek_log("badguy: giving up on g_tile after %d attempts; the ESP will box "
             "every actor, not just enemies, and nothing will retry", s_attempts);
    } else {
      ek_log("badguy: g_tile not found; the ESP will box every actor, not just enemies");
    }
    return false;
  }
  if (matches > 1) {
    const unsigned spanRva = lastRva - firstRva;
    ek_log("badguy: %d candidates spanning 0x%X bytes (rva 0x%07X..0x%07X)%s",
           matches, spanRva, firstRva, lastRva,
           spanRva <= (unsigned)bytes ? " -- shifted views of one array" : " -- AMBIGUOUS");
  }

  InterlockedExchange(&s_rva, (LONG)bestRva);
  InterlockedExchange(&s_badguys, (LONG)bestBad);
  s_tile = best;
  ek_log("badguy: g_tile at rva 0x%07X, %d tile(s) flagged as enemies", bestRva, bestBad);
  return true;
}

bool badguy_ready(void) { return s_tile != NULL; }
unsigned badguy_rva(void) { return (unsigned)InterlockedCompareExchange(&s_rva, 0, 0); }
int badguy_count(void) { return (int)InterlockedCompareExchange(&s_badguys, 0, 0); }

const unsigned char* badguy_tile_record(int picnum) {
  const unsigned char* const t = s_tile;
  if (!t || picnum < 0 || picnum >= EK_MAXTILES_ARR) return NULL;
  return t + (size_t)picnum * EK_TILEDATA_STRIDE;
}

bool picnum_is_enemy(int picnum) {
  const unsigned char* const t = s_tile;
  if (!t || picnum < 0 || picnum >= EK_MAXTILES_ARR) return false;
  return (flags_at(t, picnum) & EK_SFLAG_ANY_BADGUY) != 0;
}
