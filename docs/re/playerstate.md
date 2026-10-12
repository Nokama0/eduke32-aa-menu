# Player structure offsets, shot geometry and level-loaded rule (playerstate.h)

What the menu reads and writes in the player's structure, and the geometry facts the aim code relies on. Build:
eduke32.exe r10669. Offsets marked "bracketed" were derived, not measured (probe executables are banned); each walk
lands exactly on a measured far anchor.

## DukePlayer_t layout
`DukePlayer_t` is `#pragma pack(1)` (player.h:170-292), sizeof 640 static-asserted (player.h:267). Compiling it standalone
reproduced pos, opos, q16horiz, cursectnum, pyoff, i, auto_aim, gm and curr_weapon exactly. The rest:
- pos 0, opos 12 (player.h:185), vel.z 32 (pos 0 + opos 12 + vel 24; continuing npos 36, bobpos 48, fric 56 lands on
  q16horiz 64; the teleport zeroes it).
- bobpos 48 (x 48, y 52; player.h:187-191). player.cpp:5308 derives the sprite's xvel from pos against bobpos and
  "silently wraps if the difference is too great", so a teleport must write it.
- q16horiz 64, q16horizoff 68, oq16horiz 72, oq16horizoff 76 (between 72 and q16ang 80, one fix16_t), q16ang 80,
  oq16ang 84.
- olook_ang 372 (16 int16 fields back from cursectnum 404: sound_pitch, sbs, gravity, jetpackzincrement, max/min
  swimzvel, swimzincrement, swim/crouch speed, crouchzincrement, shrunk/water/minwater/spritez/floorzoffset;
  player.h:210), cursectnum 404, look_ang 406 (player.h:219).
- pyoff 488 (player.h:221), opyoff 490 (the 14 int16 fields pyoff..i span 28 bytes), newowner 492 (twelve int16 back
  from i: jumping_counter, airleft, fta, ftq, access_wallnum, access_spritenum, got_access, weapon_ang, visibility,
  somethingonplayer, on_crane, i; player.h:223), got_access 506 (the 7th int16 after newowner; 492 + 2*12 = 516
  closes; player.h:225), i 516.
- auto_aim 590 (player.h:240), kickback_pic 597 (uint8, player.h:241), gm 598 (player.h:243), over_shoulder_on 608 (ten
  uint8 on from gm: on_warping_sector, footprintcount, hurt_delay, hbomb_on, jumping_toggle, rapid_fire_hold, on_ground,
  inven_icon, buttonpalette, then it; 25 back from weapon_pos; player.h:245), weapon_pos 633 (int8, player.h:257),
  curr_weapon 635.
- Bracketed: vel.z 32, bobpos 48, oq16horizoff 76, olook_ang 372, look_ang 406, opyoff 490, newowner 492, got_access
  506, over_shoulder_on 608. The four the camera bracket added (oq16horizoff, look_ang, olook_ang, opyoff) are kept on
  purpose: each is the near anchor of a live offset's walk (q16ang, cursectnum, newowner). A wrong got_access offset lands
  on `i` and suppresses the keycard hint; it never leaves a lock unmarked.

## playerdata_t and the array
- playerdata_t is 133 bytes under pack(1) with MAXPLAYERS 16 and MAX_WEAPONS 13 (`ps` is the first member, player.h:271;
  g_player = &g_player_s[1], global.h:151-152, so g_player[0].ps is at g_player_s + 133).
- 133 is odd: a pointer search that strides 8 can never land on base + 133.
- g_player_s signature: the zeroth entry is null plus sixteen distinct non-null heap allocations (game.cpp:6803-6804
  allocates all sixteen in single player via G_MaybeAllocPlayer); a test that expected null neighbours could never pass.
- The struct is on the heap (game.cpp:6433); player_rva reports the image slot pointing at it.
- Health and palette are read from the player's SPRITE (hp <= 0 dead, player.cpp:2262, 3033; pal == 1 frozen, :1796,
  3595, 3608). The index is re-read from ps->i on every call (a cached latch went stale across level transitions).
