# Secret path search: doors, lotags, sector effectors (secretpath.h)

The menu searches the map graph from the player to a secret sector and needs to know which sectors open.

## Door set
- isanearoperator (sector.cpp:199-218) is exactly lotags 9, 15-19, 20, 21, 22, 23, 25, 26, 29. The menu's door set
  drops 15-19: platforms and elevators keep their gap open and are judged on their step.
- World Tour's rule accepts 20-23 and 25-31. It omits ST_9_SLIDING_ST_DOOR (game.h:539, in isanearoperator), so a
  sliding ST door would be judged on its closed gap. It accepts 27 ST_27_STRETCH_BRIDGE, 28 ST_28_DROP_FLOOR, 30
  ST_30_ROTATE_RISE_BRIDGE and 31 ST_31_TWO_WAY_TRAIN, none of which is in isanearoperator: 27 drives an SE 20 and 28 an
  SE 21 (sector.cpp:1033, :1048), both move a FLOOR (a STEP, which the door flag does not waive); 30 toggles a bridge
  sprite's extra and 31 sets an actor's t_data[4] (sector.cpp:598, :609), and neither moves ceilingz. 24 is absent from
  game.h's enum.

## Lotag high byte
Bit 15 is the door open/closed flag and bit 14 is ACTIVATORLOCKED; the engine masks them (sector.cpp:201, :830).

## Stack
ekm_spath_search uses about 11 KB of stack.

Related: [secretvol.md](secretvol.md), [secret-triggers.md](secret-triggers.md).

## Method
Engine source reading (sector.cpp, game.h) against World Tour's rule.
