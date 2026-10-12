# Polymer projection and viewingrange

How the menu projects world points to the screen under Polymer. Build: eduke32.exe r10669.

## Matrices
- polymer_drawrooms sets the modelview at polymer.cpp:1113-1171.
- The projection is a gluPerspective at polymer.cpp:919-936 via glbuild.cpp:418-440.
- renderDrawRoomsQ16 branches to polymer before the classic path (engine.cpp:9561-9565).

## Pitch
Polymer forms the pitch as `atan2(horiz - 100, 128)` (polymer.cpp:1121), so dead level is horiz == 100. The classic
"globalhoriz is a screen line" formula is real (engine.cpp:9447) but belongs to a renderer the menu does not use.

## viewingrange must be derived, not read
- G_DrawRooms restores `viewingrange` to its entry value when the frame ends (game.cpp:1250-1254), and the menu draws
  after that, so the global reads a constant 65536 at SwapBuffers time whatever the player's field of view.
- Reading it would give a plausible number that is wrong at every fov but the default. game.cpp:886-895 gives the real
  expression. Under Polymer videoSetCorrectedAspect takes the branch that leaves the second factor at 65536
  (engine.cpp:13719), so what survives is the tangent term.

## Polymost correction divisor (projmath.h, ekm_zenglitch_divisor)
- polymost.cpp:6870-6879 builds gvrcorrection = viewingrange/65536, then at r_projectionhack 2 divides it by a term from
  the vertical fov; every Polymost projection is (ghalfx/gvrcorrection)/v.z (polymost.cpp:6981, 7049, 5168).
- Under Polymost videoSetCorrectedAspect takes its non-Polymer branch (engine.cpp:13719) before G_DrawRooms (game.cpp:807-811,
  891-894): yxaspect = 65536*5/4 = 81920 (engine.cpp:13714-13716, not 65536) and the viewingrange global is divscale16(xd*3,
  yd*4), so line 894's mulscale16 leaves 128139 where the mod says 71520. They cancel to 0.75 * viewingRange / 65536
  (0.818475 against the engine's 0.818481). At fov 95 on 3440x1440 the correction is 1.3069, 30.7%.
- Rotation (ekm_project_build), horizon check at horiz 128: correct 912.43, transposed 1150.69, term dropped 1007.89.
- ekm_build_clip_tilt: oy = dz*yxscale/gdepth, oz = halfx, oz2 = oy*shang*gvr + oz*chang; the tilted T(v) is a plane through
  the eye. The tilt margin holds to ~65536 lateral units at gdepth ~1 and ~1.35e-3 relative at 262144.

## Voxel box (voxmodel.cpp:1038-1163)
m0.x = m0.y = scale*xrepeat*(256/320)/64 and m0.z = scale*yrepeat/64 (:1057-1072); position columns /1024 horizontal and
/16384 vertical (:1090-1092), model columns /64 (:1148-1150). (1024/64)*(0.8/64) = 0.2; (16384/64)*(1/64) = 4. Pivot
recovery: gen_esp_voxels_aa.py radius_of, 4-decimal radius.

## Depth bias of mod-drawn quads (worldquad.h)
- Too large a bias is worse than too small. World Tour (wtmod/worldwt.h:13-33) splits its pull: 1e-4 for a quad on a
  surface's own plane, 1e-6 for a quad on a room with depth. Pulling a room as hard as a coincident face puts every wall in
  front of every other once the room's near-to-far extent is thinner than the pull, and the room reads as a solid box. On
  E1L1 sector 148 (24 walls) the room's z/w extent is 16.7x the 1e-4 pull at 9952 units, 1.0x at 28248 and 0.15x at 73323.
  The principle transfers, the figure does not (World Tour pulls a fraction of clip z in a shader).
- Polymer gives a wall sprite -64 of its own (polymer.cpp:1583), which is why a flat tint needs -128. Ordinary walls get no
  offset: polymer.cpp enables GL_POLYGON_OFFSET_FILL only around a sprite's plane (:1632-1637) and for the shadow pass
  (:6232); polymost.cpp never calls glPolygonOffset (both mentions are commented out). The offset only breaks a tie. A
  Polymost wall sprite's own pull is r_spritedepth 1.0001 (polymost.cpp:7557-7559, :8495).
- The factor/units pair -1 / -2 is Megaton's: units * r (r = the smallest resolvable depth difference), -1 enough by
  definition and -2 one step of margin; 64x smaller than the coincident -128 and 32x smaller than the engine's -64. The
  factor stays -1 because an edge-on panel has a steep depth gradient the units term does not cover. The Polymost flush also
  pulls every vertex 1/512 toward the eye (EK_WORLD_PM_BIAS).
- If hardware disagrees: still fighting, raise it; a distant room gone solid, lower it. Halve or double, do not creep.

## Switch models and tint boxes (esp_tint.h)
- AA ships its switch voxels commented out (duke3d.def:302), so its locks have no model and are tinted as flat
  quads. With r_models on the HRP draws a switch as a model standing proud of the plane its sprite describes; a box that
  encloses the model never competes with it for a plane.
- The box axes are the sprite's facing normal, the span the wall-sprite branch runs along (engine_priv.h:375-388) and world
  up, read off Polymer's transform (polymer.cpp:4455-4532): spos = (build_y, -build_z/16, -build_x) (:4457-4459); ang is the
  sprite's in degrees, less 90 (:4463, :4466); glRotatef(-ang, 0,1,0) turns the horizontals (:4500); glRotatef(-90, 1,0,0)
  sends (vx,vy,vz) to (vx, vz, -vy) (:4508), making the model's third component the vertical one; glScalef(scale*xrepeat,
  scale*xrepeat, scale*yrepeat) (:4531).
- The origin sits half a sprite height lower than a y-centred sprite's middle: build_dz = +2 * tilesiz.y * yrepeat downward
  (build z grows down), the same as the engine's sprite path adds (tilesizy * yrepeat) << 1. The engine's SPRITE path adds
  one more yrepeat when tilesiz.y is odd (engine.cpp:5383-5396) and Polymer's MODEL path does not; the box follows the
  model, so on an odd-height tile the two differ by 2*yrepeat build-z units.
- Polymost's own arithmetic for the bottom and top of a wall sprite is polymost.cpp:8502-8510 and :8535 (`yrepeat *
  tsiz.y * 4`), clipped at :8580-8595. Products are widened to 64 bits and clamped back, since yrepeat/yoff/tilesizy are
  unchecked engine fields.
- A wall-face tint covers the sector's whole first-wall face (the first wall and its point2 define the slope), not a
  tile-sized patch: bounding it to the texture would need repeat, panning and a two-sided wall's upper/lower split.

## Locating the player
The player is found by picnum, not by locating g_player, which GCC may have constant-folded out of existence (it is
declared `playerdata_t *const`, global.h:152).

## Method
Engine source reading.
