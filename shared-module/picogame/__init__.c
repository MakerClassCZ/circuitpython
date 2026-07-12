// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT
//
// picogame portable render core: strip-based, arbitrary-size blitting.
// Painter's order: clear strip to background, draw layers/sprites bottom-to-top.

#include <math.h>
#include <string.h>
#include "py/runtime.h"
#include "shared-module/picogame/__init__.h"
#include "shared-module/picogame/Tilemap.h"
#include "shared-module/picogame/Particles.h"
#include "shared-module/picogame/Canvas.h"
#include "shared-bindings/busdisplay/BusDisplay.h"
#include "shared-module/displayio/display_core.h"
#include "shared-bindings/displayio/__init__.h"

// Shared dirty-rect accumulator over a contiguous int32 [x1,y1,x2,y2]. Canvas and Tilemap both end in
// `int32_t dx1,dy1,dx2,dy2`, so their public dirty fns are thin wrappers passing &self->dx1 here.
// Sentinels are the INT32 extremes (not int16) so a big-world scene coord past +-32767 px still
// accumulates correctly.
void picogame_dirty_reset(int32_t *r) {
    r[0] = 0x7fffffff;
    r[1] = 0x7fffffff;
    r[2] = -0x7fffffff - 1;
    r[3] = -0x7fffffff - 1;
}

void picogame_dirty_union(int32_t *r, int x1, int y1, int x2, int y2) {
    if (x1 < r[0]) {
        r[0] = x1;
    }
    if (y1 < r[1]) {
        r[1] = y1;
    }
    if (x2 > r[2]) {
        r[2] = x2;
    }
    if (y2 > r[3]) {
        r[3] = y2;
    }
}

bool picogame_dirty_take(int32_t *r, int *x1, int *y1, int *x2, int *y2) {
    bool dirty = (r[0] < r[2]) && (r[1] < r[3]);
    if (dirty) {
        *x1 = r[0];
        *y1 = r[1];
        *x2 = r[2];
        *y2 = r[3];
    }
    picogame_dirty_reset(r);
    return dirty;
}

// Halve each RGB565 channel of a wire-order pixel (50% darken, for shadow mode).
static inline uint16_t picogame_darken(uint16_t wire) {
    uint16_t c = (uint16_t)((wire >> 8) | (wire << 8));   // wire -> native RGB565
    // Halve all three channels at once: >>1 shifts every channel right; the 0x7BEF mask clears the two
    // bits that would bleed across channel boundaries (R's LSB into G's MSB, G's LSB into B's MSB) +
    // R's now-0 top bit. Bit-identical to the per-channel r>>1/g>>1/b>>1 above, ~half the instructions.
    uint16_t o = (uint16_t)((c >> 1) & 0x7BEF);
    return (uint16_t)((o >> 8) | (o << 8));               // native -> wire
}

