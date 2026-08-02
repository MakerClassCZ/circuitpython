// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT
//
// picogame: 2D game engine bindings for the PicoPad and similar boards.
// Type definitions are consolidated here so the module has a single
// shared-bindings/shared-module .c pair (CircuitPython build convention).

#include "py/runtime.h"
#include "shared-module/picogame/pg_compat.h"
#include "shared-bindings/busdisplay/BusDisplay.h"
#include "shared-bindings/picogame/__init__.h"
#include "shared-bindings/picogame/Bitmap.h"
#include "shared-bindings/picogame/Sprite.h"
#if CIRCUITPY_PICOGAME_FAST_DISPLAY
#include "shared-bindings/picogame/Display.h"   // fast DMA backend; absent on portable ports
#include "common-hal/picogame/Display.h"        // its struct (pg_get_display unwraps the wrapper)
#endif
#include "shared-bindings/picogame/Scene.h"
#include "shared-bindings/picogame/Tilemap.h"
#include "shared-bindings/picogame/Particles.h"
#include "shared-bindings/picogame/Canvas.h"
#include "shared-bindings/picogame/Framebuffer.h"
#include "shared-module/picogame/__init__.h"
#include "shared-module/picogame/Bitmap.h"
#include "shared-module/picogame/Sprite.h"

// ---------------------------------------------------------------------------
// Bitmap
// ---------------------------------------------------------------------------

//| class Bitmap:
//|     """An image atlas of one or more equal-size frames, of arbitrary size.
//|
//|     Unlike ``_stage`` (fixed 16x16 tiles), frames may be any width/height.
//|     Pixel data and palette entries must be in the display's wire byte order
//|     (use :py:func:`picogame.rgb565` to build colors)."""
//|
//|     def __init__(
//|         self,
//|         data: ReadableBuffer,
//|         width: int,
//|         height: int,
//|         *,
//|         format: int = RGB565,
//|         palette: Optional[ReadableBuffer] = None,
//|         frames: int = 1,
//|         stride: int = 0,
//|         transparent: Optional[int] = None,
//|     ) -> None: ...
// Int pixel/scale -> 24.8 fixed-point. Shift through unsigned so a wild coordinate (e.g. 100_000_000,
// in 32-bit mp_int range but not after <<8) wraps modularly instead of hitting signed-overflow UB.
// Costs nothing over a plain shift; for any real on-screen coordinate the result is identical.
static int32_t pg_int_to_fp8(mp_int_t v) {
    return (int32_t)((uint32_t)v << 8);
}

static mp_obj_t picogame_bitmap_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_data, ARG_width, ARG_height, ARG_format, ARG_palette, ARG_frames, ARG_stride, ARG_transparent };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_format, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = PICOGAME_FMT_RGB565} },
        { MP_QSTR_palette, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_frames, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_stride, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_transparent, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // bound BEFORE any arithmetic: an unbounded width could overflow width*frames (int32) and wrap
    // small, slipping past the size guards below into an undersized buffer (OOB read in the blitter).
    mp_int_t width = mp_arg_validate_int_range(args[ARG_width].u_int, 1, 65535, MP_QSTR_width);
    mp_int_t height = mp_arg_validate_int_range(args[ARG_height].u_int, 1, 65535, MP_QSTR_height);
    mp_int_t frames = mp_arg_validate_int_range(args[ARG_frames].u_int, 1, 255, MP_QSTR_frames);
    mp_int_t format = args[ARG_format].u_int;
    if (format != PICOGAME_FMT_RGB565 && format != PICOGAME_FMT_PAL8) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid format"));
    }
    mp_int_t stride = args[ARG_stride].u_int;
    if (stride <= 0) {
        stride = width * frames;
    }
    // stride must hold the whole horizontal atlas, and the dims must fit the uint16_t fields,
    // or blits index past a row / the stored stride truncates.
    if (width * frames > 65535 || stride > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("bitmap too large"));
    }
    if (stride < width * frames) {
        mp_raise_ValueError(MP_ERROR_TEXT("stride too small"));
    }

    mp_buffer_info_t data_info;
    mp_get_buffer_raise(args[ARG_data].u_obj, &data_info, MP_BUFFER_READ);

    const uint16_t *palette = NULL;
    mp_obj_t palette_obj = MP_OBJ_NULL;
    size_t pal_len = 0;
    if (format == PICOGAME_FMT_PAL8) {
        if (args[ARG_palette].u_obj == mp_const_none) {
            mp_raise_ValueError(MP_ERROR_TEXT("PAL8 needs a palette"));
        }
        mp_buffer_info_t pal_info;
        mp_get_buffer_raise(args[ARG_palette].u_obj, &pal_info, MP_BUFFER_READ);
        palette = pal_info.buf;
        palette_obj = args[ARG_palette].u_obj;
        pal_len = pal_info.len;
    }

    size_t bpp = (format == PICOGAME_FMT_PAL8) ? 1 : 2;
    // 64-bit: stride*height*bpp can exceed 32 bits (stride,height <= 65535) and wrap small in a 32-bit
    // size_t, letting a tiny buffer pass this check -> OOB read in the blitter. Compute + compare wide.
    uint64_t need = (uint64_t)stride * (uint64_t)height * (uint64_t)bpp;
    if ((uint64_t)data_info.len < need) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer too small"));
    }
    if (format == PICOGAME_FMT_PAL8 && pal_len < 2) {
        // Need >=1 entry so pal[0] is valid. Contract (see blitter): PAL8 indices MUST be < palette
        // length. The caller may write indices into the (mutable) `data` buffer directly, so we cannot
        // validate once here. An out-of-range index is undefined behaviour (reads past the palette:
        // usually a garbage colour, but it may fault on some platforms). Per-pixel clamping was dropped
        // for speed - see the "restore full bounds-safety" note in the PAL8 blit loop if it's needed.
        mp_raise_ValueError(MP_ERROR_TEXT("palette is empty"));
    }

    picogame_bitmap_obj_t *self = mp_obj_malloc(picogame_bitmap_obj_t, type);
    self->data_obj = args[ARG_data].u_obj;
    self->palette_obj = palette_obj;
    self->data = data_info.buf;
    self->palette = palette;
    self->width = width;
    self->height = height;
    self->stride = stride;
    self->frames = frames;
    self->format = format;
    // palette length in entries (informational; blitter assumes indices < this - see blit contract).
    self->pal_entries = (uint16_t)((pal_len / 2) > 65535 ? 65535 : (pal_len / 2));
    if (args[ARG_transparent].u_obj != mp_const_none) {
        self->transparent = mp_obj_get_int(args[ARG_transparent].u_obj);
        self->has_transparent = true;
    } else {
        self->transparent = 0;
        self->has_transparent = false;
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     width: int
//|     height: int
//|     frames: int
//|     """Frame dimensions and frame count (read-only)."""
static mp_obj_t bitmap_get_width(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_bitmap_obj_t *)MP_OBJ_TO_PTR(self_in))->width);
}
static MP_DEFINE_CONST_FUN_OBJ_1(bitmap_get_width_obj, bitmap_get_width);
MP_PROPERTY_GETTER(bitmap_width_obj, (mp_obj_t)&bitmap_get_width_obj);

static mp_obj_t bitmap_get_height(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_bitmap_obj_t *)MP_OBJ_TO_PTR(self_in))->height);
}
static MP_DEFINE_CONST_FUN_OBJ_1(bitmap_get_height_obj, bitmap_get_height);
MP_PROPERTY_GETTER(bitmap_height_obj, (mp_obj_t)&bitmap_get_height_obj);

static mp_obj_t bitmap_get_frames(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_bitmap_obj_t *)MP_OBJ_TO_PTR(self_in))->frames);
}
static MP_DEFINE_CONST_FUN_OBJ_1(bitmap_get_frames_obj, bitmap_get_frames);
MP_PROPERTY_GETTER(bitmap_frames_obj, (mp_obj_t)&bitmap_get_frames_obj);

//|     format: int
//|     """RGB565 or PAL8 (read-only)."""
static mp_obj_t bitmap_get_format(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_bitmap_obj_t *)MP_OBJ_TO_PTR(self_in))->format);
}
static MP_DEFINE_CONST_FUN_OBJ_1(bitmap_get_format_obj, bitmap_get_format);
MP_PROPERTY_GETTER(bitmap_format_obj, (mp_obj_t)&bitmap_get_format_obj);

//|     stride: int
//|     """Row stride in pixels (read-only)."""
static mp_obj_t bitmap_get_stride(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_bitmap_obj_t *)MP_OBJ_TO_PTR(self_in))->stride);
}
static MP_DEFINE_CONST_FUN_OBJ_1(bitmap_get_stride_obj, bitmap_get_stride);
MP_PROPERTY_GETTER(bitmap_stride_obj, (mp_obj_t)&bitmap_get_stride_obj);

//|     palette: Optional[ReadableBuffer]
//|     """The PAL8 palette buffer this Bitmap was built with, or None for RGB565
//|     (read-only). Lets palette helpers read it back instead of holding a sidecar ref."""
static mp_obj_t bitmap_get_palette(mp_obj_t self_in) {
    picogame_bitmap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return (self->palette_obj == MP_OBJ_NULL) ? mp_const_none : self->palette_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bitmap_get_palette_obj, bitmap_get_palette);
MP_PROPERTY_GETTER(bitmap_palette_obj, (mp_obj_t)&bitmap_get_palette_obj);

//|     transparent: Optional[int]
//|     """The transparent color/index, or None if the Bitmap is fully opaque (read-only)."""
static mp_obj_t bitmap_get_transparent(mp_obj_t self_in) {
    picogame_bitmap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return self->has_transparent ? MP_OBJ_NEW_SMALL_INT(self->transparent) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bitmap_get_transparent_obj, bitmap_get_transparent);
MP_PROPERTY_GETTER(bitmap_transparent_obj, (mp_obj_t)&bitmap_get_transparent_obj);

static const mp_rom_map_elem_t picogame_bitmap_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_width), MP_ROM_PTR(&bitmap_width_obj) },
    { MP_ROM_QSTR(MP_QSTR_height), MP_ROM_PTR(&bitmap_height_obj) },
    { MP_ROM_QSTR(MP_QSTR_frames), MP_ROM_PTR(&bitmap_frames_obj) },
    { MP_ROM_QSTR(MP_QSTR_format), MP_ROM_PTR(&bitmap_format_obj) },
    { MP_ROM_QSTR(MP_QSTR_stride), MP_ROM_PTR(&bitmap_stride_obj) },
    { MP_ROM_QSTR(MP_QSTR_palette), MP_ROM_PTR(&bitmap_palette_obj) },
    { MP_ROM_QSTR(MP_QSTR_transparent), MP_ROM_PTR(&bitmap_transparent_obj) },
};
static MP_DEFINE_CONST_DICT(picogame_bitmap_locals_dict, picogame_bitmap_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    picogame_bitmap_type,
    MP_QSTR_Bitmap,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, picogame_bitmap_make_new,
    locals_dict, &picogame_bitmap_locals_dict
    );

// ---------------------------------------------------------------------------
// Sprite
// ---------------------------------------------------------------------------

//| class Sprite:
//|     """A positioned, animatable instance of a :py:class:`Bitmap`."""
//|
//|     def __init__(
//|         self,
//|         bitmap: Bitmap,
//|         x: int = 0,
//|         y: int = 0,
//|         *,
//|         frame: int = 0,
//|         visible: bool = True,
//|         flip_x: bool = False,
//|         flip_y: bool = False,
//|     ) -> None: ...
static mp_obj_t picogame_sprite_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bitmap, ARG_x, ARG_y, ARG_frame, ARG_visible, ARG_flip_x, ARG_flip_y };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bitmap, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_x, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_y, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_frame, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_visible, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
        { MP_QSTR_flip_x, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_flip_y, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t bitmap_obj = mp_arg_validate_type(args[ARG_bitmap].u_obj, &picogame_bitmap_type, MP_QSTR_bitmap);

    picogame_sprite_obj_t *self = mp_obj_malloc(picogame_sprite_obj_t, type);
    self->bitmap = MP_OBJ_TO_PTR(bitmap_obj);
    self->x = pg_int_to_fp8(args[ARG_x].u_int);   // pixel -> 24.8 fixed-point (overflow-clamped)
    self->y = pg_int_to_fp8(args[ARG_y].u_int);
    self->frame = args[ARG_frame].u_int;
    self->flags = (args[ARG_visible].u_bool ? PICOGAME_SPR_VISIBLE : 0)
        | (args[ARG_flip_x].u_bool ? PICOGAME_SPR_FLIP_X : 0)
        | (args[ARG_flip_y].u_bool ? PICOGAME_SPR_FLIP_Y : 0);
    self->anchor_x = 0;   // default pivot = top-left (0, 0)
    self->anchor_y = 0;
    self->scale = 256;    // 8.8 fixed-point: 256 = 1.0x (no scaling)
    self->angle = 0;      // no rotation -> axis-aligned fast path
    self->flash_color = 0;
    self->dither = 0;
    self->data = mp_const_none;
    self->seq = 0;            // dirty-rect change counter (mp_obj_malloc zeroes anyway; explicit)
    return MP_OBJ_FROM_PTR(self);
}

static void set_flag(picogame_sprite_obj_t *self, uint8_t flag, bool on) {
    if (on) {
        self->flags |= flag;
    } else {
        self->flags &= ~flag;
    }
}

// shadow / flash / dither are mutually exclusive (one blit effect at a time); setting one ON
// clears the others, so "the last effect you set wins". Turning one OFF clears ONLY its own flag,
// so clearing an effect you never enabled (e.g. spr.flash = 0) can't wipe a different active one.
#define PICOGAME_SPR_FX_MASK (PICOGAME_SPR_SHADOW | PICOGAME_SPR_FLASH | PICOGAME_SPR_DITHER | PICOGAME_SPR_TINT)
static void set_effect(picogame_sprite_obj_t *self, uint8_t flag, bool on) {
    if (on) {
        self->flags = (uint8_t)((self->flags & ~PICOGAME_SPR_FX_MASK) | flag);
    } else {
        self->flags &= (uint8_t) ~flag;
    }
}

