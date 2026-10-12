# Chaingun fire-rate floor (CON frame classification)

Summary: the chaingun (w03) has a floor of 12 against a `TotalTime` of 12, so a
fire-rate mechanism that SHORTENS THE CYCLE (`TotalTime`) gains 1.00x. The
binding frame is 11, a `shoot` on the shock chaingun. The two narrower readings
give 1.20x and 1.08x. A cycle-shortening mechanism is worth building only above
1.75x (floor 7 or lower at `TotalTime` 12; the old counter advance gave 2.0x),
so it is rejected for this weapon. The shipped fire rate does not shorten the
cycle: it skips unused frames (section 7). The pistol's equivalent is in
`pistol-floor.md`.

Method: the row's `TotalTime` read from AA's CON; every `kickback_pic` site
listed per site by `tools/gen_con_stops_aa.py` (static, by reading).

---

## 1. The row, and where each number came from

    w03 FIREDELAY=3 TOTALTIME=12 RELOAD=60 SPAWNTIME=1 SOUND2TIME=0 HOLDDELAY=3 WORKSLIKE=3

The row comes from the menu's weapon-row dump. The log is truncated on every
launch, so `TOTALTIME=12` is corroborated from AA's own CON:

* `<game folder>\scripts\AAPLAYER.CON:344` -- `set WEAPON3_TOTALTIME 12`, in the
  base chaingun setup. The same value is set at :783, :1088 and :1322, one per
  playable character.
* `AAPLAYER.CON:326` -- `set WEAPON3_TOTALTIME 14`, in the **shock** chaingun
  setup (also :764, :1069, :1304). **The chaingun has two cycle lengths**, and
  section 4 turns on which one is in force.

`FIREDELAY`, `SPAWNTIME` and `HOLDDELAY` are not re-read. They do not bind the
floor (3, 1 and 3 are far below 11).

## 2. How the sites were found

`tools/gen_con_stops_aa.py` produced `con_stops_aa.h`. It reproduces the
committed header byte for byte (21892 bytes, 81 script files, 371
`kickback_pic` references). Its `Extractor.record` was subclassed to emit one
row per SITE rather than a set of frames, so every frame carries a `file:line`.
The parser's rules are unchanged.

