# Picnum tables: switches, doors, pickups, breakable walls, one-shot cover

The ESP and triggerbot name tiles by picnum. Every number is this game's own (names.h); World Tour's differ.

## Door tiles (esp_switches.h, ekm_isadoorwall)
picnum -> DOORTILE name: 150..159 = DOORTILE5, 6, 1, 2, 3, 4, 7, 8, 9, 10; 395 = 22; 447..449 = 18, 19, 20; 717 = 14;
781 = 16; 1102 = 15; 1144 = 21; 1169 = 17; 1178, 1179 = 11, 12; 4391 = 23. names.h has no DOORTILE13. These are the 22
DOORTILE cases of CheckDoorTile (sector.cpp:153).

## Switch base tiles (esp_switches.h, ekm_switch_base_for kBases)
Base picnum -> name (family), following G_GetBaseSwitch (sector.cpp:1250):
130 ACCESSSWITCH (access), 132 SLOTDOOR, 134 LIGHTSWITCH, 136 SPACEDOORSWITCH, 138 SPACELIGHTSWITCH, 140
FRANKENSTINESWITCH (rest), 162 DIPSWITCH (dipswitch-like), 164 DIPSWITCH2 (rest), 166 TECHSWITCH (dipswitch-like), 168
DIPSWITCH3 (rest), 170 ACCESSSWITCH2 (access), 712 LIGHTSWITCH2, 860 POWERSWITCH1, 862 LOCKSWITCH1, 864 POWERSWITCH2,
1111 HANDSWITCH, 1122 PULLSWITCH (rest), 1142 ALIENSWITCH (dipswitch-like).

## Pickups (esp_items.h, ekm_item_name; picnum -> names.h symbol)
21 FIRSTGUNSPRITE, 22 CHAINGUNSPRITE, 23 RPGSPRITE, 24 FREEZESPRITE, 25 SHRINKERSPRITE, 26 HEAVYHBOMB (one thrown
bomb), 27 TRIPBOMBSPRITE, 28 SHOTGUNSPRITE, 29 DEVISTATORSPRITE, 37 FREEZEAMMO, 40 AMMO, 41 BATTERYAMMO, 42
DEVISTATORAMMO, 44 RPGAMMO, 45 GROWAMMO, 46 CRYSTALAMMO, 47 HBOMBAMMO (box of pipebombs), 48 AMMOLOTS (big pistol box),
49 SHOTGUNAMMO, 51 COLA (small medkit), 52 SIXPAK (large), 53 FIRSTAID (portable), 54 SHIELD, 55 STEROIDS, 56 AIRTANK,
57 JETPACK, 59 HEATSENSOR (night vision), 60 ACCESSCARD (coloured by pal), 61 BOOTS, 100 ATOMICHEALTH, 1348 HOLODUKE.
- Verification: every number resolves in names.h, matches GAME.CON actors calling addweapon / addammo / addphealth /
  addinventory, and appears live as statnum-2 target refusals in the mod's log.
- World Tour's FLAMETHROWERSPRITE 5134 and FLAMETHROWERAMMO 5135 do not exist in this game; Nuclear Winter, dukedc.grp
  and vacation.grp add no pickup actors.

## Breakable walls (esp_breakables.h, ekm_breakable_name)
293 W_TECHWALL1, 297 W_TECHWALL2, 299 W_TECHWALL15, 301 W_TECHWALL3, 305 W_TECHWALL4, 306 W_TECHWALL10, 307
W_TECHWALL16, 1212 VENDMACHINE, 1215 COLAMACHINE, 4130-4133 W_TECHWALL11..14, 4134 / 4136 / 4138 / 4140 W_TECHWALL5..8,
4142 W_TECHWALL9, 4181 W_MILKSHELF.

## Cover a single bullet opens (tbot.h, ekm_picnum_breaks_open / ekm_wall_breaks_open)
- Sprite picnums: GRATE1 595 (vent grate), FANSPRITE 407, TOILET 569, STALL 571, CIRCLEPANNEL 1113, PANNEL1 342, PANNEL2
  343, PANNEL3 4099.
