#pragma once

#include <stdint.h>
#include <string.h>
#include "gamestate.h"

// CON gamearray registry (AA keeps its upgrade state here). Field order is the REVERSE of gamevar_t's at the same
// 32-byte stride, so the EK_GV_* offsets would read a heap pointer as the size.
const int EK_GA_STRIDE  = 32;
const int EK_GA_LABEL   = 0;
const int EK_GA_PVALUES = 8;
const int EK_GA_SIZE    = 16;
const int EK_GA_FLAGS   = 24;

// A quarter of MAXGAMEVARS (gamevars.h:72).
const int EK_MAXGAMEARRAYS = 512;
const int EK_MAXARRAYLABEL = 26;

// gamevars.h:74-95; only the bits used here.
const uintptr_t EK_GAMEARRAY_RESET     = 0x00000008u;
const uintptr_t EK_GAMEARRAY_STRIDE2   = 0x00000100u;
const uintptr_t EK_GAMEARRAY_ALLOCATED = 0x00000200u;
const uintptr_t EK_GAMEARRAY_SYSTEM    = 0x00000800u;
const uintptr_t EK_GAMEARRAY_READONLY  = 0x00001000u;
const uintptr_t EK_GAMEARRAY_INT16     = 0x00004000u;
const uintptr_t EK_GAMEARRAY_INT8      = 0x00008000u;
const uintptr_t EK_GAMEARRAY_UNSIGNED  = 0x00010000u;
const uintptr_t EK_GAMEARRAY_BITMAP    = 0x00100000u;
const uintptr_t EK_GAMEARRAY_UINT16 = EK_GAMEARRAY_INT16 | EK_GAMEARRAY_UNSIGNED;
const uintptr_t EK_GAMEARRAY_UINT8  = EK_GAMEARRAY_INT8  | EK_GAMEARRAY_UNSIGNED;

const uintptr_t EK_GA_SIZE_MASK =
    EK_GAMEARRAY_INT8 | EK_GAMEARRAY_INT16 | EK_GAMEARRAY_BITMAP;
const uintptr_t EK_GA_TYPE_MASK =
    EK_GAMEARRAY_UNSIGNED | EK_GA_SIZE_MASK;

// Anchor: Gv_AddSystemVars registers six arrays first (gamevars.cpp:1852-1857) with known sizes and flags. szLabel is a
// heap copy (Gv_NewArray:1004-1008), never a literal's address.
struct EkGaSys { const char* name; intptr_t size; uintptr_t flags; };

const uintptr_t EK_GA_SYS_BITMAP_FLAGS =
    EK_GAMEARRAY_SYSTEM | EK_GAMEARRAY_BITMAP;
const uintptr_t EK_GA_SYS_TILESIZ_FLAGS = EK_GAMEARRAY_SYSTEM |
    EK_GAMEARRAY_STRIDE2 | EK_GAMEARRAY_READONLY | EK_GAMEARRAY_INT16;

const int EK_GA_SYS_COUNT = 6;
enum { EK_GA_SYS_TILESIZX = 4, EK_GA_SYS_TILESIZY = 5 };
const EkGaSys EK_GA_SYS[EK_GA_SYS_COUNT] = {
  { "gotpic",            EK_MAXTILES,   EK_GA_SYS_BITMAP_FLAGS  },
  { "gotsector",         EK_MAXSECTORS, EK_GA_SYS_BITMAP_FLAGS  },
  { "radiusdmgstatnums", EK_MAXSTATUS,  EK_GA_SYS_BITMAP_FLAGS  },
  { "show2dsector",      EK_MAXSECTORS, EK_GA_SYS_BITMAP_FLAGS  },
  { "tilesizx",          EK_MAXTILES,   EK_GA_SYS_TILESIZ_FLAGS },
  { "tilesizy",          EK_MAXTILES,   EK_GA_SYS_TILESIZ_FLAGS },
};

// memcpy, not a cast: the sweep steps through raw section bytes.
inline intptr_t ekm_ga_word(const unsigned char* rec, int off) {
  intptr_t v = 0;
  memcpy(&v, rec + off, sizeof(v));
  return v;
}

// Two known words per record, six times. GAMEARRAY_RESET is masked out: Gv_Free sets it during a CON re-parse and
// Gv_NewArray clears it (gamevars.cpp:73, :1013).
inline bool ekm_ga_shape_ok(const unsigned char* cand) {
  if (!cand) return false;
  for (int i = 0; i < EK_GA_SYS_COUNT; i++) {
    const unsigned char* const rec = cand + (size_t)i * EK_GA_STRIDE;
    if (ekm_ga_word(rec, EK_GA_SIZE) != EK_GA_SYS[i].size) return false;
    const uintptr_t f = (uintptr_t)ekm_ga_word(rec, EK_GA_FLAGS);
    if ((f & ~EK_GAMEARRAY_RESET) != EK_GA_SYS[i].flags) return false;
  }
  return true;
}

