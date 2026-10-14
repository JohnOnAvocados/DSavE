#include "gfx.h"

#include <string.h>

#define ELLIPSIS 0x2026

void gfx_init(Canvas *c, Color *px, int w, int h)
{
    c->px = px;
    c->w = w;
    c->h = h;
    gfx_unclip(c);
}

void gfx_unclip(Canvas *c)
{
    c->cx0 = c->cy0 = 0;
    c->cx1 = c->w;
    c->cy1 = c->h;
}

void gfx_clip(Canvas *c, int x, int y, int w, int h)
{
    c->cx0 = x < 0 ? 0 : x;
    c->cy0 = y < 0 ? 0 : y;
    c->cx1 = x + w > c->w ? c->w : x + w;
    c->cy1 = y + h > c->h ? c->h : y + h;
}

Color gfx_mix(Color a, Color b, int t)
{
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    int r = ar + (((br - ar) * t) >> 8);
    int g = ag + (((bg - ag) * t) >> 8);
    int bl = ab + (((bb - ab) * t) >> 8);
    return (Color)(r | (g << 5) | (bl << 10) | 0x8000);
}

static inline bool in_clip(const Canvas *c, int x, int y)
{
    return x >= c->cx0 && x < c->cx1 && y >= c->cy0 && y < c->cy1;
}

// Blend col over a pixel with coverage a (0-256).
static inline void blend(Canvas *c, int x, int y, Color col, int a)
{
    if (a <= 0 || !in_clip(c, x, y))
        return;
    Color *p = &c->px[y * c->w + x];
    *p = a >= 256 ? col : gfx_mix(*p, col, a);
}

void gfx_clear(Canvas *c, Color col)
{
    for (int i = 0; i < c->w * c->h; i++)
        c->px[i] = col;
}

void gfx_fill(Canvas *c, int x, int y, int w, int h, Color col)
{
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    for (int yy = y0; yy < y1; yy++)
    {
        Color *row = &c->px[yy * c->w];
        for (int xx = x0; xx < x1; xx++)
            row[xx] = col;
    }
}

void gfx_indexed(Canvas *c, int x, int y, int w, int h, const uint8_t *px, const uint16_t *pal, int scale)
{
    for (int yy = 0; yy < h; yy++)
    {
        for (int xx = 0; xx < w; xx++)
        {
            uint8_t i = px[yy * w + xx];
            if (i)
                gfx_fill(c, x + xx * scale, y + yy * scale, scale, scale, (Color)(pal[i] | 0x8000));
        }
    }
}

// Coverage (0-256) of pixel (px, py) by a circle of radius r centred at
// (cx, cy), all in quarter pixels; 4x4 samples per pixel.
static int coverage(int px, int py, int cx, int cy, int r)
{
    int inside = 0;
    for (int sy = 0; sy < 4; sy++)
    {
        for (int sx = 0; sx < 4; sx++)
        {
            int dx = px * 4 + sx * 1 + 0 - cx; // sample at quarter offsets
            int dy = py * 4 + sy - cy;
            // Centre each sample within its quarter cell.
            dx = dx * 2 + 1;
            dy = dy * 2 + 1;
            if (dx * dx + dy * dy <= 4 * r * r)
                inside++;
        }
    }
    return inside * 16;
}

void gfx_rrect(Canvas *c, int x, int y, int w, int h, int r, Color col)
{
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    if (r <= 0)
    {
        gfx_fill(c, x, y, w, h, col);
        return;
    }
    gfx_fill(c, x + r, y, w - 2 * r, h, col);
    gfx_fill(c, x, y + r, r, h - 2 * r, col);
    gfx_fill(c, x + w - r, y + r, r, h - 2 * r, col);
    // Corners: circle centres in quarter pixels.
    int r4 = r * 4;
    for (int py = 0; py < r; py++)
    {
        for (int px = 0; px < r; px++)
        {
            int a = coverage(px, py, r4, r4, r4);
            blend(c, x + px, y + py, col, a);
            blend(c, x + w - 1 - px, y + py, col, a);
            blend(c, x + px, y + h - 1 - py, col, a);
            blend(c, x + w - 1 - px, y + h - 1 - py, col, a);
        }
    }
}

void gfx_rrect_border(Canvas *c, int x, int y, int w, int h, int r, Color border, Color fill)
{
    gfx_rrect(c, x, y, w, h, r, border);
    gfx_rrect(c, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, fill);
}

void gfx_disc(Canvas *c, int x, int y, int d, Color col)
{
    int r4 = d * 2; // radius in quarter pixels
    for (int py = 0; py < d; py++)
    {
        for (int px = 0; px < d; px++)
            blend(c, x + px, y + py, col, coverage(px, py, r4, r4, r4));
    }
}

