# Fire-rate stops: P_ProcessWeapon and AA's CON frames (firestops.h, con_stops_aa.h, weaponspeed.cpp)

Raising the fire rate advances `kickback_pic` faster than the engine tics. The advance must stop at every value where
something else acts, or a shot, sound, shell or reload branch is skipped. The stops come from two authors: the engine's
weapon data, and AA's CON, which also tests and writes the same counter. Build: eduke32.exe r10669, AA CON scripts.

## Engine side (player.cpp)
- P_ProcessWeapon's basic path (:4587-4741) is data-driven: every decision compares weaponFrame with a PWEAPON field, so
  the stops are a function of those fields.
- The tic increments the counter first (:4588), so a value k is a stop when the tic turning it into k+1 acts: `>= X` and
  `== X` first act from k = X-1, `> Y` from k = Y. When in doubt keep the stop: a needless stop costs speed, a missing one
  lets the counter jump an action. A set that does not fit refuses the weapon rather than truncating.
- WEAPON_RESET ends the cycle at TotalTime - HoldDelay (:4716-4723): 9 on AA's non-shock chaingun, not 11. The line sits
  inside both :4684 `Flags & WEAPON_AUTOMATIC` and :4686 `!(Flags & WEAPON_SEMIAUTO)`, under the guard at :4680 `>=
  FireDelay && < TotalTime`, so the first reachable value is FireDelay-1 and the last TotalTime-1. A RESET weapon failing
  either condition wraps at :4671 (unconditional reset to !!fire, after the counter reached TotalTime): its highest
  post-tic frame is TotalTime-1. AA's Devastator (AAPLAYER.CON:75-76, 122, 131; WEAPON7_FLAGS 65552 = RESET|FIREEVERYTHIRD,
  no AUTOMATIC) is such a weapon. HoldDelay above TotalTime is clamped up to FireDelay-1, not dropped.
- FIREEVERYTHIRD and FIREEVERYOTHER arms need WEAPON_AUTOMATIC: the Devastator carries FIREEVERYOTHER without it, falls to
  the `== FireDelay` shot at :4739 and has two stops, not four.
- Sound stops at or above TotalTime: ReloadSound1 (:4635), ReloadSound2 (:4640), Sound2Time (:4614, needs Sound2Sound > 0).
  SpawnTime's action is suppressed while startFiring is set on a FIREEVERYTHIRD/OTHER weapon (:4619); startFiring is
  dynamic per tic, so both stay. A_PlaySound's only CON hook, EVENT_SOUND (AAGAME.CON:15728-16291), remaps the sound and
  touches no weapon state.
- Reload: an automatic reload on an empty magazine survives a skipped frame through `(ammo_amount % Clip) == 0`
  (:4625) and the `>=` ladder (:4650). A CON can force the ladder by setting PLAYER_RELOADING itself; the pistol's stops
  5, 8 and 26 against a TotalTime of 5 are this branch. WEAPON_RANDOMRESTART starts the knee at 1 + (krand & 3) (:4665).
- The chaingun's SpawnTime is 1. Reproducing stock Duke's weapondefaults[] with the derivation returns the ten
  hand-read rows of the old static table exactly.

## AA's CON side
- 371 `kickback_pic` references across 81 scripts test and write the same counter at frame numbers no weapondata_t field
  mentions, so the engine-derived stops cannot model it. con_stops_aa.h lists AA's frames per weapon, widened on purpose.
- Writers of TotalTime-1 (AA literal writes against every literal WEAPONn_TOTALTIME): w01 TT 8 <- `setp 7` at
  AAPLAYER.CON:12590, 22290, 22302, 22319, 22926, 22939, 22950; w01/w07 TT 6 <- 5 at :13682; w02 TT 15 <- 14 at :13153,
  TT 24 <- 23 at :22977; w09 TT 12 <- 11 at :14340, :14347, :22331. The pistol's TotalTime-1 test (:12581) writes 9, 10
  and 7 (:12587-12590). AA's own reload trigger is `ife player[].kickback_pic (WEAPONx_TOTALTIME - 1)`
  (AAPLAYER.CON:12579-12591).
- Computed writes the table cannot see: AAPLAYER.CON:8928-8930 (kickback_pic+1), :12545-12549 (FIREDELAY+1),
  `setp[].kickback_pic temp` at :11536 (knee), :13844 and :14116 (shrinker), and AAGAME.CON:18343 in EVENT_SPAWN of a
  HEAVYHBOMB the player spawned (handbomb, w05 only). Any frame such a write can reach is unsafe unless proven otherwise.
- AAPLAYER.CON:8848-8930 adds 1 to kickback_pic before the weapon tests whenever `fastfire > 0`, or on the pistol at or
  above WEAPON1_TOTALTIME with `perks & 2048` (:8851-8853), so a counter parked at F-1 is tested as F+1. The add is
  skipped at FIREDELAY-1, on an empty pistol clip, near a reload's end and for pchar 0/1 with dukeupgrades bit 8 (:8867,
  :8901-8921). With fastfire 0 that write cannot reach an in-cycle frame.
- Other writers set the counter to 0 (gotshock :13409/:13538, weapon_pos :13422/:13551, weapon changed :13531/:13610, the
  refill :13459/:13578, combotime :23186/:23341) or to 1 from 0 (:13420, :13549, :22199, :22253, :22446, :22485). AA
  otherwise does read-then-`add 1` (AAPLAYER.CON:13428/:13557, :8934/:8935 under fastfire).
- Pistol (weapon 1): the manual (partial-magazine) reload refills the clip at kickback_pic 26 (AAPLAYER.CON:12556-12563). At
  400% an advance that stepped over 26 played the animation and never refilled. The pistol's TotalTime is 9 and its CON
  table has every integer 1..9, so applying every CON frame makes the cycle all stops and the rate inert; above
  TotalTime the reload ladder has gaps up to 5 (10, 11, ..., 16, 21, 24, 25, 26, 29, ...). The Glock 18 is WEAPON1_FLAGS
  98380, and 65536 at AAPLAYER.CON:1005.
- The Executioner shares slot 3 with the chaingun (gotshock bit 2, set and cleared at `weapon_pos 10` by
  AAPLAYER.CON:16303-16326). Its live row is FIREDELAY 3, TOTALTIME 14, RELOAD 0, FLAGS 0, giving engine stops {2, 13},
  while the CON tests slot 3 at frames 1, 2, 4, 5, 8, 9, 11, 12, 14, 17, 40, 99. Accelerating it steps over a frame the
  script waits on while the engine still takes the ammo. Feeding the CON frames in as stops was measured at 400%: zero
  advances ever, because AA's frames are consecutive inside the 12-14 frame firing window. The weapon is refused by
  name, as are weapons 5, 8 and 10; the switch-speed row is untouched by this.
- The CON's WEAPONx_* rewrites are `set` statements in player code that runs only once a level does; before that the
  registry holds the values Gv_AddSystemVars registered, which are stock Duke's.
- weapon_pos runs -9..-1 lowering, 1..10 raising, 0 at rest, and AA's own 11. The engine's raise/lower flip sets it to 10
  at -9 -> 10 without a weapon change.
- Measured: weaponspeed_frame runs per presented frame, about 8 frames per kickback value at firerate 350.

## Method
Reading player.cpp against AA's AAPLAYER.CON; live weapon-data dumps from the game; tools/gen_con_stops_aa.py for the
CON attribution (frozen reading of one install: file sizes, never mtimes, checked each run by constops.cpp); hardware
checks at 400%.
