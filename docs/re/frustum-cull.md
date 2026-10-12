# Frustum culling for the whole-map sweep

The menu rejects sectors outside the view before drawing the whole map from outside (frustum.h). Nothing is cached.
Build: eduke32.exe r10669.

## Why the engine's functions are not called
- polymer_planeinfrustum is `static inline` (4 call sites) and tests a _prplane's own vertex buffer, which needs the
  _prsector / _prplane x64 layout with PR_MAXPLANELIGHTS-sized arrays. Its criterion is polymer.cpp:3865-3888.
- polymer_extractfrustum is `static`, carries no string (finding it is a third-order inference through the call graph)
  and WRITES GL state: it multiplies on the GL_TEXTURE stack and leaves identity there (polymer.cpp:3841-3846).
- The engine's external arm (polymer.cpp:1229-1243) draws all numsectors / numwalls with no test; it is a rarely taken
  fallback in the engine, while the menu's row ran it every frame.

## Never wrongly rejects
The test accepts (does not cull) when any of these hold:
- within EK_CULL_MARGIN of the left or right edge;
- a sector footprint contains the camera (the apex lies ON both planes);
- a frame with no glGetFloatv, a degenerate matrix, or an apex that is not the drawing camera;
- a sloped sector or neighbour, or an unreadable wall;
- above or below the view: there is no top or bottom plane, because Build maps spread horizontally.

## Slab
polymer_updatewall builds two-sided quads from ns->floor.buffer and ns->ceil.buffer (polymer.cpp:3404-3410,
:3478-3481). getzsofslopeptr is engine.cpp:14288-14300; ceilingz and floorz are the z at the sector's first wall.

## Apex tolerance
Float error comes from the cancellation of two terms of the map's extent, amplified by normalising a matrix that
carries polymer's 1/1000 scale.

## Method
Reading polymer.cpp and the shipped image.
