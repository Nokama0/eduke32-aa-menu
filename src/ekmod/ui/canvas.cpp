#include "canvas.h"
#include "font.h"
#include "ui.h"

// World Tour's src/wtmod/canvas.cpp with its backend replaced: every function draws what the D3D11 and OpenGL ones draw
// through Dear ImGui's ImDrawList. canvas_set_drawlist(ImDrawList*) is declared by the caller, not ui/canvas.h (kept
// verbatim across three ports); every primitive is a no-op until it is set, once a frame.

static int s_scale = UI_SCALE_DEFAULT;

void canvas_set_scale(int pct) { s_scale = ui_scale_clamped(pct); }
int  canvas_scale()            { return s_scale; }

static bool s_magnify = false;
void canvas_set_magnify_text(bool on) { s_magnify = on; }
bool canvas_magnify_text()            { return s_magnify; }
static int s_textSlot = CANVAS_SLOT_PANEL;
void canvas_set_text_slot(int slot) {
  s_textSlot = (slot >= 0 && slot < CANVAS_SLOT_COUNT) ? slot : CANVAS_SLOT_PANEL;
}
int  canvas_text_slot()             { return s_textSlot; }

static int s_frameH = 0;
int canvas_frame_height() { return s_frameH; }

static int    s_txDx = 0, s_txDy = 0;
static int    s_txCx = 0, s_txCy = 0;
static double s_txScale = 1.0;
static bool   s_txIdentity = true;

void canvas_set_transform(int dx, int dy, double scale, int cx, int cy) {
  s_txDx = dx; s_txDy = dy; s_txCx = cx; s_txCy = cy;
  s_txScale = scale;
  s_txIdentity = (dx == 0 && dy == 0 && scale == 1.0);
}

static int tx_round(double v) {
  return clamp_double_to_int(v < 0.0 ? v - 0.5 : v + 0.5);
}

static int tx_dev_x(int d) {
  if (s_txIdentity) return d;
  const double c = (double)ui_scale_px(s_txCx, s_scale);
  return tx_round(((double)d - c) * s_txScale + c + (double)ui_scale_px(s_txDx, s_scale));
}
static int tx_dev_y(int d) {
  if (s_txIdentity) return d;
  const double c = (double)ui_scale_px(s_txCy, s_scale);
  return tx_round(((double)d - c) * s_txScale + c + (double)ui_scale_px(s_txDy, s_scale));
}
static int tx_dev_len(int d) {
  if (s_txIdentity) return d;
  return tx_round((double)d * s_txScale);
}

static int dev_x(int x)   { return tx_dev_x(ui_scale_px(x, s_scale)); }
static int dev_y(int y)   { return tx_dev_y(ui_scale_px(y, s_scale)); }
static int dev_len(int v) { return tx_dev_len(ui_scale_px(v, s_scale)); }

#ifndef NO_GL
#include <math.h>
#include <string.h>
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "../log.h"
#include "fontgen.h"
#include "fontpack.h"

static int s_devW = 0, s_devH = 0;

static ImDrawList* s_dl = 0;

void canvas_set_drawlist(ImDrawList* dl) { s_dl = dl; }

// The atlas: font_data.cpp's 8-bit coverage packed into ImGui's OWN atlas as custom rects, so glyphs and flat fills
// share a texture and one draw call (fills take TexUvWhitePixel). One scaled-text rect per slot, not per frame: the ESP
// draws at 100% and the panel at the applied scale inside ONE canvas frame.

// Reserved once, at a rung that cannot overflow: ImGui allocates custom rects at atlas build, so growing one means a
// repack and a texture re-create. The panel's slot is FONTPACK_LADDER rung 3 (1024x1024, 4 MB RGBA32), the corner's
// rung 2 (1024x512, 2 MB), the smallest that holds 200% (ekm_text_slot_rung). Each slot holds one scale a frame.
struct TextSlot {
  ImFontAtlasRectId id;
  int  rectX, rectY, ready;
  int  rectPct;
  FontAtlas run;
  int  failedPct;
  int  saidFallback;
};
static TextSlot s_slots[CANVAS_SLOT_COUNT] = { { ImFontAtlasRectId_Invalid },
                                               { ImFontAtlasRectId_Invalid } };
static const char* const SLOT_NAME[CANVAS_SLOT_COUNT] = { "panel", "corner" };

static ImFontAtlas*      s_atlas  = 0;
static ImFontAtlasRectId s_rectId = ImFontAtlasRectId_Invalid;
static int s_atlasFailed = 0;

static void slot_fallback_note(int slot) {
  TextSlot& ts = s_slots[slot];
  if (ts.saidFallback) return;
  ts.saidFallback = 1;
  ek_log("canvas: %s text is magnified from the baked glyphs instead of rasterised",
         SLOT_NAME[slot]);
}

