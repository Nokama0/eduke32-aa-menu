# When is an enemy sprite alive? (targets.cpp)

The rule the triggerbot, aim and ESP use to decide that an enemy sprite is a living target. The engine source alone does
not answer it; GAME.CON inside DUKE3D.GRP does.

## Three rules that were wrong
1. No test (from World Tour, aimwt.cpp:254 records that an `extra > 0` filter hid living enemies at zero health):
   corpses kept boxes.
2. `extra <= 0`, since G_InitActor stamps `extra = strength` at insertion (game.cpp:1331-1341, map sprites :1535). This
   covers spawn, not mid-life: CON_STRENGTH assigns extra on a running actor (gameexec.cpp:3504-3506), so a choking
   Assault Trooper reaches zero while alive.
3. `extra == 0 && !hittable`, from World Tour's note that Duke zeroes a corpse's extra and clears its cstat. It does not,
   except for two enemies. Assault Trooper clears cstat in `troopdying` (GAME.CON:4341), but only after `ifactioncount 5`
   and `iffloordistl 32` (not during the fall); Pig Cop in `pigdyingstate` (:3996-4000). Enforcer `lizdyingstate`
   (:5097-5102), Octabrain `ifaction AOCTADEAD` (:5618-5627) and Protector Drone `newbeastdyingstate` (:8352-8357) run
   `strength 0` on the corpse and never touch cstat: a settled Enforcer corpse is extra 0 cstat 257, the same pair a
   choking trooper has. Ten shotgun hits on LIZSTRENGTH 100 land exactly on zero. Applied to every enemy, this rule made
   Enforcer, Octabrain and Drone corpses permanent targets.

## The rule in use
The engine's own dead predicate `extra <= 0` (CON_IFDEAD gameexec.cpp:3325-3327, VM_Move deadflag :739, A_Dodge :309,
VM_AlterAng :634; a strict `<` would miss exact kills), `pal != 1` for the frozen enemy, plus the cstat term scoped to
trooper picnums (ekm_trooper_choking, targets.h). Playing-dead troopers clear cstat like a real corpse and are not
separable in sprite[]; the CON action record tells ([actorstate.md](actorstate.md)).

## Stayput variants
g_tile has no badguy bit for variant tiles, so the stayput variants are listed by picnum (esp_names.h
ekm_picnum_is_stayput).

## Collection cap
The sweep of s_deferSticky runs for every slot even when the list is full: a flag stranded on a high slot is inherited by
the next sprite spawned there.

The per-enemy dying and choking details: [targets.md](targets.md).

## Method
GAME.CON from DUKE3D.GRP, the engine source, and logged sprite state of killed enemies (extra, cstat, pal, action).
