# Engine facts behind small modules: aim, cvars, damage numbers, mouse binds, tiles, hooks

Short findings that back single modules. Build: eduke32.exe r10669.

## Aim from the muzzle (aim.h)
- Aim is solved from the muzzle, not the eye. World Tour records (silentwt.cpp:369) that an eye-solved, muzzle-fired shot
  is a parallel line about pyoff + 1024 low, measured as 20 consecutive wall hits with the target still at full health.
- A refusal leaves the player's own shot untouched.
- `aim_solution_at` is for a caller that already has a muzzle and must agree with it exactly. The triggerbot casts its
  verification ray from the muzzle it computed, and passing that same origin in keeps the ray and the shot on one line
  instead of two. q16horizoff is passed rather than read because the engine subtracts it separately (player.cpp:498)
  and silent aim writes only q16horiz.
- Replicating World Tour's "which modifier applies" rule would need PLUTOPAK located and would miss any mod modifier;
  an observation misses none.

## Saving cvars (cvars.cpp)
The engine's "should this be saved to the config" test already answers yes for any value that differs from the cvar's
registered default (osd.cpp:105-116). The flag only covers explicitly restoring a default, so the panel's changes
persist into eduke32.cfg without the menu touching a second field of a record it found by shape.

## Damage numbers (dmgnum.h)
A_IncurDamage has four paths that return -1 having applied nothing, including a CON script vetoing
EVENT_PREACTORDAMAGE (actors.cpp:1319). World Tour pushes before the call, so it reports damage for all four. The
hook that locates A_IncurDamage is in [gamehooks.md](gamehooks.md).

