# Scanning the game image: sections and the .bss problem

Several locators read the loaded eduke32.exe image (imagescan.h). This note records which parts of the image they
may search and why.

- `ek_image_sections` drops .bss: it has SizeOfRawData 0, so there are no raw bytes to search.
- `ek_image_data_extent` exists for the same reason. A membership test built from the byte-searchable sections refused
  every .bss array: Polymer's drawingstate was refused and the whole-map sweep stayed unavailable until the check used
  the extent of the containing section.
- `ek_image_count_calls` (counting `call rel32` sites that reach a given function) was lifted out of camerascan.cpp so
  every locator shares it.

## Mask locate (maskscan.cpp)
The bracket walk is camerascan.cpp's find_getz minus the shape test; the whole-image count is ek_image_count_calls, so
both share one bounds rule and one fault policy. The trust is one-sided: a missed anchor leaves the window too long, which
reads the previous callee's setup and costs a refusal (safe); a false anchor (an 0xE8 operand byte whose displacement lands
on a function entry) cuts the window too short and could hide real argument setup, letting a wrong candidate through. The
anchor measured here is the genuine G_DoSpriteAnimations call. A site whose bytes were not captured is rejected. The
target is written first and the site last, so a reader that sees the site has seen the target. Every match is listed with
its ordinal.

Related: [polymer-sweep.md](polymer-sweep.md), [freecam.md](freecam.md).

## Method
Reading the PE section table and exception directory of the stock build (r10669).
