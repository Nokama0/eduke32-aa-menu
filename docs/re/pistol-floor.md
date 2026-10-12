# Pistol fire-rate floor (CON frame classification), and the freezer screen

Summary: the pistol (w01) has a floor of 8 against a `TotalTime` of 9, an
achievable multiplier of 1.125x for a mechanism that SHORTENS THE CYCLE. The
binding frame is **7**, a `shoot` (`AAPLAYER.CON:12828` -> `shoot WESBULLET` at
`:12835`). Frames **5 and 6 also fire shots**, so even striking out Wes and
Mando entirely gives floor 7 and 1.29x. The threshold is a MULTIPLIER (1.75x),
not a raw floor number: a floor of 7 is 1.29x on a 9-tic weapon but 1.71x on
the chaingun's 12, so floors are not comparable across weapons. 1.125x is under
the threshold, so the mechanism is rejected for the pistol. Same format and
formula as `chaingun-floor.md`.

The freezer (w09) is rejected without classification: its row is
`TOTALTIME=5 FIREDELAY=3`, so `floor >= max(3, ...) + 1 = 4` on the engine
fields alone, a ceiling of `5 / 4 = 1.25x`. No classification
can raise that, because the floor is a `max`.

Method: row corroborated from AA's CON; every `kickback_pic` site listed per
site by `tools/gen_con_stops_aa.py` (static, by reading).

---

## 1. The row, and where each number came from

    w01 FIREDELAY=2 TOTALTIME=9 RELOAD=34 SPAWNTIME=2 SOUND2TIME=0 HOLDDELAY=0 WORKSLIKE=1

The row comes from the menu's weapon-row dump. The log is truncated on every
launch, so `TOTALTIME=9` is corroborated from AA's CON, where **three different
characters' pistol setups land on exactly 9**:

