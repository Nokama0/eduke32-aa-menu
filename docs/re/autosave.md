# Autosave: which engine path counts as an autosave (autosave.cpp)

The menu's timed autosave raises the engine's real autosave request. Build: eduke32.exe r10669.

## The two save paths
- Raising g_doQuickSave (QUICKSAVE, game.cpp:5040-5078) is the wrong request. It writes g_lastusersave, opens the Save
  menu when that is empty, follows the player's own last save, and calls G_SavePlayerMaybeMulti with isAutoSave false,
  so the engine's reaper never counts it. The symptoms: the first autosave opened the Save Game window, later ones
  used the last slot, a manual save moved the target, and "Autosaves kept" did nothing.
- The real autosave is g_saveRequested (game.cpp:7185-7197): it captures the thumbnail, calls
  `G_SavePlayerMaybeMulti(g_lastautosave, TRUE)` and points g_quickload at it. isAutoSave sets the header bit
  (savegame.cpp:1730) that ReadSaveGameHeaders counts, keeping the newest cl_maxautosaves and deleting the rest while
  cl_autosavedeletion is on (savegame.cpp:274-296). With cl_autosavedeletion off nothing is reaped (engine behaviour).

## Path reset
The path is cleared before each request. G_SavePlayer reopens g_lastautosave.path when it is still valid
(savegame.cpp:860-866), so one file would be overwritten forever. Clearing is what CON_SAVE does when the slot changes
(gameexec.cpp:5332-5334) and premap.cpp:1451 does on a new game; G_SavePlayer then allocates the next saveNNNN.esv
(:868-881).

## In-play gate
G_HandleLocalKeys refused quicksave over a dead player and outside MODE_GAME; the g_saveRequested block has no
refusals, so autosave_in_play is the only guard. The request runs in the SwapBuffers hook on the game thread; the
write is a deferred request and no player state is written.

## Save name
The engine path does not fill the short save name: only CON_SAVE/CON_SAVENN write g_lastautosave.name (neither occurs in
DUKE3D.GRP) and an empty-name save is isUnreadable (hidden, then deleted). The name is the mod's own, in the engine's
strftime format "%d %b %Y %I:%M%p". The locate order is cvar registry, then signature scan, never a hardcoded offset; the
registry cannot answer because these are plain globals.

## Interval timer
World Tour restarts its timer on a MAP SIGNATURE change, for lack of a level-change callback. Here gm reads 0 for the
whole level build (premap.cpp:1465, ekm_level_loading), so the timer restarts on every frame of the load and the
interval starts when the level FINISHES loading.

## Locating the globals
There is no cvar registry entry for these (plain globals) and no hardcoded offsets: two witnesses must agree.
- Witness 1, the main loop (game.cpp:7195-7196): a 24-byte run unique in the image, `mov rA,[rip+d]; mov byte [rA],0`
  (g_saveRequested = false), then `mov rB,[rip+d]; mov byte [rB+0x77FF],0xC7` (walock[TILE_SAVESHOT] =
  CACHE1D_UNLOCKED; 0x77FF = MAXTILES-1, 0xC7 = 199). In the 96 bytes before it exactly two rip-relative leas load the
  same address (G_SavePlayerMaybeMulti's argument and g_quickload), both &g_lastautosave.
- Witness 2, CON_SAVE in VM_Execute: the strftime literal "%d %b %Y %I:%M%p" occurs once and one lea points at it.
  Within 64 bytes after it: one `call rel32`, then `mov rC,[rip+d]; mov byte [rC],1` (g_saveRequested = true,
  gameexec.cpp:5344-5347). Within 160 bytes before it is the strcmp setup: two slot loads and two `lea r,[base+0x20]`
  (the .path fields); the one whose destination is rcx is g_lastautosave (strcmp's first argument).
- One global is reached through a .rdata pointer slot at the CON_SAVE site and one directly at the main-loop site (GCC
  build), so both are resolved and bounds-checked first.

## Method
Ghidra on the stock build, engine source (game.cpp, savegame.cpp, gameexec.cpp), and in-game autosave runs.
