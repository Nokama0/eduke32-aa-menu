# Documentation

Reference notes for the EDuke32 Alien Armageddon mod menu. All notes live in
`re/` and record facts about the engine (eduke32.exe r10669) or about Alien
Armageddon's CON scripts, each with its method. Engine notes were shared with
the sibling HRP menu.

## Alien Armageddon notes

- [re/cheats.md](re/cheats.md) -- every engine cheat code; AA adds, removes and hooks none; `dndebug` as a gamevar oracle.
- [re/chaingun-floor.md](re/chaingun-floor.md) -- CON frame classification and fire-rate floor of the chaingun (1.00x), and the shipped fire rate against it.
- [re/pistol-floor.md](re/pistol-floor.md) -- the same for the pistol (1.125x) and the freezer screen.
- [re/fire-rate-stops.md](re/fire-rate-stops.md) -- where the fire-rate advance must stop: the engine's weapon loop and AA's CON frames.
- [re/projectiles.md](re/projectiles.md) -- which AA weapon slots fire hitscan and which fly, and the CON defaults behind it.

## Engine notes

- [re/actorstate.md](re/actorstate.md) -- locating `actor[]` and `apScript`, and what they say about play-dead and dying actors.
- [re/autosave.md](re/autosave.md) -- which engine path counts as an autosave.
- [re/autoswitch.md](re/autoswitch.md) -- weapon auto-switch on pickup and the `cl_weaponswitch` cvar.
- [re/cansee.md](re/cansee.md) -- locating `cansee` in the shipped image.
- [re/engine-facts.md](re/engine-facts.md) -- short findings behind small modules: aim, cvars, damage numbers, mouse binds, tiles, hooks.
- [re/freecam.md](re/freecam.md) -- free camera: draw path, locates, transports, sky, fog, teleport.
- [re/frustum-cull.md](re/frustum-cull.md) -- frustum culling for the whole-map sweep.
- [re/gamehooks.md](re/gamehooks.md) -- hooks on `P_PreFireHitscan`, `A_ShootWithZvel` and `A_IncurDamage`.
- [re/gamestate.md](re/gamestate.md) -- locating `sprite[]` and the other engine arrays.
- [re/hitscan.md](re/hitscan.md) -- calling the engine's `hitscan`: locate, side effects, self-test.
- [re/image-scan.md](re/image-scan.md) -- which parts of the loaded image may be scanned, and the `.bss` problem.
- [re/modelbox.md](re/modelbox.md) -- model bounding boxes from raw MD3 numbers to Build coordinates.
- [re/picnums.md](re/picnums.md) -- picnum tables: switches, doors, pickups, breakable walls, one-shot cover.
- [re/playerstate.md](re/playerstate.md) -- player structure offsets, shot geometry and the level-loaded rule.
- [re/polymer-projection.md](re/polymer-projection.md) -- Polymer matrices, `viewingrange` and world-to-screen projection.
- [re/polymer-sprite-crash.md](re/polymer-sprite-crash.md) -- the upstream Polymer sprite crash on explosive barrels.
- [re/polymer-sweep.md](re/polymer-sweep.md) -- Polymer whole-map sweep: sky, array bounds, culling, masks.
- [re/secret-path.md](re/secret-path.md) -- secret path search: doors, lotags, sector effectors.
- [re/secret-triggers.md](re/secret-triggers.md) -- secrets opened by activators and master switches.
- [re/secretvol.md](re/secretvol.md) -- secret sector volumes: cache and map-change window.
- [re/shot.md](re/shot.md) -- screenshot mode: HUD cvars, status bar and scuba mask redirects.
- [re/skipintro.md](re/skipintro.md) -- skip intro: `g_noLogo` located by two signature scans that must agree.
- [re/target-liveness.md](re/target-liveness.md) -- when an enemy sprite counts as alive.
- [re/targets.md](re/targets.md) -- target selection: dying, choking, hittable and deferred damage.
- [re/tsprite-sweep.md](re/tsprite-sweep.md) -- the tsprite list for the out-of-bounds camera sweep.
- [re/ud-offsets.md](re/ud-offsets.md) -- offsets of the `ud` structure read off the image.
