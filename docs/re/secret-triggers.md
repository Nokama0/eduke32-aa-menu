# Secret triggers: activators and master switches

How the menu finds secrets that are opened by an activator or master switch (secrettrig.h).

## Sprite list facts
- Free sprite slots are stale: initspritelists resets only sectnum and statnum (engine.cpp:9380, :9394); deletesprite
  only moves the statnum and sets sectnum (engine.cpp:8830-8838). MAXSTATUS is build.h:98.
- Tiles: names.h:25 TOUCHPLATE 3, :26 ACTIVATORLOCKED 4, :30 MASTERSWITCH 8. Statnums: actors.h:57 STAT_ACTIVATOR 8,
  :55 STAT_STANDABLE.

## Engine behaviour
- G_OperateActivators is sector.cpp:1117 (it operates from :1126 onward).
- G_OperateMasterSwitches (:1179) arms SP(i)=1 (:1181); the actor then counts hitag down and calls G_OperateSectors
  (actors.cpp:2556-2560). The spawn case is game.cpp:2823-2827.
- ACTIVATORLOCKED toggles `sector[...].lotag ^= 16384` (sector.cpp:1126). G_OperateSectors switches on
  `lotag & (uint16_t)~49152u` (sector.cpp:596); the sector-effector fan-out is gated on `lotag < 3` (:1146).

Related: [secretvol.md](secretvol.md), [secret-path.md](secret-path.md).

## Method
Engine source reading.