// A coverage bitmap into one of our rects: Alpha8 is a row copy, RGBA32 is white with coverage in alpha
// (imgui_impl_opengl3 asserts RGBA32). The WHOLE rect is written, since the previous occupant's leftovers would
// otherwise upload with it; a too-large source is refused, as a glyph past the rect would land on ImGui's font pixels.
static bool atlas_write_pixels(ImFontAtlas* atlas, const ImFontAtlasRect& r,
                               const unsigned char* src, int srcW, int srcH) {
  ImTextureData* tex = atlas->TexData;
  if (!tex || !tex->Pixels) {
    ek_log("canvas: ImGui's atlas has no pixel buffer; the panel will not draw");
    return false;
  }
  if (!src || srcW <= 0 || srcH <= 0 || srcW > (int)r.w || srcH > (int)r.h) {
    ek_log("canvas: a %dx%d atlas does not fit the %dx%d rect reserved for it",
           srcW, srcH, (int)r.w, (int)r.h);
    return false;
  }
  for (int y = 0; y < (int)r.h; y++) {
    const unsigned char* line = (y < srcH) ? src + (size_t)y * (size_t)srcW : 0;
    const int copied = line ? srcW : 0;
    unsigned char* dst = (unsigned char*)tex->GetPixelsAt(r.x, r.y + y);
    if (tex->Format == ImTextureFormat_Alpha8) {
      if (copied) memcpy(dst, line, (size_t)copied);
      memset(dst + copied, 0, (size_t)((int)r.w - copied));
    } else {
      ImU32* out = (ImU32*)dst;
      for (int x = 0; x < copied; x++) out[x] = IM_COL32(255, 255, 255, line[x]);
      for (int x = copied; x < (int)r.w; x++) out[x] = 0;
    }
  }
  ImFontAtlasTextureBlockQueueUpload(atlas, tex, r.x, r.y, r.w, r.h);
  return true;
}

static bool ensure_atlas() {
  if (!ImGui::GetCurrentContext()) return false;
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (!atlas) return false;
  if (atlas == s_atlas && s_rectId != ImFontAtlasRectId_Invalid) return true;
  if (s_atlasFailed) return false;
  s_atlas  = 0;
  s_rectId = ImFontAtlasRectId_Invalid;
  for (int k = 0; k < CANVAS_SLOT_COUNT; k++) {
    s_slots[k].id      = ImFontAtlasRectId_Invalid;
    s_slots[k].ready   = 0;
    s_slots[k].rectPct = 0;
  }
  if (atlas->Locked) {
    static int said = 0;
    if (!said) {
      said = 1;
      ek_log("canvas: ImGui's atlas is locked; the panel will not draw");
    }
    return false;
  }
  ImFontAtlasRect r;
  const ImFontAtlasRectId id = atlas->AddCustomRect(FONT_ATLAS_W, FONT_ATLAS_H, &r);
  if (id == ImFontAtlasRectId_Invalid) {
    s_atlasFailed = 1;
    ek_log("canvas: no room in ImGui's atlas for a %dx%d block; the panel will not draw",
           FONT_ATLAS_W, FONT_ATLAS_H);
    return false;
  }
  if (!atlas_write_pixels(atlas, r, font_atlas, FONT_ATLAS_W, FONT_ATLAS_H)) {
    atlas->RemoveCustomRect(id);
    s_atlasFailed = 1;
    return false;
  }
  s_atlas  = atlas;
  s_rectId = id;
  ek_log("canvas: atlas %dx%d packed at (%d,%d) in ImGui's texture, %d fonts",
         FONT_ATLAS_W, FONT_ATLAS_H, (int)r.x, (int)r.y, FONT_COUNT);

  // The scaled atlases' rects, reserved in the same breath so all are packed together. Not fatal: losing one costs that
  // slot crisp text at scales other than 100; it keeps drawing magnified quads from the rect above.
  for (int k = 0; k < CANVAS_SLOT_COUNT; k++) {
    const int rung = ekm_text_slot_rung(k);
    const int rw = FONTPACK_LADDER[rung].w;
    const int rh = FONTPACK_LADDER[rung].h;
    ImFontAtlasRect rr;
    const ImFontAtlasRectId rid = atlas->AddCustomRect(rw, rh, &rr);
    if (rid == ImFontAtlasRectId_Invalid) {
      ek_log("canvas: no room in ImGui's atlas for the %s's %dx%d scaled-text block; "
             "its text will be magnified instead of rasterised", SLOT_NAME[k], rw, rh);
    } else {
      s_slots[k].id = rid;
      ek_log("canvas: %s scaled-text block %dx%d (ladder rung %d) reserved at (%d,%d)",
             SLOT_NAME[k], rw, rh, rung, (int)rr.x, (int)rr.y);
    }
  }
  return true;
}

// Where the frame's glyphs and white texel are: resolved on the first primitive and held to the end of the frame.
// Re-read each frame because ImGui repacks its texture as it bakes fonts, moving the origin and uv scale.
static int    s_activeResolved = 0;
static int    s_activeOk       = 0;
static int    s_rectX = 0, s_rectY = 0;
static ImVec2 s_uvScale(0.0f, 0.0f);
static ImVec2 s_white(0.0f, 0.0f);