- APLAYER is 1405 (names.h:407) and STAT_PLAYER 10 (actors.h:59). Map authors place APLAYER sprites for multiplayer
  starts and game.cpp:2786-2794 sends those to STAT_MISC in single player, so statnum is the load-bearing half.

## Shot geometry
- ZOFFSET6 = 4 << 8 (actors.h:43). A_ShootWithZvel sets startPos.z = pos.z + pyoff + ZOFFSET6 (player.cpp:1710-1711) and
  P_PreFireHitscan subtracts it again at its single exit (:528): hitscan weapons fire from pos.z + pyoff, projectiles
  keep it. Adding it everywhere put every impact 1024 too high (Build z grows downward). About 1520 of room absorbs it
  aiming at a body's middle; 760 or 380 do not at 0.75 / 0.875, which is why E1L3 turrets died and E1L4's did not.
  Measured: a verification ray hit sprite 953 at z -77178; the following round, on identical direction and mask,
  carried on to -119808.
- Five projectile weapons carry a further z term not modelled: RPG and Devastator spawn at startPos.z - (1<<8)
  (player.cpp:1408), the freezer adds (3<<8) (:1361), the shrinker + (2<<8) (:1596), the knee (6<<8) (:1144): 256-512
  out. The knee cannot be a target (tbot_weapon_usable).
- Effective pitch is q16horiz + q16horizoff: every shot's zvel is F16(100) - q16horiz - q16horizoff (player.cpp:498, 521,
  966, 1023, 1143), autoaim tests the sum (:304) and so does the camera (game.cpp:1009-1015).
