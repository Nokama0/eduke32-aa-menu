# EDuke32 cheat codes

Summary: the engine defines 27 cheat slots, 22 of them typeable. Alien Armageddon (AA) 5.91 adds, removes
and hooks none of them, so the stock table below is AA's table. `dndebug` dumps every gamevar to the log.

Method: the source rather than a wiki:
`source/duke3d/src/cheats.h` (the enum), `cheats.cpp` (the strings, the
descriptions and the switch that runs them) and `menus.cpp` (the Cheats menu),
in `eduke32 r10669-ec5824db8`.

**How they are typed.** The prefix is two keys held in `CheatKeys`
(`global.cpp:133`), by default `D` then `N`, so `dncornholio`, `dnstuff` and so
on. It is not Duke-specific: NAM uses `NV`, and WW2GI uses `GI` (its original
`WW` prefix is disabled in source because `W` collides with WASD movement).

**The Cheats menu** under Options lists 21 of them with their descriptions. A
cheat with an empty description does not get a menu row, which is why several
below are invisible unless you type them.

---

## The full table

27 slots exist; 22 have a typeable string. `###` and `#` are digits you supply.

| cheat | what it does |
|---|---|
| `dncornholio` | God mode toggle. |
| `dnkroz` | God mode. **Same handler as `dncornholio`** — a pure alias. |
| `dncgs` | God mode. The third alias, "come get some". |
| `dnstuff` | Everything: all weapons, full ammo on each, full inventory, all three keys. |
| `dnweapons` | All weapons only. |
| `dnitems` | Full inventory and all three keys. |
| `dninventory` | Inventory only. |
| `dnkeys` | The three access keys only. |
| `dnhyper` | Steroids to 399 and night-vision/heat to 1200. |
| `dnscotty###` | Level warp. Three digits: volume then level, e.g. `dnscotty214`. |
| `dnskill#` | Change skill, then restarts the level. |
| `dnview` | Third-person camera toggle. |
| `dnshowmap` | Reveal the whole automap. |
| `dnunlock` | Toggle every lock in the map (skipped in shareware). |
| `dnclip` | No-clip toggle. |
| `dnmonsters` | Cycles **three** states, not two: On, Off, On (2). |
| `dncashman` | Toggle. Money sprays out whenever you press Use. |
| `dncoords` | Coordinate display, cycling 0-2 (0-3 under OpenGL). |
| `dnrate` | Framerate display, cycling 0-3. |
| `dndebug` | Toggles debug mode **and dumps all gamevars to the log**. |
| `dntime` | **Nothing at all.** See below. |
| `dnbeta` | Prints an easter-egg quote. No effect. |
| `dntodd` | Prints an easter-egg quote. No effect. |
| `dnallen` | Prints an easter-egg quote. No effect. |

---

## The interesting parts

### Three names for one god mode
`cornholio`, `kroz` and `cgs` fall through to the same `case` block
(`cheats.cpp:471-473`). There is no behavioural difference between them.

### `dntime` does nothing whatsoever
Its handler is two lines, and the only statement that would have done anything
is commented out:

```c
case CHEAT_TIME:
    //                    P_DoQuote(QUOTE_21,pPlayer);
    end_cheat(pPlayer);
    return;
```

It has an empty description too, so it is absent from the Cheats menu. It
exists only so the string is recognised and swallowed.

### Three cheats you cannot type
Slots 14, 15 and 25 are `"<RESERVED>"`, and a reserved string cannot be
entered. Their handlers are real:

- **Slots 14 and 25** set `ud.eog = 1` and `gm |= MODE_EOL` — end of game.
- **Slot 15** sets `player_par = 0` and `gm = MODE_EOL` — end of level.

So the engine carries a working "finish the level" and "finish the game" cheat
with no way to invoke either by typing. They are reachable only from code — a
CON mod or a fork can rename them by writing into `CheatStrings[]`, which is
exactly what the NAM and WW2GI paths do at `cheats.cpp:164-180`.

### Six cheats survive Damn I'm Good
Cheats are refused on skill 4 with a "Cheats disabled" quote — except six
(`cheats.cpp:265-276`):

```c
if (!FURY && ud.player_skill == 4)
{
    switch (cheatNum)
    {
    case CHEAT_DEBUG:
    case CHEAT_COORDS:
    case CHEAT_RATE:
    case CHEAT_RESERVED:
    case CHEAT_RESERVED2:
    case CHEAT_RESERVED3:
        break;
    default:
        P_DoQuote(QUOTE_CHEATS_DISABLED, pPlayer);
```

