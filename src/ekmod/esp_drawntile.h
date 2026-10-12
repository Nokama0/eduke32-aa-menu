#pragma once

// G_DoSpriteAnimations (game.cpp:4312) draws picnum + frameOffset + ACTION_STARTFRAME + viewtype*curframe, so STEROIDS
// (`action ASYRINGE 18776`) draws 18831 while its record reads 55. The answer is actor[i].dispicnum (actors.h:143, set
// game.cpp:4536), which persists: see actorstate.h EK_ACT_DISPICNUM.

#include "tiles.h"

// Passed when the record could not be read. Not 0: 0 is the engine's "not drawn yet".
const int EKM_DISPICNUM_UNREADABLE = -1;

// Strictly positive: a negative dispicnum used to be a flag (game.cpp:4343-4350), and CON can write the field directly.
inline bool ekm_drawn_picnum_known(int dispicnum, int maxTiles) {
  return dispicnum > 0 && dispicnum < maxTiles;
}

inline int ekm_drawn_picnum(int storedPicnum, int dispicnum, int maxTiles) {
  return ekm_drawn_picnum_known(dispicnum, maxTiles) ? dispicnum : storedPicnum;
}

// The same answer from the action when dispicnum is stale or zero; every term but startFrame is zero for a
// one-parameter CON action.
inline int ekm_action_drawn_picnum(int storedPicnum, int startFrame,
                                   int viewType, int curFrame, int frameOffset,
                                   int maxTiles) {
  const int drawn = storedPicnum + frameOffset + startFrame +
                    viewType * curFrame;
  return (drawn > 0 && drawn < maxTiles) ? drawn : storedPicnum;
}

// May a caller supply frameOffset 0? Only for viewtype 0 or -1: game.cpp:4272-4306 derives it from the camera angle for
// the other cases, which this DLL cannot compute. Not +1: the :4317 walk-back is gated on viewtype > 0.
inline bool ekm_action_frameoffset_known(int viewType) {
  return viewType == 0 || viewType == -1;
}

// Three sources, each refusal falling through: dispicnum, the action algebra, the stored picnum. All are needed: the
// Items ESP marks sprites never drawn (dispicnum 0). `actionUsable`: the caller found the action decoded.
inline int ekm_item_drawn_tile(int storedPicnum, int dispicnum,
                               bool actionUsable, int startFrame, int viewType,
                               int curFrame, int frameOffset, int maxTiles) {
  if (ekm_drawn_picnum_known(dispicnum, maxTiles)) return dispicnum;
  if (!actionUsable) return storedPicnum;
  return ekm_action_drawn_picnum(storedPicnum, startFrame, viewType, curFrame,
                                 frameOffset, maxTiles);
}
