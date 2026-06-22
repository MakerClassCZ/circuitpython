// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "py/obj.h"
#include "shared-bindings/busdisplay/BusDisplay.h"
#include "shared-module/picogame/Bitmap.h"
#include "shared-module/picogame/Sprite.h"

// Scene layer kinds (tags stored alongside items so blit/dirty can dispatch
// without cross-referencing shared-bindings type objects).
enum {
    PICOGAME_KIND_SPRITE = 0,
    PICOGAME_KIND_TILEMAP = 1,
    PICOGAME_KIND_PARTICLES = 2,
    PICOGAME_KIND_CANVAS = 3,
    PICOGAME_KIND_STRIPDRAW = 4,
    // High bit on a kind = "fixed": the item ignores the scene view offset
    // (camera), so HUD / score / dialog stay put while the world scrolls.
    PICOGAME_KIND_FIXED = 0x80,
    PICOGAME_KIND_MASK = 0x7f,
};

// StripDraw: immediate-mode layer. Holds NO pixel buffer - instead its `callback`
// is invoked once per render strip with a Canvas "view" repointed at the live strip
// buffer, so the user draws primitives straight into the strip (zero RAM vs a Canvas,
// which costs w*h*2 bytes). Its rect is repainted every frame (it's for animated /
// scanline content: pseudo-3D, gradients, procedural backgrounds). The view's local
// (0,0) maps to screen (vx, vy) handed to the callback. `faulted` latches after the
// callback raises once, so a buggy callback prints one traceback, not one per strip.
typedef struct {
    mp_obj_base_t base;
    mp_obj_t callback;       // draw(view, vx, vy, vw, vh): vx/vy = screen origin of view (0,0)
    mp_obj_t view;           // a reused picogame_canvas_obj_t (data repointed each strip)
    int32_t x, y, w, h;      // scene rect, always repainted (int32: scene coords, big-world safe)
    bool faulted;
} picogame_stripdraw_obj_t;

static inline int picogame_imin(int a, int b) {
    return a < b ? a : b;
}
static inline int picogame_imax(int a, int b) {
    return a > b ? a : b;
}

// Drawn top-left in scene pixels: the logical position minus the anchor offset
// (anchor is a 1/256 fraction of the bitmap size). Used by BOTH the blitter and
// the dirty-rect tracker so they always agree on where the sprite lands.
static inline void picogame_sprite_topleft(const picogame_sprite_obj_t *s, int *tx, int *ty) {
    int w = (s->bitmap != NULL) ? s->bitmap->width : 0;
    int h = (s->bitmap != NULL) ? s->bitmap->height : 0;
    int sw = (w * s->scale) >> 8;     // anchor is a fraction of the SCALED size
    int sh = (h * s->scale) >> 8;
    // The blitter only honours transpose on the fast path (scale==256); the scaled blitter ignores it.
    // Swap the footprint ONLY when scale==256, or aabb/topleft disagree with what's drawn (trailing).
    if ((s->flags & PICOGAME_SPR_TRANSPOSE) && s->scale == 256) {   // 90deg transpose swaps footprint
        int t = sw;                            // picogame_sprite_aabb, or the blit top-left and the
        sw = sh;                               // tracked dirty rect disagree (sprite trails)
        sh = t;
    }
    *tx = (s->x >> 8) - ((int)s->anchor_x * sw >> 8);
    *ty = (s->y >> 8) - ((int)s->anchor_y * sh >> 8);
}

// Drawn screen-space bounding box of a sprite (accounts for scale + rotation).
// Used by the dirty-rect tracker so it always covers the transformed sprite.
void picogame_sprite_aabb(const picogame_sprite_obj_t *s, int *x1, int *y1, int *x2, int *y2);

// Per-pixel blit effect, shared by all three blit paths. One mode at a time; a NULL
// pointer means "no effect" (the fast path). SHADOW darkens the destination, FLASH
// replaces opaque pixels with `color`, DITHER skips pixels via a Bayer pattern (0..16
// transparency) for fake translucency without alpha.
enum { PICOGAME_FX_NONE = 0, PICOGAME_FX_SHADOW, PICOGAME_FX_FLASH, PICOGAME_FX_DITHER, PICOGAME_FX_TINT };
typedef struct {
    uint8_t mode;
    uint16_t color;       // FLASH: solid colour to write; TINT: colour to multiply by (wire RGB565)
    uint8_t level;        // DITHER: 0..16 transparency (higher = more pixels skipped)
} picogame_fx_t;

