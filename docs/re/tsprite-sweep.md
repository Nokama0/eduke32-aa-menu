# Sprite sweep for the out-of-bounds camera (tsprites.h)

The whole-map draw needs a tsprite list for every sector. The menu builds it itself. Build: eduke32.exe r10669.

## Why the engine's scan is not called
The engine's external arm calls `polymer_scansprites(i, tsprite, &spritesortcnt)` for every sector (polymer.cpp:1234).
It is `static inline` (polymer.h:419, polymer.cpp:3891) and GCC inlined both call sites: there is no body in
eduke32.exe. The menu uses its own copy of what the inlined code does.

## renderMakeTSpriteFromSprite
- build.h:700-727 moves nineteen members from sprite[n] into a tspritetype, zeroes clipdist and sets owner to n.
- spritetypev7 (buildtypes.h:165-192) and tspritetype (buildtypes.h:196-224) have the same members in the same order,
  both 44 bytes.
- The shipped image's inlined copy at rva 0x0159714 writes every offset 0..0x2B except 0x12 (zeroed) and 0x1E (the
  sprite's own index).

## Sprite chains
- do_insertsprite_at_headofsect sets sectnum and the chain together, in the same two lines (engine.cpp:8721-8732);
  do_deletespritesect unlinks and deletesprite parks sectnum at MAXSECTORS (:8838). A sprite in a sector chain is
  therefore a live one.
- showinvisibility is set only by mapster32 (astub.cpp:5667, :10125), so it is false in the game.

## Array agreement
The sprite array has to be named by two independent searches. The image walk names `sprite` (rva 0x0AEE360 on r10669);
the content scan once named the CON VM's vm.pSprite at rva 0x0DDC150 instead. Both name the same array, rva
0x127C640. That address is what must agree; comparing the two slot rvas refused the whole sprite pass whenever they
differed while naming one array (many globals hold the sprite base). The array is described in
[gamestate.md](gamestate.md).

## Window shape for polymer_scansprites
The inlined body lies between polymer_drawsector's and polymer_updatewall's call sites inside polymer_drawrooms. The
compare against 4095 is the MAXSPRITESONSCREEN-1 test. Addresses of the window: [freecam.md](freecam.md).

## Burst log (nocullburst.h)
Two pure rules: an order-independent fingerprint of a sweep's sector ids, and the wall-clock schedule that starts a burst
of consecutive passes (unsigned subtraction, so either clock's wrap is harmless). A 17th arrival inside one burst is never
appended; a burst flushes on the count after the append that completes it.

## Method
Ghidra on the stock build, with the engine source for the member lists.