static bool atlas_resolve() {
  if (s_activeResolved) return s_activeOk != 0;
  s_activeResolved = 1;
  s_activeOk = 0;
  if (!ensure_atlas()) return false;
  ImFontAtlasRect r;
  if (!s_atlas->GetCustomRect(s_rectId, &r)) {
    s_atlas  = 0;
    s_rectId = ImFontAtlasRectId_Invalid;
    if (!ensure_atlas()) return false;
    if (!s_atlas->GetCustomRect(s_rectId, &r)) return false;
  }
  s_rectX   = r.x;
  s_rectY   = r.y;
  // Each scaled block moves with the same repacks, so its origin is re-read. Losing one is not fatal: that slot falls
  // back to magnified quads and is reserved again at the next whole-atlas pack.
  for (int k = 0; k < CANVAS_SLOT_COUNT; k++) {
    TextSlot& ts = s_slots[k];
    ts.ready = 0;
    if (ts.id == ImFontAtlasRectId_Invalid) continue;
    ImFontAtlasRect rr;
    if (s_atlas->GetCustomRect(ts.id, &rr)) {
      ts.rectX = rr.x;
      ts.rectY = rr.y;
      ts.ready = 1;
    } else {
      ts.id      = ImFontAtlasRectId_Invalid;
      ts.rectPct = 0;
    }
  }
  s_uvScale = s_atlas->TexUvScale;
  s_white   = s_atlas->TexUvWhitePixel;
  s_activeOk = 1;
  return true;
}

// Lossless text at the applied scale: the baked atlas holds fixed pixels, so a scaled quad only blows up a bitmap. At
// any scale but 100 the faces are rasterised at that scale (fontgen.cpp, GDI) into the reserved rect and placed in
// device pixels. Rebuilt on mouse release, never mid-drag.

static bool runtime_ready(const TextSlot& ts) {
  return !ekm_slot_needs_build(s_scale, ts.run.pct) && ts.run.pixels != 0;
}

static int ladder_rung(int w, int h) {
  for (int i = 0; i < 6; i++)
    if (FONTPACK_LADDER[i].w == w && FONTPACK_LADDER[i].h == h) return i;
  return -1;
}

// Builds the scaled atlas when needed (CPU and GDI only, so canvas_text_width can call it during layout). The first
// line returns before fontgen_build, so only a real refusal reaches failedPct; fontgen.cpp logged its own reason.
static void runtime_prepare(int slot) {
  TextSlot& ts = s_slots[slot];
  if (!ekm_scale_needs_atlas(s_scale)) return;
  if (s_scale == ts.failedPct)         return;
  if (runtime_ready(ts))               return;
  fontgen_free(&ts.run);
  if (!fontgen_build(s_scale, &ts.run)) {
    ts.failedPct = s_scale;
    return;
  }
  // The rung is logged on every rebuild, so a pack climbing past its slot's reservation shows in the log;
  // atlas_write_pixels then refuses the write and that slot magnifies.
  ek_log("canvas: %s scaled atlas %d%% is %dx%d, ladder rung %d; the reserved rect is rung %d",
         SLOT_NAME[slot], ts.run.pct, ts.run.w, ts.run.h, ladder_rung(ts.run.w, ts.run.h),
         ekm_text_slot_rung(slot));
}

// The slot THIS text samples (built for this scale, pixels in its rect), or null for the baked glyphs; builds and
// uploads on demand. Everything that measures or draws text asks this one question, so a width and its glyphs never
// come from different atlases. Each slot holds one scale a frame.
static TextSlot* runtime_live() {
  if (!ekm_text_uses_scaled_atlas(s_scale, s_magnify)) return 0;
  if (!atlas_resolve()) return 0;
  const int slot = s_textSlot;
  TextSlot& ts = s_slots[slot];
  if (ekm_text_slot(s_scale, s_magnify, slot, ts.ready != 0, ts.failedPct) < 0) {
    slot_fallback_note(slot);
    return 0;
  }
  runtime_prepare(slot);
  if (!runtime_ready(ts)) { slot_fallback_note(slot); return 0; }
  if (!ekm_slot_needs_build(s_scale, ts.rectPct)) return &ts;
  ImFontAtlasRect rr;
  if (!s_atlas->GetCustomRect(ts.id, &rr)) return 0;
  if (!atlas_write_pixels(s_atlas, rr, ts.run.pixels, ts.run.w, ts.run.h)) {
    ts.failedPct = s_scale;
    slot_fallback_note(slot);
    return 0;
  }
  ts.rectPct = s_scale;
  return &ts;
}

// canvas.h's colours are 0xAARRGGBB; ImGui's IM_COL32 is 0xAABBGGRR on little-endian, so red and blue swap while alpha
// and green stay put.
static ImU32 to_imgui(unsigned c) {
  return ((c & 0x00FF0000u) >> 16) |
         ( c & 0x0000FF00u)        |
         ((c & 0x000000FFu) << 16) |
         ( c & 0xFF000000u);
}

static ImDrawVert flat(float x, float y, unsigned rgba) {
  atlas_resolve();
  ImDrawVert p;
  p.pos = ImVec2(x, y);
  p.uv  = s_white;
  p.col = to_imgui(rgba);
  return p;
}