// 4x4 ordered (Bayer) dither thresholds, 0..15.
static const uint8_t picogame_bayer4[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

// Multiply two wire-order RGB565 pixels per channel (TINT: colour the source, keep its shading).
static inline uint16_t picogame_mul565(uint16_t a, uint16_t b) {
    uint16_t ca = (uint16_t)((a >> 8) | (a << 8));
    uint16_t cb = (uint16_t)((b >> 8) | (b << 8));
    // /31 and /63 via reciprocal-multiply (bit-identical over the full product domain 0..961 / 0..3969):
    // avoids a soft-divide per tinted pixel on M0+ (no HW divide); pure integer, fine on every MCU.
    uint16_t r = (uint16_t)((((ca >> 11) & 0x1f) * ((cb >> 11) & 0x1f) * 529) >> 14);
    uint16_t g = (uint16_t)((((ca >> 5) & 0x3f) * ((cb >> 5) & 0x3f) * 2081) >> 17);
    uint16_t bl = (uint16_t)(((ca & 0x1f) * (cb & 0x1f) * 529) >> 14);
    uint16_t o = (uint16_t)((r << 11) | (g << 5) | bl);
    return (uint16_t)((o >> 8) | (o << 8));
}

// Write one opaque source pixel through the effect. (x, y) are screen coords (for DITHER).
static inline void picogame_fx_put(uint16_t *dst, uint16_t src, int x, int y, const picogame_fx_t *fx) {
    if (fx == NULL) {
        *dst = src;
        return;
    }
    switch (fx->mode) {
        case PICOGAME_FX_SHADOW:
            *dst = picogame_darken(*dst);
            break;
        case PICOGAME_FX_FLASH:
            *dst = fx->color;
            break;
        case PICOGAME_FX_TINT:
            *dst = picogame_mul565(src, fx->color);   // colour the sprite, keep its shading
            break;
        case PICOGAME_FX_DITHER:
            if (picogame_bayer4[y & 3][x & 3] >= fx->level) {
                *dst = src;                       // else: pixel skipped -> shows through
            }
            break;
        default:
            *dst = src;
            break;
    }
}

// Fetch one source pixel from HOISTED scalars: the caller lifts format/data/palette/transparency
// out of the bitmap struct ONCE before its loop, so this does no per-pixel reload of bm fields (a
// `*dst` uint16_t store would otherwise force GCC to reload bm's uint16_t members every pixel). `idx`
// is the linear source offset (srow + sx). Returns false on the transparent key. Used by the scaled /
// affine / transpose paths; the unscaled fast path inlines the read directly.
static inline bool src_pixel_s(int format, const uint8_t *data, const uint16_t *pal,
    bool transp, uint16_t key, int idx, uint16_t *out) {
    if (format == PICOGAME_FMT_PAL8) {
        uint8_t i = data[idx];
        if (transp && i == (uint8_t)key) {
            return false;
        }
        *out = pal[i];                           // indices must be < palette length (see blit contract)
        return true;
    }
    // GC buffer is >=4-byte aligned; silence xtensa -Wcast-align (see picogame_blit_bitmap).
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wcast-align"
    uint16_t v = ((const uint16_t *)data)[idx];
    #pragma GCC diagnostic pop
    if (transp && v == key) {
        return false;
    }
    *out = v;
    return true;
}

void picogame_blit_bitmap(
    uint16_t *buf, int bw, int bh, int ox, int oy,
    picogame_bitmap_obj_t *bm, int dx0, int dy0, int frame, bool fx, bool fy,
    bool transpose, const picogame_fx_t *fxm) {
    if (bm == NULL) {
        return;
    }
    // Point fxm at a stack copy: its fields (uint16_t color) can't then alias the *dst stores, so the
    // effect params stay in registers across the pixel loop instead of reloading every pixel.
    picogame_fx_t fxl;
    if (fxm != NULL) {
        fxl = *fxm;
        fxm = &fxl;
    }
    int sw = bm->width;
    int sh = bm->height;

    // Guard the frame index: sprite.frame is a free uint8_t, so an out-of-range
    // value (bad wrap / overflow in game code) would read past the sheet data.
    // Wrap into [0, frames) - cheap and animation-friendly.
    if (bm->frames > 1) {
        if (frame >= bm->frames) {    // common case is in range: pay a compare, not a divide
            frame %= bm->frames;
        }
    } else {
        frame = 0;
    }

    int frame_col0 = frame * sw;
    int stride0 = bm->stride;

    // Transpose path: swap source x/y (a cheap 90deg rotate when combined with flips -> all 8
    // orientations, no cos/sin/affine). The drawn footprint swaps to sh x sw. Per-pixel sampling
    // (no per-row srow precompute), used only when requested; flips + fx still apply.
    if (transpose) {
        int dw = sh, dh = sw;                      // footprint swaps
        int xs = picogame_imax(dx0, ox), ys = picogame_imax(dy0, oy);
        int xe = picogame_imin(dx0 + dw, ox + bw), ye = picogame_imin(dy0 + dh, oy + bh);
        if (xs >= xe || ys >= ye) {
            return;
        }
        int t_fmt = bm->format;                    // hoist bm fields once (see src_pixel_s)
        const uint8_t *t_data = bm->data;
        const uint16_t *t_pal = bm->palette;
        bool t_transp = bm->has_transparent;
        uint16_t t_key = bm->transparent;
        for (int y = ys; y < ye; y++) {
            int ly = y - dy0;                      // -> source X (0..sw-1)
            int su = fx ? sw - 1 - ly : ly;        // per-row: source column is constant across the row
            uint16_t *dst = buf + (y - oy) * bw + (xs - ox);
            int lx = xs - dx0;
            int sv = fy ? sh - 1 - lx : lx;        // source row: step it, no per-pixel ternary
            int svstep = fy ? -1 : 1;
            for (int x = xs; x < xe; x++) {
                uint16_t val;
                if (src_pixel_s(t_fmt, t_data, t_pal, t_transp, t_key,
                    sv * stride0 + frame_col0 + su, &val)) {
                    picogame_fx_put(dst, val, x, y, fxm);
                }
                dst++;
                sv += svstep;
            }
        }
        return;
    }

    int x_start = picogame_imax(dx0, ox);
    int y_start = picogame_imax(dy0, oy);
    int x_end = picogame_imin(dx0 + sw, ox + bw);
    int y_end = picogame_imin(dy0 + sh, oy + bh);
    if (x_start >= x_end || y_start >= y_end) {
        return;
    }

    int frame_col = frame * sw;
    int stride = bm->stride;
    bool transp = bm->has_transparent;

    if (bm->format == PICOGAME_FMT_PAL8) {
        const uint8_t *data = bm->data;
        const uint16_t *pal = bm->palette;
        uint8_t key = (uint8_t)bm->transparent;
        // Contract: PAL8 indices MUST be < palette length (caller's responsibility). An out-of-range
        // index is undefined behaviour: it reads past the palette - usually a garbage colour, but on
        // some heap layouts / platforms (e.g. ESP32-S3 heap_caps regions) it CAN fault. We deliberately
        // do NOT clamp per pixel: that cost ~3 cyc/px (cmp+sbcs+ands) here, ~8% on blit-bound frames.
        //
        // TO RESTORE FULL BOUNDS-SAFETY (at that cost) reinstate the clamp - add `unsigned pe =
        // bm->pal_entries;` here and `if (idx >= pe) { idx = 0; }` after each `idx = data[...]` in BOTH
        // loops below, and the matching guard in src_pixel() (search "blit contract").
        for (int y = y_start; y < y_end; y++) {
            int sy = y - dy0;
            if (fy) {
                sy = sh - 1 - sy;
            }
            int srow = sy * stride + frame_col;
            uint16_t *dst = buf + (y - oy) * bw + (x_start - ox);
            int sx = x_start - dx0, xstep = 1;       // hoist flip_x: walk sx +/-1, no per-pixel test
            if (fx) {
                sx = sw - 1 - sx;
                xstep = -1;
            }
            if (fxm == NULL) {                   // plain copy (most sprites): no per-pixel fx branch/call
                #pragma GCC unroll 4   // hot path: unrolling the plain sprite blit is ~6% faster on M0+ (measured), +0.6KB
                for (int x = x_start; x < x_end; x++) {
                    uint8_t idx = data[srow + sx];
                    if (!transp || idx != key) {
                        *dst = pal[idx];
                    }
                    dst++;
                    sx += xstep;
                }
            } else {
                for (int x = x_start; x < x_end; x++) {
                    uint8_t idx = data[srow + sx];
                    if (!transp || idx != key) {
                        picogame_fx_put(dst, pal[idx], x, y, fxm);
                    }
                    dst++;
                    sx += xstep;
                }
            }
        }
    } else { // PICOGAME_FMT_RGB565
        // bm->data is a GC-allocated Python buffer (>=4-byte aligned), so the 16-bit
        // view is safe; xtensa's -Wcast-align can't see that, so silence it here.
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wcast-align"
        const uint16_t *data = (const uint16_t *)bm->data;
        #pragma GCC diagnostic pop
        uint16_t key = bm->transparent;
        for (int y = y_start; y < y_end; y++) {
            int sy = y - dy0;
            if (fy) {
                sy = sh - 1 - sy;
            }
            int srow = sy * stride + frame_col;
            uint16_t *dst = buf + (y - oy) * bw + (x_start - ox);
            int sx = x_start - dx0, xstep = 1;       // hoist flip_x: walk sx +/-1, no per-pixel test
            if (fx) {
                sx = sw - 1 - sx;
                xstep = -1;
            }
            if (fxm == NULL) {                   // plain copy (most sprites): no per-pixel fx branch/call
                if (!transp && !fx) {            // opaque + not x-flipped: the row is contiguous in
                    // both src and dst -> one memcpy (dst may be 2-byte aligned; memcpy handles that).
                    memcpy(dst, &data[srow + sx], (size_t)(x_end - x_start) * 2u);
                    continue;
                }
                #pragma GCC unroll 4   // hot path: unrolling the plain sprite blit is ~6% faster on M0+ (measured), +0.6KB
                for (int x = x_start; x < x_end; x++) {
                    uint16_t v = data[srow + sx];
                    if (!transp || v != key) {
                        *dst = v;
                    }
                    dst++;
                    sx += xstep;
                }
            } else {
                for (int x = x_start; x < x_end; x++) {
                    uint16_t v = data[srow + sx];
                    if (!transp || v != key) {
                        picogame_fx_put(dst, v, x, y, fxm);
                    }
                    dst++;
                    sx += xstep;
                }
            }
        }
    }
}


void picogame_blit_bitmap_scaled(
    uint16_t *buf, int bw, int bh, int ox, int oy,
    picogame_bitmap_obj_t *bm, int dx0, int dy0, int frame, bool fx, bool fy,
    uint16_t scale, const picogame_fx_t *fxm) {
    if (bm == NULL || scale == 0) {
        return;
    }
    picogame_fx_t fxl;                           // stack copy: fields don't alias *dst (see blit_bitmap)
    if (fxm != NULL) {
        fxl = *fxm;
        fxm = &fxl;
    }
    int sw = bm->width, sh = bm->height;
    if (bm->frames > 1) {
        if (frame >= bm->frames) {    // common case is in range: pay a compare, not a divide
            frame %= bm->frames;
        }
    } else {
        frame = 0;
    }
    int dw = (sw * scale) >> 8, dh = (sh * scale) >> 8;
    if (dw <= 0 || dh <= 0) {
        return;
    }
    int x_start = picogame_imax(dx0, ox), y_start = picogame_imax(dy0, oy);
    int x_end = picogame_imin(dx0 + dw, ox + bw), y_end = picogame_imin(dy0 + dh, oy + bh);
    if (x_start >= x_end || y_start >= y_end) {
        return;
    }
    int frame_col = frame * sw, stride = bm->stride;
    int s_fmt = bm->format;                       // hoist bm fields once (see src_pixel_s)
    const uint8_t *s_data = bm->data;
    const uint16_t *s_pal = bm->palette;
    bool s_transp = bm->has_transparent;
    uint16_t s_key = bm->transparent;
    uint32_t step = ((uint32_t)1 << 24) / scale;     // source px per dest px, 16.16
    // No per-row sy>=sh / per-pixel sx>=sw clamp: with dw=(sd*scale)>>8 and step=floor(2^24/scale),
    // the sampled index ((dw-1)*step)>>16 provably never reaches the source dimension (exhaustively
    // verified over the ENTIRE uint16 x uint16 (scale, dim) domain, 0 violations), and xacc<dim<<16
    // can't wrap uint32. So the clamps only ever cost a compare per pixel in this hot loop.
    for (int y = y_start; y < y_end; y++) {
        int sy = (int)(((uint32_t)(y - dy0) * step) >> 16);
        if (fy) {
            sy = sh - 1 - sy;
        }
        int srow = sy * stride + frame_col;
        uint16_t *drow = buf + (y - oy) * bw;
        uint32_t xacc = (uint32_t)(x_start - dx0) * step;
        for (int x = x_start; x < x_end; x++) {
            int sx = (int)(xacc >> 16);
            xacc += step;
            if (fx) {
                sx = sw - 1 - sx;
            }
            uint16_t val;
            if (src_pixel_s(s_fmt, s_data, s_pal, s_transp, s_key, srow + sx, &val)) {
                picogame_fx_put(&drow[x - ox], val, x, y, fxm);
            }
        }
    }
}

// Quarter-wave Q15 sine table (0..90 deg) -> fixed-point trig for the rotation setup, so the
// affine path needs no float `sinf`/`cosf` (RP2040 has no FPU). cos(d) = sin(d+90).
static const int16_t pg_sin_q15_quad[91] = {
    0, 572, 1144, 1715, 2286, 2856, 3425, 3993, 4560, 5126,
    5690, 6252, 6813, 7371, 7927, 8481, 9032, 9580, 10126, 10668,
    11207, 11743, 12275, 12803, 13328, 13848, 14364, 14876, 15383, 15886,
    16383, 16876, 17364, 17846, 18323, 18794, 19260, 19720, 20173, 20621,
    21062, 21497, 21925, 22347, 22762, 23170, 23571, 23964, 24351, 24730,
    25101, 25465, 25821, 26169, 26509, 26841, 27165, 27481, 27788, 28087,
    28377, 28659, 28932, 29196, 29451, 29697, 29934, 30162, 30381, 30591,
    30791, 30982, 31163, 31335, 31498, 31650, 31794, 31927, 32051, 32165,
    32269, 32364, 32448, 32523, 32587, 32642, 32687, 32722, 32747, 32762,
    32767,
};
static int32_t pg_sin_q15(int deg) {
    deg %= 360;
    if (deg < 0) {
        deg += 360;
    }
    if (deg <= 90) {
        return pg_sin_q15_quad[deg];
    }
    if (deg <= 180) {
        return pg_sin_q15_quad[180 - deg];
    }
    if (deg <= 270) {
        return -pg_sin_q15_quad[deg - 180];
    }
    return -pg_sin_q15_quad[360 - deg];
}
static int32_t pg_cos_q15(int deg) {
    return pg_sin_q15(deg + 90);
}

// Forward-transform a w*h rect's 4 corners through (integer pivot, 8.8 scale, Q15 rotation) and
// return the screen-space AABB. INTEGER (Q16) - measured ~40x faster than the old float version on
// the M0+ flagship (soft-float), and faster on every target (picobench affine_q16 vs affine_float).
// Runs once per rotated sprite (not per pixel). Each corner is FLOORED (arithmetic >>23 = /(256*32768)),
// so the returned box is <= the true min and the callers' +1/+2 keep the box CONTAINING the sprite
// (the affine blitter clips per pixel anyway, so a >=1px-too-large box only repaints, never clips).
static void corners_bbox(int sw, int sh, int px, int py, int pivx, int pivy,
    int32_t scale, int32_t cos_q, int32_t sin_q, int *minx, int *miny, int *maxx, int *maxy) {
    int nx = 1 << 30, xx = -(1 << 30), ny = 1 << 30, xy = -(1 << 30);
    int cxs[4] = { 0, sw, 0, sw }, cys[4] = { 0, 0, sh, sh };
    for (int k = 0; k < 4; k++) {
        int64_t du = (int64_t)(cxs[k] - pivx) * scale;        // (corner - pivot) in pixels<<8
        int64_t dv = (int64_t)(cys[k] - pivy) * scale;
        int X = px + (int)((du * cos_q - dv * sin_q) >> 23);  // >>23 = /(256 * 32768): drop the 8.8 + Q15
        int Y = py + (int)((du * sin_q + dv * cos_q) >> 23);
        if (X < nx) {
            nx = X;
        }
        if (X > xx) {
            xx = X;
        }
        if (Y < ny) {
            ny = Y;
        }
        if (Y > xy) {
            xy = Y;
        }
    }
    *minx = nx;
    *miny = ny;
    *maxx = xx;
    *maxy = xy;
}

// Fill the sprite's affine cache (position-relative bbox + 16.16 inverse-map steps) if stale.
// The trig LUT, the 4-corner bbox and the TWO SOFTWARE DIVIDES below used to re-run once per
// strip a rotated sprite touched (~6x/frame at strip_h=8) plus once for the dirty-rect AABB;
// now once per angle/scale/bitmap/anchor change (those setters clear xf_valid).
static void sprite_xform_fill(picogame_sprite_obj_t *s) {
    if (s->xf_valid) {
        return;
    }
    int w = (s->bitmap != NULL) ? s->bitmap->width : 0;
    int h = (s->bitmap != NULL) ? s->bitmap->height : 0;
    int pivx = ((int)s->anchor_x * w) >> 8, pivy = ((int)s->anchor_y * h) >> 8;
    int32_t cos_q = pg_cos_q15(s->angle), sin_q = pg_sin_q15(s->angle);   // Q15 LUT trig
    int minx, miny, maxx, maxy;
    corners_bbox(w, h, 0, 0, pivx, pivy, (int32_t)s->scale, cos_q, sin_q,
        &minx, &miny, &maxx, &maxy);
    // Saturate into the int16 cache fields: an extreme scale x size (public scale allows
    // ~256x) could exceed +-32767; a saturated bbox only over/under-covers the clip - the
    // blitter clips per strip anyway - instead of wrapping into a wrong-sign rect.
    s->xf_minx = (int16_t)(minx < -32768 ? -32768 : (minx > 32767 ? 32767 : minx));
    s->xf_miny = (int16_t)(miny < -32768 ? -32768 : (miny > 32767 ? 32767 : miny));
    s->xf_maxx = (int16_t)(maxx < -32768 ? -32768 : (maxx > 32767 ? 32767 : maxx));
    s->xf_maxy = (int16_t)(maxy < -32768 ? -32768 : (maxy > 32767 ? 32767 : maxy));
    // inverse map (16.16 fixed-point): u = pivx + (ic*(X-px) + is*(Y-py));
    //                                  v = pivy + (-is*(X-px) + ic*(Y-py))
    // ic = (cs/sf)*65536 = cos_q15 * 512 / scale - computed in pure integer (no float).
    int32_t nic = cos_q * 512, nis = sin_q * 512;
    int sc = (int)s->scale;
    if (sc < 1) {
        sc = 1;                   // scale is setter-clamped >= 1; belt for a zeroed struct
    }
    s->xf_ic = (nic >= 0 ? nic + sc / 2 : nic - sc / 2) / sc;
    s->xf_is = (nis >= 0 ? nis + sc / 2 : nis - sc / 2) / sc;
    s->xf_valid = 1;
}

void picogame_blit_bitmap_affine(
    uint16_t *buf, int bw, int bh, int ox, int oy,
    picogame_bitmap_obj_t *bm, int px, int py, int pivx, int pivy,
    int frame, bool fx, bool fy,
    int minx, int miny, int maxx, int maxy, int32_t ic, int32_t is,
    const picogame_fx_t *fxm) {
    if (bm == NULL) {
        return;
    }
    picogame_fx_t fxl;                           // stack copy: fields don't alias *dst (see blit_bitmap)
    if (fxm != NULL) {
        fxl = *fxm;
        fxm = &fxl;
    }
    int sw = bm->width, sh = bm->height;
    if (bm->frames > 1) {
        if (frame >= bm->frames) {    // common case is in range: pay a compare, not a divide
            frame %= bm->frames;
        }
    } else {
        frame = 0;
    }
    int frame_col = frame * sw, stride = bm->stride;
    int a_fmt = bm->format;                       // hoist bm fields once (see src_pixel_s)
    const uint8_t *a_data = bm->data;
    const uint16_t *a_pal = bm->palette;
    bool a_transp = bm->has_transparent;
    uint16_t a_key = bm->transparent;
    int x_start = picogame_imax(minx, ox), y_start = picogame_imax(miny, oy);
    int x_end = picogame_imin(maxx + 1, ox + bw), y_end = picogame_imin(maxy + 1, oy + bh);
    if (x_start >= x_end || y_start >= y_end) {
        return;
    }
    // Fold flip_x/flip_y into the inverse map ONCE, so the inner loop samples the FINAL source coord
    // directly - no per-pixel `sw-1-iu`/`sh-1-iv` and no fx/fy branch (one loop variant, fewer live
    // values -> fewer register spills). Exact: mirroring u is u'=(sw-1)-u; through the 16.16 floor that
    // is `((sw-1-pivx)<<16 + 0xFFFF) - (ic*dxf + is*dyf)`, i.e. negate the u steps + bias the pivot by
    // 0xFFFF (so floor(u') == (sw-1) - floor(u) for every in-range pixel). Bit-identical output.
    int32_t uxc = ic, uyc = is, upiv = (int32_t)pivx << 16;    // u = upiv + uxc*dxf + uyc*dyf
    int32_t vxc = -is, vyc = ic, vpiv = (int32_t)pivy << 16;   // v = vpiv + vxc*dxf + vyc*dyf
    if (fx) {
        uxc = -ic;
        uyc = -is;
        upiv = ((int32_t)(sw - 1 - pivx) << 16) + 0xFFFF;
    }
    if (fy) {
        vxc = is;
        vyc = -ic;
        vpiv = ((int32_t)(sh - 1 - pivy) << 16) + 0xFFFF;
    }
    // 32-bit 16.16 accumulators (cheaper than 64-bit on the M0+). Peak magnitude stays well within
    // int32 for any sane sprite on a handheld screen. The bounds check keeps an out-of-range sample
    // memory-safe (at worst a skipped pixel), never a crash.
    for (int y = y_start; y < y_end; y++) {
        int dyf = y - py;
        int dxf = x_start - px;
        int32_t uacc = upiv + uxc * dxf + uyc * dyf;
        int32_t vacc = vpiv + vxc * dxf + vyc * dyf;
        uint16_t *drow = buf + (y - oy) * bw;
        for (int x = x_start; x < x_end; x++) {
            int su = uacc >> 16, sv = vacc >> 16;   // already the flipped source coords
            uacc += uxc;
            vacc += vxc;
            if (su >= 0 && su < sw && sv >= 0 && sv < sh) {
                uint16_t val;
                if (src_pixel_s(a_fmt, a_data, a_pal, a_transp, a_key,
                    sv * stride + frame_col + su, &val)) {
                    picogame_fx_put(&drow[x - ox], val, x, y, fxm);
                }
            }
        }
    }
}

void picogame_sprite_aabb(const picogame_sprite_obj_t *s, int *x1, int *y1, int *x2, int *y2) {
    int w = (s->bitmap != NULL) ? s->bitmap->width : 0;
    int h = (s->bitmap != NULL) ? s->bitmap->height : 0;
    if (s->angle == 0) {
        int sw = (w * s->scale) >> 8, sh = (h * s->scale) >> 8;
        if ((s->flags & PICOGAME_SPR_TRANSPOSE) && s->scale == 256) {   // transpose only on the fast
            int t = sw;                            // path (scale==256); scaled blitter ignores it, so
            sw = sh;                               // swapping here for scale!=256 would mistrack -> trail
            sh = t;
        }
        int tx = (s->x >> 8) - ((int)s->anchor_x * sw >> 8);
        int ty = (s->y >> 8) - ((int)s->anchor_y * sh >> 8);
        *x1 = tx;
        *y1 = ty;
        *x2 = tx + sw;
        *y2 = ty + sh;
        return;
    }
    // cache-fill mutates only the derived xf_* fields - logically const for callers
    sprite_xform_fill((picogame_sprite_obj_t *)s);
    int px = s->x >> 8, py = s->y >> 8;
    *x1 = px + s->xf_minx - 1;
    *y1 = py + s->xf_miny - 1;
    *x2 = px + s->xf_maxx + 2;
    *y2 = py + s->xf_maxy + 2;
}

// TINT on a PAL8 sprite is baked into a stack palette (see blit_sprite) when the palette fits this
// cap, else it falls back to the per-pixel tint. 64 entries = 128 B of transient stack.
#define PICOGAME_TINT_PAL_CAP 64

// clip_x/clip_y = the strip buffer's screen origin; vx/vy = view offset added to
// the sprite's scene position to get its screen position.
static void blit_sprite(uint16_t *buf, int bw, int bh, int clip_x, int clip_y,
    picogame_sprite_obj_t *spr, int vx, int vy) {
    bool fx = (spr->flags & PICOGAME_SPR_FLIP_X) != 0;
    bool fy = (spr->flags & PICOGAME_SPR_FLIP_Y) != 0;
    bool tr = (spr->flags & PICOGAME_SPR_TRANSPOSE) != 0;   // 90deg transpose (fast path only)
    // One effect at a time (dither > flash > tint > shadow priority); NULL = no effect (fast path).
    picogame_fx_t fxm = { PICOGAME_FX_NONE, 0, 0 };
    if (spr->flags & PICOGAME_SPR_DITHER) {
        fxm.mode = PICOGAME_FX_DITHER;
        fxm.level = spr->dither;
    } else if (spr->flags & PICOGAME_SPR_FLASH) {
        fxm.mode = PICOGAME_FX_FLASH;
        fxm.color = spr->flash_color;
    } else if (spr->flags & PICOGAME_SPR_TINT) {
        fxm.mode = PICOGAME_FX_TINT;
        fxm.color = spr->flash_color;             // shared colour field (flash/tint exclusive)
    } else if (spr->flags & PICOGAME_SPR_SHADOW) {
        fxm.mode = PICOGAME_FX_SHADOW;
    }
    const picogame_fx_t *fxp = (fxm.mode == PICOGAME_FX_NONE) ? NULL : &fxm;
    // TINT of a PAL8 sprite is a pure function of the palette index -> bake it into a stack palette
    // ONCE and blit plain, instead of picogame_mul565 per pixel (~4x on tinted PAL8 sprites; the
    // dominant sprite format). Small palettes only; larger ones keep the per-pixel tint. Transparency
    // is keyed on the INDEX before the palette lookup, so the (unused) tinted key entry is harmless.
    picogame_bitmap_obj_t *bmuse = spr->bitmap;
    picogame_bitmap_obj_t bmtint;
    uint16_t tpal[PICOGAME_TINT_PAL_CAP];
    if (fxp != NULL && fxm.mode == PICOGAME_FX_TINT && bmuse != NULL &&
        bmuse->format == PICOGAME_FMT_PAL8 && bmuse->pal_entries <= PICOGAME_TINT_PAL_CAP) {
        for (int i = 0; i < bmuse->pal_entries; i++) {
            tpal[i] = picogame_mul565(bmuse->palette[i], fxm.color);
        }
        bmtint = *bmuse;                          // shallow copy; override only the palette
        bmtint.palette = tpal;
        bmuse = &bmtint;
        fxp = NULL;                               // tint now baked in -> plain (fast) blit
    }
    if (spr->angle == 0 && spr->scale == 256) {
        int tx, ty;
        picogame_sprite_topleft(spr, &tx, &ty);
        picogame_blit_bitmap(buf, bw, bh, clip_x, clip_y, bmuse,
            tx + vx, ty + vy, spr->frame, fx, fy, tr, fxp);
    } else if (spr->angle == 0) {
        int tx, ty;
        picogame_sprite_topleft(spr, &tx, &ty);
        picogame_blit_bitmap_scaled(buf, bw, bh, clip_x, clip_y, bmuse,
            tx + vx, ty + vy, spr->frame, fx, fy, spr->scale, fxp);
    } else {
        sprite_xform_fill(spr);            // once per angle/scale change, not per strip
        int w = (spr->bitmap != NULL) ? spr->bitmap->width : 0;
        int h = (spr->bitmap != NULL) ? spr->bitmap->height : 0;
        int pivx = ((int)spr->anchor_x * w) >> 8, pivy = ((int)spr->anchor_y * h) >> 8;
        int px = (spr->x >> 8) + vx, py = (spr->y >> 8) + vy;
        picogame_blit_bitmap_affine(buf, bw, bh, clip_x, clip_y, bmuse,
            px, py, pivx, pivy, spr->frame, fx, fy,
            px + spr->xf_minx, py + spr->xf_miny, px + spr->xf_maxx, py + spr->xf_maxy,
            spr->xf_ic, spr->xf_is, fxp);
    }
}

mp_obj_t picogame_blit_strip_layers(
    uint16_t *buf, int region_w, int strip_top, int strip_h, int x0,
    mp_obj_t *items, uint8_t *kinds, size_t n, uint16_t background, int ox, int oy) {
    mp_obj_t pending = MP_OBJ_NULL;               // a BaseException latched from a StripDraw callback
    int npix = region_w * strip_h;
    if (background == 0) {
        memset(buf, 0, (size_t)npix * 2);            // common case (black/clear) -> fast bulk clear
    } else {
        // word-fill: two packed pixels per uint32 (half the stores). The strip buffer is a
        // GC-allocated buffer (>=4-byte aligned), so the cast is safe; -Wcast-align can't see that.
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wcast-align"
        uint32_t w = (uint32_t)background | ((uint32_t)background << 16);
        uint32_t *w32 = (uint32_t *)buf;
        #pragma GCC diagnostic pop
        int nw = npix >> 1;
        for (int i = 0; i < nw; i++) {
            w32[i] = w;
        }
        if (npix & 1) {                              // odd tail pixel
            buf[npix - 1] = background;
        }
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t raw = (kinds != NULL) ? kinds[i] : PICOGAME_KIND_SPRITE;
        uint8_t kind = raw & PICOGAME_KIND_MASK;
        // Fixed (HUD) items are drawn in screen space -> no view offset.
        int iox = (raw & PICOGAME_KIND_FIXED) ? 0 : ox;
        int ioy = (raw & PICOGAME_KIND_FIXED) ? 0 : oy;
        if (kind == PICOGAME_KIND_TILEMAP) {
            picogame_blit_tilemap(buf, region_w, strip_top, strip_h, x0,
                MP_OBJ_TO_PTR(items[i]), iox, ioy);
        } else if (kind == PICOGAME_KIND_PARTICLES) {
            picogame_blit_particles(buf, region_w, strip_top, strip_h, x0,
                MP_OBJ_TO_PTR(items[i]), iox, ioy);
        } else if (kind == PICOGAME_KIND_CANVAS) {
            picogame_blit_canvas(buf, region_w, strip_top, strip_h, x0,
                MP_OBJ_TO_PTR(items[i]), iox, ioy);
        } else if (kind == PICOGAME_KIND_STRIPDRAW) {
            // Immediate mode: hand the callback a view of the part of THIS strip that
            // overlaps the layer's rect (so it can only paint within its rect - a
            // full-view fill stays inside it). Skip strips the rect doesn't touch.
            picogame_stripdraw_obj_t *sd = MP_OBJ_TO_PTR(items[i]);
            int ry0 = sd->y, ry1 = sd->y + sd->h;
            int s0 = strip_top > ry0 ? strip_top : ry0;       // first screen row to draw
            int st_end = strip_top + strip_h;
            int s1 = st_end < ry1 ? st_end : ry1;             // one past last screen row
            if (s0 >= s1) {
                continue;                                      // strip is outside the layer
            }
            picogame_canvas_obj_t *v = MP_OBJ_TO_PTR(sd->view);
            v->data = buf + (s0 - strip_top) * region_w;      // view row 0 == screen row s0
            v->w = region_w;
            v->h = s1 - s0;
            v->x = 0;
            v->y = 0;
            v->has_transparent = false;
            mp_obj_t cbargs[5] = {
                sd->view,
                MP_OBJ_NEW_SMALL_INT(x0),
                MP_OBJ_NEW_SMALL_INT(s0),
                MP_OBJ_NEW_SMALL_INT(region_w),
                MP_OBJ_NEW_SMALL_INT(s1 - s0),
            };
            // We're inside an open display bus transaction here, so a raised
            // exception must NOT unwind past it (that would wedge the bus / leave a
            // DMA running). Catch it, keep the transaction intact, and latch so the
            // traceback prints once instead of every strip every frame.
            nlr_buf_t nlr;
            if (nlr_push(&nlr) == 0) {
                mp_call_function_n_kw(sd->callback, 5, 0, cbargs);
                nlr_pop();
            } else {
                mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
                if (!mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(exc)),
                    MP_OBJ_FROM_PTR(&mp_type_Exception))) {
                    // A BaseException (KeyboardInterrupt / ReloadException / SystemExit) must reach the
                    // supervisor - latch it and stop; the caller re-raises it once the display
                    // transaction has safely closed, so Ctrl-C and USB auto-reload actually work.
                    pending = exc;
                    break;
                }
                if (!sd->faulted) {
                    sd->faulted = true;
                    mp_obj_print_exception(&mp_plat_print, exc);
                }
            }
        } else {
            picogame_sprite_obj_t *spr = MP_OBJ_TO_PTR(items[i]);
            if (!(spr->flags & PICOGAME_SPR_VISIBLE)) {
                continue;
            }
            blit_sprite(buf, region_w, strip_h, x0, strip_top, spr, iox, ioy);
        }
    }
    return pending;
}

