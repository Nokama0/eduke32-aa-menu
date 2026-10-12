// Verbatim from Megaton's font.h; keep identical.
#pragma once

enum { FONT_UI = 0, FONT_SMALL = 1, FONT_MONO = 2, FONT_COUNT = 3 };

struct Glyph {
  short x, y, w, h;
  short bx, by;
  short adv;
};

extern const int FONT_ATLAS_W;
extern const int FONT_ATLAS_H;
extern const unsigned char font_atlas[];
extern const Glyph font_glyphs[FONT_COUNT][96];
extern const int font_height[FONT_COUNT];