bool canvas_white_texel(float* u, float* v) {
  if (u) *u = 0.0f;
  if (v) *v = 0.0f;
  if (!atlas_resolve()) return false;
  if (u) *u = s_white.x;
  if (v) *v = s_white.y;
  return true;
}

static void cv_quad(const ImDrawVert& a, const ImDrawVert& b,
                    const ImDrawVert& c, const ImDrawVert& d) {
  if (!s_dl) return;
  s_dl->PrimReserve(6, 4);
  const unsigned int base = s_dl->_VtxCurrentIdx;
  s_dl->PrimWriteIdx((ImDrawIdx)base);
  s_dl->PrimWriteIdx((ImDrawIdx)(base + 1));
  s_dl->PrimWriteIdx((ImDrawIdx)(base + 2));
  s_dl->PrimWriteIdx((ImDrawIdx)base);
  s_dl->PrimWriteIdx((ImDrawIdx)(base + 2));
  s_dl->PrimWriteIdx((ImDrawIdx)(base + 3));
  s_dl->PrimWriteVtx(a.pos, a.uv, a.col);
  s_dl->PrimWriteVtx(b.pos, b.uv, b.col);
  s_dl->PrimWriteVtx(c.pos, c.uv, c.col);
  s_dl->PrimWriteVtx(d.pos, d.uv, d.col);
}

static void flat_quad(float x0, float y0, float x1, float y1,
                      float x2, float y2, float x3, float y3, unsigned rgba) {
  const ImDrawVert a = flat(x0, y0, rgba), b = flat(x1, y1, rgba);
  const ImDrawVert c = flat(x2, y2, rgba), d = flat(x3, y3, rgba);
  cv_quad(a, b, c, d);
}

// A rect already in device pixels; everything below draws through this so a multi-part shape scales ONCE. Accepted: the
// device right edge ui_scale_px(x) + ui_scale_px(w) can differ by one from ui_scale_px(x + w), so abutting fills can
// show a one-pixel seam at a non-integer scale.
static void fill_device(int x, int y, int w, int h, unsigned rgba) {
  flat_quad((float)x,       (float)y,
            (float)(x + w), (float)y,
            (float)(x + w), (float)(y + h),
            (float)x,       (float)(y + h), rgba);
}

void canvas_begin(int w, int h) {
  s_devW = w;
  s_devH = h;
  s_frameH = h;
  canvas_set_transform(0, 0, 1.0, 0, 0);
  s_magnify = false;
  s_textSlot = CANVAS_SLOT_PANEL;
  s_activeResolved = 0;
  s_activeOk       = 0;
}

void canvas_end() {
  s_dl = 0;
}

void canvas_rect(int x, int y, int w, int h, unsigned rgba) {
  fill_device(dev_x(x), dev_y(y), dev_len(w), dev_len(h), rgba);
}

// GL draws a line as one pixel between pixel centres; here a quad `hw` either side, which at hw 0.5 rasterises the same
// for axis-aligned lines and within a pixel otherwise.
static void line_device(int dx0, int dy0, int dx1, int dy1, float hw, unsigned rgba) {
  const float ax = (float)dx0 + 0.5f, ay = (float)dy0 + 0.5f;
  const float bx = (float)dx1 + 0.5f, by = (float)dy1 + 0.5f;
  float nx = hw, ny = 0.0f;
  const float ex = bx - ax, ey = by - ay;
  const float len = (float)sqrt((double)(ex * ex + ey * ey));
  if (len > 0.0f) { nx = -ey / len * hw; ny = ex / len * hw; }
  flat_quad(ax - nx, ay - ny, bx - nx, by - ny,
            bx + nx, by + ny, ax + nx, ay + ny, rgba);
}

void canvas_line(int x0, int y0, int x1, int y1, unsigned rgba) {
  line_device(dev_x(x0), dev_y(y0), dev_x(x1), dev_y(y1), 0.5f, rgba);
}

void canvas_line_w(int x0, int y0, int x1, int y1, unsigned rgba, int w) {
  int dw = dev_len(w > 0 ? w : 0);
  if (dw < 1) dw = 1;
  line_device(dev_x(x0), dev_y(y0), dev_x(x1), dev_y(y1), (float)dw * 0.5f, rgba);
}

void canvas_bevel(int x, int y, int w, int h, unsigned fill, unsigned hi, unsigned lo) {
  const int dx = dev_x(x), dy = dev_y(y);
  const int dw = dev_len(w), dh = dev_len(h);
  int e = dev_len(1);
  if (e < 1) e = 1;
  fill_device(dx, dy, dw, dh, fill);
  fill_device(dx,          dy,          dw, e,          hi);
  fill_device(dx,          dy + dh - e, dw, e,          lo);
  fill_device(dx,          dy + e,      e,  dh - 2 * e, hi);
  fill_device(dx + dw - e, dy + e,      e,  dh - 2 * e, lo);
}

void canvas_push_clip(int x, int y, int w, int h) {
  if (!s_dl) return;
  const int dx = dev_x(x), dy = dev_y(y);
  const int dw = dev_len(w), dh = dev_len(h);
  s_dl->PushClipRect(ImVec2((float)dx, (float)dy),
                     ImVec2((float)(dx + dw), (float)(dy + dh)), false);
}

