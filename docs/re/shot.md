# Screenshot mode: HUD cvars, status bar and scuba mask redirects (shot.cpp)

Screenshot mode hides the HUD while the free camera flies. Build: eduke32.exe r10669.

## Cvars
- Changed: `crosshair` = ud.crosshair, CVAR_BOOL 0..1 (osdcmds.cpp:1596); `r_drawweapon` = ud.drawweapon, CVAR_INT 0..2
  (:1713; 0 off, 1 weapon, 2 icon only); `hud_stats` = ud.levelstats, CVAR_BOOL (:1665; a judgement call, included
  because it is informational text burned into every shot).
- Left alone: `hud_statusbarmode` (CVAR_BOOL|CVAR_FUNCPTR; the handler calls G_UpdateScreenArea after the store,
  osdcmds.cpp:1483-1486, and cvar_set_int writes only the pointer; ud.screen_size is the same trap with no cvar; World
  Tour hit it), `hud_showmapname` (load only), P_DisplayScuba (no cvar; r_drawweapon does not gate it), `hud_messages`
  (ud.fta_on; left on, nothing new appears while the camera flies), `hud_althud` / `hud_position` (style only).

## settings.cfg hazard
- CONFIG_WriteSettings runs unconditionally at the end of CONFIG_WriteSetup (config.cpp:1044), reached on exit
  (G_Shutdown, game.cpp:6040) and on every save including the menu's autosave (G_SavePlayerMaybeMulti,
  savegame.cpp:957). A missed restore leaves crosshair and weapon off in the player's config.
- Detach restore covers the DLL unloading while the game runs; on a clean quit settings.cfg is already written inside
  G_Shutdown, so detach is the belt.
- The menu cannot be opened while the camera flies (input.cpp camera_owns_input returns 0 for every WM_KEYDOWN /
  WM_SYSKEYDOWN, Esc included; freecam_enter_rule refuses while the menu is open), so a menu quit never reaches an OFF
  edge. The on-disk stash (the mod's ini, written before the first 0) closes it.
- Detach deliberately does not clear the stash: WritePrivateProfileStringA under the loader lock at process exit is what
  it must not do. Residual cost: one stale ini section that the next launch recognises (shot_stash_healed) and drops.
  Deleting version.dll after a crash mid-flight never replays the stash.

## Status bar redirect
- "Game Paused" (screens.cpp:1301) occurs once in .rdata at rva 0x0519092, with one lea at 0x00E7F6C inside
  G_DisplayRest, bracket 0x00E6360..0x00E99AC. 85 call sites reach 40 distinct entries. One shaped function, 0x00E2330
  (11745 bytes), loads "On", "Off" and "Auto" (sbar.cpp 691-704, 782-788, 1012-1020), with one call site at 0x00E65F3.
  Across all 8554 functions it is the only one loading all three literals and has only that call site, so
  screens.cpp:1160's second call is not compiled in.
- The locate counts sites in a separate pass because the walk skips targets already seen. It is a call-site redirect,
  not an entry hook: four operand bytes of a five-byte call (hook.cpp).
- Skipping G_DrawStatusBar is safe: its only non-drawing effects are a redraw dirty flag it sets itself (pus,
  sbar.cpp:587) and two lazily filled static icon tables.
- The thunk runs on the game thread and reads s_active one frame after shot_frame wrote it (draw precedes the swap).

## Scuba mask redirect
- No `call P_DisplayScuba` exists (screens.cpp:1069 and :1075; the second is inside SPLITSCREEN_MOD_HACKS, not defined;
  P_DisplayWeapon, the paired arm, has one call image-wide). The body is inlined in G_DisplayRest (8554 of the 8561
  RUNTIME_FUNCTION rows have End > Begin; six are all-zero, one zero-length).
- Redirected are its three draws (player.cpp:2178-2181 G_DrawTileScaled: GL seam half, left, right). They are found by
  x coordinate as `mov ecx, imm32`: 320-43 once in the bracket and once image-wide, 43 and 44 once each; the one `call
  rel32` within 18-21 bytes (window 32) reaching a function entry; all three reach the same entry, reached from
  exactly three sites (a fourth would be redirected blind); `mov ecx, EVENT_DISPLAYSCUBA` (player.cpp:2161, events_defs.h
  order frozen) corroborates. The thunk re-checks its arguments at run time. The opcode choice is the one compiler fact.
- Seven int arguments: four in ecx / edx / r8d / r9d, the rest at [rsp+0x20..0x30]; all three sites write the seventh
  slot and keep nothing in a volatile register across the call.

## AA HUD events (r10669)
- WEAPON: `mov ecx,39` at 0x0C1C79 and `call 0x067820` at 0x0C1C86 (P_DisplayWeapon). REST: `mov ecx,60` at 0x0E760F,
  `call 0x067820` at 0x0E7642. CROSSHAIR: `mov ecx,59` at 0x0E929E, `call 0x067B80` at 0x0E92C3 (both in G_DisplayRest).
- 0x067820 reads ecx/edx/r8d (never r9); 0x067B80 also reads r9d. There are five `mov ecx,39` in the image; REST's next
  call renderSetAspect (screens.cpp:1297) sits inside the window behind it.
- The status bar and scuba redirects count their sites in one pass over the 8554 RUNTIME_FUNCTIONs; `mov ecx, 320-43`
  occurs once in the bracket and once image-wide.

## Install
All-or-none with unwind. s_scubaPatched stays 0 during install and is zeroed before the first removal, so any thunk still
reachable forwards. The original address is cleared only after every patch is out.

## Method
Ghidra on the stock build, engine source for the cvars and call sites; restore paths exercised in game.
