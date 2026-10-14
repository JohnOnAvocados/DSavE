// A small 2D renderer for 16-bit bitmaps: filled and rounded rectangles,
// circles and antialiased proportional text. Pure C, no DS headers, so the
// whole UI can also be drawn on a PC (see tests/test_ui.c).
//
// Pixels are BGR555 with bit 15 set, the format of a DS 16-bit bitmap
// background.

#ifndef DSAVE_GFX_H
#define DSAVE_GFX_H

#include <stdbool.h>
#include <stdint.h>

typedef uint16_t Color;

// From 8-bit-per-channel RGB.
#define GFX_RGB(r, g, b) \
    ((Color)(((r) >> 3) | (((g) >> 3) << 5) | (((b) >> 3) << 10) | 0x8000))

typedef struct {
    uint16_t cp;       // Unicode code point
    uint8_t w, h;      // Bitmap size
    int8_t x, y;       // Bitmap offset from the pen position and line top
    uint8_t advance;
    uint32_t offset;   // Into Font.bits: 4-bit alpha, rows padded to a byte
} Glyph;

typedef struct {
    uint8_t height;    // Line height
    uint8_t ascent;
    uint16_t count;
    const Glyph *glyphs; // Sorted by cp
    const uint8_t *bits;
} Font;

extern const Font font_small, font_body, font_bold, font_title;

typedef struct {
    Color *px;
    int w, h;
    int cx0, cy0, cx1, cy1; // Clip rectangle, exclusive end
} Canvas;

void gfx_init(Canvas *c, Color *px, int w, int h);

// Limit drawing to a rectangle (intersected with the canvas), or reset it.
void gfx_clip(Canvas *c, int x, int y, int w, int h);
void gfx_unclip(Canvas *c);

// Mix two colours; t runs from 0 (a) to 256 (b).
Color gfx_mix(Color a, Color b, int t);

void gfx_clear(Canvas *c, Color col);
void gfx_fill(Canvas *c, int x, int y, int w, int h, Color col);
// Rounded rectangle with antialiased corners.
void gfx_rrect(Canvas *c, int x, int y, int w, int h, int r, Color col);
// Rounded rectangle with a one-pixel border.
void gfx_rrect_border(Canvas *c, int x, int y, int w, int h, int r, Color border, Color fill);
// Paletted pixels (one byte each, index 0 transparent) with a BGR555
// palette, each pixel drawn as a scale x scale block.
void gfx_indexed(Canvas *c, int x, int y, int w, int h, const uint8_t *px, const uint16_t *pal, int scale);
// Antialiased disc in the d x d box at (x, y).
void gfx_disc(Canvas *c, int x, int y, int d, Color col);
// Same, as a ring of the given thickness.
void gfx_ring(Canvas *c, int x, int y, int d, int thickness, Color col);

// Text is UTF-8; y is the top of the line. Characters without a glyph are
// drawn as '?'. Returns the pen position after the text.
int gfx_text(Canvas *c, const Font *f, int x, int y, const char *s, Color col);
int gfx_text_width(const Font *f, const char *s);
// Draw at most max_w pixels of text, ending with "…" when cut. Returns the
// width drawn.
int gfx_text_fit(Canvas *c, const Font *f, int x, int y, int max_w, const char *s, Color col);
// Same, keeping the end of the text ("…/roms/game.sav"), for paths.
int gfx_text_fit_end(Canvas *c, const Font *f, int x, int y, int max_w, const char *s, Color col);
void gfx_text_right(Canvas *c, const Font *f, int right, int y, const char *s, Color col);
void gfx_text_center(Canvas *c, const Font *f, int x, int y, int w, const char *s, Color col);

// Decode one UTF-8 character and advance *s. Bad bytes come back as '?'.
uint32_t utf8_next(const char **s);

#endif