bool picogame_strip_begin(
    busdisplay_busdisplay_obj_t *display,
    int *x0p, int *y0p, int *x1p, int *y1p, size_t buffer_pixels,
    int *region_w, int *strip_h) {
    // Clamp the window to the panel (post-rotation w/h): an out-of-range region makes the controller
    // wrap/garble rows. Callers usually pass clamped dirty rects, but StripDraw / direct render_region
    // can hand us a rect that runs off the panel. Clamp AND write back through the pointers so the
    // caller's strip loop + blit origin use the SAME clamped bounds (else only the GRAM window is
    // clamped while the data loop still pushes off-panel rows -> the wrap/garble we're preventing).
    int pw = display->core.width, ph = display->core.height;
    int x0 = *x0p, y0 = *y0p, x1 = *x1p, y1 = *y1p;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > pw) {
        x1 = pw;
    }
    if (y1 > ph) {
        y1 = ph;
    }
    *x0p = x0;
    *y0p = y0;
    *x1p = x1;
    *y1p = y1;
    int rw = x1 - x0;
    int rh = y1 - y0;
    if (rw <= 0 || rh <= 0) {
        return false;
    }
    int sh = (int)(buffer_pixels / (size_t)rw);
    if (sh < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer too small"));
    }
    if (sh > rh) {
        sh = rh;
    }

    displayio_area_t area;
    area.x1 = x0;
    area.y1 = y0;
    area.x2 = x1;
    area.y2 = y1;
    area.next = NULL;
    displayio_display_bus_set_region_to_update(&display->bus, &display->core, &area);

    while (!displayio_display_bus_begin_transaction(&display->bus)) {
        RUN_BACKGROUND_TASKS;
    }
    display->bus.send(display->bus.bus, DISPLAY_COMMAND,
        CHIP_SELECT_TOGGLE_EVERY_BYTE, &display->write_ram_command, 1);

    *region_w = rw;
    *strip_h = sh;
    return true;
}

