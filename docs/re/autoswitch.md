# Weapon auto-switch on pickup (cl_weaponswitch)

The menu's pickup-switch control (autoswitch.h) maps onto the engine's `cl_weaponswitch` cvar.

- `cl_weaponswitch` is CVAR_INT|CVAR_MULTI with range 0..7 (osdcmds.cpp:1628): 0 disabled, 1 if new, 2 if empty, 4
  determined by the weapon-choice order (wchoice).
- The engine menu encodes it at menus.cpp:4136-4146 and decodes at :4493: `(ws&1) ? ((ws&4) ? 2 : 1) : 0`.
- P_AddWeaponMaybeSwitch (gameexec.cpp:935-960) takes the wchoice compare only when `(ws & 5) == 5`, else
  `P_AddWeapon(ps, w, ws & 1)`. Bit 4 alone reads as "never switch on pickup", a value two independent rows could
  produce.
- World Tour's executable has no WeaponSwitchMode / autoswitch (removed), so that port hooks addweapon and
  checkavailweapon instead.

## Method
Engine source reading.