**`kW03` (`con_stops_aa.h:254`) holds STOPS, not frames.** A stop is a value the
fire-rate advance may leave the counter on, derived per operator as `F - 1`
and/or `F` (the header's own off-by-one). The stops listed are
`1, 2, 3, 4, 5, 7, 8, 9, 10, 11`. The parser's FRAMES for weapon 3 are

    1, 2, 4, 5, 8, 9, 11, 12, 14, 17, 40, 99

and frames are what a floor is about: a CON `ife kickback_pic 11` needs frame 11
to be REACHED. Frames 1-11 are classified below; 3, 6, 7 and 10 have no
chaingun site.

## 3. Frame by frame

| Frame | Verdict | `file:line` | What the CON does there |
|---|---|---|---|
| 1 | **LOAD-BEARING** | `AAPLAYER.CON:12990` | `ifg player[].kickback_pic 1 setp[].kickback_pic 1` -- Duke's spin-up clamp: while `spinup < 10` (:12987) the counter is pinned at 1 so the barrel spins before the gun fires. Writes `kickback_pic`. |
| 1 | **LOAD-BEARING** | `AAPLAYER.CON:13770` | The same clamp on Nova's path (`ife pchar 4`, :13723; `curr_weapon CHAINGUN_WEAPON`, :13725). |
| 1 | **LOAD-BEARING** | `AAPLAYER.CON:13414` | `ifvarand player[].kickback_pic 1 nullop else` -- parity gate standing in front of the rifle clip reload `set m4reload 1` (:13420) and `setp[].kickback_pic 0` (:13427). |
| 1 | **LOAD-BEARING** | `AAPLAYER.CON:13543` | The same gate in front of the Uzi clip reload `set uzireload 1` (:13549), `setp[].kickback_pic 0` (:13556). Block gated on `curr_weapon CHAINGUN_WEAPON` at :13534. |
| 1 | **LOAD-BEARING** | `AAPLAYER.CON:14539` | `setp[].kickback_pic 1` in the shock chaingun's single-shot branch, which also spends five rounds (:14541-14543). |
| 2 | **LOAD-BEARING** | `AAPLAYER.CON:13732` | `setp[].kickback_pic 2` -- Nova's burst fire; also `add overheat 1` (:13733) and it can clear `WEAPON3_SHOTSPERBURST` (:13742). |
| 2 | **LOAD-BEARING** | `AAPLAYER.CON:23060` | `setp[].kickback_pic 2` -- Nova's laser-minigun alt-fire; sets `WEAPON3_SHOTSPERBURST 3` (:23062) and `orvar WEAPON3_FLAGS 8` (:23063). |
| 3 | no CON site | -- | No `kickback_pic 3` site attributes to weapon 3. The engine's `FireDelay 3` constrains this frame; the CON does not. |
| 4 | **LOAD-BEARING** | `AAPLAYER.CON:20742` | `case CHAINGUN_WEAPON ifl player[].kickback_pic 4 set temp NO else setp[].kickback_pic 0` -- the quick-kick gate in `EVENT_DOFIRE`'s neighbour at :20693: below frame 4 the kick is suppressed, at or above it the firing cycle is **cancelled** so the kick can run. |
| 5 | **LOAD-BEARING** | `AAPLAYER.CON:21284` | `ife player[].kickback_pic 5 set temp YES` -> `add B 1` / `setp[].ammo_amount CHAINGUN_WEAPON B` (:21288-21290). Duke's ammo-saver upgrade (`dukeupgrades[CHAINGUN_WEAPON] 1`, :21281) refunds a round. **Ammo movement**, inside `EVENT_DOFIRE` (:20936). |
| 5 | not this weapon | `AAPLAYER.CON:13682` | `setp[].kickback_pic 5` reaches this file's uncertain list, but its enclosing block is gated on `GROW_WEAPON`/`SHRINKER_WEAPON` at :13615-13617 (`ife temp YES`, :13617). It is Wes's SKAR, not the chaingun. |
| 6 | NO CON SITE | -- | See "HUD lines" below. |
| 7 | NO CON SITE | -- | See "HUD lines" below. |
| 8 | **LOAD-BEARING** | `CHEERBOT.CON:326` | `ifn player[].kickback_pic 0` / `ifle player[].kickback_pic 8` gating an enemy's dodge decision, with `case 3` naming the chaingun at :330. It changes no weapon state, so it is classified LOAD-BEARING ("anything you cannot classify is LOAD-BEARING"). **It imposes no floor of its own**: it is a RANGE `1..8`, and frame 1 exists at every cycle length. |
| 9 | **LOAD-BEARING** | `AAPLAYER.CON:21285` | `ife player[].kickback_pic 9 set temp YES` -> the same ammo refund as frame 5 (:21288-21290). Non-shock chaingun: the block is `ifvarand gotshock 2 nullop else` / `ife player[].curr_weapon CHAINGUN_WEAPON` (:21226-21227). |
| 9 | **LOAD-BEARING** | `AAPLAYER.CON:14502` | `setp[].kickback_pic 9` -- the shock chaingun's own sub-loop takes frame 12 back to 9. |
| 9 | **LOAD-BEARING** | `AAPLAYER.CON:14529` | `setp[].kickback_pic 9` starting chain lightning; spends a round at :14531-14533. |
| 10 | no CON site | -- | No site names frame 10 for this weapon. |
| 11 | **LOAD-BEARING** | `AAPLAYER.CON:14485` | `ife player[].kickback_pic 11 { ... shoot SHOCKBEAM ... }` (the `shoot` is :14494). **This frame fires the shot.** Inside `ife player[].curr_weapon CHAINGUN_WEAPON` (:14453) and `ifvarand gotshock 2` / `ifvarand dukeupgrades[CHAINGUN_WEAPON] 8` (:14465-14466). |

Frame 12 is not below `TotalTime` 12, but it is the other half of the shock
loop: `AAPLAYER.CON:14500` is `ife player[].kickback_pic 12` and its body is the
`setp[].kickback_pic 9` above.

**HUD lines.** `weapons_hud.CON:4559-4562` (`:4559`, `:4560`, `:4561`) and `:9346-9349`
(`:9346`, `:9347`, `:9348`) look like
`kickback_pic` sites for frames 4-7 but are not the chaingun's: they are
`draw_newrpg`, the RPG's HUD zoom-scope draw state, reached only from RPG
branches. The chaingun's own HUD block, `weapons_hud.CON:8390-8558`, names no
`kickback_pic` at all. Frames 4 and 5 are load-bearing on their AAPLAYER.CON
citations; 6 and 7 have no site. The same table is reused per weapon, so the
RPG attribution must not be copied. The `add z <offset>` lines there feed the
`rotatesprite` pair below them (a vertical draw offset for the HUD gun); the
generator classifies them draw-only in its `DRAW_ONLY` table, published as data
in `ekm_con_draw_only()`.

## 4. The floor

    floor = max(highest LOAD-BEARING frame below TotalTime,
                FireDelay, SpawnTime, Sound2Time, HoldDelay) + 1

Both floor notes use this one formula. Both weapons have `SOUND2TIME=0`.

**One number for the weapon:**

    floor = max(11, 3, 1, 3) + 1 = 12
    achievable multiplier = 12 / 12 = 1.00x

The binding frame is 11, `AAPLAYER.CON:14485`, and it is a `shoot`.

**Two narrower readings, because the chaingun has two cycle lengths.**

* *Shock path excluded.* `AAPLAYER.CON:14485` runs only under `gotshock 2` with
  `dukeupgrades[CHAINGUN_WEAPON] 8` (:14465-14466), and in that configuration AA
  has already set `WEAPON3_TOTALTIME 14` (:326). If the shock sites are struck
  out, the highest LOAD-BEARING frame below 12 is **9** (`AAPLAYER.CON:21285`,
  the ammo refund): `floor = max(9, 3, 1, 3) + 1 = 10`, multiplier
  `12 / 10 = ` **1.20x**.
* *Shock configuration on its own terms*, baseline 14: the highest LOAD-BEARING
  frame below 14 is **12** (`AAPLAYER.CON:14500`), so
  `floor = max(12, 3, 1, 3) + 1 = 13`, multiplier `14 / 13 = ` **1.08x**.

1.00x, 1.20x, 1.08x: all under the 1.75x threshold.

**A missed site can only make this worse, never better.** The floor is a `max`,
so a LOAD-BEARING frame the classification missed raises it.

## 5. Threshold

A cycle-shortening mechanism is rejected when its floor is 7 or higher at this
weapon's `TotalTime` (an achievable multiplier under 1.75x, less than the 2.0x
the earlier counter advance gave). The chaingun's floor is 12, multiplier 1.00x.