void picogame_render_region(
    busdisplay_busdisplay_obj_t *display,
    mp_obj_t *items, uint8_t *kinds, size_t n,
    uint16_t *buffer, size_t buffer_pixels,
    int16_t x0, int16_t y0, int16_t x1, int16_t y1,
    uint16_t background, int ox, int oy) {

    int region_w, strip_h;
    int cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;   // strip_begin clamps these to the panel in place
    if (!picogame_strip_begin(display, &cx0, &cy0, &cx1, &cy1, buffer_pixels, &region_w, &strip_h)) {
        return;
    }
    for (int sy = cy0; sy < cy1; sy += strip_h) {
        int sh = picogame_imin(strip_h, cy1 - sy);
        mp_obj_t exc = picogame_blit_strip_layers(buffer, region_w, sy, sh, cx0, items, kinds, n, background, ox, oy);
        display->bus.send(display->bus.bus, DISPLAY_DATA,
            CHIP_SELECT_UNTOUCHED, (uint8_t *)buffer, region_w * sh * 2);
        if (exc != MP_OBJ_NULL) {                 // a StripDraw callback raised a BaseException: close the
            displayio_display_bus_end_transaction(&display->bus);   // bus, then re-raise (Ctrl-C / reload)
            nlr_raise(MP_OBJ_TO_PTR(exc));
        }
    }
    displayio_display_bus_end_transaction(&display->bus);
}

