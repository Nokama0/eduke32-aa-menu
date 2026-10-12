# Calling the engine's `hitscan`: locate, side effects, self-test (hitscan.cpp, hitscan.h)

The menu casts rays with the engine's own `hitscan`, found by content. Build: eduke32.exe r10669.

## Locating hitscan
- Anchor: `LOG_F(ERROR, "hitscan: tempshortnum >= MAXCLIPSECTORS at (%d,%d,%d)!")` at clip.cpp:2985-2986 (source text
  survives optimisation); the function is whichever references it. Checked against clip.cpp:2836-3120: the string
  occurs once at rva 0x051C880; exactly one rip-relative lea r64 in .text resolves to it, at rva 0x0119C1C, inside the
  PE exception-directory function 0x0118EF0..0x011AEBC. The entry is found via the exception directory, not a fixed delta.
- Its first fifty instructions: (1) `mov rsi,[rsp+0x1F8]`, `mov eax,[rsp+0x200]` = the 6th/7th arguments hit and
  cliptype (build.h:1345); (2) `mov word [rsi+0x10],-1` then `mov dword [rsi+0xC],-1` = clip.cpp:2851 sect / wall /
  sprite = -1, which also confirms the hitdata_t order; (3) `test bx,bx` / `js` :2852-2853; (4) `movzx edi,ax`, `shr
  eax,0x10` :2848-2849 the cliptype split; (5) an 8-byte global load into [rsi+0] :2855 (hitscangoal); (6) `movsx
  esi,word[..]; add esi,7; sar esi,3` then three memsets :2864-2868 (ceilmap, floormap, clipsectormap, so YAX_ENABLE);
  (7) `movsx eax,bx; sar eax,3; and r10d,7; or byte[rsi+rdx],al` :2869. The log call carries `mov r8d,0xBF1` = 3057
  (its line).
- cansee.cpp matches 53 bytes of compiled loop (compiler-dependent); hitscan locates by content instead
  ([cansee.md](cansee.md)).

## Side effects of a cast
- Mutation list: clip.cpp:2857-2869 writes clipsectorlist[0], zeroes clipsectormap and yax_hitscan_ceilmap/floormap
  over bitmap_size(numsectors) bytes, clears clipspritenum and clipmove_warned, and carries tempshortnum, which
  hitscan_addclipsect (:2824-2834) pushes into the same clipsectorlist as clipmove / cansee / neartag.
- Shooter bits: Proj_DoHitscan (player.cpp:548-560) clears `cstatmask` on the shooter's sprite, runs updatesector and
  hitscan, then restores; the mask is 256+1 for SHOTSPARK1 / SHOTGUN / CHAINGUN (player.cpp:1207) and GROWSPARK (:1240).
  A logged play blocked 940 rays of 10927 while the player killed the same trooper by hand. The menu's write is
  restored in the same hook call. A faulted restore leaves the player non-blocking: the next cast reads the cleared
  value and writes nothing, and a level load recovers it. That needs the sprite array freed inside one SwapBuffers
  hook call.
- Origin sector: a ceiling-turret test fired 45 rounds and every ray reported the turret. Same angle and pitch (vz
  -348160 = (100-270)<<11 against zvel -5440 = (100-270)<<5); the one differing term was the start sector
  (player.cpp:548-560 runs updatesector at the origin; the muzzle is pyoff + ZOFFSET6 below the eye, and a jetpack
  crosses boundaries).
- Bounds: cansee checks both sectors against MAXSECTORS (engine.cpp:12747-12748); hitscan checks only `sectnum < 0`
  (clip.cpp:2852) and then `bitmap_set(clipsectormap, sectnum)` (:2869), with clipsectormap = bitmap_size(MAXSECTORS)
  (clip.cpp:21). An out-of-bounds sector is an engine-global write no `__except` can undo.

## updatesector witnesses
- updatesector passes INITIALUPDATESECTORDIST (256) 4th and MAXUPDATESECTORDIST (1536) 5th (engine.cpp:13367), which
  are r9d and [rsp+0x20]; updatesectorz has z first, so [rsp+0x20], [rsp+0x28]. Of the 8554 functions with End > Begin
  (of 8561 pdata rows) exactly one matches, and exactly one other the z-shape. Prologues are byte-identical for 16 bytes.
- The call before the engine's `call hitscan` alone fails as a witness: the image has twelve `call hitscan` sites,
  three with a preceding call, resolving to updatesector (Proj_DoHitscan), updatesectorz and a gamevar reader. So the
  shape identifies and the call site corroborates.