void gfx_ring(Canvas *c, int x, int y, int d, int thickness, Color col)
{
    int r4 = d * 2, inner = (d - 2 * thickness) * 2;
    for (int py = 0; py < d; py++)
    {
        for (int px = 0; px < d; px++)
        {
            int a = coverage(px, py, r4, r4, r4);
            if (inner > 0)
                a -= coverage(px, py, r4, r4, inner);
            blend(c, x + px, y + py, col, a);
        }
    }
}

uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp = p[0];
    int extra = 0;
    if (cp < 0x80)
        extra = 0;
    else if ((cp & 0xE0) == 0xC0)
    {
        cp &= 0x1F;
        extra = 1;
    }
    else if ((cp & 0xF0) == 0xE0)
    {
        cp &= 0x0F;
        extra = 2;
    }
    else if ((cp & 0xF8) == 0xF0)
    {
        cp &= 0x07;
        extra = 3;
    }
    else
    {
        *s += 1;
        return '?';
    }
    for (int i = 1; i <= extra; i++)
    {
        if ((p[i] & 0xC0) != 0x80)
        {
            *s += i;
            return '?';
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += 1 + extra;
    return cp;
}

static const Glyph *find_glyph(const Font *f, uint32_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp)
            return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return cp == '?' ? NULL : find_glyph(f, '?');
}

static int draw_glyph(Canvas *c, const Font *f, const Glyph *g, int x, int y, Color col)
{
    const uint8_t *bits = f->bits + g->offset;
    int stride = (g->w + 1) / 2;
    for (int row = 0; row < g->h; row++)
    {
        int yy = y + g->y + row;
        if (yy < c->cy0 || yy >= c->cy1)
            continue;
        for (int i = 0; i < g->w; i++)
        {
            uint8_t b = bits[row * stride + i / 2];
            int a = (i & 1) ? b >> 4 : b & 15;
            if (a)
                blend(c, x + g->x + i, yy, col, a == 15 ? 256 : a * 17);
        }
    }
    return g->advance;
}

int gfx_text(Canvas *c, const Font *f, int x, int y, const char *s, Color col)
{
    while (*s)
    {
        const Glyph *g = find_glyph(f, utf8_next(&s));
        if (g)
            x += draw_glyph(c, f, g, x, y, col);
    }
    return x;
}

static int char_width(const Font *f, uint32_t cp)
{
    const Glyph *g = find_glyph(f, cp);
    return g ? g->advance : 0;
}

int gfx_text_width(const Font *f, const char *s)
{
    int w = 0;
    while (*s)
        w += char_width(f, utf8_next(&s));
    return w;
}

int gfx_text_fit(Canvas *c, const Font *f, int x, int y, int max_w, const char *s, Color col)
{
    int full = gfx_text_width(f, s);
    if (full <= max_w)
    {
        gfx_text(c, f, x, y, s, col);
        return full;
    }
    int ell = char_width(f, ELLIPSIS), w = 0;
    int start = x;
    while (*s)
    {
        const Glyph *g = find_glyph(f, utf8_next(&s));
        if (!g)
            continue;
        if (w + g->advance + ell > max_w)
            break;
        x += draw_glyph(c, f, g, x, y, col);
        w += g->advance;
    }
    const Glyph *e = find_glyph(f, ELLIPSIS);
    if (e)
        x += draw_glyph(c, f, e, x, y, col);
    return x - start;
}

int gfx_text_fit_end(Canvas *c, const Font *f, int x, int y, int max_w, const char *s, Color col)
{
    int full = gfx_text_width(f, s);
    if (full <= max_w)
    {
        gfx_text(c, f, x, y, s, col);
        return full;
    }
    // Drop characters from the front until the rest fits after "…".
    int budget = max_w - char_width(f, ELLIPSIS);
    const char *p = s;
    while (*p && gfx_text_width(f, p) > budget)
        utf8_next(&p);
    const Glyph *e = find_glyph(f, ELLIPSIS);
    int pen = x;
    if (e)
        pen += draw_glyph(c, f, e, pen, y, col);
    pen = gfx_text(c, f, pen, y, p, col);
    return pen - x;
}

void gfx_text_right(Canvas *c, const Font *f, int right, int y, const char *s, Color col)
{
    gfx_text(c, f, right - gfx_text_width(f, s), y, s, col);
}

void gfx_text_center(Canvas *c, const Font *f, int x, int y, int w, const char *s, Color col)
{
    gfx_text(c, f, x + (w - gfx_text_width(f, s)) / 2, y, s, col);
}