**Why.** The weapon was picked as a candidate for having sparse CON frames: 12
frames against the pistol's 28. Sparseness is the wrong property. What a floor
is made of is the HIGHEST load-bearing frame, and AA hangs the chaingun's shock
shot on frame 11 of a 12-tic cycle and its ammo refund on frame 9. A weapon can
have three load-bearing frames and no headroom at all if one sits at the top.

## 6. Limits of the reading

All by reading (static); no probe was run.

1. `TOTALTIME=12` is corroborated by `AAPLAYER.CON:344`; the other row fields
   are not re-read and do not bind the floor.
2. A CON site reading `TotalTime` itself is real for this weapon but harmless.
   `weapons_hud.CON:2231` and `:3348` do `setvarvar hud_totaltime WEAPON3_TOTALTIME` and use it only as the bound
   on `weaponcount` when drawing the HUD gun. It scales WITH a shortened cycle. No chaingun site compares
   `kickback_pic` against `WEAPON3_TOTALTIME`. **The shape exists elsewhere**:
   `AAPLAYER.CON:8851` is `ifge player[].kickback_pic WEAPON1_TOTALTIME` on the
   pistol (see `pistol-floor.md`), so grep for it per weapon.
3. **AA writes `WEAPON3_TOTALTIME` itself, to two values**, 12 and 14, selected
   by the shock upgrade. Any write path must adopt AA's value as the baseline
   rather than caching one.