// Shared dirty-rect accumulator over a contiguous int32 [x1,y1,x2,y2] (Canvas + Tilemap both end in
// dx1,dy1,dx2,dy2). INT32 sentinels so big-world (>32767 px) scene coords still accumulate.
void picogame_dirty_reset(int32_t *r);
void picogame_dirty_union(int32_t *r, int x1, int y1, int x2, int y2);
bool picogame_dirty_take(int32_t *r, int *x1, int *y1, int *x2, int *y2);

// Blit one frame of a bitmap at screen (dx0, dy0) into the strip buffer that
// covers [ox, ox+bw) x [oy, oy+bh). Shared by sprites and tilemap tiles.
// fxm: per-pixel effect (NULL = plain colour copy).
void picogame_blit_bitmap(
    uint16_t *buf, int bw, int bh, int ox, int oy,
    picogame_bitmap_obj_t *bm, int dx0, int dy0, int frame, bool flip_x, bool flip_y,
    bool transpose, const picogame_fx_t *fxm);

// Nearest-neighbour scaled blit (axis-aligned); scale is 8.8 fixed-point.
void picogame_blit_bitmap_scaled(
    uint16_t *buf, int bw, int bh, int ox, int oy,
    picogame_bitmap_obj_t *bm, int dx0, int dy0, int frame, bool flip_x, bool flip_y,
    uint16_t scale, const picogame_fx_t *fxm);

// Full affine blit (scale + rotation about the anchor); (px,py)=screen anchor point,
// (pivx,pivy)=that anchor in SOURCE pixels, scale 8.8, angle in whole degrees.
// Nearest-neighbour inverse map.
void picogame_blit_bitmap_affine(
    uint16_t *buf, int bw, int bh, int ox, int oy,
    picogame_bitmap_obj_t *bm, int px, int py, int pivx, int pivy,
    int frame, bool flip_x, bool flip_y, uint16_t scale, int angle, const picogame_fx_t *fxm);

// Fill a strip with background, then composite items (sprites and tilemaps) in
// order (items[0] = bottom). kinds[i] selects the type; kinds == NULL means
// every item is a sprite. (ox, oy) is the view offset added to item positions
// (scene space -> screen space) for camera/centering.
void picogame_blit_strip_layers(
    uint16_t *buf, int region_w, int strip_top, int strip_h, int x0,
    mp_obj_t *items, uint8_t *kinds, size_t n, uint16_t background, int ox, int oy);

// Compute strip geometry and open a render window on the display (set region,
// begin transaction, send RAMWR). Returns false if the region is empty; raises
// if the buffer is too small for the region width. Fills *region_w and *strip_h.
bool picogame_strip_begin(
    busdisplay_busdisplay_obj_t *display,
    int *x0, int *y0, int *x1, int *y1, size_t buffer_pixels,
    int *region_w, int *strip_h);   // clamps *x0..*y1 to the panel in place (caller loops on them)

// Portable backend: strip-render a layered scene region to ANY busdisplay via
// its (blocking) bus.send - single buffer, no DMA. Same layer dispatch as the
// fast path (kinds + view offset), so it is the cross-port fallback for Scene on
// targets without the platform DMA Display. `kinds == NULL` => all sprites.
void picogame_render_region(
    busdisplay_busdisplay_obj_t *display,
    mp_obj_t *items, uint8_t *kinds, size_t n,
    uint16_t *buffer, size_t buffer_pixels,
    int16_t x0, int16_t y0, int16_t x1, int16_t y1,
    uint16_t background, int ox, int oy);

// Toggle the panel's hardware colour inversion (INVON/INVOFF) - a free full-screen flash.
void picogame_set_invert(busdisplay_busdisplay_obj_t *display, bool on);

#if CIRCUITPY_PICOGAME_RGB444   // compiled in only on boards that opt into RGB444 (default off)
// Set panel pixel format (COLMOD): rgb444 -> 12-bit RGB444, else 16-bit RGB565.
void picogame_set_pixel_format(busdisplay_busdisplay_obj_t *display, bool rgb444);

// Pack `npix` (even) wire-order RGB565 pixels in `buf` IN-PLACE to 12-bit RGB444; returns bytes.
size_t picogame_pack_rgb444(uint16_t *buf, size_t npix);
#endif

// Universal sprite-only convenience wrapper over picogame_render_region.
void picogame_render(
    busdisplay_busdisplay_obj_t *display,
    mp_obj_t *items, size_t n,
    uint16_t *buffer, size_t buffer_pixels,
    int16_t x0, int16_t y0, int16_t x1, int16_t y1,
    uint16_t background);
