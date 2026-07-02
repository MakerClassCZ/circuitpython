// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "shared-module/picogame/Canvas.h"
#include "shared-module/picogame/Bitmap.h"
#include "shared-module/picogame/__init__.h"
#include "shared-module/fontio/BuiltinFont.h"
#include "shared-bindings/displayio/Bitmap.h"

// Thin wrappers over the shared int32 accumulator (dx1,dy1,dx2,dy2 are contiguous int32 at the
// struct tail). See picogame_dirty_* in __init__.c.
void picogame_canvas_dirty_reset(picogame_canvas_obj_t *cv) {
    picogame_dirty_reset(&cv->dx1);
}

void picogame_canvas_dirty_union(picogame_canvas_obj_t *cv, int x1, int y1, int x2, int y2) {
    picogame_dirty_union(&cv->dx1, x1, y1, x2, y2);
}

bool picogame_canvas_take_dirty(picogame_canvas_obj_t *cv, int *x1, int *y1, int *x2, int *y2) {
    return picogame_dirty_take(&cv->dx1, x1, y1, x2, y2);
}

// Union a canvas-local rect (clamped to the surface) into the dirty rect (scene coords).
static void mark(picogame_canvas_obj_t *cv, int lx1, int ly1, int lx2, int ly2) {
    if (lx1 < 0) {
        lx1 = 0;
    }
    if (ly1 < 0) {
        ly1 = 0;
    }
    if (lx2 > cv->w) {
        lx2 = cv->w;
    }
    if (ly2 > cv->h) {
        ly2 = cv->h;
    }
    if (lx1 >= lx2 || ly1 >= ly2) {
        return;
    }
    picogame_dirty_union(&cv->dx1, cv->x + lx1, cv->y + ly1, cv->x + lx2, cv->y + ly2);
}

// NOT inlined on purpose: the shape primitives call put() many times (circle =
// 8 calls/iteration). Inlining bloated them (circle was ~1.4 KB); a real call keeps
// them small. Shapes aren't the hot path (the sprite/tilemap blits don't use put).
static __attribute__((noinline)) void put(picogame_canvas_obj_t *cv, int x, int y, uint16_t c) {
    if (x >= 0 && y >= 0 && x < cv->w && y < cv->h) {
        cv->data[y * cv->w + x] = c;
    }
}

// Fill `n` RGB565 pixels at `p` with `color`, word-filling two pixels per store (half the writes of
// a 16-bit loop); memset for the common 0 case. Handles a leading odd (2-byte-but-not-4-byte) address
// so it stays safe on Cortex-M0+ (RP2040), which faults on an unaligned 32-bit access - a StripDraw
// view's rows into the render strip can start on an odd pixel. This is the per-frame path for
// view.clear / Sky / HUD-bar / Fade fills, so the word-fill is worth it.
static void fill565(uint16_t *p, int n, uint16_t color) {
    if (n <= 0) {
        return;
    }
    if (color == 0) {
        memset(p, 0, (size_t)n * 2);
        return;
    }
    if ((uintptr_t)p & 3) {                    // align to 4 bytes: one leading pixel
        *p++ = color;
        n--;
    }
    uint32_t w = (uint32_t)color | ((uint32_t)color << 16);
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wcast-align"
    uint32_t *w32 = (uint32_t *)p;             // now 4-byte aligned
    #pragma GCC diagnostic pop
    int nw = n >> 1;
    for (int i = 0; i < nw; i++) {
        w32[i] = w;
    }
    if (n & 1) {                               // trailing odd pixel
        p[n - 1] = color;
    }
}

void picogame_canvas_clear(picogame_canvas_obj_t *cv, uint16_t color) {
    fill565(cv->data, cv->w * cv->h, color);
    mark(cv, 0, 0, cv->w, cv->h);
}

void picogame_canvas_pixel(picogame_canvas_obj_t *cv, int x, int y, uint16_t color) {
    put(cv, x, y, color);
    mark(cv, x, y, x + 1, y + 1);
}