void canvas_pop_clip() {
  if (!s_dl) return;
  s_dl->PopClipRect();
}

void canvas_hazard(int x, int y, int w, int h, unsigned stripe, unsigned ground, int period) {
  const int dx = dev_x(x), dy = dev_y(y);
  const int dw = dev_len(w), dh = dev_len(h);
  const int dp = dev_len(period);
  if (!canvas_hazard_params_ok(dw, dh, dp)) return;
  if (!s_dl) return;

  fill_device(dx, dy, dw, dh, ground);
  // 45 degree bars clipped to the strip. GL scissors from the bottom left; ImGui's clip rect is measured from the top
  // left, which is the space this file is in. Pushed without intersecting, like the scissor box.
  s_dl->PushClipRect(ImVec2((float)dx, (float)dy),
                     ImVec2((float)(dx + dw), (float)(dy + dh)), false);
  for (int i = -dh; i < dw + dh; i += dp * 2) {
    flat_quad((float)(dx + i),           (float)(dy + dh),
              (float)(dx + i + dp),      (float)(dy + dh),
              (float)(dx + i + dp + dh), (float)dy,
              (float)(dx + i + dh),      (float)dy, stripe);
  }
  s_dl->PopClipRect();
}

static int round_rect_segments(int r) {
  const int s = 3 + (r > 0 ? r / 2 : 0);
  return s < 3 ? 3 : s > 12 ? 12 : s;
}

// One tessellation shared by the rounded rectangle and the backdrop: the tint draws over the backdrop, so mismatched
// tessellations would leave a hairline of untinted scene at every corner. 4*seg points, each quarter arc inclusive.
static const int ROUND_FAN_MAX = 4 * 12;
static int round_fan_points(int dx, int dy, int dw, int dh, int dr, int seg,
                            float* px, float* py) {
  if (seg < 3) seg = 3;
  if (seg > 12) seg = 12;
  const double pi = 3.14159265358979323846;
  const float cx[4] = { (float)(dx + dr),      (float)(dx + dw - dr),
                        (float)(dx + dw - dr), (float)(dx + dr) };
  const float cy[4] = { (float)(dy + dr),      (float)(dy + dr),
                        (float)(dy + dh - dr), (float)(dy + dh - dr) };
  int n = 0;
  for (int c = 0; c < 4; c++) {
    const double a0 = (180.0 + 90.0 * c) * pi / 180.0;
    for (int i = 0; i < seg; i++) {
      const double a = a0 + (pi / 2.0) * (double)i / (double)(seg - 1);
      px[n] = cx[c] + (float)(dr * cos(a));
      py[n] = cy[c] + (float)(dr * sin(a));
      n++;
    }
  }
  return n;
}

static void round_rect_device(int dx, int dy, int dw, int dh, int dr, int seg,
                              unsigned rgba) {
  float px[ROUND_FAN_MAX], py[ROUND_FAN_MAX];
  const int n = round_fan_points(dx, dy, dw, dh, dr, seg, px, py);
  const ImDrawVert c = flat((float)dx + (float)dw * 0.5f,
                            (float)dy + (float)dh * 0.5f, rgba);
  if (!s_dl || n <= 0) return;
  s_dl->PrimReserve(3 * n, n + 1);
  const unsigned int base = s_dl->_VtxCurrentIdx;
  for (int i = 0; i < n; i++) {
    const int j = (i + 1 < n) ? i + 1 : 0;
    s_dl->PrimWriteIdx((ImDrawIdx)base);
    s_dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (unsigned int)i));
    s_dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (unsigned int)j));
  }
  s_dl->PrimWriteVtx(c.pos, c.uv, c.col);
  for (int i = 0; i < n; i++)
    s_dl->PrimWriteVtx(ImVec2(px[i], py[i]), c.uv, c.col);
}

static int corner_radius_device(int dw, int dh, int r) {
  int dr = dev_len(r > 0 ? r : 0);
  const int half = (dw < dh ? dw : dh) / 2;
  if (dr > half) dr = half;
  return dr < 0 ? 0 : dr;
}

void canvas_round_rect(int x, int y, int w, int h, int r, unsigned rgba) {
  const int dx = dev_x(x), dy = dev_y(y);
  const int dw = dev_len(w), dh = dev_len(h);
  if (dw <= 0 || dh <= 0) return;
  const int dr = corner_radius_device(dw, dh, r);
  round_rect_device(dx, dy, dw, dh, dr, round_rect_segments(dr), rgba);
}