// Toggle the panel's hardware colour inversion (INVON 0x21 / INVOFF 0x20). Instant, sends NO
// pixel data - a brief invert is a free full-screen "flash" (a 1-bit negative hit look).
void picogame_set_invert(busdisplay_busdisplay_obj_t *display, bool on) {
    uint8_t cmd = on ? 0x21 : 0x20;
    while (!displayio_display_bus_begin_transaction(&display->bus)) {
        RUN_BACKGROUND_TASKS;
    }
    display->bus.send(display->bus.bus, DISPLAY_COMMAND, CHIP_SELECT_TOGGLE_EVERY_BYTE, &cmd, 1);
    displayio_display_bus_end_transaction(&display->bus);
}

#if CIRCUITPY_PICOGAME_RGB444
// The RGB444 machinery (this + pack_rgb444 + the Display rgb444 path) is compiled in ONLY when a
// board sets CIRCUITPY_PICOGAME_RGB444=1. It is a transfer-bound-only win (on a CPU-balanced panel
// like the PicoPad's the pack ~= the SPI byte saving, so it loses); default off (port ?= 0) until
// multi-platform experience justifies flipping the default. See pack_rgb444's measured-optimal note.
// Set the panel pixel format (COLMOD 0x3A): rgb444 -> 12-bit RGB444 (0x53), else 16-bit RGB565
// (0x55). Asserting it on every Display construct also recovers from a previous program that left
// the panel in the other format (survives soft reset).
void picogame_set_pixel_format(busdisplay_busdisplay_obj_t *display, bool rgb444) {
    uint8_t cmd = 0x3A;
    uint8_t param = rgb444 ? 0x53 : 0x55;
    while (!displayio_display_bus_begin_transaction(&display->bus)) {
        RUN_BACKGROUND_TASKS;
    }
    display->bus.send(display->bus.bus, DISPLAY_COMMAND, CHIP_SELECT_TOGGLE_EVERY_BYTE, &cmd, 1);
    display->bus.send(display->bus.bus, DISPLAY_DATA, CHIP_SELECT_UNTOUCHED, &param, 1);
    displayio_display_bus_end_transaction(&display->bus);
}