void picogame_canvas_fill_rect(picogame_canvas_obj_t *cv, int x, int y, int w, int h, uint16_t color) {
    int x2 = x + w, y2 = y + h;
    int cx1 = x < 0 ? 0 : x, cy1 = y < 0 ? 0 : y;
    int cx2 = x2 > cv->w ? cv->w : x2, cy2 = y2 > cv->h ? cv->h : y2;
    for (int yy = cy1; yy < cy2; yy++) {
        fill565(cv->data + yy * cv->w + cx1, cx2 - cx1, color);
    }
    mark(cv, x, y, x2, y2);
}

void picogame_canvas_blit(picogame_canvas_obj_t *cv, picogame_bitmap_obj_t *bm,
    int x, int y, int frame, bool flip_x, bool flip_y) {
    // Composite a bitmap FRAME into the canvas buffer (honours the bitmap's transparent key).
    // Reuses the sprite blit path, targeting the canvas's own RGB565 surface instead of a strip.
    picogame_blit_bitmap(cv->data, cv->w, cv->h, 0, 0, bm, x, y, frame, flip_x, flip_y, false, NULL);
    mark(cv, x, y, x + bm->width, y + bm->height);
}

void picogame_canvas_rect(picogame_canvas_obj_t *cv, int x, int y, int w, int h, uint16_t color) {
    picogame_canvas_fill_rect(cv, x, y, w, 1, color);
    picogame_canvas_fill_rect(cv, x, y + h - 1, w, 1, color);
    picogame_canvas_fill_rect(cv, x, y, 1, h, color);
    picogame_canvas_fill_rect(cv, x + w - 1, y, 1, h, color);
}

void picogame_canvas_line(picogame_canvas_obj_t *cv, int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    int err = adx - ady;
    int x = x0, y = y0;
    while (true) {
        put(cv, x, y, color);
        if (x == x1 && y == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 > -ady) {
            err -= ady;
            x += sx;
        }
        if (e2 < adx) {
            err += adx;
            y += sy;
        }
    }
    int lx1 = x0 < x1 ? x0 : x1, ly1 = y0 < y1 ? y0 : y1;
    int lx2 = (x0 > x1 ? x0 : x1) + 1, ly2 = (y0 > y1 ? y0 : y1) + 1;
    mark(cv, lx1, ly1, lx2, ly2);
}

void picogame_canvas_fill_circle(picogame_canvas_obj_t *cv, int cx, int cy, int r, uint16_t color) {
    if (r < 0) {
        return;
    }
    for (int dy = -r; dy <= r; dy++) {
        // half-width of the circle at this row
        int span = 0;
        long rr = (long)r * r - (long)dy * dy;     // long (like ellipse): int r*r overflows at big radii
        while ((long)(span + 1) * (span + 1) <= rr) {
            span++;
        }
        int y = cy + dy;
        for (int x = cx - span; x <= cx + span; x++) {
            put(cv, x, y, color);
        }
    }
    mark(cv, cx - r, cy - r, cx + r + 1, cy + r + 1);
}

void picogame_canvas_circle(picogame_canvas_obj_t *cv, int cx, int cy, int r, uint16_t color) {
    picogame_canvas_ellipse(cv, cx, cy, r, r, color);   // a circle is an ellipse with rx == ry
}

void picogame_canvas_ring(picogame_canvas_obj_t *cv, int cx, int cy, int r, int thick, uint16_t color) {
    if (r < 0) {
        return;
    }
    int inner = r - thick;
    if (inner < 0) {
        inner = 0;
    }
    for (int dy = -r; dy <= r; dy++) {
        int out = 0;
        long rr = (long)r * r - (long)dy * dy;     // long (like ellipse): int r*r overflows at big radii
        while ((long)(out + 1) * (out + 1) <= rr) {
            out++;
        }
        int y = cy + dy;
        if (dy >= -inner && dy <= inner) {
            int ins = 0, ri = inner * inner - dy * dy;
            while ((ins + 1) * (ins + 1) <= ri) {
                ins++;
            }
            for (int x = cx - out; x < cx - ins; x++) {
                put(cv, x, y, color);
            }
            for (int x = cx + ins + 1; x <= cx + out; x++) {
                put(cv, x, y, color);
            }
        } else {
            for (int x = cx - out; x <= cx + out; x++) {
                put(cv, x, y, color);
            }
        }
    }
    mark(cv, cx - r, cy - r, cx + r + 1, cy + r + 1);
}