void canvas_round_rect_outline(int x, int y, int w, int h, int r, int thickness,
                               unsigned rgba) {
  const int dx = dev_x(x), dy = dev_y(y);
  const int dw = dev_len(w), dh = dev_len(h);
  if (dw <= 0 || dh <= 0) return;
  int t = dev_len(thickness > 0 ? thickness : 0);
  if (t < 1) t = 1;
  const int dr  = corner_radius_device(dw, dh, r);
  const int seg = round_rect_segments(dr);
  if (dw <= 2 * t || dh <= 2 * t) {
    round_rect_device(dx, dy, dw, dh, dr, seg, rgba);
    return;
  }
  float ox[ROUND_FAN_MAX], oy[ROUND_FAN_MAX];
  float ix[ROUND_FAN_MAX], iy[ROUND_FAN_MAX];
  const int n = round_fan_points(dx, dy, dw, dh, dr, seg, ox, oy);
  const int ir = dr - t < 0 ? 0 : dr - t;
  round_fan_points(dx + t, dy + t, dw - 2 * t, dh - 2 * t, ir, seg, ix, iy);
  atlas_resolve();
  const ImU32 col = to_imgui(rgba);
  if (!s_dl || n <= 0) return;
  // The two triangles of each pair of rungs (outer, inner, outer then inner, inner, outer), the last rung wrapping onto
  // the first to close the ring.
  s_dl->PrimReserve(6 * n, 2 * n);
  const unsigned int base = s_dl->_VtxCurrentIdx;
  for (int i = 0; i < n; i++) {
    const int j = (i + 1 < n) ? i + 1 : 0;
    const unsigned int o0 = base + (unsigned int)(2 * i);
    const unsigned int i0 = o0 + 1;
    const unsigned int o1 = base + (unsigned int)(2 * j);
    const unsigned int i1 = o1 + 1;
    s_dl->PrimWriteIdx((ImDrawIdx)o0);
    s_dl->PrimWriteIdx((ImDrawIdx)i0);
    s_dl->PrimWriteIdx((ImDrawIdx)o1);
    s_dl->PrimWriteIdx((ImDrawIdx)i0);
    s_dl->PrimWriteIdx((ImDrawIdx)i1);
    s_dl->PrimWriteIdx((ImDrawIdx)o1);
  }
  for (int i = 0; i < n; i++) {
    s_dl->PrimWriteVtx(ImVec2(ox[i], oy[i]), s_white, col);
    s_dl->PrimWriteVtx(ImVec2(ix[i], iy[i]), s_white, col);
  }
}

void canvas_gradient_rect(int x, int y, int w, int h, unsigned top, unsigned bottom) {
  const int dx = dev_x(x), dy = dev_y(y);
  const int dw = dev_len(w), dh = dev_len(h);
  if (dw <= 0 || dh <= 0) return;
  const ImDrawVert a = flat((float)dx,        (float)dy,        top);
  const ImDrawVert b = flat((float)(dx + dw), (float)dy,        top);
  const ImDrawVert c = flat((float)(dx + dw), (float)(dy + dh), bottom);
  const ImDrawVert d = flat((float)dx,        (float)(dy + dh), bottom);
  cv_quad(a, b, c, d);
}

void canvas_glow_rect(int x, int y, int w, int h, unsigned rgba) {
  const int dx = dev_x(x), dy = dev_y(y);
  const int dw = dev_len(w), dh = dev_len(h);
  if (dw <= 0 || dh <= 0) return;
  // Additive is approximated with alpha, deliberately: ImGui has one blend state per draw data. The error is dst*a
  // darker; over C_PANEL at C_GLASS_GLOW's alpha 0x66 that is (8,9,10)/255, invisible. Do not "fix" this with
  // AddCallback.
  fill_device(dx, dy, dw, dh, rgba);
}

// STUBBED, like World Tour's. The Glass ground would be the finished scene copied out of the back buffer (Megaton's
// canvas.cpp:384-556: glReadBuffer(GL_BACK), glCopyTexSubImage2D into a power-of-two texture) from the SwapBuffers
// hook. Megaton does not branch on the capture's return, so a false one leaves the tint drawn alone.
static int s_backSaid = 0;

bool canvas_backdrop_capture(int, int, int, int) {
  if (!s_backSaid) {
    s_backSaid = 1;
    ek_log("canvas: the Glass backdrop capture is not implemented yet; the style degrades to its tint");
  }
  return false;
}

void canvas_backdrop_draw(int x, int y, int w, int h, int r, unsigned tint,
                          double, double, int, int) {
  static int said = 0;
  if (!said) { said = 1; ek_log("canvas: no captured ground; drawing the Glass tint alone"); }
  canvas_round_rect(x, y, w, h, r, tint);
}

void canvas_backdrop_release() {
  static int said = 0;
  if (!said) { said = 1; ek_log("canvas: canvas_backdrop_release is a stub; nothing to release"); }
}

// The metrics twins: at any scale but 100 these are the scaled atlas's DEVICE metrics. runtime_live decides which and
// canvas_text asks it too, so the advances a string was laid out with and the glyphs drawn always come from one atlas.
// The baked answer stays whenever fontgen refuses a scale.
static const Glyph* metrics_for(int font) {
  const TextSlot* live = runtime_live();
  return live ? live->run.glyphs[font] : font_glyphs[font];
}
static bool metrics_are_device() { return runtime_live() != 0; }