- Silent aim sets auto_aim = 0: A_FindTargetSprite returns nothing at 0 (player.cpp:220-221), so the shot lands on the
  menu's line. Setting 2 was wrong: GetAutoAimAng computes `*pZvel = (z - startPos->z - zCenter) * projVel /
  safeldist(...)` (player.cpp:380-382) with Build's approximate distance, ~5% long at 11432 units on E1L3's far turret,
  shallowing by ~1200 z against a body 3040 tall: 308 rounds produced 8 damage events. At 0 every weapon returns -1, so
  RPG / freezer / shrinker (player.cpp:224-242) keep their lead too.
- The weapon is keyed on curr_weapon, not projecTile: World Tour renumbered tiles, so a picnum list silently refused
  every shot there (silentwt.cpp:73).
- Projectile speeds per tic: RPG, Devastator and Freezer share one case at xvel 644 (player.cpp:1360-1369; the +128
  further down is a BOSS2); Shrinker SHRINKSPARK 768 (:1597); Flamethrower FIREBALL falls to FIRELASER 840 (:1290).
  The flamethrower is unconfirmed in game: WORLDTOUR is `(DUKE && g_scriptVersion >= 16)` (duke3d.h:55), DNSTUFF sets
  the bit (cheats.cpp:416-421) but curr_weapon never reaches 12 on stock; under WT, player.cpp:1603-1610
  FLAMETHROWERFLAME (vel 400, `if (!WORLDTOUR) break;`) may fire instead.
- Zero means do not lead: hitscan weapons (1, 2, 3, 11) arrive this tic; the pipe bomb (5) is thrown (xvel 140 + (hold
  << 5), zvel -512 - horiz*20, player.cpp:4496-4509); the trip bomb (8) is a hitscan wall placement (:1483-1520); the
  knee (0) and handremote (10) fire nothing.

## Level-loaded rule
- Level loaded means EK_MODE_GAME by exact equality: gm reads exactly 0x04 only once the level is resident
  (premap.cpp:2046), and gm = 0 during load (premap.cpp:1465).
- Three transitions keep MODE_GAME set through a load: CON `setlevel` ORs MODE_EOL (gameexec.cpp:4413), the cheat restart
  sets MODE_RESTART (cheats.cpp:571, :586), and reload-after-death with the menu open (menus.cpp:4541). Exact equality
  also fails MODE_DEMO (0x02).
- Side effects are intended: the ESP is suppressed over the pause menu (MODE_MENU) and chat (MODE_TYPE). A failed read
  is false, and a failed read is not a level change.
- ekm_level_loading is NOT !esp_level_loaded (the latter is also false over pause and chat). Three per-sprite-index
  memories need the reset: tracer.cpp's ring, targets.cpp's sticky deferral flag, and esp.cpp's health maxima.
- An in-session load drives gm to 0 and back inside one synchronous call without presenting a frame, so it is not
  visible as a render-thread signal ([secretvol.md](secretvol.md)).

## Writes
- View: P_UpdateAngles sets q16ang / q16horiz (player.cpp:3080-3084). Weapon counters: P_ProcessWeapon steps kickback_pic
  (:4588) and weapon_pos (:4212). Shot view substitution: six fields saved and restored. Shooter cstat: Proj_DoHitscan
  (player.cpp:548-560).
- Teleport writes ten fields: pos.xyz and opos.xyz (Build interpolates from o toward live, game.cpp:1001-1003),
  bobpos.x/y, cursectnum, vel.z = 0. The engine's own transporter writes the same set: P_FinishWaterChange sets
  bobpos.x/y = opos.x/y = pos.x/y (actors.cpp:3874-3875), cursectnum and changespritesect (:3880-3881); P_Emerge sets
  opos.z = pos.z and vel.z = 0 (:3859-3860); P_ProcessInput re-derives cursectnum by updatesector (player.cpp:5322;
  :5745 is inside `if (ud.noclip)`). A fault partway is not rolled back.
- The tic clock is written only from camera_frame (overlay.cpp:271), outside the G_DrawRooms bracket (camera.cpp:1512 is
  inside), where aim.cpp:1819 and weaponspeed.cpp:145, :157 also write. A sector is trusted only if >= 0, inside the
  static 4096-slot array and below the live sector count.

## Spin-up guns that hold the triggerbot (tbotspin.h)
The cap holds `spinup` at 9 while the pool is 0. AAPLAYER.CON, pchar per AADEFS.CON:3267:
- Duke's minigun: `ife pchar 0` (:12959), curr_weapon CHAINGUN (:12963), `dukeupgrades[CHAINGUN_WEAPON] & 1` (:12976; bit 1
  "minigun", :1689), cap on ammo_amount[CHAINGUN] (:12986). Not when the Executioner is on top: `& 8` plus `gotshock & 2`
  takes the shock branch (:12973-12975); the `else` runs when either fails.
- Dea Nova's chaingun: `ife pchar 4` (:13723), CHAINGUN (:13725), no upgrade bit, the same Executioner exclusion
  (:13757-13758), cap on ammo_amount[CHAINGUN] (:13767).
- Devastator: `ifge pchar 3` (:14247), curr_weapon DEVISTATOR (:14167), not `gotrailgun & 2` (:14169, railgun on top), cap
  on ammo_amount[DEVISTATOR] (:14259-14260).
The pool is ammo_amount[weapon] in all three; the minigun's extra `shoot` subtracts from it (:13002-13005). Constants:
EKM_SPIN_UP_EXECUTIONER from :1668, gotshock bits AADEFS.CON:4334-4335, gotrailgun AAPLAYER.CON:21.

## Open questions
- The Flamethrower's projectile speed (FIREBALL falling to FIRELASER 840, or FLAMETHROWERFLAME 400 under World Tour) is
  unconfirmed in game; it needs CON version 16 (duke3d.h:55).

Related: [freecam.md](freecam.md), [hitscan.md](hitscan.md), [gamestate.md](gamestate.md).

## Method
Compiling the player struct standalone for measured offsets, bracketing for the rest, Ghidra on the stock build for
code shapes, and in-game checks of each shipped behaviour.