- Wall picnums: GLASS 503 (cstat = 0 and the neighbour's, sector.cpp:1676-1685), STAINGLASS1 510 (:1698-1703), FANSPRITE
  407 as a masked wall (`cstat &= 65535-65`).

## Enemy names and health maxima (esp_names.h)
Picnum -> names.h symbol -> label; health maxima come from this build's GAME.CON (`actor <PIC> <STRENGTH>`), 0 = unknown.
- Assault Trooper 30: LIZTROOP 1680, RUNNING 1681, STAYPUT 1682, SHOOT 1715, JETPACK 1725, ONTOILET 1741, JUSTSIT 1742,
  DUCKING 1744 (pal 21 is the Assault Captain, twice the spawn strength: `checktrooppalette`, GAME.CON:4709-4718).
- Protozoid Slimer 2370..2377 (GREENSLIME and frames, no actor line). Egg 675 (20, actor EGG TOUGH). Rat 1267 (no
  actor line). Shark 1550 (35). Cannon 1810 (400). Octabrain 1820/1821 (175). Sentry Drone 1880 (150). Assault
  Commander 1920/1921 (350). Recon Patrol 1960 (no actor line). Pig Cop Tank 1975 (500). Pig Cop 2000/2001/2045 (100).
  Enforcer 2120/2121/2150/2160/2165 (LIZSTRENGTH 100; LIZMANFEEDING 2160 has no actor line). Turret 2360 (ROTTURRETSTRENGTH
  40). Organic turret 2420 (TURRETSTRENGTH 30). Battlelord 2630/2631 (4500). Overlord 2710/2711 (4500). Cycloid
  Emperor 2760/2761 (4500). Protector Drone 4610/4611/4670/4690 (NEWBEASTSTRENGTH 300; NEWBEASTHANGDEAD 4671 is its own
  actor line at TOUGH 20). Alien Queen 4740/4741 (6000). Firefly Trooper 5180 and Alien Overlord 5310/5311 are named but
  not defined in this build's CON (Atomic Edition content); BOSS2STAYPUT and BOSS3STAYPUT are undefined too.
- Pal'd bosses set strength on their first tic: BOSS1 (Battlelord Sentry, any non-zero pal) 1000 (GAME.CON:6464-6470),
  BOSS2 and BOSS3 a literal 1 (:6796, :7071). A_Spawn doubles `extra` for every non-zero pal (game.cpp:2060-2062).
  The STAYPUT variants are not covered: 2631 becomes 2630 on its first tic.

## AA enemy picnums (esp_names_aa.h, generated)
- 22 AA enemy names are resolved from AADEFS.CON because names.h has no #define: ARMLIZTROOP 22784, ARMLIZTROOPDUCK
  22839, CANON 25088, CANONDUCK 25128, GRAVESNPC 22460, GUNSHIP 22520, HITYU 25600, HITYUDUCK 25640, KUKKIMON 25856,
  NATTYBRA 22730, NIPTROOP 24320, NIPTROOPDUCKING 24355, PIKABOO 24576, REPAIRDRONE 18817, SHIPTURRET 22521, SIN 25344,
  SINDUCK 25384, SSLIME 23552, ZOMBIE2 23650, ZOMBIE2CASE 23635, ZUKA 24832, ZUKADUCK 24872.
- names.h and AADEFS.CON disagree on LIGHTNING_TRAIL1..6 (25650..25655 vs 27650..27655) and STATUESTAT (976 vs 973);
  names.h is used.
- Not expressed by the header: MANDOCROUCH (14160) and MANDOFETT (14085) would be "Mando Phett" under `ife VOLUME 6`.
- Switch sources: weapons_hud.CON:2836-2931 (nameplate switch, case arms 2837-2840, default :2930), AAUSER.CON and
  NPCQUOTES.CON definequote lines. Team 3 is a neutral non-combatant (AADEFS.CON:3830); the mtype gamevar is
  AADEFS.CON:3977.
- Headshot whitelist (aa_headshot.h): the case list is NOTENEMY.CON:11693-11710; AADEFS-only rows at AADEFS.CON:2933,
  2938-2940, 2842, 2844.
- Curated item tier (esp_items_curated_aa.h) cites AAITEMS.CON, AAUSER.CON, weapons_hud.CON, NPCQUOTES.CON, CIVILIAN.CON
  (GRAVES :5171, AAXON :6251) and GRAVES.CON:330 for the MINI* figurine and pickup rows.

## AA voxels (esp_voxels_aa.h, generated)
- AA's install holds 5 .def files with 525 voxel blocks (all .kvx loaded), 612 tiletovox stores and 608 rows; picnums
  556, 557, 559 and 4885 are claimed twice (the LAST is kept; defs.cpp:2001, :2019). voxels/switches.def (36 blocks) is
  not reached. 504 .kvx files, 25870201 bytes, all mip chains land on the palette.
- Models beat voxels (polymost.cpp:7613-7618): picnums 19960, 19962, 22323, 22324, 22325 and 22516 have both; 19960
  STAKESPRITE is sized from a 16x224 voxel that never reaches the screen (the model extents table does not exist).
- The table is a frozen reading of AA's install and carries stock pickup tiles (21-24, 26-29, 36-42) too.
- X/Y pivots are not emitted: per-axis half-extents come from sizX/sizY and radius, exact where the pivot is centred and
  an upper bound otherwise (155 of the 608 rows). md3_vox_calcmat_common (mdsprite.cpp:1869-1880) spins x/y about the
  pivot.

## Tile name table (tilenames.h)
The loader accepts only `#define` lines with a plain decimal value in [0, MAXTILES) and a name that fits; comments,
blanks, #ifdefs, expressions, other names and negative values are refused. It keeps the first-seen name per picnum and
counts the rest, so the live table never holds two entries for one picnum.

## Method
names.h and GAME.CON in DUKE3D.GRP; the tables checked against live ESP output.