4. `CHEERBOT.CON:326` and `ATARASHII.CON:739` read the player's `kickback_pic`
   from enemy AI, not weapon code. Both are range tests whose lower bound is
   frame 1, so neither can be starved by a shorter cycle.
5. `AAPLAYER.CON:13640`, `:13674` and `:13682` reach weapon 3 only through the
   generator's deliberate widening of sites it cannot attribute. All three sit
   in a block gated on `GROW_WEAPON`/`SHRINKER_WEAPON` (:13615-13617). Excluding
   them does not move the floor: the highest is frame 5.

## 7. The shipped fire rate against this floor

The fire-rate readout measured the plain chaingun at **1.2x** at 200% (`sum 73 samples 10`,
stock 9), three rounds per cycle as in stock. That does not
contradict the floor; they measure different things:

1. **Mechanism.** The floor bounds a speed-up that SHORTENS THE CYCLE
   (`TotalTime`). The shipped fire rate SKIPS UNUSED FRAMES between
   load-bearing ones; section 4's formula does not bound it. What bounds it is
   its stop set: the frames the game must observe.
2. **Configuration.** The binding frame 11 is live only under `gotshock 2` with
   `dukeupgrades[CHAINGUN_WEAPON] 8`. The measured weapon was the plain
   chaingun, no upgrades; section 4's narrower reading gives 1.20x there.
3. **Baseline.** Section 4 takes the stock cycle as `TotalTime` 12. With fire
   held, the `WEAPON_RESET` cut-off ends it at 9 (`player.cpp:4716-4723`,
   `ekm_stock_cycle_span`).

**Sweep with a uniform park.** Plain chaingun, one magazine each at 200%, 300%
and 400%: all three `sum 73 samples 10 x100 120`, the same span sequence `7 7 7 7 7 8 7 8 7 8`, 30 rounds each, no MISMATCH. The setting did nothing above
200%. Cause, from `weaponspeed.h:199-207` and the applied set (`firestops: w03
derived 13 stop(s): 2,5,8,9,...`, in-cycle 2,5,8,9; CON frames are applied
above `TotalTime` only): parking at `next event - 1` for EVERY stop is right for AA's CON
frames, which are observed from outside `P_ProcessWeapon` after the engine's
`++`, but one tic early for engine-derived stops, which are already stored one
early (`FireDelay-1`, the FIREEVERYTHIRD `f-1`). Hand-traced, the game observed
1,2,3,5,6,8,9 per cycle: 7 tics at any rate of 200% or more.
`weaponspeed.h:178-186` records this cost ("the chaingun ... collapses from
about 2.0x to 1.2x").

**Shipped rule.** Each stop carries its provenance: ENGINE stops are parked ON,
CON stops one below. For the plain chaingun the game then observes 1,3,6,9
(prediction): 1.5x at 200%, 2.25x from 300% (a ceiling near 9 / 4 = 2.25x),
shots intact. Frame 5, where Duke's ammo-saver upgrade refunds a round
(`AAPLAYER.CON:21284`), is skipped. The fire-rate readout's prediction was
verified against the measured line on hardware.

## Open questions

- The occasional `span 8` in the sweep is unexplained.
- The plain chaingun's multipliers at 300% and 400% under the shipped rule are
  predicted (2.25x), not recorded here.
