#pragma once

#include <stddef.h>
#include "gamestate.h"

// The whole-map sweep reimplements polymer_scansprites (polymer.cpp:1234; `static inline`), copy:
// renderMakeTSpriteFromSprite (build.h:700); docs/re/tsprite-sweep.md.

const int EK_MAXSPRITESONSCREEN = 4096;

const int EK_TSPR_STRIDE   = EK_SPR_STRIDE;
const int EK_TSPR_CLIPDIST = 0x12;
const int EK_TSPR_OWNER    = EK_SPR_OWNER;

// A bad counter stops the pass: an append past tsprite[] corrupts .bss.
inline bool ekm_tsprite_admit(int count, int max) {
  return max > 0 && count >= 0 && count < max;
}

inline bool ekm_tsprite_eligible(unsigned cstat, int xrepeat, int yrepeat,
                                 int showInvisible) {
  if ((cstat & 0x8000u) != 0 && !showInvisible) return false;
  return xrepeat > 0 && yrepeat > 0;
}

// Chain walked for cost (all 16384 records per frame was choppy); sprite[].sectnum stays as a witness
// (engine.cpp:8721).
inline bool ekm_sprite_swept(int statnum, int sectnum, int sweptEnd,
                             int maxSectors) {
  if (sweptEnd < 0 || sweptEnd > maxSectors) return false;
  if (statnum < 0 || statnum >= EK_MAXSTATUS) return false;
  if (sectnum < 0 || sectnum >= sweptEnd) return false;
  return true;
}

const int EK_HEADSPRITESECT_SLOTS = EK_MAXSECTORS + 1;
const int EK_NEXTSPRITESECT_SLOTS = EK_MAXSPRITES;

inline bool ekm_sprite_chain_live(int i, int maxSprites) {
  return i >= 0 && i < maxSprites;
}

inline bool ekm_sprite_chain_agrees(int sectnum, int expectSect) {
  return sectnum == expectSect;
}

// MAXSPRITES links is every chain.
inline bool ekm_chain_budget_ok(int walked, int maxSprites) {
  return walked >= 0 && walked <= maxSprites;
}

inline bool ekm_tsprite_owner_ok(int owner, int maxSprites) {
  return owner >= 0 && owner < maxSprites;
}

// Pointers come via mingw `.refptr` slots in the inlined polymer_scansprites: one `imul r64,r64,44`, one `add`, two
// `mov r64,[reg]`, one `movsxd` + compare 4095.
inline bool ekm_scansprites_shape_ok(int imuls, int adds, int derefs,
                                     int spriteDeref, int counters) {
  return imuls == 1 && adds == 1 && derefs == 2 && spriteDeref == 1 &&
         counters == 1;
}

inline bool ekm_scansprites_slots_ok(int spriteLoads, int tspriteLoads,
                                     int countLoads, size_t spriteSlot,
                                     size_t tspriteSlot, size_t countSlot) {
  if (spriteLoads != 1 || tspriteLoads != 1 || countLoads != 1) return false;
  if (spriteSlot == 0 || tspriteSlot == 0 || countSlot == 0) return false;
  if (spriteSlot == tspriteSlot || spriteSlot == countSlot) return false;
  return tspriteSlot != countSlot;
}

// The chain array indexed by the imul'd register is nextspritesect[].
inline bool ekm_chain_shape_ok(int heads, int nexts) {
  return heads == 1 && nexts == 1;
}

inline bool ekm_chain_slots_ok(int headLoads, int nextLoads, size_t headSlot,
                               size_t nextSlot, size_t spriteSlot,
                               size_t tspriteSlot, size_t countSlot) {
  if (headLoads != 1 || nextLoads != 1) return false;
  if (headSlot == 0 || nextSlot == 0) return false;
  if (headSlot == nextSlot) return false;
  if (headSlot == spriteSlot || headSlot == tspriteSlot ||
      headSlot == countSlot)
    return false;
  return nextSlot != spriteSlot && nextSlot != tspriteSlot &&
         nextSlot != countSlot;
}

inline bool ekm_tsprite_slot_ok(size_t slotRva, size_t targetRva,
                                size_t imageSize, int slotInData,
                                size_t bytesNeeded) {
  if (!slotInData || imageSize == 0) return false;
  if (slotRva + sizeof(void*) > imageSize) return false;
  if (targetRva == 0 || targetRva >= imageSize) return false;
  return imageSize - targetRva >= bytesNeeded;
}

inline bool ekm_tsprite_array_ok(size_t arrayRva, size_t imageSize) {
  if (imageSize == 0 || arrayRva == 0 || arrayRva >= imageSize) return false;
  return imageSize - arrayRva >=
         (size_t)EK_MAXSPRITESONSCREEN * (size_t)EK_TSPR_STRIDE;
}

// Compare on the array, not the slot; zero is not known.
inline bool ekm_sprite_array_agrees(unsigned predicted, unsigned found) {
  return predicted != 0 && found != 0 && predicted == found;
}