int canvas_text(int x, int y, const char* s, unsigned rgba, int font) {
  if (!s || font < 0 || font >= FONT_COUNT) return x;
  if (!atlas_resolve()) return x;
  if (!s_dl) return x;
  const TextSlot* live = runtime_live();
  const bool device = live != 0;
  const Glyph* glyphs = live ? live->run.glyphs[font] : font_glyphs[font];
  // u and v are over ImGui's whole texture and our blocks sit at an origin a repack can move, so every glyph's
  // coordinate carries the origin of the block it came from.
  const int rx = live ? live->rectX : s_rectX;
  const int ry = live ? live->rectY : s_rectY;
  const float iw = s_uvScale.x, ih = s_uvScale.y;
  const ImU32 c = to_imgui(rgba);
  // On the scaled atlas the pen is DEVICE and starts at zero (advances are already device, nothing rounds per
  // character). On the baked one it stays logical and the scale applies to the accumulated pen, since scaling each
  // advance would round per character. Either way the quad moves with the pen.
  const int ox = device ? ui_scale_px(x, s_scale) : 0;
  const int oy = device ? ui_scale_px(y, s_scale) : 0;
  int pen = device ? 0 : x;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if (*p < 0x20 || *p > 0x7F) continue;
    const Glyph& g = glyphs[*p - 0x20];
    if (g.w > 0) {
      const float gx = device ? (float)tx_dev_x(ox + pen + g.bx) : (float)dev_x(pen + g.bx);
      const float gy = device ? (float)tx_dev_y(oy + g.by)       : (float)dev_y(y + g.by);
      const float gw = device ? (float)tx_dev_len(g.w)           : (float)dev_len(g.w);
      const float gh = device ? (float)tx_dev_len(g.h)           : (float)dev_len(g.h);
      const float u0 = (float)(rx + g.x) * iw;
      const float v0 = (float)(ry + g.y) * ih;
      const float u1 = (float)(rx + g.x + g.w) * iw;
      const float v1 = (float)(ry + g.y + g.h) * ih;
      s_dl->PrimReserve(6, 4);
      const unsigned int base = s_dl->_VtxCurrentIdx;
      s_dl->PrimWriteIdx((ImDrawIdx)base);
      s_dl->PrimWriteIdx((ImDrawIdx)(base + 1));
      s_dl->PrimWriteIdx((ImDrawIdx)(base + 2));
      s_dl->PrimWriteIdx((ImDrawIdx)base);
      s_dl->PrimWriteIdx((ImDrawIdx)(base + 2));
      s_dl->PrimWriteIdx((ImDrawIdx)(base + 3));
      s_dl->PrimWriteVtx(ImVec2(gx,      gy),      ImVec2(u0, v0), c);
      s_dl->PrimWriteVtx(ImVec2(gx + gw, gy),      ImVec2(u1, v0), c);
      s_dl->PrimWriteVtx(ImVec2(gx + gw, gy + gh), ImVec2(u1, v1), c);
      s_dl->PrimWriteVtx(ImVec2(gx,      gy + gh), ImVec2(u0, v1), c);
    }
    pen += g.adv;
  }
  return device ? x + ui_unscale_px(pen, s_scale) : pen;
}
#else
       // NO_GL: recording bodies, so menu.cpp links into a test host without a renderer; this file stays the sole
       // definition of every canvas_* symbol. They were no-ops once, which let mutations to menu_draw survive the
       // suite; recording each call makes the panel's output testable.
#include <string.h>

static CanvasOp s_ops[CANVAS_RECORD_MAX];
static int  s_opCount = 0;
static bool s_recording = false;
static bool s_overflowed = false;

void canvas_record_begin() { s_opCount = 0; s_overflowed = false; s_recording = true; }
void canvas_record_end()   { s_recording = false; }
int  canvas_record_count() { return s_opCount; }
bool canvas_record_overflowed() { return s_overflowed; }
const CanvasOp* canvas_record_op(int i) {
  return (i >= 0 && i < s_opCount) ? &s_ops[i] : nullptr;
}
const CanvasOp* canvas_record_find_text(const char* s) {
  if (!s) return nullptr;
  for (int i = 0; i < s_opCount; i++)
    if (s_ops[i].kind == CANVAS_OP_TEXT && strcmp(s_ops[i].text, s) == 0) return &s_ops[i];
  return nullptr;
}
const CanvasOp* canvas_record_find_kind(int kind, unsigned colour) {
  for (int i = 0; i < s_opCount; i++)
    if (s_ops[i].kind == kind && s_ops[i].colour == colour) return &s_ops[i];
  return nullptr;
}

// The one place an op is appended. Overflow sets a flag rather than wrapping: a truncated recording would let a "never
// drawn" check pass for the wrong reason.
static CanvasOp* push_op(int kind, int x, int y, int w, int h, unsigned colour) {
  if (!s_recording) return nullptr;
  if (s_opCount >= CANVAS_RECORD_MAX) { s_overflowed = true; return nullptr; }
  CanvasOp& o = s_ops[s_opCount++];
  o.kind = kind; o.x = x; o.y = y; o.w = w; o.h = h;
  o.colour = colour; o.font = -1; o.text[0] = 0;
  return &o;
}

