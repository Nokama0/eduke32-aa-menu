# Target selection: dying, choking, hittable and deferred damage (targets.h)

A target is scoped by picnum, with sitters excluded, frozen enemies excluded and a suffering action required for the
choking case. This note holds the per-enemy facts. The liveness rule itself: [target-liveness.md](target-liveness.md).

## Choking and dying troopers
- Stock GAME.CON `checktroophit` (:4415-4421) gives a choking trooper `strength 0` and leaves cstat. Every real death
  state clears cstat first: `troopdying` (:4341-4349, clears at ifactioncount 5 = settle), checktroophit's suffering
  branch (:4362-4368), `troopsufferingstate` (:4490-4502). So for a trooper a cleared cstat is a death signal. Not for
  the Enforcer (:5097), Octabrain (:5618) or Protector Drone (:8352): they hold `strength 0` on a corpse whose cstat
  still reads 257.
- A killing hit landing on exactly zero reads alive for the ~1 s ATROOPDYING fall.
- Sitting picnums 1741/1742 carry the stock hittable cstat. They cannot be choking, because ATROOPSUFFERING is reached
  only from `troopcode`, which a sitter does not run until `cactor LIZTROOP` has changed the picnum to 1680
  (:4750-4777).
- A frozen trooper (freeze branch :4373-4381: spritepal 1, ATROOPFROZEN, strength 0, cstat kept) is picnum 1680, extra
  0, cstat 257, pal 1: the choking pair bit for bit. They are mutually exclusive in the CON (freeze `break`s before
  suffering; a suffering trooper has cstat cleared at the top of checktroophit; thaw restores pal via getlastpal
  :4531-4535).
- ATROOPSUFFERING (choking) and ATROOPDYING (falling, extra 0 pal 0 cstat 257) are different action records;
  ekm_action_is_suffering tells them apart. EKM_SUFFER_UNKNOWN (the decoder has no answer: actor[] / apScript not
  located, or t_data[4] zero) keeps the unscoped behaviour.
- The sticky deferral flag is only SET by a sitting observation, unlike the "discount every actor" rule World Tour
  warns off.

## Deferred damage
A_DamageObject accumulates in actor[].htextra; health changes on the victim's own next CON tic (actors.cpp:1322), so
~33 ms of lingering is unavoidable without reading actor[].

## Hittable cstat
ekm_target_hittable uses bit 8 alone, as the engine does. World Tour's mask 0x8101 / value 0x101 also demanded bit 0 and
cost 7162 refusals.

Related: [actorstate.md](actorstate.md).

## Breakable objects (esp_breakobj_aa.h, generated)
- The map's lotag is no alternative to the `droptile` gamevar: AAPRELOAD.CON's copy ends `seta[].lotag 0`, so the lotag is
  zeroed once the value is taken and the PERACTOR gamevar is the only surviving copy. `droptile` is filled at map load by
  AAPRELOAD.CON:1556-1568 and spawned by `espawnvar droptile` at the end of `state breakobject`. AA reuses `droptile` as
  scratch (a wave number, a palette, a sector z) in actors that are not breakables, so it means nothing until the sprite's
  actor is known to run `state breakobject`.
- Membership is the CON call graph, transitively; AA writes many as `useractor notenemy TABLET1 0 state flashercode state
  breakobject enda` and some as multi-line blocks. The last declaration wins (CON_ACTOR overwrites execPtr, gamedef.cpp:3195-3263,
  no duplicate error): DANCINGPANDA is declared twice (INTERACT.CON:2189, :2344); 313 call sites fall on 312 names. A raw
  grep for "state breakobject" over INTERACT.CON over-counts (the defstate line and commented-out actors at :1988, :2040,
  :2403).
- The header is a frozen reading of one install: every CON file read is recorded with its size, never its mtime.