// Pack a strip of `npix` (must be even) WIRE-order RGB565 pixels IN-PLACE to ST7789 12-bit RGB444
// (2 px -> 3 bytes): R0G0 / B0R1 / G1B1. Returns the packed byte count. Cuts SPI traffic ~25%.
// In-place is safe: the write offset (1.5*i) always trails the read offset (2*i).
size_t picogame_pack_rgb444(uint16_t *buf, size_t npix) {
    // Pack wire RGB565 -> ST7789 12-bit RGB444 (2 px -> 3 bytes) in place; the write offset (1.5*i)
    // always trails the read offset (2*i). NB: the byte-swap to native order is LOAD-BEARING, not
    // waste - it nibble-aligns the channels so each extracts in ~2 ops; extracting straight from the
    // wire value makes GREEN (split across the byte boundary) cost ~5 ops and is SLOWER on device
    // (measured, M0+). Likewise do NOT wide-unroll (8 registers -> spills). This tight form is the
    // measured-optimal pack; RGB444 still loses to RGB565 on a CPU-balanced panel (pack ~= the SPI
    // byte saving), so it is a transfer-bound-only option (default off).
    uint8_t *out = (uint8_t *)buf;
    size_t o = 0;
    for (size_t i = 0; i + 1 < npix; i += 2) {
        uint32_t w0 = buf[i], w1 = buf[i + 1];
        uint32_t n0 = (w0 >> 8) | (w0 << 8);          // wire -> native RGB565
        uint32_t n1 = (w1 >> 8) | (w1 << 8);
        uint8_t r0 = (n0 >> 12) & 0xF, g0 = (n0 >> 7) & 0xF, b0 = (n0 >> 1) & 0xF;
        uint8_t r1 = (n1 >> 12) & 0xF, g1 = (n1 >> 7) & 0xF, b1 = (n1 >> 1) & 0xF;
        out[o++] = (r0 << 4) | g0;
        out[o++] = (b0 << 4) | r1;
        out[o++] = (g1 << 4) | b1;
    }
    return o;
}
#endif // CIRCUITPY_PICOGAME_RGB444

