# Locating sprite[] and the other engine arrays (gamestate.cpp)

How the menu finds the sprite array and what the locating scans cost. Build: eduke32.exe r10669.

## sprite
- `sprite` is a POINTER (build.h:619-621), loaded with `mov reg,[rip+disp]`. In r10669 it is at rva 0x0AEE360 (181
  rip-relative loads, 83 within twelve instructions of a multiply by 44 = sizeof(spritetype); the runner-up has 62
  loads, 21%).
- The debug build's adjacent sprite / wall / sector pointers do NOT transfer: both neighbouring slots have zero loads
  in release, and the enginePreInit pointer-block locator was removed for the same reason.
- The array is at rva 0x127C640 in 66 logged launches (enginePreInit's lea at rva 0x0155147 stores it into the sprite
  pointer). The slot the scan read it from varied: 64 times the engine's `sprite` at 0x0AEE360, 2 times the CON VM's
  vm.pSprite at 0x0DDC150 (same value), because "most free slots" is re-measured while sprites churn.
- A global holding &sprite[n] would pass every test and shift every index, so camera.cpp's decode of the engine's
  sprite load decides and this scan only witnesses it.

## Validation
- MAXSECTORS (4096) is a legal stored sectnum: initspritelists (engine.cpp:9380) and deletesprite (:8838) park free
  slots there. statnum = MAXSTATUS on free sprites (engine.cpp:8851), so a real array carries thousands of records with
  statnum 1024.
- A range-only validator accepts zeroed .bss; samples need non-zero coordinates and an exact match.
- No system call per candidate: VirtualQuery per 8-byte slot is about 2.7M kernel transitions and hung the game when it
  ran on the render thread every 2 s. Regions are enumerated once and merged; a fixed 128-slot table filled up and hid
  the rest, and the 720 KB array must lie in ONE entry.

## Game variables (gamevars.cpp)
- aGameVars cannot be found by label address: Gv_NewVar (gamevars.cpp:1084-1091) copies every label into a 26-byte heap
  block (Xcalloc(MAXVARLABEL, 1)), so no word holds a literal's address. The anchor is the 19 SPECIAL vars registered
  first, in fixed source order (gamevars.cpp:1681-1699), all with default -1 and one flags word; since none is per-player
  or per-actor, Gv_NewVar:1133 also stores -1 in `global`. That is 19 consecutive records each holding three known 8-byte
  words (456 bytes), then the ordered nineteen-label check. A view shifted by +8, +16 or +24 reads defaultValue's flags,
  the flags or a heap pointer where -1 is needed.
- Cross-check: COOP and MULTIMODE hold &ud.coop and &ud.multimode in `global` (gamevars.cpp:1783, :1790), and &ud is
  already known from two cvars ([ud-offsets.md](ud-offsets.md)).
- Gv_Free resets g_gameVarCount (gamevars.cpp:78) and a CON re-parse re-assigns indices in registration order, so a slot
  can come back holding a different var. Flags decide the shape: PERPLAYER 0x1, PERACTOR 0x2; an INT32PTR var (COOP,
  MULTIMODE, LEVEL, and the READONLY system vars `viewingrange` and `yxaspect`, gamevars.cpp:1834, :1843) holds a valid
  pointer in the same field, so a per-player read of a global record would dereference -1 and a global read of a
  per-player one would return its pValues pointer. Per-actor slots are bounded by MAXSPRITES, per-player by MAXPLAYERS
  (16) (Gv_NewVar:1112-1132).
- WEAPON%d_FIREDELAY and the other weapon fields (13 weapons x 10 fields = 130 names) are parsed from the labels of one
  walk of up to 2048 records, not resolved one by one; unresolved indices are -1, never 0 (which is `sprite`).
  SPAWNTIME and SOUND2TIME are read too.
- Gamevars the menu reads, all `gamevar ... 2` (per-actor) unless noted: droptile (the picnum a breakable spawns; AA reuses
  it as scratch in actors that are not breakables), monstatus, bottarget (AADEFS.CON:3975, read at the player's sprite),
  countvarc (:3962, YES on an armed Dukinator torso), countvar (:3960) and dashtime (:4076) (the worm boss's hit windows),
  myspawner (:3973), initflags (:3983, 2 on a Polymorph pad spawned growing over a body), monstflags (:3984, bit 131072
  marks a mimic). Per-player (131073): gotDB (AADEFS.CON:4329; AAPLAYER.CON:13300 `ifvarand gotDB 2` gates Mando's frame-6
  shotgun shot), dbammo (:3554), rightDB_weapcount (:4332, `0 1`), steroidstack (:3549), inven_upgrades (:3545).
  `mlevel_bonus` (:4023) is a non-peractor sibling of `mlevel`, so names are matched exactly.

## Sector array (sectorstate.cpp)
Same shape as the wall scan, reusing gamestate's region table (which already paid for a 128-entry cap that truncated
silently and for an unmerged table that could not hold a 720 KB array). The scan runs on the scan thread, never the
render thread; the pointer store is aligned and pointer-sized, so the render side sees NULL or a validated array.

## Scan budget and the 1 Hz sampler
- 900 attempts at 2 s is 30 minutes; then the loop is gone for the process. sectorstate needs >= 16 sectors, so small
  maps can lose sector[] permanently. Only the extent and the secret scans re-run each second on the 1 Hz sampler.
- What runs on the sampler and why: maskscan_probe waits for camera_scan's bracket (a cheap load and branch);
  modelbounds_scan can precede def parsing; secretvol / secrettrig / secretpath answers change on map load and when a
  found secret's lotag is zeroed; secretpath is the largest piece (map wall walk, sprite walk, BFS over 8192 edges) and
  runs last, its sub-tick running seven times between full scans.
- mapextent_ready is deliberately not in the loop's break test: it would keep the loop running and silence
  player_log_tick for 30 minutes, in exactly the run that debugs a missing extent.

## What each lost array costs
Without apScript a play-dead trooper reads as an ordinary corpse; without g_tile the ESP boxes every actor; without
wall[] the triggerbot reads every wall hit as blocked; without sector[] the free camera out of bounds draws the whole
map and no sky.

Related: [actorstate.md](actorstate.md), [tsprite-sweep.md](tsprite-sweep.md), [playerstate.md](playerstate.md).

## Method
Ghidra rip-relative load counts on the stock build; logged scan results over many launches.