- updatesector sites in the menu: the SwapBuffers hook; the polymer_drawrooms bracket (polymer.cpp:1175 makes this call
  on the same arguments 3 instructions later); the G_DrawRooms hook (outer of the camera's two hooks, earlier than the
  inner site; it asks whether the camera's point is inside any sector, the transport gate in camera_transport_tic). The
  engine resolves sectors in its own draw path (updatesectorz, game.cpp:1088, :1103, via updatesectorneighborz
  engine.cpp:13489, :13546). The witness is not Proj_DoHitscan (a firing path). This requires hitscan armed: one
  witness precedes the engine's `call hitscan`.
- hitdata_t binary confirmation: the located function opens with `mov word [rsi+0x10], -1` (sect) and `mov dword
  [rsi+0xc], -1` (sprite = wall).

## Self-tests
- hitscan: a bad-sector call (sectnum -1) returns -1 and leaves three -1s over a 0x7777 sentinel. That establishes
  argument order, the 16-bit argument, the hitdata offsets and the return convention, and touches no state. The live
  half casts from an engine-given sector (returns 0); what it hits is logged, not asserted. The test ray points due
  east: vx 16384 (sintable[512]), vy 0 (engine.cpp:8661-8663); vz 0 is ordinary and both hitscan_trysector branches
  guard their divisor (clip.cpp:2653, :2665).
- updatesector: with the player inside their sector getsectordist is 0 and the value comes back unchanged
  (engine.cpp:13509-13519, :13366-13371). From -1, updatesector_tryremaining scans all sectors (:13308-13330) and must
  find the player's sector (a no-op leaves -1). Which sector is logged, not asserted (inside_p is 2D; TROR).

## Slope z (slopez.h)
Ported from getceilzofslopeptr/getflorzofslopeptr (engine.cpp:14250-14286), read as the image was built. platform build
props set NOASM=1 for x64, so nsqrtasm is engine_priv.h:138-145's portable fallback built from msqrtasm (engine.cpp:8461)
and initksqrt's table fill (:8486-8499); uhypsq is build.h:1391; dmulscale3 and scale take pragmas.h's generic-C path.
enginecompatibilitymode is declared `extern` at build.h:1097 and defined at engine.cpp:195 as ENGINE_EDUKE32; nothing
under duke3d/ reassigns it, so the `shift` at engine.cpp:14265/14284 is always 1 (taken on the source, not located).
uhypsq widens each term to uint32_t before adding and wraps like the engine; only the two products inside dmulscale3 and
scale widen to int64_t. `stat` and `heinum` are one surface's own fields (ceilingstat+ceilingheinum or
floorstat+floorheinum), never the sector's OR'd `sloped` summary.

## Aim arithmetic (aimsolve.h)
- Build z is sixteen times the horizontal unit, so a rise is dz/16 before dividing by a horizontal distance; z grows
  downward while the view angle grows upward.
- The engine masks q16ang to EKM_YAW_UNITS_Q16 - 1 (player.cpp:3080); yaw is 2048 units to the turn, so 180 degrees is
  1024 and angles wrap the short way (yaw 2040 and 10 are 18 apart).
- The pitch quantum is one horiz unit = 1/128 of the tangent, displacing the impact by d * EKM_Z_PER_XY /
  EKM_HORIZ_PER_SLOPE z units. At a measured range of 5873 one unit is 734 z, against a ceiling turret 3040 tall.
- 39 chaingun rounds at a ceiling turret landed 3, aimed at 0.75 of the body from 5873 away (margin above 380 z); the
  pistol hit every time because accurateAim skips Proj_MaybeAddSpread, whose spread was four quanta wide.
- ekm_shot_ray builds the engine's zvel as (100 - horiz - horizoff) << 5 and hands hitscan that shifted left 6 again
  (shotray.h, player.cpp:498-501), so the term is shifted left 11: 4096 << 11 is 8388608, eight bits of int32 headroom.
  vx/vy keep the sintable's 16384 amplitude, so the steepest shot is atan(4096/128) = 88.2 degrees from horizontal.
- Silent aim substitutes q16horiz inside the shot call, after P_GetInput's clamp, and the engine derives zvel from it with
  no clamp (player.cpp:521, the chaingun/shotgun path), so the shot is not bound by -99..299.
- On the jetpack, 16 of 105 logged cast-to-fire samples read dz exactly -2048 (rounds miss); 2048 is the jetpack's fixed
  per-tic ascent, which the shot leaves above the pitch's origin. The 88 standing samples read dz +0.
- G_DoSpriteAnimations draws every actor at bpos + smoothratio * (pos - bpos) (game.cpp:3882-3884) and the camera is
  interpolated from opos the same way, while targets_collect reads the raw tic position. The engine moves an actor once
  per tic, so the delta between two CHANGED readings is one tic of motion. A tic is 33.333 ms; the acquisition gate is 33
  ms on a clock that counts whole milliseconds.
- Occlusion ladder: a cast that returns no answer is not visibility; a wall hit leaves hit.sprite at -1, so `hit.sprite ==
  target` against a target of -1 would accept every wall. On E1L4 a run of 8..8 of 9 samples on a body 14560 tall missed
  its aim point by 207 z; half a sample step there is 682 z.

## AA hitscan slots
Which AA weapon slots fire hitscan and which fly, including the Penetrator on the Devastator slot: [projectiles.md](projectiles.md).

## Method
Ghidra on the stock build with the PE exception directory for function bounds; the located function checked against
clip.cpp instruction by instruction.