void picogame_render(
    busdisplay_busdisplay_obj_t *display,
    mp_obj_t *items, size_t n,
    uint16_t *buffer, size_t buffer_pixels,
    int16_t x0, int16_t y0, int16_t x1, int16_t y1,
    uint16_t background) {
    picogame_render_region(display, items, NULL, n, buffer, buffer_pixels,
        x0, y0, x1, y1, background, 0, 0);
}

#if CIRCUITPY_PICOGAME_FRAMEBUFFER
// Full-frame RAM-framebuffer backend. Same layered compositor as the SPI strip path
// (picogame_blit_strip_layers -> identical kinds dispatch, view offset, StripDraw),
// but the destination is a caller-owned framebuffer instead of a bus window - no
// transaction, no strip transfer. This is the shared render target for scanout-buffer
// platforms (RP2350 DVI/HSTX, the desktop sim, the WASM playground): the framebuffer
// IS the composite surface, so a region is composited straight into fb in place.
//
// fb        : destination, wire-order RGB565, fb_stride*fb_h pixels (caller-owned).
// fb_stride : framebuffer row stride in pixels (>= x1); rows may be wider than x1.
// items/kinds/n, background, ox/oy : the scene layer list, as picogame_render_region.
// [x0,y0,x1,y1): the framebuffer region to (re)composite - a dirty rect, or the frame.
// Bounds are clamped to [0,fb_stride) x [0,fb_h); an empty region is a no-op.
// Returns a latched BaseException raised by a StripDraw callback (the caller re-raises
// it), or MP_OBJ_NULL - the SAME contract as the strip path, minus the bus to close.
// Convert a contiguous run of `n` pixels in place from wire-order to native RGB565.
// `((v&0x00ff00ff)<<8)|((v&0xff00ff00)>>8)` byte-swaps both halfwords of a 32-bit word at once
// (GCC lowers it to a single Cortex REV16), so two pixels per word; unrolled x4 (8 px/iter) so the
// ldr/str stream pipelines on zero-wait SRAM. A run may start at an odd x (partial dirty rect), so
// peel one leading pixel to keep the word accesses 4-byte aligned (unaligned faults on M0+).
// Portable + little-endian, so it also serves the WASM native565 canvas target.
static void picogame_fb_to_native(uint16_t *px, int n) {
    int i = 0;
    if (n > 0 && ((uintptr_t)px & 2u) != 0) {
        px[0] = (uint16_t)__builtin_bswap16(px[0]);
        i = 1;
    }
    int pairs = (n - i) >> 1;
    uint32_t *w = (uint32_t *)(px + i);
    int p = 0;
    for (; p + 4 <= pairs; p += 4) {
        uint32_t v0 = w[p], v1 = w[p + 1], v2 = w[p + 2], v3 = w[p + 3];
        w[p] = ((v0 & 0x00ff00ffu) << 8) | ((v0 & 0xff00ff00u) >> 8);
        w[p + 1] = ((v1 & 0x00ff00ffu) << 8) | ((v1 & 0xff00ff00u) >> 8);
        w[p + 2] = ((v2 & 0x00ff00ffu) << 8) | ((v2 & 0xff00ff00u) >> 8);
        w[p + 3] = ((v3 & 0x00ff00ffu) << 8) | ((v3 & 0xff00ff00u) >> 8);
    }
    for (; p < pairs; p++) {
        uint32_t v = w[p];
        w[p] = ((v & 0x00ff00ffu) << 8) | ((v & 0xff00ff00u) >> 8);
    }
    i += pairs << 1;
    if (i < n) {
        px[i] = (uint16_t)__builtin_bswap16(px[i]);
    }
}

