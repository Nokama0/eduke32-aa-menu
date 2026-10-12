# actor[] and apScript: locating them, and what they answer (actorstate.cpp)

The menu reads the engine's per-sprite `actor[]` records and the compiled CON script image (`apScript`) to tell a
play-dead trooper from a corpse and to read a dying actor's action record.

## actor[]
- actor[] has a content anchor: every live sprite's position is mirrored in actor[i].bpos at the head of each tic
  (G_RecordOldSpritePos, actors.cpp:9286-9331). The scan looks for an array whose records agree with sprite[] index by
  index.
- Lessons carried over from the sprite scan ([gamestate.md](gamestate.md)): no system call per candidate (VirtualQuery
  per slot was 2.7M kernel transitions and killed the process); a validator that can fail (zeroed .bss passed a
  range-only test, so samples need non-zero coordinates and an exact match on three int32s); failure logs say how far
  the scan got.
- `htowner == i` is NOT an invariant: actors.cpp:1348, :1366, :3326 and :3701 set it to other sprites' indices; only
  :1601 sets it to its own (see actorstate.h).

## apScript
- apScript is solved for, not scanned. g_tile[pic].execPtr (offset 0 of tiledata_t, actors.h:315) is assigned
  `apScript + g_scriptActorOffset` (gamedef.cpp:3261) and badguy.cpp has located g_tile. One VirtualQuery bounds the
  allocation, and the base is the single address where TWO KNOWN ACTION RECORDS decode exactly:
  - `actor LIZTROOP TROOPSTRENGTH ATROOPSTAND` (GAME.CON:4780) with `action ATROOPSTAND 0 1 5 1 1` (:4008);
  - `actor LIZTROOPSTAYPUT TROOPSTRENGTH ATROOPSTAYSTAND` (:4740) with `action ATROOPSTAYSTAND -2 1 5 1 1` (:4010).
- Compile order of execPtr: [0] strength, [1] action, [2] move, [3] ai (gamedef.cpp:3288-3311; read by G_InitActor,
  game.cpp:1335-1340). The two actions are declared two lines apart, so their ids differ by exactly two strides, a
  check that needs no apScript. ATROOPGROW is :4009.
- Ambiguity (several bases decode) is a refusal. The lowest base would be a guess that decodes every trooper's action
  from the wrong place with solved confidence. A refusal leaves actorstate_is_playing_dead false.

## Dying phase
Assault Commander's dying phase: `ifai AICOMMDYING` runs `strength 0` every tic while cstat stays hittable
(GAME.CON:7372-7392), so actorstate_dying_record scopes to troopers and commanders. The rules that use these answers:
[target-liveness.md](target-liveness.md), [targets.md](targets.md).

## CON action records and picnums
Trooper down-states, GAME.CON:4018-4022 (startframe numframes viewtype incval delay): ATROOPDYING 50 5 1 1 16; ATROOPDEAD 54;
ATROOPPLAYDEAD 54; ATROOPSUFFERDEAD 58 2 1 -4 24; ATROOPSUFFERING 59 2 1 1 21. AA's, NEWTROOP.CON:29-33 in the same order:
ANEWTDYING 101 6 1 1 16; ANEWTDEAD 106; ANEWTPLAYDEAD 106; ANEWTSUFFERDEAD 98 1 1 1 30; ANEWTSUFFERING 99 2 1 1 21.
Commander: ACOMMDYING 30 8 1 1 12 (:7126), ACOMMDEAD 38 1 1 1 1 (:7127). AOCTADEAD (GAME.CON:5450) shares ACOMMDEAD's five
values, hence the picnum scope.

Picnums: LIZTROOPONTOILET 1741 (names.h:435), LIZTROOPJUSTSIT 1742 (:436), LIZTROOP 1680 (:428); AA: NEWTROOP on toilet
16993 / just sit 16895 / standing 16896 (AADEFS.CON:2246, :2247, :2239); NIPTROOP 24417 / 24439 / 24320 (:2939, :2940, :2933);
COMMANDER 1920 (names.h:451); NEWRECON 16865 (AADEFS.CON:2234, stock RECON 1960); PIGSUV 6374 (:1050); TERMTORSO 6227
(:1028), TERMINATORCRAWLING 6194 (:1027); AA YES = 1 (AADEFS.CON:3165).

## Method
Ghidra on the stock build, GAME.CON from DUKE3D.GRP, and the engine source for the record layouts.