void picogame_canvas_triangle(picogame_canvas_obj_t *cv,
    int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
    picogame_canvas_line(cv, x0, y0, x1, y1, color);
    picogame_canvas_line(cv, x1, y1, x2, y2, color);
    picogame_canvas_line(cv, x2, y2, x0, y0, color);
}

void picogame_canvas_fill_triangle(picogame_canvas_obj_t *cv,
    int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
    int X[3] = { x0, x1, x2 }, Y[3] = { y0, y1, y2 };
    for (int i = 0; i < 2; i++) {
        for (int j = i + 1; j < 3; j++) {
            if (Y[j] < Y[i]) {
                int t = Y[i];
                Y[i] = Y[j];
                Y[j] = t;
                t = X[i];
                X[i] = X[j];
                X[j] = t;
            }
        }
    }
    for (int y = Y[0]; y <= Y[2]; y++) {
        int xac = (Y[2] == Y[0]) ? X[0] : X[0] + (X[2] - X[0]) * (y - Y[0]) / (Y[2] - Y[0]);
        int xsh;
        if (y < Y[1]) {
            xsh = (Y[1] == Y[0]) ? X[0] : X[0] + (X[1] - X[0]) * (y - Y[0]) / (Y[1] - Y[0]);
        } else {
            xsh = (Y[2] == Y[1]) ? X[1] : X[1] + (X[2] - X[1]) * (y - Y[1]) / (Y[2] - Y[1]);
        }
        int xs = xac < xsh ? xac : xsh, xe = xac < xsh ? xsh : xac;
        for (int x = xs; x <= xe; x++) {
            put(cv, x, y, color);
        }
    }
    int mnx = X[0] < X[1] ? X[0] : X[1];
    mnx = mnx < X[2] ? mnx : X[2];
    int mxx = X[0] > X[1] ? X[0] : X[1];
    mxx = mxx > X[2] ? mxx : X[2];
    mark(cv, mnx, Y[0], mxx + 1, Y[2] + 1);
}