## Mouse-bound game functions (mousebind.h)
- The SK_ bit values are plain #defines in duke3d/src/macros.h and were confirmed against the shipped binary: an `shl
  eax, 0x1a` at rva 0x4b3ce is SK_GAMEQUIT = 26, which fixes the numbering of the whole block.
- P_HandleSharedKeys edge-masks steroids, nightvision, medkit, holoduke and jetpack against `interface_toggle`
  (sector.cpp:2694-2699), so a held bit fires once and does not refire until the button is released.
- QUICK KICK is the exception: it is tested unmasked at sector.cpp:2683, guarded only by a 14-tic counter, so holding the
  button auto-repeats a kick roughly every 14 tics.

## Projection (project.cpp)
See [polymer-projection.md](polymer-projection.md).

## Tiles (tiles.cpp, tiles.h)
- `vec2_16_t tilesiz[MAXTILES]` (build/include/build.h:889) is a DIRECT array in .bss, not a pointer, so it is found
  differently from sprite[]: the locator looks for the array itself rather than for something pointing at it.
- The engine cannot draw a sprite whose tile is 0x0, but it happily HOLDS one and skips it. artReadManifest drops
  zero-size tiles (tiles.cpp:415-419) and six separate draw paths guard with `if (tilesiz[pic].x <= 0) return;`.

## Call-site redirects (hook.cpp)
A redirect rewrites the four operand bytes of a five-byte `call rel32`. The displacement is from the END of the five
bytes: 0x1000 + 5 + 0x0FFB = 0x2000 (synthetic addresses; real ones are re-derived at run time).

## Guarded reads
A write beginning at 0xFFFFFFFF and ending at 0 moved the counter, and moving makes a read bad on either side.

## Polymost globals and sites (AA image, r10669)
Every rva is a search result of this image, never an input.
- numbunches 0x1407E00, numscans 0x1407E02 and globalcursectnum 0x1407E04 form one 6-byte neighbourhood, which is what
  says the scan found the right words. The signature's last ten bytes are `mov rax,[rip+0x44A69C]` then `mov word
  [rax],dx`, adjacent.
- domostpolymethod: the global at rva 0x1991650 in .bss (0x05E5000..0x1A2C5C0) is named by 8 `mov r8d,[rip+d]` loads
  (0x01AEEBD..0x01AEEDB) feeding the 8 calls to polymost_drawpoly inside polymost_domost (0x01ACC90..0x01AF109).
  Elsewhere in .text it takes 2 `mov dword ptr [rip+d], -1` and 4 `, 0` immediate stores.
- xbl/xbr: polymost_domost's head holds exactly two `mov rax,[rip+d]; movss xmm,[rax]` pairs, at 0x01ACE26 and
  0x01ACE41. Their .rdata slots 0x059FBA0 and 0x059FBB0 name the .bss floats 0x13F1D88 and 0x13F1D84, four bytes apart,
  which is what `static float xbl, xbr, xbt, xbb` looks like. In this build the LOWER address is xbr and the higher xbl
  (read from the comiss/ja at 0x01ACE2A and 0x01ACE45), so a rule taking the first as the left bound is backwards.
- g_nodraw is reached through the .refptr slot 0x05A2050 -> rva 0x103E1C0. Sites: drawrooms stores 1 at 0x015874D
  (polymost.cpp:6993) and calls at 0x01587D2 (:6999); drawalls stores 1 at 0x01B0E14 and 0 at 0x01B0E44 (:5870-5873),
  also 0x01B0DE0 and 0x01B5F97; polymost_domost loads it 8 times, one per `if (g_nodraw)` (:4140-4318).
- The level global (`cmp [g], 8`): site A is polymost_drawrooms' load through a .refptr slot (polymost.cpp:6989), `mov
  rax,[rip+d]` at 0x015871D (slot 0x05A4C70) and `cmp dword [rax],8` at 0x0158724; site B is yax_drawrooms' one store
  of 8 before its base-level call (engine.cpp:1199), `mov dword [rip+d],8` at 0x012E7FA naming 0x04D50D8, 0x43 bytes
  before the call at 0x012E83D. Two independent sites must name one global.
- yax_polymostclearzbuffer: guard `mov rax,[rip+d]` at 0x01570B1, block at 0x01580D3 (glClear slot 0x05A2680).
- polymost_drawmaskwallinternal: entry rva 0x01BE270 saves eight registers, subtracts 0x198, spills xmm6-xmm15, then
  `movsxd r14, ecx` at 0x01BE2F0 (the first access to rcx, a read; `mov ecx, 3` at 0x01BE2F3 is a debug message's
  level). Callers: `mov ecx, ebx` at 0x016B231 before the call at 0x016B233 (inside 0x016AA40..0x016B6DF,
  polymost_prepareMirror, polymost.cpp:7466); `movsx ecx, word ptr [rax+rdx*2]` at 0x014B5D0 before a TAIL `jmp` at
  0x014B5E4 (inside 0x014AE30..0x014B630, polymost_drawmaskwall's two lines inlined, :7398). LTO's IPA-SRA removed
  polymost_scansector's parameter (callers write a global), so each hooked static function's ABI is read from the image.
- polymost_internal_nonparallaxed pushes eight registers, subtracts 0x168 and loads its ninth argument with `mov
  eax,[rsp+0x1F0]` (entry rsp + 0x48). polymost_domost's entry saves xmm6-xmm15 and loads arguments five to eight from
  [entry rsp + 0x28..0x40].
- The sweep budget: the reference ports halve it because their binary does not bound numscans/numbunches itself and the
  engine's polymost_drawalls scans each unmarked sector a second time after the hook returns. This binary removes only
  the first reason (polymost.cpp:6673 bounds numscans itself); a map needing more than the sweep's ceiling can still hit
  the guard and log "!!numscans". The engine's sky is a fake plane 32 m above the camera, one piece of which paints down
  into everything still open. polymost.cpp:7175-7400 makes no polymost_domost call at all.

## localInput (localinput.cpp)
- localInput is the 20-byte, mostly zero input_t (global.h:89) in a 21 MB .bss with no COFF symbol and no cvar record, so
  it is found by the instruction that writes it, and the displacements are the payload. Pattern, 42 bytes from rva
  0x4b3ad in r10669: the inlined early-out of P_GetInput, `48 C7 05 d32 00000000` mov qword [rip+d],0; `C7 05 d32
  00000000` mov dword [rip+d],0; `C7 05 d32 80000000` mov dword [rip+d],0x80; `8B 00` mov eax,[rax]; `C1 E0 1A` shl
  eax,26 (SK_GAMEQUIT); `89 05 d32` mov [rip+d],eax (localInput.bits).
- The last displacement is at pattern offset 38 and the instruction ends at +42, so localInput = matchRva + 42 + disp32,
  landing on `.bits`. It occurs once image-wide, inside one RUNTIME_FUNCTION; zero, several or a faulted section mean no
  feature.
- The write needs no clearing: drawframe_do calls P_GetInput (game.cpp:6485) and reaches videoNextPage (:6508) with
  nothing between touching localInput, and dukeFillInputForTic clears the record every tic (game.cpp:6542) as do the
  early-outs. A bit OR'd every frame is never cleared by the mod, on the game thread inside the SwapBuffers hook.

## Perf zones (perf.h)
With the free camera up the Polymost zone is mostly yax_drawrooms' per-bunch passes (engine.cpp:1072, :1160) plus
mirrors: hundreds of calls a frame on a TROR map. The sweep zone is nocull.cpp's seed thunk, a sibling of the cutaway's
drawalls zone.

## Method
Engine source reading; Ghidra on the stock build for the SK_ block; behaviour checked in game.