// Cross-check: tilesizx/y.pValues are &tilesiz[0].x/.y (gamevars.cpp:1856-1857) and tilesiz is located independently
// (tiles.cpp). Zero tilesizRva means "not located", never a match.
inline bool ekm_ga_tilesiz_crosscheck(unsigned xRva, unsigned yRva,
                                      unsigned tilesizRva) {
  if (!tilesizRva || !xRva || !yRva) return false;
  return xRva == tilesizRva && yRva == tilesizRva + 2;
}

// Mirrors Gv_GetArrayElementSize (gamevars.cpp:959-971): only GAMEARRAY_SIZE_MASK matters; BITMAP returns 0 because it
// is addressed as a bit.
inline unsigned ekm_ga_element_size(uintptr_t flags) {
  switch (flags & EK_GA_SIZE_MASK) {
    case 0:                 return (unsigned)sizeof(uintptr_t);
    case EK_GAMEARRAY_INT8: return 1u;
    case EK_GAMEARRAY_INT16:return 2u;
    default:                return 0u;
  }
}

// Where element `index` lives; false when out of range. The bound is the record's own `size`, applied BEFORE the
// STRIDE2 doubling as the engine does (gamevars.cpp:1307, gamevars.h:129-130). CHECK_INDEX is unsigned
// (gamevars.cpp:1176-1181), so negatives are refused explicitly.
inline bool ekm_ga_element_span(uintptr_t flags, intptr_t size, int index,
                                size_t* outOff, size_t* outBytes) {
  if (!outOff || !outBytes) return false;
  *outOff = 0; *outBytes = 0;
  if (size <= 0 || index < 0 || (intptr_t)index >= size) return false;
  if (flags & EK_GAMEARRAY_BITMAP) {
    *outOff = (size_t)(index >> 3);
    *outBytes = 1;
    return true;
  }
  const unsigned es = ekm_ga_element_size(flags);
  if (!es) return false;
  const size_t i = (flags & EK_GAMEARRAY_STRIDE2) ? (size_t)index * 2u
                                                  : (size_t)index;
  *outOff = i * es;
  *outBytes = es;
  return true;
}

// Mirrors Gv_GetArrayValue's switch (gamevars.h:128-136); `index` only picks the bit within a bitmap byte.
inline int ekm_ga_decode(uintptr_t flags, const unsigned char* p, int index) {
  if (!p) return 0;
  switch (flags & EK_GA_TYPE_MASK) {
    case EK_GAMEARRAY_INT16: {
      int16_t v = 0; memcpy(&v, p, sizeof(v)); return (int)v;
    }
    case EK_GAMEARRAY_UINT16: {
      uint16_t v = 0; memcpy(&v, p, sizeof(v)); return (int)v;
    }
    case EK_GAMEARRAY_INT8:  return (int)(int8_t)*p;
    case EK_GAMEARRAY_UINT8: return (int)*p;
    case EK_GAMEARRAY_BITMAP:
      return (*p >> (index & 7)) & 1;
    default: {
      intptr_t v = 0; memcpy(&v, p, sizeof(v)); return (int)v;
    }
  }
}

// -1 is the permanent answer on stock Duke and World Tour; as an index it would be slot 0, AA's `gotpic`.
inline bool ekm_ga_index_ok(int idx) {
  return idx >= 0 && idx < EK_MAXGAMEARRAYS;
}

// AA's five per-character upgrade arrays (AADEFS.CON:4381-4385), no type flags, so intptr_t bitfields by weapon slot.
enum { EK_GA_UP_DUKE = 0, EK_GA_UP_SHELLY = 1, EK_GA_UP_WES = 2,
       EK_GA_UP_MANDO = 3, EK_GA_UP_NOVA = 4, EK_GA_UP_COUNT = 5 };

// Worker thread only (walks every writable section). Defers until tilesiz is located, since the cross-check is the
// evidence; a deferral costs no budget.
bool gamearrays_scan(void);
bool gamearrays_ready(void);

unsigned gamearrays_rva(void);

// Entries walked before the first null szLabel (AA declares 53, stock Duke six).
int gamearrays_walked(void);

int gamearrays_upgrades_resolved(void);

// False for stock Duke and World Tour, kept apart from the read so "not this game" differs from "read failed".
bool gamearrays_upgrade_present(int which);

// False means no answer (not located, no array, slot out of range, unreadable) and callers must behave as before. Call
// it in its own statement: reading the out-param in the argument list that fills it is indeterminately sequenced and
// MSVC gets it wrong.
bool gamearrays_upgrade(int which, int slot, int* out);
