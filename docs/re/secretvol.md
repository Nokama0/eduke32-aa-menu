# Secret sector volumes: cache and map-change window (secretvol.h)

The menu draws the volume of each secret sector from a double-buffered cache that a 1 Hz sampler refreshes
(`secretvol_generation`).

## Map-change window
- Nothing invalidates the cache on level load; it is replaced on the next 1 Hz sampler tick. For up to one scan
  interval the old map's geometry can draw for a sector that is a secret in both maps.
- The lotag re-check reads the NEW map's lotag, so it does not cover this. The producer also compares the live wallnum
  against the staged `n`, which turns away changed loop lengths.
- What remains: same index, same wall count, different vertices, for under a second; it corrects itself.
- A `(numsectors << 16) ^ numwalls` signature lags equally: mapextent republishes on the same sampler, and
  esp_breakables.h's ekm_wall_scan_due states the same lag.
- `ekm_level_loading` is not a render-thread level signal. An in-session load drives gm to 0 and back inside one
  synchronous call without presenting a frame (see [playerstate.md](playerstate.md)).

## Map extent (mapextent.h)
- The engine's numwalls and numsectors are not an adjacent pair: on E1L1 numwalls sits at rva 0x0AD5AF8 and numsectors at
  0x0AD5B44, 76 bytes apart and in the opposite order to build.h's declaration, each isolated among zeros and both just past
  the sector pointer at 0x0AD5AF0. No scan for an adjacent pair can find them; every other position holding either value
  was an arithmetic index table. The chain walk derived 325 sectors over 1975 walls unaided on that map, and the probe
  confirmed them as the engine's numbers.
- Residual risk of the walk alone: at index numsectors a stale record from the previous map can continue this map's chain
  (generally not, but it can). Then the walk runs to the end of the previous map and publishes exactly the previous map's
  (numsectors, numwalls), stably for the whole level (nothing rewrites sector[numsectors], so a re-walk reads the same
  bytes); the log shows one ordinary-looking line. No read leaves the array, so it cannot crash.
- Hardening: once a map is up, a narrow window around sectorstate's located anchor (a few hundred bytes either side) is
  searched for the walk's own (n, w), accepted only where each value sits at exactly one position, and read beside every
  walk. Agreement publishes; disagreement publishes zero and logs loudly, so the ESP draws nothing rather than the wrong
  map's secrets. Where the window cannot name the two the walk stands alone, unhardened. No offset is hardcoded.
- What the bootstrap trusts: it looks for the values the walk produced when it ran, on the first plausible walk after
  process start, when .bss past the map is still zeroed. A process that re-locates later has no such bound.
- An implausible walk outranks everything else (a half-loaded level is not a disagreement). Any agreeing scan resets the
  counter. The floor on n is sectorstate's own constant (a map under sixteen sectors never reaches the rule). At the true
  end, sector[n] is zeroed on a fresh game, or after a larger map a stale record whose wallptr is the old map's prefix
  sum, which generally does not follow this map's sector n-1. n == EK_MAXSECTORS has no index n to test.
- Why not prsectors[]: it is the extent and camerascan finds it, but it would tie a map feature to one renderer.

## Footprints (footprint.h)
Build's renderer is wall-driven: polymost_scansector (polymost.cpp:6681) admits only front-facing walls, flats are drawn
inside wall bunches, and a sector with no front-facing wall is never submitted. The cutaway therefore paints a sector's
whole footprint when a floor or ceiling call arrives for it. The tessellator needs one contour per loop (the outer wall
run and each inner hole are separate closed chains); a loop under three walls is not a polygon. The cache builds on the
draw thread, from nocull.cpp's seed seam, so one thread builds and reads. The (sector, wall) pack is exact: numsectors <=
EK_MAXSECTORS (4096) and numwalls <= EK_WALL_SLOTS (16896). The pointer is valid until the next footprint_frame_begin.

Related: [secret-triggers.md](secret-triggers.md), [secret-path.md](secret-path.md).

## Method
Reading the sampler and producer code against the engine's level-load path.