* `<game folder>\scripts\AAPLAYER.CON:141` -- `set WEAPON1_TOTALTIME 9`, in
  `defstate shellyweapons` (Shelly's base Desert Eagle).
* `AAPLAYER.CON:665` -- `set WEAPON1_TOTALTIME 9`, in `defstate wesweapons`
  under `ifvarand wesupgrades[PISTOL_WEAPON] 4` (Wes's DUAL revolvers).
* `AAPLAYER.CON:1182` -- `set WEAPON1_TOTALTIME 9`, in `defstate mandoweapons`
  (Mando's base wrist weapon).

`FIREDELAY`, `SPAWNTIME`, `SOUND2TIME` and `HOLDDELAY` are not re-read. They
cannot change the verdict: 2, 2, 0 and 0 are far below the binding CON frame 7.
The engine-only floor is `max(2,2,0,0) + 1 = 3`, ceiling 3.0x, which the CON
then destroys.

## 2. How the sites were found

`tools/gen_con_stops_aa.py` produced `con_stops_aa.h`. It reproduces the
committed header byte for byte (`cmp` clean, 81 script files, 371
`kickback_pic` references, all 8 `DRAW_ONLY` entries matched). Its
`Extractor.record` was subclassed to emit one row per SITE rather than a set of
frames, so every frame carries a `file:line`. The parser's rules are unchanged.

**`kW01` holds STOPS, not frames, and the two differ more here than anywhere.**
`kW01 = 1,2,3,4,5,6,7,8,9,10,...`. The generator's FRAMES for weapon 1 are

    1, 3, 4, 5, 7, 8, 9, 10, 12, 14, 15, 16, 17, 22, 25, 26, 27,
    30, 33, 34, 40, 42, 46, 47, 53, 54, 55, 99

-- **frames 2 and 6 are absent from that list and both matter below.** A stop is
a counter value the advance may leave the counter on, derived per operator as
`F - 1` and/or `F`; the contiguous run `1..16` in `kW01` is that off-by-one
smearing 28 scattered frames into a solid block. A floor is about FRAMES: an
`ife kickback_pic 7` needs frame 7 to be REACHED.

**The frame list is itself incomplete, which is the point of frame 6.** The
generator can only read a LITERAL operand. `AAPLAYER.CON:12715` is
`ife player[].kickback_pic temp`, where `temp` was set to 5 or 6 two lines
above, so the generator files it under "no literal to read"
(`con_stops_aa.h:442`) and neither 5 nor 6 reaches `kW01` from it. That site
fires a shot. A floor derived from the generated table alone would miss it and
call frame 6 "no CON site", the misclassification that produces an inert
weapon.

## 3. Frame by frame

Frames below `TotalTime` 9 are 1 through 8. All eight are classified.

| Frame | Verdict | `file:line` | What the CON does there |
|---|---|---|---|
| 1 | **LOAD-BEARING** | `AAPLAYER.CON:12914` | `setp[].kickback_pic 1` -- Mando's wrist flamethrower loop: frame 7 is sent back to 1 so the flame is continuous. Writes `kickback_pic`. Inside `ife player[].curr_weapon PISTOL_WEAPON` (`:12533`), `ife pchar 3` (`:12855`), `ife wristmode 1` (`:12900`), which is the mode that sets `WEAPON1_SHOOTS FLAMEPROJ2` (`:12869`). |
| 2 | COSMETIC | `AAPLAYER.CON:12535` | `ife player[].kickback_pic WEAPON1_FIREDELAY` -> `stopactorsound` + an `espawn SOUNDPLATE` carrying `WEAPON1_FIRESOUND` (`:12537-12540`). **Sound only.** It is also DERIVED from `FireDelay`, which is left untouched, and the engine's own `FireDelay 2` already keeps frame 2 inside the cycle. |
| 3 | **LOAD-BEARING** | `AAPLAYER.CON:12650` | `ife player[].kickback_pic 3` -> `shoot SHOTSPARK1` (`:12655`), `sub THISCLIP 1` (`:12656`) and an `ammo_amount` decrement (`:12657-12659`). Duke's Glock 18. **Fires a shot and moves ammo.** |
| 3 | **LOAD-BEARING** | `AAPLAYER.CON:12675` | The same shot on Shelly's Glock 18: `shoot SHOTSPARK1` (`:12682`), clip and ammo at `:12683-12686`. |
| 3 | **LOAD-BEARING** | `AAPLAYER.CON:12545-12549` | `ifle player[].kickback_pic WEAPON1_FIREDELAY { set tempb WEAPON1_FIREDELAY / add tempb 1 / setp[].kickback_pic tempb }` -- during a reload the counter is re-seated to `FireDelay + 1` = 3. Writes `kickback_pic`. Derived from `FireDelay`, not from `TotalTime`. |
| 3 | **LOAD-BEARING** | `AAPLAYER.CON:20740` | `case PISTOL_WEAPON ifl player[].kickback_pic 3 set temp NO break` -- the quick-kick gate: below frame 3 the kick is suppressed. Sets a gamevar the branch below it tests. (The chaingun's arm at `:20742` additionally cancels the cycle; the pistol's does not.) |
| 3 | **LOAD-BEARING** | `AAPLAYER.CON:12776` | `ife player[].kickback_pic 3` -> `set horizfall` / `set savedhoriz` / `setp[].horiz` (`:12778-12783`), Wes's recoil. The screen kick is cosmetic but `horizfall` and `savedhoriz` are gamevars other branches read, so it is LOAD-BEARING under the conservative rule. Frame 3 is load-bearing on the two `shoot`s above regardless. |
| 4 | COSMETIC | `AAGAME.CON:18157-18158` | `ife player[].curr_weapon PISTOL_WEAPON / ifg player[].kickback_pic 4 seta[RETURN].cstat 4` -- x-flips the just-spawned TRACER sprite. **A sprite change.** It is also an `ifg` RANGE, true from frame 5 upward, so it names no single frame the cycle must reach. |
| 5 | **LOAD-BEARING** | `AAPLAYER.CON:12611` | `ife player[].kickback_pic 5` -> `shoot SHOTSPARK1` (`:12616`) or `shoot SHOTGUN` (`:12619`). Duke's DUAL pistols (`ifvarand dukeupgrades[PISTOL_WEAPON] 4`, `:12608`; `ifg player[].player_par 5`, `:12610`). **Fires a shot.** |
| 5 | **LOAD-BEARING** | `AAPLAYER.CON:12706` | `ife player[].kickback_pic 5 shoot SHOTSPARK1` (`:12707`) -- Shelly's dual Glock 18 (`shellyupgrades[PISTOL_WEAPON]` 8 and 4, `:12701`/`:12703`). |
| 5 | **LOAD-BEARING** | `AAPLAYER.CON:12713`+`:12715` | `ifg fastfire 0 set temp 5 else set temp 6` then `ife player[].kickback_pic temp` -> `shoot DEAGLEBULLET` (`:12717`). With `fastfire` on, that literal is **5**. Shelly's dual Desert Eagles. |
| 6 | **LOAD-BEARING** | `AAPLAYER.CON:12713`+`:12715` | The same site with `fastfire` off: `temp` is **6** and frame 6 fires `shoot DEAGLEBULLET` (`:12717`), then `set horizfall` / `setp[].horiz` (`:12722-12728`). Shelly's dual Deagles sit at `TotalTime 9` (`:141`, untouched by `shellyupgrades ... 4` at `:223-227`), so frame 6 is inside this cycle. **Invisible to `con_stops_aa.h`** (section 2). |
| 7 | **LOAD-BEARING** | `AAPLAYER.CON:12828` | `ife player[].kickback_pic 7 // 6` -> ammo read/decrement (`:12831-12834`) and **`shoot WESBULLET` (`:12835`)**. Wes's DUAL revolvers (`ifvarand wesupgrades[PISTOL_WEAPON] 4`, `:12825`; `ifg player[].player_par 6`, `:12827`; `ifg player[].ammo_amount PISTOL_WEAPON 0`, `:12829`). **THE BINDING FRAME.** That same upgrade sets `WEAPON1_TOTALTIME 9` at `:665`, so frame 7 is the second-last frame of this weapon's cycle. |
| 7 | **LOAD-BEARING** | `AAPLAYER.CON:12913` | `ife player[].kickback_pic 7` -> `setp[].kickback_pic 1` (`:12914`). Mando's wrist flamethrower loop, the other half of frame 1 above. Mando's base `WEAPON1_TOTALTIME` is 9 (`:1182`). |
| 7 | **LOAD-BEARING** | `AAPLAYER.CON:12590` | `setplayer[THISACTOR].kickback_pic 7` + `setplayer[THISACTOR].reloading YES` (`:12591`) -- the reload entry for Duke and Mando. Writes `kickback_pic` and sets `reloading`. |
| 7 | **LOAD-BEARING** | `AAPLAYER.CON:22290`, `:22302`, `:22319`, `:22926`, `:22939`, `:22950` | The same `setp[].kickback_pic 7` + `setp[].reloading YES` in AA's two "RELOAD WHEN USING PISTOL" blocks (`:22274-22307` and `:22918-22954`), both gated on `ife player[].curr_weapon PISTOL_WEAPON`. |
| 8 | **NO FLOOR -- derived** | `AAPLAYER.CON:12579-12581` | `set tempb WEAPON1_TOTALTIME / sub tempb 1 / ife player[].kickback_pic tempb` -- the reload trigger. It is computed FROM `TotalTime` and moves with it, so a shortened cycle relocates it rather than starving it. **It imposes no floor.** |
| 8 | LOAD-BEARING, **but no floor** | `CHEERBOT.CON:326` | `ifn player[].kickback_pic 0` / `ifle player[].kickback_pic 8` gating an enemy's dodge decision, with `case 1` naming the PISTOL at `:329`. It changes no weapon state, so it is LOAD-BEARING under "anything you cannot classify is LOAD-BEARING". **It is a RANGE `1..8`**, and frame 1 exists at every cycle length, so it can never be starved. |

### Frames 4-7

Three of the four are load-bearing, and all three fire shots: 5, 6 and 7 each
carry a `shoot` on a different character's pistol. Only frame 4 is cosmetic, a
tracer's sprite flag read through an `ifg` range. If all four had been cosmetic
or absent, the floor would have been 4 and the multiplier 2.25x.

**No `weapons_hud.CON` line is cited for any of them, deliberately.** That file
holds exactly eight `kickback_pic` references, `:4559-4562` and `:9346-9349`,
and neither group is on the pistol's path:

* `:4559-4562` sit inside `defstate draw_newrpg` (`:4519-4649`), the RPG's HUD
  zoom-scope state.
* `:9346-9349` sit inside `ife currentweapon RPG_WEAPON` (`:9310`) within
  `defstate draw_wes_weapons` (`:8898`) -- the RPG again, under `ife zoomin YES`
  (`:9341`).
* The pistol's OWN HUD states -- `draw_pistol` (`:449-643`), `draw_left_pistol`
  (`:645-915`), `draw_upgraded_pistol` (`:917-1118`), `draw_dual_glock`
  (`:3551-3704`), `draw_glock18` (`:3706-3782`) and `displaylaserpistol`
  (`:4772-4892`) -- contain **no `kickback_pic` reference at all**.

Frame 4's cosmetic verdict rests on `AAGAME.CON:18158`, which is gated on
`PISTOL_WEAPON` one line above it.

## 4. The floor

    floor = max(highest LOAD-BEARING frame below TotalTime, FireDelay,
                SpawnTime, Sound2Time, HoldDelay) + 1

**One number for the weapon:**

    floor = max(7, 2, 2, 0, 0) + 1 = 8
    achievable multiplier = 9 / 8 = 1.125x

The binding frame is 7, `AAPLAYER.CON:12828`, and it is a `shoot`.

**Per configuration, because the pistol is five weapons wearing one slot.** None
clears the 1.75x threshold.

| Configuration | `TotalTime` set at | highest load-bearing frame below it | floor | multiplier |
|---|---|---|---|---|
| Wes, dual revolvers | 9 (`:665`) | 7 (`:12828`, `shoot WESBULLET`) | 8 | **1.125x** |
| Mando, base wrist | 9 (`:1182`) | 7 (`:12913`, loop back to 1) | 8 | **1.125x** |
| Shelly, dual Deagles | 9 (`:141`) | 6 (`:12715`, `shoot DEAGLEBULLET`) | 7 | **1.29x** |
| Duke, dual pistols | 6 (`:1560`/`:1577`) | 5 (`:12611`, `shoot`) | 6 | **1.00x** |
| Shelly, dual Glock 18 | 6 (`:211`) | 5 (`:12706`, `shoot`) | 6 | **1.00x** |
| Shelly, single Glock 18 | 4 (`:216`) | 3 (`:12675`, `shoot`) | 4 | **1.00x** |
| Duke, single Glock 18 | 4 (`:1566`) | 3 (`:12650`, `shoot`) | 4 | **1.00x** |
| Nova | 5 (`:1003`), `FIREDELAY 3` (`:1002`) | engine alone gives 4 | 4 | **1.25x** |

1.125x for the weapon; 1.29x at the very best; 1.00x on four of the eight
configurations.

**AA has already spent the headroom.** The pistol's cycle is not 9 tics of
animation with a shot somewhere inside it: AA tunes `TotalTime` per upgrade tier
(9, 8, 6, 5, 4) and hangs each tier's extra shot one or two frames below its own
ceiling. There is nothing left for a multiplier to take.

**A missed site can only make this worse, never better.** The floor is a `max`.

## 5. Threshold and the weapon table

A cycle-shortening mechanism is rejected when its achievable multiplier is under
1.75x (less than the 2.0x the earlier counter advance gave). Floor 8, 1.125x.

| Weapon | `TotalTime` | floor | multiplier | how settled |
|---|---|---|---|---|
| w03 chaingun | 12 | 12 | **1.00x** | classified, `chaingun-floor.md` |
| w01 pistol | 9 | 8 | **1.125x** | classified, this note |
| w09 freezer | 5 | >= 4 | **<= 1.25x** | engine fields alone |

**Screen the whole weapon table on the engine fields first.** `FireDelay` is an
engine field the mechanism leaves untouched, so `floor >= max(engine frames) + 1`
and any CON site can only push it higher. Where `TotalTime / (max(engine frames) + 1)` is already under 1.75x, the weapon is rejected and the expensive
classification is never needed.

For comparison, the older counter-advance mechanism measured at 400% on RPG
about 1.6x, chaingun about 1.2x, freezer about 1.25x. The cycle-shortening
mechanism would give the chaingun 1.00x and the pistol 1.125x, worse than what
it was meant to replace.

## 6. Limits of the reading

All by reading (static); no probe was run.

1. `TOTALTIME=9` is corroborated by `AAPLAYER.CON:141`, `:665` and `:1182`; the
   other five row fields are not re-read and none of them binds the floor.

2. **A CON site reading `TotalTime` itself is live on this weapon and is NOT
   harmless.** `AAPLAYER.CON:8848-8853` is

       set temp NO
       ifg fastfire 0 set temp YES
       ife LASERMODE 0 ife player[].curr_weapon PISTOL_WEAPON
       ifge player[].kickback_pic WEAPON1_TOTALTIME
       ifvarand perks 2048
       set temp YES

   and `temp YES` arms **AA's own counter advance** at `:8926-8930`
   (`getp[].kickback_pic tempb / add tempb 1 / setp[].kickback_pic tempb`).
   Today `kickback_pic >= WEAPON1_TOTALTIME` means "in the reload tail", because
   the engine only lets the counter exceed `TotalTime` while `reloading`
   (`player.cpp`: at `*weaponFrame >= TotalTime` the reset is skipped when
   `pPlayer->reloading == 1`, and the counter runs on for `Reload - TotalTime`
   tics). With a shorter `TotalTime` that test fires during NORMAL FIRING, so a
   player holding the reload-speed perk would have AA double-advance the counter
   through the firing cycle, skipping frames 5, 6 and 7, which carry shots. The
   chaingun's equivalent read (`weapons_hud.CON:2231`) scales harmlessly; this
   one does not.

3. `weapons_hud.CON:2236-2237` reads `WEAPON1_FIREDELAY` and
   `WEAPON1_TOTALTIME`, and that one IS harmless. `hud_totaltime` is used only
   as the upper bound on `weaponcount` when drawing the HUD gun (`draw_pistol`,
   `:457-458`), so it scales with a shortened cycle. `hud_temp4` (= `FireDelay`)
   is compared at `:466` for a 3-pixel x-nudge and `FireDelay` is untouched.

4. **AA drives `WEAPON1_TOTALTIME` to five values across five characters** -- 4,
   5, 6, 8 and 9 at `AAPLAYER.CON:141/211/216/611/665/1003/1181/1182/1498/1560/
   1566/1577`. Any write path must adopt AA's live value as the baseline and
   recompute when AA changes it; a cached baseline would be wrong within one
   shop visit.

5. **The reload tail runs ABOVE `TotalTime`**, which is why `setp[].kickback_pic`
   9 and 10 at `:12587`/`:12589` are not a contradiction. Those are Wes's and
   Shelly's reload entries with `TotalTime` 9; the engine keeps counting past
   `TotalTime` while `reloading == 1`. Frame 7's reload-entry writes might
   survive a shortened cycle on their own, but it does not matter: frame 7's
   floor comes from `:12828`, which is not gated on `reloading` and fires during
   the ordinary cycle.

6. `AAPLAYER.CON:13640`, `:13674`, `:13682` and `:13688` reach weapon 1 only
   through the generator's deliberate widening of sites it cannot attribute. All
   four sit inside `ife pchar 2` (`:13612`) and a block gated on
   `GROW_WEAPON`/`SHRINKER_WEAPON` (`:13615-13617`, `ife temp YES` at `:13617`)
   -- Wes's SKAR, not the pistol. The highest of them below 9 is 5, so excluding
   them does not move the floor.

7. `ATARASHII.CON:739` (`ifl ... 12`) names frame 12, above `TotalTime` 9, so it
   is outside the classified range. Like `CHEERBOT.CON:326` it is enemy AI
   reading the player's counter, a range whose lower bound cannot be starved.

8. `AAPLAYER.CON:11536` and `AAGAME.CON:18343` are the two other
   variable-operand `setp[].kickback_pic` sites that could have hidden a pistol
   frame. `:11529` gates on `ife player[].curr_weapon 0` (the knee) and `:18344`
   on `HANDBOMB_WEAPON`; neither reaches weapon 1. `AAPLAYER.CON:8921`/`:8930`
   are the fast-fire advance of item 2 and are derived, not absolute.

## Related

The shipped fire rate protects the pistol's shot frames (dual Deagles at 6, or
5 under fastfire; Duke's dual 1911 at 5; Wes's dual revolvers at 7) by stop
provenance; see `chaingun-floor.md` section 7.