// Round a float to 24.8 fixed-point (shared by the position/scale/anchor setters so the soft-float
// round sequence is emitted once, not per call site).
static int32_t pg_round_fp8(mp_float_t f) {
    return (int32_t)(f * 256 + (f >= 0 ? (mp_float_t)0.5 : (mp_float_t)-0.5));
}

// Cast a (possibly subclassed) BusDisplay arg to its native object, raising if it isn't one.
// Also accepts the pg.Display fast-DMA wrapper (unwrapped to its underlying busdisplay - the
// portable send path): any handle that identifies the panel works wherever a display is
// expected, so the same object a Scene renders through also works for render()/invert().
// Without this, code holding the wrapper (custom setup, rgb444) worked on ports WITHOUT the
// fast backend and TypeError'd on ports WITH it.
static busdisplay_busdisplay_obj_t *pg_get_display(mp_obj_t obj) {
    #if CIRCUITPY_PICOGAME_FAST_DISPLAY
    if (mp_obj_is_type(obj, &picogame_display_type)) {
        return ((picogame_display_obj_t *)MP_OBJ_TO_PTR(obj))->display;
    }
    #endif
    mp_obj_t native = mp_obj_cast_to_native_base(obj, &busdisplay_busdisplay_type);
    if (!mp_obj_is_type(native, &busdisplay_busdisplay_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected a BusDisplay"));
    }
    return MP_OBJ_TO_PTR(native);
}

// Accept an int or float and store as 24.8 fixed-point (rounded). Integer fast path avoids
// software float on RP2040 - game code sets x/y (via move/setters) every frame, usually with ints.
static int32_t obj_to_fp(mp_obj_t o) {
    if (mp_obj_is_int(o)) {
        return pg_int_to_fp8(mp_obj_get_int(o));
    }
    mp_float_t f = mp_obj_get_float(o);
    return pg_round_fp8(f);
}

//|     x: int
//|     y: int
//|     """Integer pixel position (scene coords). Setting accepts a float for
//|     sub-pixel placement; reading returns the floored pixel."""
//|     fx: float
//|     fy: float
//|     """Sub-pixel position (use for smooth physics: e.g. ``sprite.fx += 2.4``)."""
// FLASH NOTE (property objects vs. a single `attr` handler): Sprite (and the other property-dense
// types) expose each attribute as its own getter/setter + MP_PROPERTY object below - the standard
// CircuitPython shared-bindings idiom (~423 such uses across CP). Collapsing these onto ONE `attr`
// load/store function per type (a qstr switch, like py/objcomplex.c's complex_attr for .real/.imag)
// would save ~3-4 KB of flash across the whole binding layer (Sprite alone ~1.5-2.5 KB). It is a
// valid MicroPython mechanism and would NOT break the .pyi stubs (those are generated from the //|
// comments, not the C property objects). We deliberately KEEP property objects: they are the
// idiomatic, most readable shared-bindings form and keep every type's definition uniform. Revisit
// only if flash becomes critical (then convert Sprite first - the densest cluster - not every type).
static mp_obj_t sprite_get_x(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->x >> 8);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_x_obj, sprite_get_x);
static mp_obj_t sprite_set_x(mp_obj_t self_in, mp_obj_t v) {
    ((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->x = obj_to_fp(v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_x_obj, sprite_set_x);
MP_PROPERTY_GETSET(sprite_x_obj, (mp_obj_t)&sprite_get_x_obj, (mp_obj_t)&sprite_set_x_obj);

static mp_obj_t sprite_get_y(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->y >> 8);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_y_obj, sprite_get_y);
static mp_obj_t sprite_set_y(mp_obj_t self_in, mp_obj_t v) {
    ((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->y = obj_to_fp(v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_y_obj, sprite_set_y);
MP_PROPERTY_GETSET(sprite_y_obj, (mp_obj_t)&sprite_get_y_obj, (mp_obj_t)&sprite_set_y_obj);

static mp_obj_t sprite_get_fx(mp_obj_t self_in) {
    return mp_obj_new_float(((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->x * (mp_float_t)(1.0 / 256.0));
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_fx_obj, sprite_get_fx);
// set_fx is byte-identical to set_x (both store obj_to_fp(v) into ->x) -> reuse set_x's fun obj.
MP_PROPERTY_GETSET(sprite_fx_obj, (mp_obj_t)&sprite_get_fx_obj, (mp_obj_t)&sprite_set_x_obj);

static mp_obj_t sprite_get_fy(mp_obj_t self_in) {
    return mp_obj_new_float(((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->y * (mp_float_t)(1.0 / 256.0));
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_fy_obj, sprite_get_fy);
// set_fy is byte-identical to set_y -> reuse set_y's fun obj.
MP_PROPERTY_GETSET(sprite_fy_obj, (mp_obj_t)&sprite_get_fy_obj, (mp_obj_t)&sprite_set_y_obj);

static mp_obj_t sprite_get_frame(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->frame);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_frame_obj, sprite_get_frame);
static mp_obj_t sprite_set_frame(mp_obj_t self_in, mp_obj_t v) {
    ((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->frame = mp_obj_get_int(v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_frame_obj, sprite_set_frame);
MP_PROPERTY_GETSET(sprite_frame_obj, (mp_obj_t)&sprite_get_frame_obj, (mp_obj_t)&sprite_set_frame_obj);

static mp_obj_t sprite_get_visible(mp_obj_t self_in) {
    return mp_obj_new_bool((((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->flags & PICOGAME_SPR_VISIBLE) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_visible_obj, sprite_get_visible);
static mp_obj_t sprite_set_visible(mp_obj_t self_in, mp_obj_t v) {
    set_flag(MP_OBJ_TO_PTR(self_in), PICOGAME_SPR_VISIBLE, mp_obj_is_true(v));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_visible_obj, sprite_set_visible);
MP_PROPERTY_GETSET(sprite_visible_obj, (mp_obj_t)&sprite_get_visible_obj, (mp_obj_t)&sprite_set_visible_obj);

static mp_obj_t sprite_get_flip_x(mp_obj_t self_in) {
    return mp_obj_new_bool((((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->flags & PICOGAME_SPR_FLIP_X) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_flip_x_obj, sprite_get_flip_x);
static mp_obj_t sprite_set_flip_x(mp_obj_t self_in, mp_obj_t v) {
    set_flag(MP_OBJ_TO_PTR(self_in), PICOGAME_SPR_FLIP_X, mp_obj_is_true(v));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_flip_x_obj, sprite_set_flip_x);
MP_PROPERTY_GETSET(sprite_flip_x_obj, (mp_obj_t)&sprite_get_flip_x_obj, (mp_obj_t)&sprite_set_flip_x_obj);

static mp_obj_t sprite_get_flip_y(mp_obj_t self_in) {
    return mp_obj_new_bool((((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->flags & PICOGAME_SPR_FLIP_Y) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_flip_y_obj, sprite_get_flip_y);
static mp_obj_t sprite_set_flip_y(mp_obj_t self_in, mp_obj_t v) {
    set_flag(MP_OBJ_TO_PTR(self_in), PICOGAME_SPR_FLIP_Y, mp_obj_is_true(v));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_flip_y_obj, sprite_set_flip_y);
MP_PROPERTY_GETSET(sprite_flip_y_obj, (mp_obj_t)&sprite_get_flip_y_obj, (mp_obj_t)&sprite_set_flip_y_obj);

//|     scale: float
//|     """Uniform draw scale (nearest-neighbour). 1.0 = native (fast path); 2.0 = double
//|     size, fractional values are allowed (e.g. a powerup grow tween). Anchor stays put."""
static mp_obj_t sprite_get_scale(mp_obj_t self_in) {
    return mp_obj_new_float(((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->scale * (mp_float_t)(1.0 / 256.0));
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_scale_obj, sprite_get_scale);
static mp_obj_t sprite_set_scale(mp_obj_t self_in, mp_obj_t v) {
    int q;
    if (mp_obj_is_int(v)) {                          // int fast path (no software float on RP2040)
        q = pg_int_to_fp8(mp_obj_get_int(v));        // overflow-clamped <<8
    } else {
        mp_float_t f = mp_obj_get_float(v);
        q = pg_round_fp8(f);
    }
    if (q < 1) {
        q = 1;
    }
    if (q > 65535) {
        q = 65535;
    }
    picogame_sprite_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->scale = (uint16_t)q;
    self->xf_valid = 0;                              // affine cache depends on scale
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_scale_obj, sprite_set_scale);
MP_PROPERTY_GETSET(sprite_scale_obj, (mp_obj_t)&sprite_get_scale_obj, (mp_obj_t)&sprite_set_scale_obj);

//|     angle: float
//|     """Rotation in degrees about the anchor (0 = none, the fast path). Nearest-neighbour,
//|     so integer scales stay crisp; rotation shimmers slightly (pixel-art trade-off)."""
static mp_obj_t sprite_get_angle(mp_obj_t self_in) {
    return mp_obj_new_float((mp_float_t)((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->angle);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_angle_obj, sprite_get_angle);
static mp_obj_t sprite_set_angle(mp_obj_t self_in, mp_obj_t v) {
    int a;
    if (mp_obj_is_int(v)) {                          // int fast path (no software float on RP2040)
        a = mp_obj_get_int(v);
    } else {
        mp_float_t f = mp_obj_get_float(v);
        a = (int)(f + (f >= 0 ? (mp_float_t)0.5 : (mp_float_t)-0.5));
    }
    a %= 360;
    picogame_sprite_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->angle = (int16_t)a;
    self->xf_valid = 0;                              // affine cache depends on angle
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_angle_obj, sprite_set_angle);
MP_PROPERTY_GETSET(sprite_angle_obj, (mp_obj_t)&sprite_get_angle_obj, (mp_obj_t)&sprite_set_angle_obj);

//|     shadow: bool
//|     """Draw opaque pixels as a darkened destination instead of colour - a drop-shadow
//|     silhouette (offset copy below the sprite) or a dim overlay (a solid sprite scaled
//|     over a dialog/pause area)."""
static mp_obj_t sprite_get_shadow(mp_obj_t self_in) {
    return mp_obj_new_bool((((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->flags & PICOGAME_SPR_SHADOW) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_shadow_obj, sprite_get_shadow);
static mp_obj_t sprite_set_shadow(mp_obj_t self_in, mp_obj_t v) {
    set_effect(MP_OBJ_TO_PTR(self_in), PICOGAME_SPR_SHADOW, mp_obj_is_true(v));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_shadow_obj, sprite_set_shadow);
MP_PROPERTY_GETSET(sprite_shadow_obj, (mp_obj_t)&sprite_get_shadow_obj, (mp_obj_t)&sprite_set_shadow_obj);

//|     flash: int
//|     """Draw opaque pixels as a solid colour (a wire-order RGB565 int from rgb565) instead
//|     of their own colour - a hit-flash or tint. Set to a colour to enable, 0/False to turn
//|     off. Pulse it for 1-3 frames on impact. Mutually exclusive with shadow/dither."""
static mp_obj_t sprite_get_flash(mp_obj_t self_in) {
    picogame_sprite_obj_t *s = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT((s->flags & PICOGAME_SPR_FLASH) ? s->flash_color : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_flash_obj, sprite_get_flash);
static mp_obj_t sprite_set_flash(mp_obj_t self_in, mp_obj_t v) {
    picogame_sprite_obj_t *s = MP_OBJ_TO_PTR(self_in);
    // Falsy (None / False / 0) turns flash OFF. A non-zero colour enables it. (You can't flash
    // pure black - use a near-black colour if you ever need that; off is the common case.)
    if (!mp_obj_is_true(v)) {
        set_effect(s, PICOGAME_SPR_FLASH, false);
    } else {
        s->flash_color = (uint16_t)mp_obj_get_int(v);
        set_effect(s, PICOGAME_SPR_FLASH, true);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_flash_obj, sprite_set_flash);
MP_PROPERTY_GETSET(sprite_flash_obj, (mp_obj_t)&sprite_get_flash_obj, (mp_obj_t)&sprite_set_flash_obj);

//|     dither: int
//|     """Fake transparency via an ordered (Bayer) dither, no alpha blending: 0 = opaque
//|     (off), 8 = ~50% see-through, 16 = invisible. A classic 1-bit look - for ghosts,
//|     fading/spawning enemies, fog, force fields. Mutually exclusive with shadow/flash."""
static mp_obj_t sprite_get_dither(mp_obj_t self_in) {
    picogame_sprite_obj_t *s = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT((s->flags & PICOGAME_SPR_DITHER) ? s->dither : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_dither_obj, sprite_get_dither);
static mp_obj_t sprite_set_dither(mp_obj_t self_in, mp_obj_t v) {
    picogame_sprite_obj_t *s = MP_OBJ_TO_PTR(self_in);
    int lv = mp_obj_get_int(v);
    if (lv < 0) {
        lv = 0;
    }
    if (lv > 16) {
        lv = 16;
    }
    s->dither = (uint8_t)lv;
    set_effect(s, PICOGAME_SPR_DITHER, lv > 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_dither_obj, sprite_set_dither);
MP_PROPERTY_GETSET(sprite_dither_obj, (mp_obj_t)&sprite_get_dither_obj, (mp_obj_t)&sprite_set_dither_obj);

//|     tint: int
//|     """Multiply opaque pixels by a colour (wire-order RGB565 from rgb565), keeping the
//|     sprite's shading - coloured lighting, a red damage flush, a blue freeze, a power-up
//|     glow. Unlike `flash` (flat replace) `tint` preserves detail. 0/False = off. Mutually
//|     exclusive with shadow/flash/dither."""
static mp_obj_t sprite_get_tint(mp_obj_t self_in) {
    picogame_sprite_obj_t *s = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT((s->flags & PICOGAME_SPR_TINT) ? s->flash_color : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_tint_obj, sprite_get_tint);
static mp_obj_t sprite_set_tint(mp_obj_t self_in, mp_obj_t v) {
    picogame_sprite_obj_t *s = MP_OBJ_TO_PTR(self_in);
    if (!mp_obj_is_true(v)) {
        set_effect(s, PICOGAME_SPR_TINT, false);
    } else {
        s->flash_color = (uint16_t)mp_obj_get_int(v);   // shared colour field with flash
        set_effect(s, PICOGAME_SPR_TINT, true);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_tint_obj, sprite_set_tint);
MP_PROPERTY_GETSET(sprite_tint_obj, (mp_obj_t)&sprite_get_tint_obj, (mp_obj_t)&sprite_set_tint_obj);

//|     transpose: bool
//|     """Swap the sprite's X/Y axes - a cheap 90deg turn (no shimmer, unlike `angle`).
//|     Combined with `flip_x`/`flip_y` it gives all 8 orientations for free. Only on the fast
//|     path (scale 1.0, angle 0); for rotation WITH scaling use `angle`. The drawn footprint
//|     swaps width/height."""
static mp_obj_t sprite_get_transpose(mp_obj_t self_in) {
    return mp_obj_new_bool((((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->flags & PICOGAME_SPR_TRANSPOSE) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_transpose_obj, sprite_get_transpose);
static mp_obj_t sprite_set_transpose(mp_obj_t self_in, mp_obj_t v) {
    set_flag(MP_OBJ_TO_PTR(self_in), PICOGAME_SPR_TRANSPOSE, mp_obj_is_true(v));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_transpose_obj, sprite_set_transpose);
MP_PROPERTY_GETSET(sprite_transpose_obj, (mp_obj_t)&sprite_get_transpose_obj, (mp_obj_t)&sprite_set_transpose_obj);

//|     data: Any
//|     """Arbitrary per-sprite user payload for game state (default None)."""
static mp_obj_t sprite_get_data(mp_obj_t self_in) {
    return ((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->data;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_data_obj, sprite_get_data);
static mp_obj_t sprite_set_data(mp_obj_t self_in, mp_obj_t v) {
    ((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->data = v;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_data_obj, sprite_set_data);
MP_PROPERTY_GETSET(sprite_data_obj, (mp_obj_t)&sprite_get_data_obj, (mp_obj_t)&sprite_set_data_obj);

//|     bitmap: Bitmap
//|     """The sprite's source bitmap. Assigning a new one swaps graphics and may
//|     change size; the scene repaints both the old and new bounds next refresh
//|     (e.g. powerups, resizable HUD bars, text labels)."""
static mp_obj_t sprite_get_bitmap(mp_obj_t self_in) {
    picogame_sprite_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return self->bitmap != NULL ? MP_OBJ_FROM_PTR(self->bitmap) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_bitmap_obj, sprite_get_bitmap);
static mp_obj_t sprite_set_bitmap(mp_obj_t self_in, mp_obj_t v) {
    picogame_sprite_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t bm = mp_arg_validate_type(v, &picogame_bitmap_type, MP_QSTR_bitmap);
    self->bitmap = MP_OBJ_TO_PTR(bm);
    self->xf_valid = 0;                              // affine cache depends on bitmap dims
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_bitmap_obj, sprite_set_bitmap);
MP_PROPERTY_GETSET(sprite_bitmap_obj, (mp_obj_t)&sprite_get_bitmap_obj, (mp_obj_t)&sprite_set_bitmap_obj);

//|     anchor: Tuple[float, float]
//|     """Pivot as fractions of the bitmap size: ``(0, 0)`` = top-left (default),
//|     ``(0.5, 0.5)`` = center, ``(0.5, 1.0)`` = bottom-center. ``x``/``y`` then
//|     refer to this point, so rotating frames or swapping to a different size
//|     stays aligned. Stored in 1/256 steps."""
static mp_obj_t sprite_get_anchor(mp_obj_t self_in) {
    picogame_sprite_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t t[2] = {
        mp_obj_new_float(self->anchor_x * (mp_float_t)(1.0 / 256.0)),
        mp_obj_new_float(self->anchor_y * (mp_float_t)(1.0 / 256.0)),
    };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_get_anchor_obj, sprite_get_anchor);
static int anchor_to_fp(mp_obj_t o) {
    mp_float_t f = mp_obj_get_float(o);
    int v = pg_round_fp8(f);
    return v < 0 ? 0 : (v > 256 ? 256 : v);
}
static mp_obj_t sprite_set_anchor(mp_obj_t self_in, mp_obj_t v) {
    picogame_sprite_obj_t *self = MP_OBJ_TO_PTR(self_in);
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(v, &len, &items);
    if (len != 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("anchor must be (x, y)"));
    }
    self->anchor_x = anchor_to_fp(items[0]);
    self->anchor_y = anchor_to_fp(items[1]);
    self->xf_valid = 0;                              // affine cache depends on the pivot
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sprite_set_anchor_obj, sprite_set_anchor);
MP_PROPERTY_GETSET(sprite_anchor_obj, (mp_obj_t)&sprite_get_anchor_obj, (mp_obj_t)&sprite_set_anchor_obj);

//|     def move(self, x: int, y: int) -> None:
//|         """Set the sprite position."""
//|         ...
static mp_obj_t sprite_move(mp_obj_t self_in, mp_obj_t x_in, mp_obj_t y_in) {
    picogame_sprite_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->x = obj_to_fp(x_in);
    self->y = obj_to_fp(y_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(sprite_move_obj, sprite_move);

//|     def touch(self) -> None:
//|         """Force this sprite to repaint on the next ``Scene.refresh()`` even though none
//|         of its tracked properties (position, frame, scale, angle, bitmap) changed. Call
//|         it after mutating the sprite's bitmap pixels IN PLACE (e.g. streaming a new frame
//|         into the same buffer), which the dirty-rect tracker can't otherwise detect."""
//|         ...
static mp_obj_t sprite_touch(mp_obj_t self_in) {
    ((picogame_sprite_obj_t *)MP_OBJ_TO_PTR(self_in))->seq++;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sprite_touch_obj, sprite_touch);

// Fill (x1,y1,x2,y2) from a Sprite (its drawn aabb), a (x,y) point, or a (x1,y1,x2,y2) rect.
static void pg_obj_to_box(mp_obj_t o, int *x1, int *y1, int *x2, int *y2) {
    if (mp_obj_is_type(o, &picogame_sprite_type)) {
        picogame_sprite_aabb(MP_OBJ_TO_PTR(o), x1, y1, x2, y2);
        return;
    }
    if (mp_obj_is_type(o, &mp_type_tuple) || mp_obj_is_type(o, &mp_type_list)) {
        size_t len;
        mp_obj_t *items;
        mp_obj_get_array(o, &len, &items);
        if (len == 2) {                       // a point -> a zero-size box
            *x1 = *x2 = mp_obj_get_int(items[0]);
            *y1 = *y2 = mp_obj_get_int(items[1]);
            return;
        }
        if (len == 4) {                       // a rect
            *x1 = mp_obj_get_int(items[0]);
            *y1 = mp_obj_get_int(items[1]);
            *x2 = mp_obj_get_int(items[2]);
            *y2 = mp_obj_get_int(items[3]);
            return;
        }
    }
    mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("%q must be of type %q or %q, not %q"),
        MP_QSTR_other, MP_QSTR_Sprite, MP_QSTR_tuple, mp_obj_get_type(o)->name);
}

//|     def overlaps(self, other: "Sprite | tuple", inset: int = 0) -> bool:
//|         """True if this sprite's drawn box overlaps `other` - an inclusive AABB, so they
//|         collide the moment they touch. `other` may be another Sprite, a point `(x, y)`,
//|         or a rect `(x1, y1, x2, y2)` (e.g. a trigger zone or the screen for culling).
//|         The box is anchor/scale/rotation aware. `inset` shrinks THIS sprite's box by N px
//|         on each side, for a fair hitbox smaller than the art."""
//|         ...
static mp_obj_t sprite_overlaps(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_other, ARG_inset };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_other, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_inset, MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    int ax1, ay1, ax2, ay2, bx1, by1, bx2, by2;
    picogame_sprite_aabb(MP_OBJ_TO_PTR(pos_args[0]), &ax1, &ay1, &ax2, &ay2);
    pg_obj_to_box(args[ARG_other].u_obj, &bx1, &by1, &bx2, &by2);
    int in = args[ARG_inset].u_int;                        // inset shrinks the CALLER's box (kw or positional)
    bool hit = ((ax1 + in) <= bx2) && ((ax2 - in) >= bx1) &&
        ((ay1 + in) <= by2) && ((ay2 - in) >= by1);
    return mp_obj_new_bool(hit);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(sprite_overlaps_obj, 2, sprite_overlaps);

//|     def near(self, other: "Sprite | tuple", r: int) -> bool:
//|         """True if this sprite's centre is within `r` pixels of `other`'s centre (squared
//|         distance, no sqrt) - the round/forgiving test for bullets, pickups, explosions.
//|         `other` may be a Sprite or a point `(x, y)`. Centres come from the drawn box, so
//|         it is anchor aware."""
//|         ...
static mp_obj_t sprite_near(mp_obj_t self_in, mp_obj_t other_in, mp_obj_t r_in) {
    int ax1, ay1, ax2, ay2;
    picogame_sprite_aabb(MP_OBJ_TO_PTR(self_in), &ax1, &ay1, &ax2, &ay2);
    int acx = (ax1 + ax2) / 2, acy = (ay1 + ay2) / 2, bcx, bcy;
    if (mp_obj_is_type(other_in, &picogame_sprite_type)) {
        int bx1, by1, bx2, by2;
        picogame_sprite_aabb(MP_OBJ_TO_PTR(other_in), &bx1, &by1, &bx2, &by2);
        bcx = (bx1 + bx2) / 2;
        bcy = (by1 + by2) / 2;
    } else {
        size_t len;
        mp_obj_t *items;
        mp_obj_get_array(other_in, &len, &items);
        if (len != 2) {
            mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("%q must be of type %q or %q, not %q"),
                MP_QSTR_other, MP_QSTR_Sprite, MP_QSTR_tuple, mp_obj_get_type(other_in)->name);
        }
        bcx = mp_obj_get_int(items[0]);
        bcy = mp_obj_get_int(items[1]);
    }
    mp_int_t r = mp_obj_get_int(r_in), dx = acx - bcx, dy = acy - bcy;
    return mp_obj_new_bool(dx * dx + dy * dy < r * r);
}
static MP_DEFINE_CONST_FUN_OBJ_3(sprite_near_obj, sprite_near);

static const mp_rom_map_elem_t picogame_sprite_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_x), MP_ROM_PTR(&sprite_x_obj) },
    { MP_ROM_QSTR(MP_QSTR_y), MP_ROM_PTR(&sprite_y_obj) },
    { MP_ROM_QSTR(MP_QSTR_fx), MP_ROM_PTR(&sprite_fx_obj) },
    { MP_ROM_QSTR(MP_QSTR_fy), MP_ROM_PTR(&sprite_fy_obj) },
    { MP_ROM_QSTR(MP_QSTR_frame), MP_ROM_PTR(&sprite_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_visible), MP_ROM_PTR(&sprite_visible_obj) },
    { MP_ROM_QSTR(MP_QSTR_flip_x), MP_ROM_PTR(&sprite_flip_x_obj) },
    { MP_ROM_QSTR(MP_QSTR_flip_y), MP_ROM_PTR(&sprite_flip_y_obj) },
    { MP_ROM_QSTR(MP_QSTR_scale), MP_ROM_PTR(&sprite_scale_obj) },
    { MP_ROM_QSTR(MP_QSTR_angle), MP_ROM_PTR(&sprite_angle_obj) },
    { MP_ROM_QSTR(MP_QSTR_shadow), MP_ROM_PTR(&sprite_shadow_obj) },
    { MP_ROM_QSTR(MP_QSTR_flash), MP_ROM_PTR(&sprite_flash_obj) },
    { MP_ROM_QSTR(MP_QSTR_dither), MP_ROM_PTR(&sprite_dither_obj) },
    { MP_ROM_QSTR(MP_QSTR_tint), MP_ROM_PTR(&sprite_tint_obj) },
    { MP_ROM_QSTR(MP_QSTR_transpose), MP_ROM_PTR(&sprite_transpose_obj) },
    { MP_ROM_QSTR(MP_QSTR_data), MP_ROM_PTR(&sprite_data_obj) },
    { MP_ROM_QSTR(MP_QSTR_bitmap), MP_ROM_PTR(&sprite_bitmap_obj) },
    { MP_ROM_QSTR(MP_QSTR_anchor), MP_ROM_PTR(&sprite_anchor_obj) },
    { MP_ROM_QSTR(MP_QSTR_overlaps), MP_ROM_PTR(&sprite_overlaps_obj) },
    { MP_ROM_QSTR(MP_QSTR_near), MP_ROM_PTR(&sprite_near_obj) },
    { MP_ROM_QSTR(MP_QSTR_move), MP_ROM_PTR(&sprite_move_obj) },
    { MP_ROM_QSTR(MP_QSTR_touch), MP_ROM_PTR(&sprite_touch_obj) },
};
static MP_DEFINE_CONST_DICT(picogame_sprite_locals_dict, picogame_sprite_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    picogame_sprite_type,
    MP_QSTR_Sprite,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, picogame_sprite_make_new,
    locals_dict, &picogame_sprite_locals_dict
    );

// ---------------------------------------------------------------------------
// StripDraw  (immediate-mode draw layer; struct in shared-module/picogame/__init__.h)
// ---------------------------------------------------------------------------

//| class StripDraw:
//|     """An immediate-mode draw layer that holds NO pixel buffer. Each refresh, for
//|     every render strip overlapping its rect, ``callback(view, vx, vy, vw, vh)`` is
//|     called with a :py:class:`Canvas` ``view`` pointing straight at the live strip
//|     buffer - so you draw primitives directly into the frame (zero RAM, vs a Canvas
//|     which costs width*height*2 bytes). The view's local (0, 0) is screen pixel
//|     (vx, vy); (vw, vh) is the strip size. Draw only the rows in [vy, vy+vh) for
//|     speed (anything outside the view is clipped anyway). The rect is repainted every
//|     frame, so use it for animated / scanline content (pseudo-3D, gradients,
//|     procedural backgrounds), not static art (use Canvas for that).
//|
//|     COORDINATE CONTRACT: ``vx`` is the RENDER REGION's origin (NOT this layer's x), and the
//|     view spans the FULL region WIDTH (the layer's rect only gates which ROWS run). So draw at
//|     ABSOLUTE screen coords minus (vx, vy), and fill only your own rect with ``fill_rect`` -
//|     ``view.clear()`` fills the whole region width. (When you render a StripDraw via
//|     ``picogame.render([sd], buf, x,y,x+w,y+h)`` the region == the rect, so vx == x.)
//|     Text via ``Canvas.text`` is ASCII (the built-in font); non-ASCII has no glyph."""
//|
//|     def __init__(
//|         self,
//|         callback: Callable[[Canvas, int, int, int, int], None],
//|         x: int = 0,
//|         y: int = 0,
//|         width: int = 0,
//|         height: int = 0,
//|     ) -> None: ...
static mp_obj_t picogame_stripdraw_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_callback, ARG_x, ARG_y, ARG_width, ARG_height, ARG_always_dirty };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_callback, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_x, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_y, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_width, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_height, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_always_dirty, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    picogame_stripdraw_obj_t *self = mp_obj_malloc(picogame_stripdraw_obj_t, type);
    self->callback = args[ARG_callback].u_obj;
    self->x = args[ARG_x].u_int;
    self->y = args[ARG_y].u_int;
    self->w = args[ARG_width].u_int;
    self->h = args[ARG_height].u_int;
    self->faulted = false;
    self->always_dirty = args[ARG_always_dirty].u_bool;
    picogame_dirty_reset(&self->dx1);        // render once on first refresh (even when always_dirty=False)
    picogame_dirty_union(&self->dx1, self->x, self->y, self->x + self->w, self->y + self->h);
    // A buffer-less Canvas reused as the per-strip drawing view: its `data` is
    // repointed at the live strip each blit, so no surface RAM is allocated here.
    picogame_canvas_obj_t *view = mp_obj_malloc(picogame_canvas_obj_t, &picogame_canvas_type);
    view->data = NULL;
    view->data_obj = MP_OBJ_NULL;
    view->w = 0;
    view->h = 0;
    view->x = 0;
    view->y = 0;
    view->transparent = 0;
    view->has_transparent = false;
    picogame_canvas_dirty_reset(view);
    self->view = MP_OBJ_FROM_PTR(view);
    return MP_OBJ_FROM_PTR(self);
}

//|     x: int
//|     y: int
//|     width: int
//|     height: int
//|     """The screen rect repainted each refresh (read/write). Move or resize the layer
//|     by assigning these. Shrinking the rect leaves stale pixels behind - follow a
//|     shrink with ``scene.invalidate()`` for a clean repaint (as the fx helpers do)."""
static mp_obj_t sd_get_x(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_stripdraw_obj_t *)MP_OBJ_TO_PTR(self_in))->x);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sd_get_x_obj, sd_get_x);
static mp_obj_t sd_set_x(mp_obj_t self_in, mp_obj_t v) {
    ((picogame_stripdraw_obj_t *)MP_OBJ_TO_PTR(self_in))->x = mp_obj_get_int(v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sd_set_x_obj, sd_set_x);
MP_PROPERTY_GETSET(sd_x_obj, (mp_obj_t)&sd_get_x_obj, (mp_obj_t)&sd_set_x_obj);

static mp_obj_t sd_get_y(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_stripdraw_obj_t *)MP_OBJ_TO_PTR(self_in))->y);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sd_get_y_obj, sd_get_y);
static mp_obj_t sd_set_y(mp_obj_t self_in, mp_obj_t v) {
    ((picogame_stripdraw_obj_t *)MP_OBJ_TO_PTR(self_in))->y = mp_obj_get_int(v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sd_set_y_obj, sd_set_y);
MP_PROPERTY_GETSET(sd_y_obj, (mp_obj_t)&sd_get_y_obj, (mp_obj_t)&sd_set_y_obj);

static mp_obj_t sd_get_width(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_stripdraw_obj_t *)MP_OBJ_TO_PTR(self_in))->w);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sd_get_width_obj, sd_get_width);
static mp_obj_t sd_set_width(mp_obj_t self_in, mp_obj_t v) {
    ((picogame_stripdraw_obj_t *)MP_OBJ_TO_PTR(self_in))->w = mp_obj_get_int(v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sd_set_width_obj, sd_set_width);
MP_PROPERTY_GETSET(sd_width_obj, (mp_obj_t)&sd_get_width_obj, (mp_obj_t)&sd_set_width_obj);

static mp_obj_t sd_get_height(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_stripdraw_obj_t *)MP_OBJ_TO_PTR(self_in))->h);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sd_get_height_obj, sd_get_height);
static mp_obj_t sd_set_height(mp_obj_t self_in, mp_obj_t v) {
    ((picogame_stripdraw_obj_t *)MP_OBJ_TO_PTR(self_in))->h = mp_obj_get_int(v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sd_set_height_obj, sd_set_height);
MP_PROPERTY_GETSET(sd_height_obj, (mp_obj_t)&sd_get_height_obj, (mp_obj_t)&sd_set_height_obj);

//|     always_dirty: bool
//|     """True (default): repaint every frame - for animated content (pseudo-3D, gradients). False:
//|     repaint only when ``invalidate()``d (or overlapped by another dirty layer) - for on-change UI,
//|     so a static panel doesn't re-rasterize+re-push every frame. With False you MUST invalidate() on
//|     every content/visibility change (it's invisible until you do)."""
static mp_obj_t sd_get_always_dirty(mp_obj_t self_in) {
    return mp_obj_new_bool(((picogame_stripdraw_obj_t *)MP_OBJ_TO_PTR(self_in))->always_dirty);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sd_get_always_dirty_obj, sd_get_always_dirty);
static mp_obj_t sd_set_always_dirty(mp_obj_t self_in, mp_obj_t v) {
    ((picogame_stripdraw_obj_t *)MP_OBJ_TO_PTR(self_in))->always_dirty = mp_obj_is_true(v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sd_set_always_dirty_obj, sd_set_always_dirty);
MP_PROPERTY_GETSET(sd_always_dirty_obj, (mp_obj_t)&sd_get_always_dirty_obj, (mp_obj_t)&sd_set_always_dirty_obj);

//|     def invalidate(self, x: int = 0, y: int = 0, w: int = 0, h: int = 0) -> None:
//|         """Mark dirty so the layer repaints on the next refresh (only needed when
//|         ``always_dirty=False``). With no args, the whole layer repaints. Pass a rect in
//|         VIEW-LOCAL coordinates (the same (0,0)-at-``(vx, vy)`` space the draw callback uses) to
//|         repaint only that region - like Canvas/Tilemap, the Scene then recomposites and pushes just
//|         those rows. Repeated calls union; the rect is clamped to the layer."""
static mp_obj_t sd_invalidate(size_t n_args, const mp_obj_t *args) {
    picogame_stripdraw_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (n_args >= 5) {                       // (x, y, w, h) in view-local coords -> clamped scene rect
        int x1 = self->x + mp_obj_get_int(args[1]);
        int y1 = self->y + mp_obj_get_int(args[2]);
        int x2 = x1 + mp_obj_get_int(args[3]);
        int y2 = y1 + mp_obj_get_int(args[4]);
        if (x1 < self->x) {
            x1 = self->x;
        }
        if (y1 < self->y) {
            y1 = self->y;
        }
        if (x2 > self->x + self->w) {
            x2 = self->x + self->w;
        }
        if (y2 > self->y + self->h) {
            y2 = self->y + self->h;
        }
        if (x2 > x1 && y2 > y1) {
            picogame_dirty_union(&self->dx1, x1, y1, x2, y2);
        }
    } else {                                 // whole layer
        picogame_dirty_union(&self->dx1, self->x, self->y, self->x + self->w, self->y + self->h);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sd_invalidate_obj, 1, 5, sd_invalidate);

static const mp_rom_map_elem_t picogame_stripdraw_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_x), MP_ROM_PTR(&sd_x_obj) },
    { MP_ROM_QSTR(MP_QSTR_y), MP_ROM_PTR(&sd_y_obj) },
    { MP_ROM_QSTR(MP_QSTR_width), MP_ROM_PTR(&sd_width_obj) },
    { MP_ROM_QSTR(MP_QSTR_height), MP_ROM_PTR(&sd_height_obj) },
    { MP_ROM_QSTR(MP_QSTR_always_dirty), MP_ROM_PTR(&sd_always_dirty_obj) },
    { MP_ROM_QSTR(MP_QSTR_invalidate), MP_ROM_PTR(&sd_invalidate_obj) },
};
static MP_DEFINE_CONST_DICT(picogame_stripdraw_locals_dict, picogame_stripdraw_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    picogame_stripdraw_type,
    MP_QSTR_StripDraw,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, picogame_stripdraw_make_new,
    locals_dict, &picogame_stripdraw_locals_dict
    );

// ---------------------------------------------------------------------------
// Module-level functions
// ---------------------------------------------------------------------------

//| """2D game engine for the PicoPad and similar boards.
//|
//| Draws arbitrary-size sprites (unlike ``_stage``'s fixed 16x16 tiles) to a
//| ``busdisplay`` through a reusable strip buffer, with a dirty-rect scene,
//| tilemaps, particles, a drawing canvas and camera/effects."""
//|
//| RGB565: int
//| """16-bit color bitmap format (wire byte order)."""
//| PAL8: int
//| """8-bit paletted bitmap format."""
//|
//| def rgb565(r: int, g: int, b: int) -> int:
//|     """Build a display wire-order RGB565 color from 8-bit components."""
//|     ...
static mp_obj_t picogame_rgb565(mp_obj_t r_in, mp_obj_t g_in, mp_obj_t b_in) {
    int r = mp_obj_get_int(r_in) & 0xff;
    int g = mp_obj_get_int(g_in) & 0xff;
    int b = mp_obj_get_int(b_in) & 0xff;
    uint16_t c = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
    uint16_t wire = (uint16_t)((c >> 8) | (c << 8));
    return MP_OBJ_NEW_SMALL_INT(wire);
}
static MP_DEFINE_CONST_FUN_OBJ_3(picogame_rgb565_obj, picogame_rgb565);

// raycast(map, mw, mh, posx, posy, lrx, lry, srx, sry, sh, stride, ncols, wcolors, top, bot, col, dist)
// C DDA wall raycaster for picogame_ray.Raycaster - INTEGER ONLY (16.16 fixed-point, no FPU; the paint,
// temporal invalidate, pose-cache and billboard math stay in Python; Python does the once-per-frame
// trig and passes Q16 ray params). map: read-only bytes, mw*mh wall types (0 = empty). pos*, l*x/l*y
// (leftRay, column 0), s*x/s*y (rayStep per column) are all 16.16. wcolors: uint16[(maxtype+1)*2] -
// [t*2] near, [t*2+1] side colour. top/bot/col: uint16 write buffers (len>=ncols); dist: int32 write
// buffer (perpendicular distance, 16.16). The int64 divides/muls are ONLY the per-column setup
// (O(ncols)); the DDA step loop is pure 32-bit. Mirrors the Python float fallback closely.
static mp_obj_t picogame_raycast(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t mi, wi, ti, bi, ci, di;
    mp_get_buffer_raise(args[0], &mi, MP_BUFFER_READ);
    int mw = mp_obj_get_int(args[1]);
    int mh = mp_obj_get_int(args[2]);
    int32_t posx = mp_obj_get_int(args[3]);        // camera x, 16.16
    int32_t posy = mp_obj_get_int(args[4]);
    int32_t rdx = mp_obj_get_int(args[5]);         // leftRay x (column 0), 16.16 - accumulates per column
    int32_t rdy = mp_obj_get_int(args[6]);
    int32_t srx = mp_obj_get_int(args[7]);         // rayStep x per column, 16.16
    int32_t sry = mp_obj_get_int(args[8]);
    int sh = mp_obj_get_int(args[9]);
    int stride = mp_obj_get_int(args[10]);
    int ncols = mp_obj_get_int(args[11]);
    mp_get_buffer_raise(args[12], &wi, MP_BUFFER_READ);
    mp_get_buffer_raise(args[13], &ti, MP_BUFFER_WRITE);
    mp_get_buffer_raise(args[14], &bi, MP_BUFFER_WRITE);
    mp_get_buffer_raise(args[15], &ci, MP_BUFFER_WRITE);
    mp_get_buffer_raise(args[16], &di, MP_BUFFER_WRITE);
    (void)stride;
    const uint8_t *map = mi.buf;
    const uint16_t *wc = wi.buf;
    int wc_types = (int)(wi.len >> 2);
    uint16_t *top = ti.buf;
    uint16_t *bot = bi.buf;
    uint16_t *col = ci.buf;
    int32_t *dist_out = di.buf;                    // perpendicular distance, 16.16
    int half = sh >> 1;
    int imapx0 = posx >> 16;
    int imapy0 = posy >> 16;
    int32_t fracx = posx & 0xFFFF;                 // fractional part of pos, 16.16
    int32_t fracy = posy & 0xFFFF;
    const int32_t DD_CAP = (int32_t)1 << 24;       // cap deltaDist so a 64-step accumulation stays in int32
    if (ncols > (int)(ti.len >> 1)) {
        ncols = (int)(ti.len >> 1);
    }
    for (int c = 0; c < ncols; c++) {
        int mapx = imapx0;
        int mapy = imapy0;
        int32_t ax = rdx < 0 ? -rdx : rdx;
        int32_t ay = rdy < 0 ? -rdy : rdy;
        // deltaDist = |1/rayDir| in 16.16 = (1<<32)/|rayDir_q16| (int64; per-column setup, not per-step)
        int64_t ddx64 = ax ? (((int64_t)1 << 32) / ax) : (int64_t)DD_CAP;
        int64_t ddy64 = ay ? (((int64_t)1 << 32) / ay) : (int64_t)DD_CAP;
        int32_t ddx = ddx64 > DD_CAP ? DD_CAP : (int32_t)ddx64;
        int32_t ddy = ddy64 > DD_CAP ? DD_CAP : (int32_t)ddy64;
        int stepx, stepy;
        int32_t sidex, sidey;
        // sideDist to the first grid line = (fractional distance) * deltaDist, 16.16 (int64 mul, setup only)
        if (rdx < 0) {
            stepx = -1;
            sidex = (int32_t)(((int64_t)fracx * ddx) >> 16);
        } else {
            stepx = 1;
            sidex = (int32_t)(((int64_t)(65536 - fracx) * ddx) >> 16);
        }
        if (rdy < 0) {
            stepy = -1;
            sidey = (int32_t)(((int64_t)fracy * ddy) >> 16);
        } else {
            stepy = 1;
            sidey = (int32_t)(((int64_t)(65536 - fracy) * ddy) >> 16);
        }
        int side = 0;
        int cell = 1;
        for (int i = 0; i < 64; i++) {             // DDA - pure 32-bit
            if (sidex < sidey) {
                sidex += ddx;
                mapx += stepx;
                side = 0;
            } else {
                sidey += ddy;
                mapy += stepy;
                side = 1;
            }
            cell = (mapx >= 0 && mapx < mw && mapy >= 0 && mapy < mh) ? map[mapy * mw + mapx] : 1;
            if (cell) {
                break;
            }
        }
        int32_t perp = (side == 0) ? (sidex - ddx) : (sidey - ddy);   // perpWallDist, 16.16
        if (perp < 655) {
            perp = 655;                            // ~0.01 in 16.16
        }
        int lh = (int)(((int32_t)sh << 16) / perp);   // sh / perpWallDist (px); 32-bit (sh<<16 <= ~15.7M)
        int t = half - (lh >> 1);
        int b = t + lh;
        if (t < 0) {
            t = 0;
        }
        if (b > sh) {
            b = sh;
        }
        top[c] = (uint16_t)t;
        bot[c] = (uint16_t)b;
        int ct = (cell < wc_types) ? cell : 1;     // unknown type -> type 1 (matches Python default)
        col[c] = wc[ct * 2 + side];
        dist_out[c] = perp;
        rdx += srx;                                // accumulate ray direction for the next column (no overflow)
        rdy += sry;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(picogame_raycast_obj, 17, 17, picogame_raycast);

#if defined(PICOGAME_CORE1_PROBE)
// core1(on) - PROBE toggle: route splittable kernels (mode7 rows for now) through the second-core
// fork-join helper. Temporary API for A/B measurement; the final shape lands after the probe verdict.
static mp_obj_t picogame_core1_fn(mp_obj_t on) {
    picogame_core1_set_enabled(mp_obj_is_true(on));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(picogame_core1_obj, picogame_core1_fn);

// refresh_async(on) - PROBE: scene.refresh() submits the whole compose+send to core1 and returns
// immediately (frame = max(Python, refresh) instead of the sum). Sprite/Tilemap/Canvas scenes only
// (StripDraw falls back to today's synchronous path). Temporary API for the Stage-3 estimate.
extern bool picogame_scene_refresh_async_enabled;
static mp_obj_t picogame_refresh_async_fn(mp_obj_t on) {
    picogame_scene_refresh_async_enabled = mp_obj_is_true(on);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(picogame_refresh_async_obj, picogame_refresh_async_fn);
#endif

// road_edges(rl, rr, hw, n, cx0, dist, cfg) - one racing-road frame's curve accumulator + integer
// edges in one call (the OutRun-genre compute_road loop; core + cfg layout documented in
// shared-module). rl/rr = int16 out, hw = int32 Q16 half-widths, cx0 = Q16 screen centre
// (incl. lateral), dist = integer world distance, cfg = int32[7].
static mp_obj_t picogame_road_edges_fn(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t rli, rri, hwi, cfgi;
    mp_get_buffer_raise(args[0], &rli, MP_BUFFER_WRITE);
    mp_get_buffer_raise(args[1], &rri, MP_BUFFER_WRITE);
    mp_get_buffer_raise(args[2], &hwi, MP_BUFFER_READ);
    mp_get_buffer_raise(args[6], &cfgi, MP_BUFFER_READ);
    int n = mp_obj_get_int(args[3]);
    int cap = (int)(rli.len < rri.len ? rli.len : rri.len) / 2;
    if (n > cap) {
        n = cap;
    }
    if (n > (int)(hwi.len / 4)) {
        n = (int)(hwi.len / 4);
    }
    if (n <= 0 || cfgi.len < 7 * 4) {
        return mp_const_none;
    }
    picogame_road_edges((int16_t *)rli.buf, (int16_t *)rri.buf, (const int32_t *)hwi.buf, n,
        mp_obj_get_int(args[4]), mp_obj_get_int(args[5]), (const int32_t *)cfgi.buf);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(picogame_road_edges_obj, 7, 7, picogame_road_edges_fn);

// project(cam, pts, n, out_sx, out_sy) - batch perspective projection of `n` 3D points to screen.
//   cam  = 15 camera params: ex,ey,ez, rx,rz, ux,uy,uz, fx,fy,fz, focal, cx0, cy0, near
//   pts  = n*3 world coords (x,y,z per point)
//   out_sx/out_sy = int16 screen coords; a point behind the near plane gets sentinel -32768
// On an FPU board (CIRCUITPY_PICOGAME_FPU) cam/pts are float32; else they are 16.16 fixed int32.
// This is the shared hot path for blocky pseudo-3D (project the 8 corners of each box, then fill).
static mp_obj_t picogame_project(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t ci, pi, xi, yi;
    mp_get_buffer_raise(args[0], &ci, MP_BUFFER_READ);
    mp_get_buffer_raise(args[1], &pi, MP_BUFFER_READ);
    int n = mp_obj_get_int(args[2]);
    mp_get_buffer_raise(args[3], &xi, MP_BUFFER_WRITE);
    mp_get_buffer_raise(args[4], &yi, MP_BUFFER_WRITE);
    int16_t *osx = xi.buf;
    int16_t *osy = yi.buf;
    if (n > (int)(xi.len >> 1)) {
        n = (int)(xi.len >> 1);
    }
    #if CIRCUITPY_PICOGAME_FPU
    const float *cam = ci.buf;
    const float *pts = pi.buf;
    float ex = cam[0], ey = cam[1], ez = cam[2];
    float rx = cam[3], rz = cam[4];
    float ux = cam[5], uy = cam[6], uz = cam[7];
    float fx = cam[8], fy = cam[9], fz = cam[10];
    float focal = cam[11], cx0 = cam[12], cy0 = cam[13], near = cam[14];
    for (int i = 0; i < n; i++) {
        float X = pts[i * 3] - ex, Y = pts[i * 3 + 1] - ey, Z = pts[i * 3 + 2] - ez;
        float cz = X * fx + Y * fy + Z * fz;
        if (cz < near) {
            osx[i] = -32768;
            osy[i] = -32768;
            continue;
        }
        float k = focal / cz;                       // hardware divide on an FPU part
        osx[i] = (int16_t)(cx0 + (X * rx + Z * rz) * k);
        osy[i] = (int16_t)(cy0 - (X * ux + Y * uy + Z * uz) * k);
    }
    #else
    const int32_t *cam = ci.buf;                    // all values 16.16
    const int32_t *pts = pi.buf;
    int32_t ex = cam[0], ey = cam[1], ez = cam[2];
    int32_t rx = cam[3], rz = cam[4];
    int32_t ux = cam[5], uy = cam[6], uz = cam[7];
    int32_t fx = cam[8], fy = cam[9], fz = cam[10];
    int32_t focal = cam[11], cx0 = cam[12], cy0 = cam[13], near = cam[14];
    // Full-precision Q16 dot products (int64 mul per term). A Q8-prescaled-basis/MULS variant was
    // ~30% faster, but its error grows with |coord| (~0.2%/axis) and k = focal/cz AMPLIFIES it near
    // the near plane - host-measured 23-34 px warps on close fly-bys at a file-browser world scale
    // (walls visibly broke). Correctness first: Q16 keeps the worst error a few px at any cz >= near,
    // for coords up to +-32k units; still ~4-5x faster than the same math in Python on the M0+.
    #define FMUL(a, b) ((int32_t)(((int64_t)(a) * (b)) >> 16))
    for (int i = 0; i < n; i++) {
        int32_t X = pts[i * 3] - ex, Y = pts[i * 3 + 1] - ey, Z = pts[i * 3 + 2] - ez;
        int32_t cz = FMUL(X, fx) + FMUL(Y, fy) + FMUL(Z, fz);
        if (cz < near) {
            osx[i] = -32768;
            osy[i] = -32768;
            continue;
        }
        // focal/cz in 16.16. A 32-bit divide (focal<<8 = Q24, cz>>8 = Q8 -> Q16) is ~4x cheaper than
        // an int64 divide on the M0+ (no HW divide) and the lost cz precision costs <0.02 px (host-
        // measured). Needs FOCAL < ~250 (focal<<8 in uint32) and near >= 1/256 (cz>>8 nonzero).
        int32_t k = (int32_t)(((uint32_t)focal << 8) / (uint32_t)(cz >> 8));
        int32_t rr = FMUL(X, rx) + FMUL(Z, rz);
        int32_t uu = FMUL(X, ux) + FMUL(Y, uy) + FMUL(Z, uz);
        osx[i] = (int16_t)((cx0 + FMUL(rr, k)) >> 16);
        osy[i] = (int16_t)((cy0 - FMUL(uu, k)) >> 16);
    }
#undef FMUL
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(picogame_project_obj, 5, 5, picogame_project);

//| def invert(display: busdisplay.BusDisplay, on: bool) -> None:
//|     """Toggle the panel's hardware colour inversion (INVON/INVOFF). Instant and sends NO
//|     pixel data, so a brief invert is a FREE full-screen flash (a 1-bit negative 'hit' look)
//|     - cheaper than a Fade overlay. ST7789/ST7735 support it."""
//|     ...
static mp_obj_t picogame_invert(mp_obj_t display_in, mp_obj_t on_in) {
    #if CIRCUITPY_PICOGAME_FRAMEBUFFER
    // A Framebuffer target (RP2350 DVI, the WASM playground) has no hardware INVON/INVOFF -
    // emulate the flash by XORing the composite (mirrors the Scene/render Framebuffer handling).
    if (mp_obj_is_type(display_in, &picogame_framebuffer_type)) {
        picogame_fb_set_invert(mp_obj_is_true(on_in));
        return mp_const_none;
    }
    #endif
    picogame_set_invert(pg_get_display(display_in), mp_obj_is_true(on_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(picogame_invert_obj, picogame_invert);

//| def render(
//|     display: busdisplay.BusDisplay,
//|     sprites: List[Sprite],
//|     buffer: WriteableBuffer,
//|     x0: int,
//|     y0: int,
//|     x1: int,
//|     y1: int,
//|     *,
//|     background: int = 0,
//| ) -> None:
//|     """Render ``sprites`` into the screen region [x0,x1) x [y0,y1) and push it
//|     to ``display``. ``buffer`` is a reusable strip buffer (>= region_width*2 bytes)."""
//|     ...
static mp_obj_t picogame_render_fun(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_display, ARG_sprites, ARG_buffer, ARG_x0, ARG_y0, ARG_x1, ARG_y1, ARG_background };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_display, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_sprites, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_x0, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_y0, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_x1, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_y1, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_background, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Accept a picogame.Framebuffer (RAM scanout buffer) as the target too, when built
    // in: immediate render composites straight into it (no strip buffer, no bus), so
    // pg.render(board.DISPLAY, ...) works when board.DISPLAY is a Framebuffer - the HUD /
    // HudBar / immediate-mode path on scanout-buffer platforms. Mirrors the Scene change.
    #if CIRCUITPY_PICOGAME_FRAMEBUFFER
    picogame_framebuffer_obj_t *fbt =
        mp_obj_is_type(args[ARG_display].u_obj, &picogame_framebuffer_type)
        ? MP_OBJ_TO_PTR(args[ARG_display].u_obj) : NULL;
    busdisplay_busdisplay_obj_t *display = fbt ? NULL : pg_get_display(args[ARG_display].u_obj);
    #else
    busdisplay_busdisplay_obj_t *display = pg_get_display(args[ARG_display].u_obj);
    #endif

    size_t n = 0;
    mp_obj_t *items;
    mp_obj_get_array(args[ARG_sprites].u_obj, &n, &items);

    // Classify items into layer kinds. All-Sprite lists stay on the NULL-kinds fast path (no alloc -
    // the common case). Any non-Sprite layer (StripDraw/Canvas/Tilemap/Particles) builds a small kinds
    // array so immediate render uses the SAME multi-layer blitter the Scene does - e.g. a StripDraw
    // composited straight into the strip with `view.text()` = 0-RAM immediate HUD / text screen.
    uint8_t kbuf[16];
    uint8_t *kinds = NULL;
    for (size_t i = 0; i < n; i++) {
        if (!mp_obj_is_type(items[i], &picogame_sprite_type)) {
            kinds = (n <= MP_ARRAY_SIZE(kbuf)) ? kbuf : m_new(uint8_t, n);
            break;
        }
    }
    if (kinds != NULL) {
        for (size_t i = 0; i < n; i++) {
            mp_obj_t it = items[i];
            if (mp_obj_is_type(it, &picogame_sprite_type)) {
                kinds[i] = PICOGAME_KIND_SPRITE;
            } else if (mp_obj_is_type(it, &picogame_stripdraw_type)) {
                kinds[i] = PICOGAME_KIND_STRIPDRAW;
            } else if (mp_obj_is_type(it, &picogame_canvas_type)) {
                kinds[i] = PICOGAME_KIND_CANVAS;
            } else if (mp_obj_is_type(it, &picogame_tilemap_type)) {
                kinds[i] = PICOGAME_KIND_TILEMAP;
            } else if (mp_obj_is_type(it, &picogame_particles_type)) {
                kinds[i] = PICOGAME_KIND_PARTICLES;
            } else {
                if (n > MP_ARRAY_SIZE(kbuf)) {
                    m_del(uint8_t, kinds, n);
                }
                mp_raise_TypeError(MP_ERROR_TEXT("expected a Sprite, Tilemap, Particles, Canvas or StripDraw"));
            }
        }
    }

    #if CIRCUITPY_PICOGAME_FRAMEBUFFER
    if (fbt != NULL) {
        // Framebuffer target: composite the region straight into it (no strip buffer, no
        // bus). Same compositor as the SPI path; re-raise a latched StripDraw exception.
        mp_obj_t exc = picogame_render_framebuffer(fbt->fb, fbt->width, fbt->height, fbt->fmt,
            fbt->scratch, fbt->scratch_rows,
            items, kinds, n,
            args[ARG_x0].u_int, args[ARG_y0].u_int, args[ARG_x1].u_int, args[ARG_y1].u_int,
            args[ARG_background].u_int, 0, 0);
        if (kinds != NULL && n > MP_ARRAY_SIZE(kbuf)) {
            m_del(uint8_t, kinds, n);
        }
        if (exc != MP_OBJ_NULL) {
            nlr_raise(MP_OBJ_TO_PTR(exc));
        }
        return mp_const_none;
    }
    #endif

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &bufinfo, MP_BUFFER_WRITE);

    if (kinds == NULL) {
        picogame_render(display, items, n,
            (uint16_t *)bufinfo.buf, bufinfo.len / 2,
            args[ARG_x0].u_int, args[ARG_y0].u_int, args[ARG_x1].u_int, args[ARG_y1].u_int,
            args[ARG_background].u_int);
    } else {
        picogame_render_region(display, items, kinds, n,
            (uint16_t *)bufinfo.buf, bufinfo.len / 2,
            args[ARG_x0].u_int, args[ARG_y0].u_int, args[ARG_x1].u_int, args[ARG_y1].u_int,
            args[ARG_background].u_int, 0, 0);
        if (n > MP_ARRAY_SIZE(kbuf)) {
            m_del(uint8_t, kinds, n);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(picogame_render_obj, 7, picogame_render_fun);

//| def collide(
//|     x1: int, y1: int, x2: int, y2: int, ax1: int, ay1: int, ax2: int = ..., ay2: int = ...
//| ) -> bool:
//|     """AABB overlap test with INCLUSIVE bounds - both corners are part of the box, so two
//|     boxes collide the moment they TOUCH (no visible overlap, no gap). Pass sprite hitboxes
//|     as (x, y, x+w, y+h): collision fires on contact, the usual game feel. With 8 args: box
//|     (x1,y1,x2,y2) vs box (ax1,ay1,ax2,ay2). With 6 args: box vs point (ax1, ay1).
//|     NOTE: this is intentionally inclusive, unlike render's half-open [x0,x1) pixel ranges -
//|     render is about pixels, collide is about game hitboxes (touch = hit)."""
//|     ...
static mp_obj_t picogame_collide(size_t n_args, const mp_obj_t *args) {
    int x1 = mp_obj_get_int(args[0]);
    int y1 = mp_obj_get_int(args[1]);
    int x2 = mp_obj_get_int(args[2]);
    int y2 = mp_obj_get_int(args[3]);
    bool hit;
    if (n_args == 8) {
        int bx1 = mp_obj_get_int(args[4]);
        int by1 = mp_obj_get_int(args[5]);
        int bx2 = mp_obj_get_int(args[6]);
        int by2 = mp_obj_get_int(args[7]);
        hit = (x1 <= bx2) && (x2 >= bx1) && (y1 <= by2) && (y2 >= by1);
    } else if (n_args == 6) {
        int px = mp_obj_get_int(args[4]);
        int py = mp_obj_get_int(args[5]);
        hit = (px >= x1) && (px <= x2) && (py >= y1) && (py <= y2);
    } else {
        mp_raise_TypeError(MP_ERROR_TEXT("collide takes 6 or 8 args"));
    }
    return mp_obj_new_bool(hit);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(picogame_collide_obj, 6, 8, picogame_collide);

// ---- procedural value-noise in C (the desktop simulator sim/picogame.py mirrors it) ----
// The CANONICAL implementation is FIXED-POINT (Q16.16 coords, Q0.16 values), exposed
// under the plain names value2d/value1d/fbm2d/fbm1d (see further down). It benchmarked
// ~1.8x faster than float on-device (0.649 s vs 1.186 s / 5000 fbm2d), so the float
// version was retired (2026-06-18) to free flash for future engine features.
// The float reference is preserved but DISABLED in the `#if 0` below (cf. PicoLibSDK's
// own Noise2D, which is likewise float) - revive by flipping it to `#if 1` and pointing
// the module table at the *_obj names instead of the *_fx_obj ones.
#if 0   // float reference implementation - superseded by the fixed-point path below
static inline float pg_nhash(int32_t x, int32_t y, int32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 362437u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (float)(h & 0xFFFFu) / 65535.0f;
}
static inline float pg_nsmooth(float t) {
    return t * t * (3.0f - 2.0f * t);
}
static inline int32_t pg_ifloor(float x) {
    int32_t i = (int32_t)x;
    return (x < (float)i) ? i - 1 : i;
}
static float pg_value2d(float x, float y, int32_t seed) {
    int32_t xi = pg_ifloor(x), yi = pg_ifloor(y);
    float xf = x - (float)xi, yf = y - (float)yi;
    float a = pg_nhash(xi, yi, seed), b = pg_nhash(xi + 1, yi, seed);
    float c = pg_nhash(xi, yi + 1, seed), d = pg_nhash(xi + 1, yi + 1, seed);
    float u = pg_nsmooth(xf), v = pg_nsmooth(yf);
    return (a * (1.0f - u) + b * u) * (1.0f - v) + (c * (1.0f - u) + d * u) * v;
}
static float pg_value1d(float x, int32_t seed) {
    int32_t xi = pg_ifloor(x);
    float xf = x - (float)xi;
    float a = pg_nhash(xi, 0, seed), b = pg_nhash(xi + 1, 0, seed);
    return a + (b - a) * pg_nsmooth(xf);
}

//| def value2d(x: float, y: float, *, seed: int = 0) -> float:
//|     """Smooth 2-D value noise in 0..1 (fast C)."""
//|     ...
static mp_obj_t picogame_value2d(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    static const mp_arg_t spec[] = { {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
                                     {MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}}, {MP_QSTR_seed, MP_ARG_INT, {.u_int = 0}} };
    mp_arg_val_t a[3];
    mp_arg_parse_all(n_args, pos, kw, 3, spec, a);
    return mp_obj_new_float(pg_value2d(mp_obj_get_float(a[0].u_obj), mp_obj_get_float(a[1].u_obj), a[2].u_int));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(picogame_value2d_obj, 2, picogame_value2d);

//| def value1d(x: float, *, seed: int = 0) -> float: ...
static mp_obj_t picogame_value1d(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    static const mp_arg_t spec[] = { {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
                                     {MP_QSTR_seed, MP_ARG_INT, {.u_int = 0}} };
    mp_arg_val_t a[2];
    mp_arg_parse_all(n_args, pos, kw, 2, spec, a);
    return mp_obj_new_float(pg_value1d(mp_obj_get_float(a[0].u_obj), a[1].u_int));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(picogame_value1d_obj, 1, picogame_value1d);
#endif  // float value2d / value1d

// Shared arg spec for both fbm2d (disabled float) and fbm2d_fx (active fixed-point).
static const mp_arg_t pg_fbm2d_args[] = {
    { MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_octaves, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 4} },
    { MP_QSTR_seed, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    { MP_QSTR_lacunarity, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_gain, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
};

#if 0   // float reference fbm - superseded by the fixed-point path below
//| def fbm2d(x, y, *, octaves=4, seed=0, lacunarity=2.0, gain=0.5) -> float: ...
static mp_obj_t picogame_fbm2d(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    mp_arg_val_t a[6];
    mp_arg_parse_all(n_args, pos, kw, 6, pg_fbm2d_args, a);
    float x = mp_obj_get_float(a[0].u_obj), y = mp_obj_get_float(a[1].u_obj);
    int octaves = a[2].u_int;
    int32_t seed = a[3].u_int;
    float lac = (a[4].u_obj == MP_OBJ_NULL) ? 2.0f : mp_obj_get_float(a[4].u_obj);
    float gain = (a[5].u_obj == MP_OBJ_NULL) ? 0.5f : mp_obj_get_float(a[5].u_obj);
    float total = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; i++) {
        total += amp * pg_value2d(x * freq, y * freq, seed);
        norm += amp;
        amp *= gain;
        freq *= lac;
    }
    return mp_obj_new_float(norm > 0.0f ? total / norm : 0.0f);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(picogame_fbm2d_obj, 2, picogame_fbm2d);

//| def fbm1d(x, *, octaves=4, seed=0, lacunarity=2.0, gain=0.5) -> float: ...
static mp_obj_t picogame_fbm1d(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    static const mp_arg_t spec[] = { {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
                                     {MP_QSTR_octaves, MP_ARG_INT, {.u_int = 4}}, {MP_QSTR_seed, MP_ARG_INT, {.u_int = 0}},
                                     {MP_QSTR_lacunarity, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}}, {MP_QSTR_gain, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}} };
    mp_arg_val_t a[5];
    mp_arg_parse_all(n_args, pos, kw, 5, spec, a);
    float x = mp_obj_get_float(a[0].u_obj);
    int octaves = a[1].u_int;
    int32_t seed = a[2].u_int;
    float lac = (a[3].u_obj == MP_OBJ_NULL) ? 2.0f : mp_obj_get_float(a[3].u_obj);
    float gain = (a[4].u_obj == MP_OBJ_NULL) ? 0.5f : mp_obj_get_float(a[4].u_obj);
    float total = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; i++) {
        total += amp * pg_value1d(x * freq, seed);
        norm += amp;
        amp *= gain;
        freq *= lac;
    }
    return mp_obj_new_float(norm > 0.0f ? total / norm : 0.0f);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(picogame_fbm1d_obj, 1, picogame_fbm1d);
#endif  // float fbm2d / fbm1d

// ---- fixed-point (Q16.16 coords, Q0.16 values) noise: the CANONICAL value-noise impl,
// exposed under the plain names value2d/value1d/fbm2d/fbm1d. The inner math is integer
// (float only at the Python boundary); ~1.8x faster than the retired float path. ----
static inline uint32_t pg_nhash_raw(int32_t x, int32_t y, int32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 362437u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h ^ (h >> 16)) & 0xFFFFu;                      // Q0.16 in [0,1)
}
static inline uint32_t pg_smooth16(uint32_t t) {           // t,result Q0.16: t*t*(3-2t)
    uint32_t t2 = (t * t) >> 16;
    uint32_t e = (3u << 16) - 2u * t;
    return (uint32_t)(((uint64_t)t2 * e) >> 16);
}
static inline uint32_t pg_lerp16(uint32_t a, uint32_t b, uint32_t u) {
    return (uint32_t)((int32_t)a + (int32_t)(((int64_t)((int32_t)b - (int32_t)a) * (int32_t)u) >> 16));
}
static uint32_t pg_value2d_fx(int32_t X, int32_t Y, int32_t seed) {     // X,Y Q16.16 -> Q0.16
    int32_t xi = X >> 16, yi = Y >> 16;
    uint32_t xf = (uint32_t)(X - (xi << 16)), yf = (uint32_t)(Y - (yi << 16));
    uint32_t a = pg_nhash_raw(xi, yi, seed), b = pg_nhash_raw(xi + 1, yi, seed);
    uint32_t c = pg_nhash_raw(xi, yi + 1, seed), d = pg_nhash_raw(xi + 1, yi + 1, seed);
    uint32_t u = pg_smooth16(xf), v = pg_smooth16(yf);
    return pg_lerp16(pg_lerp16(a, b, u), pg_lerp16(c, d, u), v);
}
static uint32_t pg_value1d_fx(int32_t X, int32_t seed) {
    int32_t xi = X >> 16;
    uint32_t xf = (uint32_t)(X - (xi << 16));
    return pg_lerp16(pg_nhash_raw(xi, 0, seed), pg_nhash_raw(xi + 1, 0, seed), pg_smooth16(xf));
}
#define PG_Q16(f) ((int32_t)((f) * 65536.0f))

static mp_obj_t picogame_value2d_fx(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    static const mp_arg_t spec[] = { {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
                                     {MP_QSTR_y, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}}, {MP_QSTR_seed, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0}} };
    mp_arg_val_t a[3];
    mp_arg_parse_all(n_args, pos, kw, 3, spec, a);
    int32_t v = pg_value2d_fx(PG_Q16(mp_obj_get_float(a[0].u_obj)), PG_Q16(mp_obj_get_float(a[1].u_obj)), a[2].u_int);
    return mp_obj_new_float((float)v * (1.0f / 65536.0f));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(picogame_value2d_fx_obj, 2, picogame_value2d_fx);

static mp_obj_t picogame_value1d_fx(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    static const mp_arg_t spec[] = { {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
                                     {MP_QSTR_seed, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0}} };
    mp_arg_val_t a[2];
    mp_arg_parse_all(n_args, pos, kw, 2, spec, a);
    int32_t v = pg_value1d_fx(PG_Q16(mp_obj_get_float(a[0].u_obj)), a[1].u_int);
    return mp_obj_new_float((float)v * (1.0f / 65536.0f));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(picogame_value1d_fx_obj, 1, picogame_value1d_fx);

static mp_obj_t picogame_fbm2d_fx(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    mp_arg_val_t a[6];
    mp_arg_parse_all(n_args, pos, kw, 6, pg_fbm2d_args, a);
    int32_t X = PG_Q16(mp_obj_get_float(a[0].u_obj)), Y = PG_Q16(mp_obj_get_float(a[1].u_obj));
    int octaves = a[2].u_int;
    int32_t seed = a[3].u_int;
    int32_t lacq = (a[4].u_obj == MP_OBJ_NULL) ? (2 << 16) : PG_Q16(mp_obj_get_float(a[4].u_obj));
    int32_t gainq = (a[5].u_obj == MP_OBJ_NULL) ? (1 << 15) : PG_Q16(mp_obj_get_float(a[5].u_obj));
    int32_t amp = 1 << 16, freq = 1 << 16;
    int64_t total = 0, norm = 0;
    for (int i = 0; i < octaves; i++) {
        int32_t sx = (int32_t)(((int64_t)X * freq) >> 16), sy = (int32_t)(((int64_t)Y * freq) >> 16);
        total += ((int64_t)amp * pg_value2d_fx(sx, sy, seed)) >> 16;
        norm += amp;
        amp = (int32_t)(((int64_t)amp * gainq) >> 16);
        freq = (int32_t)(((int64_t)freq * lacq) >> 16);
    }
    return mp_obj_new_float(norm ? (float)total / (float)norm : 0.0f);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(picogame_fbm2d_fx_obj, 2, picogame_fbm2d_fx);

static mp_obj_t picogame_fbm1d_fx(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    static const mp_arg_t spec[] = { {MP_QSTR_x, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
                                     {MP_QSTR_octaves, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 4}}, {MP_QSTR_seed, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0}},
                                     {MP_QSTR_lacunarity, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL}}, {MP_QSTR_gain, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL}} };
    mp_arg_val_t a[5];
    mp_arg_parse_all(n_args, pos, kw, 5, spec, a);
    int32_t X = PG_Q16(mp_obj_get_float(a[0].u_obj));
    int octaves = a[1].u_int;
    int32_t seed = a[2].u_int;
    int32_t lacq = (a[3].u_obj == MP_OBJ_NULL) ? (2 << 16) : PG_Q16(mp_obj_get_float(a[3].u_obj));
    int32_t gainq = (a[4].u_obj == MP_OBJ_NULL) ? (1 << 15) : PG_Q16(mp_obj_get_float(a[4].u_obj));
    int32_t amp = 1 << 16, freq = 1 << 16;
    int64_t total = 0, norm = 0;
    for (int i = 0; i < octaves; i++) {
        int32_t sx = (int32_t)(((int64_t)X * freq) >> 16);
        total += ((int64_t)amp * pg_value1d_fx(sx, seed)) >> 16;
        norm += amp;
        amp = (int32_t)(((int64_t)amp * gainq) >> 16);
        freq = (int32_t)(((int64_t)freq * lacq) >> 16);
    }
    return mp_obj_new_float(norm ? (float)total / (float)norm : 0.0f);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(picogame_fbm1d_fx_obj, 1, picogame_fbm1d_fx);

#if CIRCUITPY_PICOGAME_FRAMEBUFFER
// ---------------------------------------------------------------------------
// Framebuffer  (a RAM render target used in place of a BusDisplay; scanout-buffer
// platforms - WASM playground, desktop sim, FruitJam DVI/HSTX)
// ---------------------------------------------------------------------------
//| class Framebuffer:
//|     """A RAM framebuffer render target that a Scene or :py:func:`render` can draw
//|     into instead of a BusDisplay. ``buffer`` must be a writable buffer of at least
//|     ``width*height*2`` bytes (``width*height`` for ``rgb332=True``); the caller owns it
//|     (a ``bytearray`` in the browser, the DVI scanout buffer on FruitJam). By default the
//|     pixels are wire-order RGB565 (the engine's internal format); ``native_rgb565=True``
//|     byte-swaps each finished region to NATIVE RGB565 - the format 16-bit picodvi /
//|     canvas scanout targets expect; ``rgb332=True`` quantizes each finished region to
//|     RGB332 bytes - the format of 8-bit picodvi scanout (FruitJam 640x480, which the
//|     hardware only offers at 8bpp). Assets, palettes and ``rgb565()`` stay wire-order
//|     RGB565 throughout regardless of the output format."""
//|
//|     def __init__(self, buffer: WriteableBuffer, width: int, height: int, *,
//|                  native_rgb565: bool = False, rgb332: bool = False) -> None: ...
static mp_obj_t picogame_framebuffer_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_buffer, ARG_width, ARG_height, ARG_native_rgb565, ARG_rgb332 };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_native_rgb565, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_rgb332, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t width = mp_arg_validate_int_range(args[ARG_width].u_int, 1, 4096, MP_QSTR_width);
    mp_int_t height = mp_arg_validate_int_range(args[ARG_height].u_int, 1, 4096, MP_QSTR_height);
    if (args[ARG_native_rgb565].u_bool && args[ARG_rgb332].u_bool) {
        mp_raise_ValueError(MP_ERROR_TEXT("native_rgb565 and rgb332 are exclusive"));
    }
    bool rgb332 = args[ARG_rgb332].u_bool;

    mp_buffer_info_t bi;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &bi, MP_BUFFER_WRITE);
    uint64_t need = (uint64_t)width * (uint64_t)height * (rgb332 ? 1u : 2u);
    if ((uint64_t)bi.len < need) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer too small"));
    }

    picogame_framebuffer_obj_t *self = mp_obj_malloc(picogame_framebuffer_obj_t, type);
    self->buffer = args[ARG_buffer].u_obj;
    self->fb = (uint16_t *)bi.buf;
    self->width = width;
    self->height = height;
    self->fmt = rgb332 ? PICOGAME_FB_RGB332
        : (args[ARG_native_rgb565].u_bool ? PICOGAME_FB_NATIVE565 : PICOGAME_FB_WIRE565);
    // A LIVE scanout buffer (picodvi/HDMI) is read continuously, so picogame_render_framebuffer
    // composes each band into this PRIVATE strip and only memcpys the FINISHED band into the fb.
    // That serves BOTH targets: (a) native -> also byte-swap the strip so the fb never holds wire
    // (no pink); (b) wire -> no swap, but the off-screen compose still stops the beam from sampling
    // a half-composited region (background filled, sprite not yet drawn) = no sprite/HUD flicker.
    // Always allocated for the FB target; the WASM/sim path (read out after present, not live) just
    // pays a small strip + one memcpy. See PICOGAME_FB_SCRATCH_H.
    self->scratch_buf = mp_const_none;
    self->scratch = NULL;
    self->scratch_rows = 0;
    {
        int rows = PICOGAME_FB_SCRATCH_H;
        if (rows > height) {
            rows = height;
        }
        mp_obj_t sb = mp_obj_new_bytearray_of_zeros((size_t)width * (size_t)rows * 2u);
        mp_buffer_info_t sbi;
        mp_get_buffer_raise(sb, &sbi, MP_BUFFER_WRITE);
        self->scratch_buf = sb;
        self->scratch = (uint16_t *)sbi.buf;
        self->scratch_rows = rows;
    }
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t picogame_framebuffer_get_width(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_framebuffer_obj_t *)MP_OBJ_TO_PTR(self_in))->width);
}
static MP_DEFINE_CONST_FUN_OBJ_1(picogame_framebuffer_get_width_obj, picogame_framebuffer_get_width);
MP_PROPERTY_GETTER(picogame_framebuffer_width_obj, (mp_obj_t)&picogame_framebuffer_get_width_obj);

static mp_obj_t picogame_framebuffer_get_height(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((picogame_framebuffer_obj_t *)MP_OBJ_TO_PTR(self_in))->height);
}
static MP_DEFINE_CONST_FUN_OBJ_1(picogame_framebuffer_get_height_obj, picogame_framebuffer_get_height);
MP_PROPERTY_GETTER(picogame_framebuffer_height_obj, (mp_obj_t)&picogame_framebuffer_get_height_obj);

static const mp_rom_map_elem_t picogame_framebuffer_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_width), MP_ROM_PTR(&picogame_framebuffer_width_obj) },
    { MP_ROM_QSTR(MP_QSTR_height), MP_ROM_PTR(&picogame_framebuffer_height_obj) },
};
static MP_DEFINE_CONST_DICT(picogame_framebuffer_locals_dict, picogame_framebuffer_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    picogame_framebuffer_type,
    MP_QSTR_Framebuffer,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, picogame_framebuffer_make_new,
    locals_dict, &picogame_framebuffer_locals_dict
    );
#endif // CIRCUITPY_PICOGAME_FRAMEBUFFER

// ===== ROMFS-XIP asset region (Stage 1; review/romfs-xip-implementation-plan.md) =====
// Assets live in a reserved flash region between NVM and the FAT drive (PICOGAME_ROMFS_BASE_ADDR /
// PICOGAME_ROMFS_LEN from mpconfigport.h) and are read 0-copy through the XIP window. Compiled
// only when the board carves a region (CIRCUITPY_PICOGAME_ROMFS_KB > 0); a universal build keeps
// just ROMFS_SUPPORTED=False. Supersedes the Stage-0 frozen-blob spike (gate results: see the plan).
#if CIRCUITPY_PICOGAME_ROMFS_KB > 0
#include "extmod/vfs.h"
#include "extmod/vfs_rom.h"
#include "supervisor/filesystem.h"
#include "py/objarray.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "supervisor/shared/tick.h"

// Valid ROMFS image at the region base? (header magic per extmod/vfs_rom.c)
static bool picogame_romfs_present(void) {
    const uint8_t *base = (const uint8_t *)PICOGAME_ROMFS_BASE_ADDR;
    return base[0] == (0x80 | 'R') && base[1] == (0x80 | 'M') && base[2] == '1';
}

//| def romfs_region() -> Optional[Tuple[int, int]]:
//|     """The reserved asset region as ``(xip_address, length)``, or ``None`` when no valid
//|     ROMFS image is flashed there (supported-but-empty). The function itself is absent on
//|     builds without a region - test ``picogame.ROMFS_SUPPORTED`` first."""
//|     ...
static mp_obj_t picogame_romfs_region(void) {
    if (!picogame_romfs_present()) {
        return mp_const_none;
    }
    mp_obj_t items[2] = {
        mp_obj_new_int_from_uint((uintptr_t)PICOGAME_ROMFS_BASE_ADDR),
        mp_obj_new_int_from_uint(PICOGAME_ROMFS_LEN),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(picogame_romfs_region_obj, picogame_romfs_region);

//| def romfs_mount(path: str) -> VfsRom:
//|     """Mount the asset region's ROMFS at `path` (e.g. ``"/rom"``); files open 0-copy over
//|     XIP flash. Raises OSError(ENODEV) when no valid image is flashed. Bypasses storage.mount
//|     (which casts to the FAT-specific fs_user_mount_t) by inserting into the VFS table directly."""
//|     ...
static mp_obj_t picogame_romfs_mount(mp_obj_t path_in) {
    if (!picogame_romfs_present()) {
        mp_raise_OSError(MP_ENODEV);   // no image flashed - see romfs_region() / (Stage 2) romfs_program()
    }
    const char *path = mp_obj_str_get_str(path_in);
    mp_obj_t buf = mp_obj_new_bytearray_by_ref(PICOGAME_ROMFS_LEN, (void *)PICOGAME_ROMFS_BASE_ADDR);  // 0-copy over flash
    mp_obj_t vfsrom = mp_call_function_1(MP_OBJ_FROM_PTR(&mp_type_vfs_rom), buf);
    mp_obj_t dest[4];
    mp_load_method(vfsrom, MP_QSTR_mount, dest);
    dest[2] = mp_const_true;   // readonly
    dest[3] = mp_const_false;  // mkfs
    mp_call_method_n_kw(2, 0, dest);
    size_t path_len = strlen(path);
    char *path_copy = m_new(char, path_len + 1);
    memcpy(path_copy, path, path_len + 1);
    mp_vfs_mount_t *vfs = m_new_obj(mp_vfs_mount_t);
    vfs->str = path_copy;
    vfs->len = path_len;
    vfs->obj = vfsrom;
    vfs->next = MP_STATE_VM(vfs_mount_table);
    MP_STATE_VM(vfs_mount_table) = vfs;
    return vfsrom;
}
static MP_DEFINE_CONST_FUN_OBJ_1(picogame_romfs_mount_obj, picogame_romfs_mount);

//| def romfs_program(path: str) -> int:
//|     """Program the asset region from a ROMFS image file (e.g. copied onto CIRCUITPY or the
//|     SD card) and return the image size written. The file is read through the filesystem, so
//|     source fragmentation is fine; each sector is erased, written and readback-verified.
//|     Raises ValueError on a non-ROMFS file (checked before anything is erased),
//|     OSError(EFBIG) when the image exceeds the region, OSError(EBUSY) while a ROMFS is
//|     mounted (reboot/unmount first), OSError(EIO) on a verify mismatch or when the source
//|     file changed size mid-program. The source filesystem is locked against other writers
//|     where possible (SD, ejected drive); while the host has CIRCUITPY mounted the file is
//|     read unlocked - don't rewrite it while programming (worst case: an image that fails
//|     to mount; re-run). The header sector is erased FIRST and programmed LAST, so a
//|     power loss mid-program leaves no valid image magic: firmware and the CIRCUITPY drive
//|     are untouched and the next boot sees an absent region (re-run to finish). The only
//|     unsafe window is the final header-sector program itself; a loss there leaves a
//|     truncated header that fails to mount - again just re-run."""
//|     ...
static mp_obj_t picogame_romfs_program(mp_obj_t path_in) {
    // Refuse while any VfsRom is mounted: its buffer points into the region being rewritten.
    for (mp_vfs_mount_t *vfs = MP_STATE_VM(vfs_mount_table); vfs != NULL; vfs = vfs->next) {
        if (mp_obj_is_type(vfs->obj, &mp_type_vfs_rom)) {
            mp_raise_OSError(MP_EBUSY);
        }
    }
    mp_obj_t open_args[2] = { path_in, MP_OBJ_NEW_QSTR(MP_QSTR_rb) };
    mp_obj_t file = mp_vfs_open(2, open_args, (mp_map_t *)&mp_const_empty_map);
    // Size first (seek end), so an oversized image is rejected BEFORE any erase.
    mp_obj_t dest[4];
    mp_load_method(file, MP_QSTR_seek, dest);
    dest[2] = MP_OBJ_NEW_SMALL_INT(0);
    dest[3] = MP_OBJ_NEW_SMALL_INT(2);
    mp_int_t size = mp_obj_get_int(mp_call_method_n_kw(2, 0, dest));
    mp_load_method(file, MP_QSTR_seek, dest);
    dest[2] = MP_OBJ_NEW_SMALL_INT(0);
    dest[3] = MP_OBJ_NEW_SMALL_INT(0);
    mp_call_method_n_kw(2, 0, dest);
    if (size > (mp_int_t)PICOGAME_ROMFS_LEN) {
        mp_stream_close(file);
        mp_raise_OSError(MP_EFBIG);
    }
    if (size < 4) {
        mp_stream_close(file);
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid format"));
    }
    // TRY to claim the source filesystem (guards SD/BLE/web-workflow writers for free). USB MSC
    // holds the blockdev lock for its whole session (tud_msc_is_writable_cb), i.e. whenever the
    // drive is mounted on a host - the NORMAL dev state - so a hard EBUSY here would break the
    // primary flow (copy the image over USB, run program from the REPL). Unlocked fallback: the
    // size recheck below + the header re-read + per-sector readback catch a file that changed
    // mid-program; worst case is an image that fails to mount - re-run.
    const char *path = mp_obj_str_get_str(path_in);
    const char *under = NULL;
    fs_user_mount_t *src_fs = filesystem_for_path(path, &under);
    bool locked = (src_fs != NULL) && filesystem_lock(src_fs);
    uint8_t *buf = m_new(uint8_t, PICOGAME_ROMFS_SECTOR);
    uint32_t off = 0;                                    // sector-aligned region offset
    uint32_t remaining = (uint32_t)size;
    int errcode = 0;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        while (remaining > 0) {
            // Exactly min(remaining, sector) bytes per sector: short reads are valid stream
            // semantics, so accumulate; EOF before that (the file shrank) is an EIO, and the
            // exact accounting is what keeps the sector count inside the region.
            uint32_t want = remaining < PICOGAME_ROMFS_SECTOR ? remaining : PICOGAME_ROMFS_SECTOR;
            uint32_t got = 0;
            while (got < want) {
                mp_uint_t n = mp_stream_rw(file, buf + got, want - got, &errcode, MP_STREAM_RW_READ);
                if (errcode != 0 || n == 0) {
                    mp_raise_OSError(errcode != 0 ? errcode : MP_EIO);
                }
                got += (uint32_t)n;
            }
            if (want < PICOGAME_ROMFS_SECTOR) {
                memset(buf + want, 0xff, PICOGAME_ROMFS_SECTOR - want);   // erased-state tail
            }
            if (off == 0) {
                if (!(buf[0] == (0x80 | 'R') && buf[1] == (0x80 | 'M') && buf[2] == '1')) {
                    mp_raise_ValueError(MP_ERROR_TEXT("Invalid format"));  // nothing erased yet
                }
                // POWER-LOSS ORDERING: erase the header sector now (the OLD image's magic dies
                // before any mixed state can exist) but program it LAST (below) - until then a
                // power loss leaves no valid magic, so the next boot sees an absent region.
                common_hal_picogame_romfs_erase_sector(PICOGAME_ROMFS_XIP_OFFSET);
            } else {
                common_hal_picogame_romfs_write_sector(PICOGAME_ROMFS_XIP_OFFSET + off, buf);
                // Verify via the XIP window (the flash calls flush the XIP cache themselves).
                if (memcmp((const void *)(PICOGAME_ROMFS_BASE_ADDR + off), buf, PICOGAME_ROMFS_SECTOR) != 0) {
                    mp_raise_OSError(MP_EIO);
                }
            }
            off += PICOGAME_ROMFS_SECTOR;
            remaining -= want;
            RUN_BACKGROUND_TASKS;                        // keep USB serviced (source stays locked)
        }
        // Body complete + verified. Recheck the source size (cheap concurrent-write detector
        // for the unlocked fallback), then re-read the header sector and program it last.
        mp_obj_t dest2[4];
        mp_load_method(file, MP_QSTR_seek, dest2);
        dest2[2] = MP_OBJ_NEW_SMALL_INT(0);
        dest2[3] = MP_OBJ_NEW_SMALL_INT(2);
        if (mp_obj_get_int(mp_call_method_n_kw(2, 0, dest2)) != size) {
            mp_raise_OSError(MP_EIO);                    // the file changed while programming
        }
        mp_load_method(file, MP_QSTR_seek, dest2);
        dest2[2] = MP_OBJ_NEW_SMALL_INT(0);
        dest2[3] = MP_OBJ_NEW_SMALL_INT(0);
        mp_call_method_n_kw(2, 0, dest2);
        uint32_t want0 = (uint32_t)size < PICOGAME_ROMFS_SECTOR ? (uint32_t)size : PICOGAME_ROMFS_SECTOR;
        uint32_t got0 = 0;
        while (got0 < want0) {
            mp_uint_t n = mp_stream_rw(file, buf + got0, want0 - got0, &errcode, MP_STREAM_RW_READ);
            if (errcode != 0 || n == 0) {
                mp_raise_OSError(errcode != 0 ? errcode : MP_EIO);
            }
            got0 += (uint32_t)n;
        }
        if (want0 < PICOGAME_ROMFS_SECTOR) {
            memset(buf + want0, 0xff, PICOGAME_ROMFS_SECTOR - want0);
        }
        common_hal_picogame_romfs_write_sector(PICOGAME_ROMFS_XIP_OFFSET, buf);
        if (memcmp((const void *)PICOGAME_ROMFS_BASE_ADDR, buf, PICOGAME_ROMFS_SECTOR) != 0) {
            mp_raise_OSError(MP_EIO);
        }
        nlr_pop();
        mp_stream_close(file);
        if (locked) {
            filesystem_unlock(src_fs);
        }
    } else {
        // common cleanup for every error path above, then re-raise
        mp_stream_close(file);
        if (locked) {
            filesystem_unlock(src_fs);
        }
        nlr_raise(MP_OBJ_FROM_PTR(nlr.ret_val));
    }
    m_del(uint8_t, buf, PICOGAME_ROMFS_SECTOR);
    return mp_obj_new_int_from_uint((uint32_t)size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(picogame_romfs_program_obj, picogame_romfs_program);
#endif // CIRCUITPY_PICOGAME_ROMFS_KB > 0
// ===== end ROMFS-XIP =====

static const mp_rom_map_elem_t picogame_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_picogame) },
    #if CIRCUITPY_PICOGAME_ROMFS_KB > 0
    { MP_ROM_QSTR(MP_QSTR_ROMFS_SUPPORTED), MP_ROM_TRUE },
    { MP_ROM_QSTR(MP_QSTR_romfs_region), MP_ROM_PTR(&picogame_romfs_region_obj) },
    { MP_ROM_QSTR(MP_QSTR_romfs_mount), MP_ROM_PTR(&picogame_romfs_mount_obj) },
    { MP_ROM_QSTR(MP_QSTR_romfs_program), MP_ROM_PTR(&picogame_romfs_program_obj) },
    { MP_ROM_QSTR(MP_QSTR_VfsRom), MP_ROM_PTR(&mp_type_vfs_rom) },                 // also collects the qstr
    #else
    { MP_ROM_QSTR(MP_QSTR_ROMFS_SUPPORTED), MP_ROM_FALSE },
    #endif
    { MP_ROM_QSTR(MP_QSTR_Bitmap), MP_ROM_PTR(&picogame_bitmap_type) },
    { MP_ROM_QSTR(MP_QSTR_Sprite), MP_ROM_PTR(&picogame_sprite_type) },
    #if CIRCUITPY_PICOGAME_FAST_DISPLAY
    { MP_ROM_QSTR(MP_QSTR_Display), MP_ROM_PTR(&picogame_display_type) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_Scene), MP_ROM_PTR(&picogame_scene_type) },
    { MP_ROM_QSTR(MP_QSTR_Tilemap), MP_ROM_PTR(&picogame_tilemap_type) },
    { MP_ROM_QSTR(MP_QSTR_Particles), MP_ROM_PTR(&picogame_particles_type) },
    { MP_ROM_QSTR(MP_QSTR_Canvas), MP_ROM_PTR(&picogame_canvas_type) },
    { MP_ROM_QSTR(MP_QSTR_StripDraw), MP_ROM_PTR(&picogame_stripdraw_type) },
    #if CIRCUITPY_PICOGAME_FRAMEBUFFER
    { MP_ROM_QSTR(MP_QSTR_Framebuffer), MP_ROM_PTR(&picogame_framebuffer_type) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_render), MP_ROM_PTR(&picogame_render_obj) },
    { MP_ROM_QSTR(MP_QSTR_raycast), MP_ROM_PTR(&picogame_raycast_obj) },
    { MP_ROM_QSTR(MP_QSTR_road_edges), MP_ROM_PTR(&picogame_road_edges_obj) },
    #if defined(PICOGAME_CORE1_PROBE)
    { MP_ROM_QSTR(MP_QSTR_core1), MP_ROM_PTR(&picogame_core1_obj) },
    { MP_ROM_QSTR(MP_QSTR_refresh_async), MP_ROM_PTR(&picogame_refresh_async_obj) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_project), MP_ROM_PTR(&picogame_project_obj) },
    // True when the pseudo-3D/math primitives use the hardware-float path (FPU board). Python packs
    // camera/point buffers as float32 when this is set, else as 16.16 fixed int32.
    { MP_ROM_QSTR(MP_QSTR_FPU), MP_ROM_INT(CIRCUITPY_PICOGAME_FPU) },
    { MP_ROM_QSTR(MP_QSTR_invert), MP_ROM_PTR(&picogame_invert_obj) },
    { MP_ROM_QSTR(MP_QSTR_collide), MP_ROM_PTR(&picogame_collide_obj) },
    // Canonical noise = the fixed-point implementation (float retired; see `#if 0` above).
    { MP_ROM_QSTR(MP_QSTR_value2d), MP_ROM_PTR(&picogame_value2d_fx_obj) },
    { MP_ROM_QSTR(MP_QSTR_value1d), MP_ROM_PTR(&picogame_value1d_fx_obj) },
    { MP_ROM_QSTR(MP_QSTR_fbm2d), MP_ROM_PTR(&picogame_fbm2d_fx_obj) },
    { MP_ROM_QSTR(MP_QSTR_fbm1d), MP_ROM_PTR(&picogame_fbm1d_fx_obj) },
    { MP_ROM_QSTR(MP_QSTR_rgb565), MP_ROM_PTR(&picogame_rgb565_obj) },
    { MP_ROM_QSTR(MP_QSTR_RGB565), MP_ROM_INT(PICOGAME_FMT_RGB565) },
    { MP_ROM_QSTR(MP_QSTR_PAL8), MP_ROM_INT(PICOGAME_FMT_PAL8) },
    // Engine API level: bump by 1 whenever the PYTHON-VISIBLE surface grows (new method/property/
    // module function/constant), so picogame-libs can diagnose a too-old firmware up front
    // ("needs API_LEVEL >= N") instead of failing later with a random missing attribute.
    // Level 1 = the 2026-07 surface (post API-freeze + Canvas.text/Framebuffer/StripDraw
    // always_dirty/ROMFS). Older firmwares have no attribute at all -> getattr(pg, "API_LEVEL", 0).
    { MP_ROM_QSTR(MP_QSTR_API_LEVEL), MP_ROM_INT(1) },
    // Build-time capability flag: does THIS board's panel controller support 12-bit RGB444
    // (COLMOD)? The board declares it (it knows its controller); a game reads it to enable
    // Display(rgb444=...) only where it works - one codebase runs on ST7789 AND ILI9341.
    #if CIRCUITPY_PICOGAME_RGB444
    { MP_ROM_QSTR(MP_QSTR_RGB444_SUPPORTED), MP_ROM_TRUE },
    #else
    { MP_ROM_QSTR(MP_QSTR_RGB444_SUPPORTED), MP_ROM_FALSE },
    #endif
    // Build-time default render-strip height (rows). picogame_game.setup() uses it when strip_h is
    // None; games can override per call; a board can override the default in mpconfigboard.h.
    // MEASURED (RP2040): with async DMA double-buffering, SMALL strips overlap render+transfer best ->
    // 8 is both fastest and least RAM (the two w*strip_h*2 buffers shrink). WITHOUT the DMA backend
    // there's no overlap, so a blocking send per strip makes LARGER strips win -> 24.
    #ifndef PICOGAME_STRIP_H
    #if CIRCUITPY_PICOGAME_FAST_DISPLAY
    #define PICOGAME_STRIP_H 8
    #else
    #define PICOGAME_STRIP_H 24
    #endif
    #endif
    { MP_ROM_QSTR(MP_QSTR_STRIP_H), MP_ROM_INT(PICOGAME_STRIP_H) },
};
static MP_DEFINE_CONST_DICT(picogame_module_globals, picogame_module_globals_table);

const mp_obj_module_t picogame_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&picogame_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_picogame, picogame_module);
