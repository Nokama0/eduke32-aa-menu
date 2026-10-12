# AA projectiles: which weapons fire hitscan and which fly (con_proj_aa.h)

The aim lead needs to know whether a weapon slot fires a real projectile (led) or a hitscan (not led). AA's CON decides
per slot, and the stock table is wrong for AA. The generated header is a frozen reading of one install.

## Membership
- A tile is a projectile when C_DefineProjectile ends with `g_tile[j].flags |= SFLAG_PROJECTILE` (gamedef.cpp:2324).
  A_ShootWithZvel runs A_ShootCustom when the flag is set and A_ShootHardcoded when not (player.cpp:1757-1759).
- AA's WEAPONn_SHOOTS names nine stock tiles (RPG, SHOTSPARK1, CHAINGUN, SHOTGUN, SHRINKER, FREEZEBLAST, GROWSPARK, KNEE,
  HANDHOLDINGLASER) that fall back to the stock table. Stock Duke and World Tour register Shoots as a system var
  (ADDWEAPONVAR), so the read succeeds there and the generated table carries no stock tile.
- WEAPONn_WORKSLIKE is useless: AA sets it to the slot index (w06 = 6, w08 = 8, w09 = 9).

## Findings
- The Penetrator sits on the Devastator slot (the HUD indexes shellyupgrades[DEVISTATOR_WEAPON], weapons_hud.CON:18388).
  AAPLAYER.CON:66 sets WEAPON7_SHOOTS to BARRETPROJ with PROJ_WORKSLIKE 1179649 (AADEFS.CON:4871); bit 0 is
  PROJECTILE_HITSCAN (actors.h:368). The stock table said 644 and every Penetrator shot was led.
- The mirror case: the pistol slot fires MANDOLASER, a real 768 projectile the stock table refuses to lead.
- Muzzle z: a Devastator-slot hitscan BARRETPROJ kept zoffset6, giving an impact z of 794 against 1818 expected (the
  difference is EK_ZOFFSET6), passing over a crouching NEWTROOPDUCKING. Hitscan tiles per P_PreFireHitscan
  (player.cpp:1194-1236): SHOTSPARK1, SHOTGUN and CHAINGUN share a case, GROWSPARK has its own (names.h 582 / 585 / 565 /
  518).

## Defaults trap
C_InitProjectiles (gamedef.cpp:6551-6570) sets workslike 1 (= HITSCAN) and vel 600 (:6564-6565): a declared projectile
with no PROJ_WORKSLIKE is HITSCAN, and one with no PROJ_VEL flies at 600. Both are baked into the rows.

## Declaration order
The last declaration wins for defineprojectile (gamedef.cpp:2289, :2295), but CON_DEFINE keeps the FIRST value
(:2898-2906), so the generator walks the include chain in the engine's order. The script-file fingerprint (sizes, never
mtime) is checked against con_stops_aa.h's table by the suite and by constops.cpp at run time.

## Measured walk
80 script files, 5372 defines, 292 PROJ_WORKSLIKE/PROJ_VEL lines, 158 projectiles (24 hitscan, 134 flying), 24 of them on
the vel default of 600; nothing dropped or ignored.

## Method
Walking AA's CON include chain with tools/gen_con_proj_aa.py against the engine source.
