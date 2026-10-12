<div align="center">

# Alien Armageddon Mod Menu

**Outlines, aim assist and a free camera that know Alien Armageddon's enemies, items and weapons.**

A mod menu for Alien Armageddon, the Duke Nukem 3D total conversion, on EDuke32.

![Game](https://img.shields.io/badge/Game-Alien%20Armageddon-FFB000?labelColor=0E0F13&style=flat)
![Platform](https://img.shields.io/badge/Platform-Windows-FFB000?labelColor=0E0F13&style=flat)
![Release](https://img.shields.io/github/v/release/Nokama0/eduke32-aa-menu?label=Release&color=FFB000&labelColor=0E0F13&style=flat)
![Build](https://img.shields.io/github/actions/workflow/status/Nokama0/eduke32-aa-menu/build.yml?branch=main&label=Build&labelColor=0E0F13&style=flat)
![License](https://img.shields.io/badge/License-Unlicense-FFB000?labelColor=0E0F13&style=flat)

</div>

---

A mod menu for **Alien Armageddon** 5.91, the Duke Nukem 3D total conversion, running on EDuke32 r10669 x64
(`r10669-ec5824db8`) with the Polymost renderer. Press **Insert** in game to open it.

It loads as a `version.dll` next to the game's `eduke32.exe`, which forwards every call to the real one in System32.
It does not change any game file.

This is the Alien Armageddon build of the [EDuke32 HRP menu](https://github.com/Nokama0/eduke32-hrp-menu): the same
panel, with features that know Alien Armageddon's enemies, items and weapons.

## What it does

The menu has six tabs.

**Visuals**
- Enemy boxes with health bars, names, distance and level; friendlies drawn apart from enemies
- Markers for items and pickups (with Alien Armageddon's own item names), figurines and a figurine counter, EDF
  chests and bomb parts, explodables, breakables, cash containers, switches and locks
- Secret areas, secret triggers and a secret path
- Damage numbers; draw distance

**Combat**
- Aimbot: target choice, aim point with Alien Armageddon's headshot zones, cone, range, lead for moving targets,
  hold key, smoothing
- Triggerbot, and silent aim
- Companion bots, including ones waiting to be recruited, are left alone

**Weapons**
- Perfect accuracy per weapon
- Fire rate, reload speed and weapon switch speed sliders that account for Alien Armageddon's weapon timings

**Items**
- Coin magnet with speed, and health coins

**View**
- Custom field of view, and screenshot mode
- A free camera: flight speed, show player, teleport marker, copy and paste a view

**Settings**
- Menu key and scale, skip intro
- Checkpoint autosave with a limit on how many are kept
- Diagnostics: debug log, shot tracers, frame timings

Cheat-style features start off. Hover a row for a short explanation.

## Get it

You need Alien Armageddon 5.91 with the EDuke32 r10669 x64 `eduke32.exe` it ships with. Other versions are not
supported.

1. **Download** `version.dll` from the [latest release](https://github.com/Nokama0/eduke32-aa-menu/releases/latest).
2. **Copy** it into the Alien Armageddon folder, next to `eduke32.exe`. If a `version.dll` is already there (another
   mod's), move it somewhere safe first.
3. **Start** the game and press **Insert**.

To remove it, delete `version.dll`, `ekmod.ini` and `ekmod.log` from that folder.

## Good to know

- **Only the game.** The menu starts only inside `eduke32.exe`. The map editor (`mapster32.exe`) in the same folder,
  or a renamed exe, loads the DLL but gets no menu and no `ekmod.log`.
- **Where files go.** Settings (`ekmod.ini`) and the log (`ekmod.log`) are written beside `eduke32.exe`. The menu
  never rewrites the game's own configuration.
- **Generated tables.** Item names, enemy names and weapon data come from tables built from Alien Armageddon's own
  scripts, so they match version 5.91 and may be wrong for any other.

---

## Build it

You need Windows with Visual Studio 2022 (the MSVC x64 toolset) and a Windows 10 SDK. The scripts find them with
`vswhere`.

```powershell
powershell -File src/ekmod/build.ps1
```

### Tests

`build.ps1` runs a unit-test host of the pure rules first, then compiles `build\version.dll`. A red test fails the
build; a good run prints a line ending in `N passed, 0 failed`. The build also checks that the DLL exports every
function the real `version.dll` does.

### Generated tables

The scripts in [`tools/`](tools) generate those tables from your own Alien Armageddon install:

```powershell
python tools/<script>.py --game-dir <Alien Armageddon folder>
```

Or set `AA_GAME_DIR` instead of passing `--game-dir`.

Design notes and findings are in [`docs/`](docs/README.md).

## Other menus

The same menu, ported to other Duke Nukem games.

| Game | Repository |
|---|---|
| Duke Nukem Forever | [dukenukem-forever-menu](https://github.com/Nokama0/dukenukem-forever-menu) |
| Duke Nukem: Manhattan Project | [dukenukem-manhattanproject-menu](https://github.com/Nokama0/dukenukem-manhattanproject-menu) |
| Duke Nukem 3D on EDuke32 with the High Resolution Pack | [eduke32-hrp-menu](https://github.com/Nokama0/eduke32-hrp-menu) |
| Duke Nukem 3D: 20th Anniversary World Tour | [duke3d-worldtour-menu](https://github.com/Nokama0/duke3d-worldtour-menu) |
| Duke Nukem 3D: Megaton Edition | [duke3d-megaton-menu](https://github.com/Nokama0/duke3d-megaton-menu) |

## License

[The Unlicense](LICENSE): public domain, no warranty.

The bundled [Dear ImGui](https://github.com/ocornut/imgui) 1.92.6 (`src/ekmod/imgui`) is MIT licensed; see its
[LICENSE.txt](src/ekmod/imgui/LICENSE.txt).