The three typeable survivors are `dndebug`, `dncoords` and `dnrate` — they are
treated as diagnostics rather than cheats. The other three are the untypeable
end-of-level ones above, which means **the level-skip cheats are exempt from
the difficulty lock** even though nothing can reach them.

### `dndebug` dumps gamevars
```c
case CHEAT_DEBUG:
    g_Debug = 1-g_Debug;
    G_DumpDebugInfo();
```
This writes the full gamevar state to the log. For the Alien Armageddon work
that is directly useful: AA's XP and upgrade system lives in CON gamevars, and
this is a built-in way to see all of them without reading any memory.

### `dntodd` is a name, not a joke
In NAM the quote is replaced with the cheat's own description string, which is
`"Matt Saettler.  <email address>"`, a contact address left in the source
(`cheats.cpp:77`). Saettler did the NAM and WW2GI conversions.

### `dnmonsters` has a third state
Most references say it toggles. It cycles `g_noEnemies` through 0, 1, 2 and
prints "Monsters: On / Off / On (2)". State 2 is a separate mode, not a
duplicate of On.

---

## Relevance to the menu

- `dndebug`'s gamevar dump is a built-in way to see AA's XP/upgrade state
  without reading memory. It is also the independent cross-check for the
  menu's own gamevar lookup (see "`dndebug` is the verification oracle").
- `dncoords` and `dnrate` overlap with the menu's own readouts; on skill 4
  they are the only ones available.
- `dnmonsters` and `dnclip` are useful when testing ESP and the aimbot against
  a quiet map.

---

## Alien Armageddon 5.91

CON can change the cheat table: `gamedef.cpp` has five keywords for it,
`definecheat`, `definecheatdescription`, `undefinecheat`, `cheatkeys` and
`activatecheat`. **AA uses none of them**: zero occurrences across all 91
scripts in `scripts\` and the bundled Duke Caribbean set in
`maps\Caribbean\script\`. The stock table is AA's table.

### AA adds no cheats, removes none, and hooks none

It never takes `EVENT_ACTIVATECHEAT`, the hook that can remap or veto a cheat
by return value (`cheats.cpp:395`). The only cheat-adjacent event AA uses
anywhere is a single `EVENT_CHEATGETJETPACK`.

The `CheatStrings[]` rewrite at `cheats.cpp:164-180` is inside the NAM/WW2GI
game-mode branch, and AA ships the stock `eduke32.exe` (its SHA-256
matched against the install). That branch never runs.
AA's cheat prefix is `D`,`N` and its 22 typeable strings are the stock ones.
The three untypeable `<RESERVED>` slots stay untypeable under AA too.

### A name trap: `CHEATLEVEL` is not a cheat

`AADEFS.CON:71` defines `CHEATLEVEL 77` and `AAPRELOAD.CON:1095` has
`case CHEATLEVEL`. It is a **sprite tile number**: a map-placed sprite whose
hitag is multiplied by 1000 and added to the player's credits. Nothing to do
with cheat codes.

### `dndebug` is the verification oracle for the gamevar capability

AA's allies, characters and XP all live in CON gamevars, and reading gamevars
by name is the capability the menu depends on for them. This cheat is a free,
independent cross-check for it.

`G_DumpDebugInfo` (`game.cpp:1259`) writes to `eduke32.log`:

- **Every gamevar label and its flags**, via `Gv_DumpValues`
  (`gamevars.cpp`): a complete inventory of the table the lookup walks.
- **Per-actor values per sprite**: it walks every statnum, and for each sprite
  prints each `GAMEVAR_PERACTOR` var as
  `gamevar <szLabel> <value> GAMEVAR_PERACTOR`. That is `team` printed per
  sprite, the ally data the aimbot needs, straight from the engine.

It also names the structure the lookup walks: `aGameVars[i].szLabel`,
`.pValues[]`, `.defaultValue`, `.flags`, and `g_gameVarCount`.

Three caveats:

1. **The per-actor dump prints only values that DIFFER from the default.**
   `team` defaults to 0, which is "the aliens", so an ordinary hostile prints no
   `team` line at all. **Absence means 0, not unknown**; reading it the other
   way would invert the ally test.
2. **It writes a file.** The last statement is
   `saveboard("debug.map", ...)`, so `dndebug` drops a `debug.map` into the game
   folder (`<game folder>`). Harmless, but a write.
3. For a PERPLAYER or PERACTOR var, `Gv_DumpValues` prints `.global`, which is
   not where those values live. The label and the flags from that dump are
   trustworthy; the printed value is not. Per-actor values come from the sprite
   loop, not from there.
