#pragma once
#include "projmath.h"
#include "esp_voxels_aa.h"
#include "tiles.h"

// Tile or voxel box: AA draws most pickups as voxels, so a `tilesiz` box measured a surface never seen (RAILGUNSPRITE
// 5329). A miss is the old answer. Stock Duke is not exempt: esp_voxels_aa.h carries picnums 21-24, 26-29, 36-42.

struct EkItemBox {
  float halfWidth;
  float heightZ;
  int   feetZ;
  bool  fromVoxel;
};

// `yawDelta` (sprite ang minus camera ang) is read only on the voxel branch; sign and 180-degree offset do not matter.
inline EkItemBox ekm_item_box(int picnum, int spriteZ, int xrepeat, int yrepeat,
                              unsigned cstat, int tileW, int tileH,
                              int yawDelta) {
  EkItemBox b;
  const EkVoxAa v = ekm_vox_aa_find(picnum);
  if (v.found && ekm_voxel_drawn(cstat)) {
    b.halfWidth = ekm_voxel_halfwidth_yaw(xrepeat, v.sizX, v.sizY, v.radius,
                                          v.scale, yawDelta);
    b.heightZ   = ekm_voxel_height_z(yrepeat, v.sizZ, v.scale);
    b.feetZ     = ekm_voxel_feet_z(spriteZ, yrepeat, v.sizZ, v.pivZ, v.scale,
                                   cstat);
    b.fromVoxel = true;
    return b;
  }
  b.halfWidth = ekm_sprite_halfwidth(xrepeat, tileW);
  b.heightZ   = ekm_sprite_height_z(yrepeat, tileH);
  b.feetZ     = ekm_feet_z(spriteZ, b.heightZ, cstat);
  b.fromVoxel = false;
  return b;
}

// Is the box sized or guessed? Only the tile path can say no, when the raw tile_size is the fallback shape (tiles.h).
inline bool ekm_item_box_known(bool voxelPath, int rawTw, int rawTh) {
  return voxelPath || !ekm_tile_size_is_fallback(rawTw, rawTh);
}
