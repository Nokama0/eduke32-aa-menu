#pragma once

// AA's Polymorph pole, pad and pod (AADEFS.CON), the actors running `state polybackstate`.
const int EK_PICNUM_POLYPOLE     = 7270;
const int EK_PICNUM_POLYMORPHPAD = 7275;
const int EK_PICNUM_POLYMORPHPOD = 7280;

inline bool ekm_picnum_is_polyback(int picnum) {
  return picnum == EK_PICNUM_POLYPOLE || picnum == EK_PICNUM_POLYMORPHPAD ||
         picnum == EK_PICNUM_POLYMORPHPOD;
}

// Growing over a body, so no hit can land: `ife initflags 2` grows the pad before ifhitweapon.
inline bool ekm_polypad_growing_aa(int picnum, bool gotInitflags, int initflags,
                                   int xrepeat) {
  return ekm_picnum_is_polyback(picnum) && gotInitflags && initflags == 2 &&
         xrepeat != 32;
}