mp_obj_t picogame_render_framebuffer(
    uint16_t *fb, int fb_stride, int fb_h, bool native_rgb565,
    mp_obj_t *items, uint8_t *kinds, size_t n,
    int x0, int y0, int x1, int y1,
    uint16_t background, int ox, int oy) {
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > fb_stride) {
        x1 = fb_stride;
    }
    if (y1 > fb_h) {
        y1 = fb_h;
    }
    if (x1 <= x0 || y1 <= y0) {
        return MP_OBJ_NULL;
    }
    int region_w = x1 - x0;
    // The compositor (blitters, effects, palettes) works in wire order throughout; a NATIVE
    // target is converted in place only after a region is fully composed. On a latched StripDraw
    // exception the region was partially composed in wire order - convert it anyway so the fb is
    // never left half wire / half native (the Scene keeps cleared=false until its render loop
    // finishes, so the refresh after the exception repaints the full frame).

    // Fast path: a FULL-WIDTH region has contiguous rows, so the whole rect IS one valid strip
    // buffer. Composite it in a SINGLE strip_layers call (h = the whole band) instead of one call
    // per row - this amortizes all the per-row/per-layer setup (tile-blit dispatch, clip tests,
    // background fill, and crucially one StripDraw Python callback per band instead of per row)
    // over the band, and converts the whole contiguous region in one pass. This is the
    // full-repaint / camera-scroll case (set_view -> cleared=false -> a full-screen dirty rect).
    if (x0 == 0 && x1 == fb_stride) {
        uint16_t *base = fb + (size_t)y0 * (size_t)fb_stride;
        mp_obj_t exc = picogame_blit_strip_layers(
            base, fb_stride, y0, y1 - y0, 0, items, kinds, n, background, ox, oy);
        if (native_rgb565) {
            picogame_fb_to_native(base, (y1 - y0) * fb_stride);
        }
        return exc;   // MP_OBJ_NULL on success, else a latched StripDraw exception
    }

    // Partial-width region: rows are non-contiguous, so composite (and convert) row by row.
    for (int sy = y0; sy < y1; sy++) {
        uint16_t *row = fb + (size_t)sy * (size_t)fb_stride + x0;
        mp_obj_t exc = picogame_blit_strip_layers(
            row, region_w, sy, 1, x0, items, kinds, n, background, ox, oy);
        if (native_rgb565) {
            picogame_fb_to_native(row, region_w);
        }
        if (exc != MP_OBJ_NULL) {
            return exc;   // StripDraw raised a BaseException; caller re-raises (no bus open)
        }
    }
    return MP_OBJ_NULL;
}
#endif // CIRCUITPY_PICOGAME_FRAMEBUFFER