void picogame_canvas_ellipse(picogame_canvas_obj_t *cv, int cx, int cy, int rx, int ry, uint16_t color) {
    if (rx <= 0 || ry <= 0) {
        return;
    }
    // 32-bit `long`: rx2*ry2 stays in range while rx*ry <= 46340 (both radii <~210 px). That covers any
    // canvas that fits in RAM on this target. A larger ellipse (only reachable on a big-RAM board with an
    // oversized canvas) renders a wrong shape - never a fault, since put() clips every pixel to the canvas.
    long rx2 = (long)rx * rx, ry2 = (long)ry * ry, rr = rx2 * ry2;
    for (int dy = -ry; dy <= ry; dy++) {
        int s = 0;
        while ((long)(s + 1) * (s + 1) * ry2 + (long)dy * dy * rx2 <= rr) {
            s++;
        }
        put(cv, cx - s, cy + dy, color);
        put(cv, cx + s, cy + dy, color);
    }
    for (int dx = -rx; dx <= rx; dx++) {
        int s = 0;
        while ((long)(s + 1) * (s + 1) * rx2 + (long)dx * dx * ry2 <= rr) {
            s++;
        }
        put(cv, cx + dx, cy - s, color);
        put(cv, cx + dx, cy + s, color);
    }
    mark(cv, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
}

void picogame_canvas_fill_ellipse(picogame_canvas_obj_t *cv, int cx, int cy, int rx, int ry, uint16_t color) {
    if (rx <= 0 || ry <= 0) {
        return;
    }
    // 32-bit `long`: rx2*ry2 stays in range while rx*ry <= 46340 (both radii <~210 px). That covers any
    // canvas that fits in RAM on this target. A larger ellipse (only reachable on a big-RAM board with an
    // oversized canvas) renders a wrong shape - never a fault, since put() clips every pixel to the canvas.
    long rx2 = (long)rx * rx, ry2 = (long)ry * ry, rr = rx2 * ry2;
    for (int dy = -ry; dy <= ry; dy++) {
        int s = 0;
        while ((long)(s + 1) * (s + 1) * ry2 + (long)dy * dy * rx2 <= rr) {
            s++;
        }
        int y = cy + dy;
        for (int x = cx - s; x <= cx + s; x++) {
            put(cv, x, y, color);
        }
    }
    mark(cv, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
}

void picogame_canvas_fill_round_rect(picogame_canvas_obj_t *cv, int x, int y, int w, int h, int r, uint16_t color) {
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    if (r < 0) {
        r = 0;
    }
    picogame_canvas_fill_rect(cv, x + r, y, w - 2 * r, h, color);
    picogame_canvas_fill_rect(cv, x, y + r, r, h - 2 * r, color);
    picogame_canvas_fill_rect(cv, x + w - r, y + r, r, h - 2 * r, color);
    picogame_canvas_fill_circle(cv, x + r, y + r, r, color);
    picogame_canvas_fill_circle(cv, x + w - r - 1, y + r, r, color);
    picogame_canvas_fill_circle(cv, x + r, y + h - r - 1, r, color);
    picogame_canvas_fill_circle(cv, x + w - r - 1, y + h - r - 1, r, color);
}

void picogame_canvas_frame3d(picogame_canvas_obj_t *cv, int x, int y, int w, int h, uint16_t light, uint16_t dark) {
    picogame_canvas_fill_rect(cv, x, y, w, 1, light);
    picogame_canvas_fill_rect(cv, x, y, 1, h, light);
    picogame_canvas_fill_rect(cv, x, y + h - 1, w, 1, dark);
    picogame_canvas_fill_rect(cv, x + w - 1, y, 1, h, dark);
}

void picogame_blit_canvas(
    uint16_t *buf, int region_w, int strip_top, int strip_h, int x0,
    picogame_canvas_obj_t *cv, int ox, int oy) {
    // Reuse the bitmap blitter by viewing the canvas as a 1-frame RGB565 bitmap.
    picogame_bitmap_obj_t bm;
    bm.data = (const uint8_t *)cv->data;
    bm.palette = NULL;
    bm.width = cv->w;
    bm.height = cv->h;
    bm.stride = cv->w;
    bm.transparent = cv->transparent;
    bm.format = PICOGAME_FMT_RGB565;
    bm.frames = 1;
    bm.has_transparent = cv->has_transparent;
    picogame_blit_bitmap(buf, region_w, strip_h, x0, strip_top, &bm,
        cv->x + ox, cv->y + oy, 0, false, false, false, NULL);
}

// Composite a string's glyphs straight into the surface in C: rasterize each glyph from the
// font's 1-bit atlas on the fly (no Python glyph cache, no per-call Bitmap/Sprite). Because the
// StripDraw `view` is a Canvas pointing at the live strip buffer, view.text() draws immediate-mode
// text into the frame with zero retained RAM - the same primitive serves retained Canvas screens.
void picogame_canvas_text(picogame_canvas_obj_t *cv, int x, int y, const char *text,
    uint16_t fg, uint16_t bg, bool has_bg, const void *font) {
    const fontio_builtinfont_t *f = font;
    displayio_bitmap_t *sheet = (displayio_bitmap_t *)f->bitmap;
    int fw = f->width, fh = f->height;
    int tpr = sheet->width / fw;            // glyph tiles per atlas row
    int x0 = x;
    for (const uint8_t *p = (const uint8_t *)text; *p; p++) {
        uint8_t gi = fontio_builtinfont_get_glyph_index(f, *p);
        if (gi != 0xff) {                   // 0xff = no glyph -> blank advance
            int tx = (gi % tpr) * fw, ty = (gi / tpr) * fh;
            for (int gy = 0; gy < fh; gy++) {
                for (int gx = 0; gx < fw; gx++) {
                    if (common_hal_displayio_bitmap_get_pixel(sheet, tx + gx, ty + gy)) {
                        put(cv, x + gx, y + gy, fg);
                    } else if (has_bg) {
                        put(cv, x + gx, y + gy, bg);
                    }
                }
            }
        }
        x += fw;
    }
    mark(cv, x0, y, x, y + fh);
}