// Recorded coordinates are device space, matching the device bodies (within a pixel for a text op's width): a recorder
// logging logical numbers would let "the scale is applied" pass while nothing moved. The transform is cleared here too.
void canvas_begin(int, int h) {
  canvas_set_transform(0, 0, 1.0, 0, 0);
  s_magnify = false;
  s_textSlot = CANVAS_SLOT_PANEL;
  s_frameH = h;
}
void canvas_end() {}
bool canvas_white_texel(float* u, float* v) {
  if (u) *u = 0.0f;
  if (v) *v = 0.0f;
  return false;
}
CanvasTransform canvas_record_transform() {
  CanvasTransform t;
  t.dx = s_txDx; t.dy = s_txDy; t.cx = s_txCx; t.cy = s_txCy; t.scale = s_txScale;
  return t;
}
void canvas_rect(int x, int y, int w, int h, unsigned c) {
  push_op(CANVAS_OP_RECT, dev_x(x), dev_y(y), dev_len(w), dev_len(h), c);
}
// Only the fill colour is recorded: the one callers vary; the edges are always C_BEVELHI/C_BEVELLO.
void canvas_bevel(int x, int y, int w, int h, unsigned fill, unsigned, unsigned) {
  push_op(CANVAS_OP_BEVEL, dev_x(x), dev_y(y), dev_len(w), dev_len(h), fill);
}
void canvas_line(int x0, int y0, int x1, int y1, unsigned c) {
  const int dx0 = dev_x(x0), dy0 = dev_y(y0);
  const int dx1 = dev_x(x1), dy1 = dev_y(y1);
  push_op(CANVAS_OP_LINE, dx0, dy0, dx1 - dx0, dy1 - dy0, c);
}
void canvas_line_w(int x0, int y0, int x1, int y1, unsigned c, int) {
  canvas_line(x0, y0, x1, y1, c);
}
void canvas_hazard(int x, int y, int w, int h, unsigned stripe, unsigned, int) {
  push_op(CANVAS_OP_HAZARD, dev_x(x), dev_y(y), dev_len(w), dev_len(h), stripe);
}
// NOT RECORDED: a sweep sees every op a clipped call made, so it knows what the panel decided to draw but not what the
// screen showed of it.
void canvas_push_clip(int, int, int, int) {}
void canvas_pop_clip() {}
void canvas_round_rect(int x, int y, int w, int h, int, unsigned c) {
  push_op(CANVAS_OP_ROUNDRECT, dev_x(x), dev_y(y), dev_len(w), dev_len(h), c);
}
void canvas_round_rect_outline(int x, int y, int w, int h, int, int, unsigned c) {
  push_op(CANVAS_OP_ROUNDOUTLINE, dev_x(x), dev_y(y), dev_len(w), dev_len(h), c);
}
void canvas_gradient_rect(int x, int y, int w, int h, unsigned top, unsigned) {
  push_op(CANVAS_OP_GRADIENT, dev_x(x), dev_y(y), dev_len(w), dev_len(h), top);
}
void canvas_glow_rect(int x, int y, int w, int h, unsigned c) {
  push_op(CANVAS_OP_GLOW, dev_x(x), dev_y(y), dev_len(w), dev_len(h), c);
}
// Its own op kind: capture and draw are different events, and a "drawn once a frame" sweep must tell them apart. True:
// no device to fail.
bool canvas_backdrop_capture(int x, int y, int w, int h) {
  push_op(CANVAS_OP_CAPTURE, dev_x(x), dev_y(y), dev_len(w), dev_len(h), 0u);
  return true;
}
void canvas_backdrop_draw(int x, int y, int w, int h, int, unsigned tint,
                          double, double, int, int) {
  push_op(CANVAS_OP_BACKDROP, dev_x(x), dev_y(y), dev_len(w), dev_len(h), tint);
}
void canvas_backdrop_release() {}

static const Glyph* metrics_for(int font) { return font_glyphs[font]; }
static bool metrics_are_device() { return false; }

int canvas_text(int x, int y, const char* s, unsigned c, int font) {
  // The recorded width is the rendered one, since tab_text_fits compares it against a device edge. Accepted:
  // ui_scale_px of the advance sum can differ by about a pixel from the device body's per-glyph rounding at a non-100
  // scale; a per-glyph replay would duplicate the layout in the recorder.
  CanvasOp* o = push_op(CANVAS_OP_TEXT, dev_x(x), dev_y(y),
                        dev_len(canvas_text_width(s, font)), 0, c);
  if (o) {
    o->font = font;
    if (s) strncpy_s(o->text, sizeof(o->text), s, _TRUNCATE);
  }
  return x;
}
#endif

// Logical for every caller, measured through the atlas that will be drawn: menu.cpp lays out in logical pixels, so a
// width that moved with the scale would apply the factor twice. But which glyphs get drawn moves with the scale (GDI
// hinting is not linear), so the device sum is taken over the live atlas and divided back out.
int canvas_text_width(const char* s, int font) {
  if (!s || font < 0 || font >= FONT_COUNT) return 0;
  const Glyph* glyphs = metrics_for(font);
  int w = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    if (*p < 0x20 || *p > 0x7F) continue;
    w += glyphs[*p - 0x20].adv;
  }
  return metrics_are_device() ? ui_unscale_px(w, s_scale) : w;
}
