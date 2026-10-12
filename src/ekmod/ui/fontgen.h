#pragma once
#include "font.h"

struct FontAtlas {
  int w, h;
  unsigned char* pixels;
  Glyph glyphs[FONT_COUNT][96];
  int height[FONT_COUNT];
  int pct;
};

// All or nothing: on failure `out` is untouched.
bool fontgen_build(int pct, FontAtlas* out);

void fontgen_free(FontAtlas* a);