## Coin magnet (coinmagnet.cpp)
The instant path works because A_Execute runs the coin CON before VM_Move (gameexec.cpp:7015-7021), `ifpdistl 896`
measures from the player's opos (sector.cpp:221-226), and P_ProcessInput sets opos = pos before moving (player.cpp:5316-5319,
clipmove :5772) and runs ahead of G_MoveWorld (game.cpp:7339-7345). A coin written at pos.xy, pos.z + (28<<8) reads
distance 0.

## Chests (esp_chests_aa.h)
- EDF chest: tile 179 (EDFCHEST, AADEFS.CON:125, a CON define). Its map hitag picks one bit of `startguns_perma`
  (AADEFS.CON:3245, NORESET, saved) through the table at AADEFS.CON:3246-3258, not 1<<(hitag-1); AAITEMS.CON:4352-4389
  reads it to restore "already opened" at load. EVENT_LOADACTOR copies the hitag into `initsprite` without zeroing it
  (AAPRELOAD.CON:546-552), so sprite.hitag still holds it.
- Bomb-part chest: tile 177 (ITEMCHEST), hitag 2600..2604 = BOMBPART1..5, the tile it spawns. The part sets `mcguffins` bit
  (hitag - 2600) when taken (AAITEMS.CON:4538-4542); the chest's own open state is not restored at load, so that bit is the
  only signal. The mask has one bit per weapon, so a duplicate hitag still counts once.

## Companions and mimics (aa_companion.h)
Companion bots are every pose tile of DUKEBOT, BOMBSHELL, WESBOT and DEABOT.CON (`useractor enemy`). An unread monstflags
is not a companion. Team -1 is the leftover from the last level, `killit` the same tic (WESBOT.CON:1793): hostile. Team 0
at monstatus 1 is a live mimic even without the monstflags bit: AA's ice aura overwrites monstflags whole (AAGAME.CON:23550)
and later xors it to 0 (:12209). A waiting bot reads monstatus 0 and a recruited one 30, so both reads are required.
The Polymorph pad/pole/pod (7275/7270/7280, `state polybackstate`) cannot be hit while growing over a body (`ife
initflags 2` grows the pad before ifhitweapon); APOLTRANSBACK is entered only past that break or after `sizeat 32 28`
(:751, :759), so the xrepeat != 32 test over-refuses two tics: the spawn tic's action 0 and the tic whose `sizeto` takes
xrepeat 31 to 32 before the `ife` test lets the hit in.

## Pickup range (pickuprange_aa.h)
Only AAITEMS.CON actors with AA's standard gate (`ifpdistl RETRIEVEDISTANCE` then `ifcanseetarget`; ACCESSCARD: either
`ifpdistl 384` or sight, :405-406) may widen. Never: GLOCKSPRITE (a "press use to swap" prompt, :2042-2043), the
TRIPBOMBSPRITE (armed ones share it), VENDORUPGRADE / VRREWARD / TOKYOJIHEN (story), figurines, EDF and ITEM chests (use
key), BOMBPART1-5 (BOMBPART5 ends the level), FECES (a hazard at 844), credit stacks and coins (the coin magnet's),
TOILETWATER, CHICKEN. The keycard's automap twin shares ACCESSCARD's picnum and breaks before its gate (`ife float 32768
break`, AAITEMS.CON:380).

## Why the item census walks the other way (itemcensus.h)
esp_collect_items (esp.cpp:815-837) tests statnum before it asks ekm_item_name_pal, so it has already dropped every pickup
on another status list when it knows a sprite is a pickup. The census asks the name first and replays the guards after,
the only ordering that answers "why is this named pickup not on screen". It was built for AA's 51 MINI* figurines: named,
54 of them in CKBASE.map, ammo in the same room draws, and no figurine box ever appeared. A sprite that is both
wrong-statnum and hidden reports STATNUM, since the walk never reads the cstat there. The bit-15 mask arrives
sign-extended from a short, which is why ekm_item_present uses a mask and not an equality. A wall-clock subtraction is
unsigned so a wrap reads as a small elapsed time.

## Method
GAME.CON from DUKE3D.GRP and the engine source; refusals counted in the mod's log.
