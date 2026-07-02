// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "py/objproperty.h"
#include "shared-bindings/picogame/Canvas.h"
#include "shared-bindings/picogame/Bitmap.h"
#include "shared-bindings/fontio/BuiltinFont.h"
#include "shared-module/picogame/Canvas.h"

//| class Canvas:
//|     """A RAM drawing surface (any size) composited as a Scene layer. Draw
//|     primitives into it; only redrawn areas repaint. Colors are wire-order
//|     (use picogame.rgb565)."""
//|
//|     def __init__(self, width: int, height: int, *, transparent: Optional[int] = None,
//|                  buffer: Optional[WriteableBuffer] = None) -> None:
//|         """If ``buffer`` is given (>= width*height*2 bytes, e.g. a memoryview from
//|         picogame_arena), the Canvas draws into it instead of allocating its own -
//|         lets you pre-allocate big surfaces once and dodge heap fragmentation."""
//|         ...
static mp_obj_t picogame_canvas_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_width, ARG_height, ARG_transparent, ARG_buffer };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_transparent, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_buffer, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t w = mp_arg_validate_int_range(args[ARG_width].u_int, 1, 1024, MP_QSTR_width);
    mp_int_t h = mp_arg_validate_int_range(args[ARG_height].u_int, 1, 1024, MP_QSTR_height);

    picogame_canvas_obj_t *self = mp_obj_malloc(picogame_canvas_obj_t, type);
    self->w = w;
    self->h = h;
    self->x = 0;
    self->y = 0;
    if (args[ARG_buffer].u_obj != mp_const_none) {
        // external buffer (e.g. an arena slice) - draw into it, don't allocate/own it
        mp_buffer_info_t bi;
        mp_get_buffer_raise(args[ARG_buffer].u_obj, &bi, MP_BUFFER_RW);
        if (bi.len < (size_t)w * h * 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("buffer too small"));
        }
        if ((uintptr_t)bi.buf & 1) {    // odd byte address -> uint16 pixel stores fault on Cortex-M0+
            mp_raise_ValueError(MP_ERROR_TEXT("buffer must be 2-byte aligned"));
        }
        self->data = bi.buf;
        self->data_obj = args[ARG_buffer].u_obj;   // keep the backing object alive (GC-traced)
    } else {
        self->data = m_new(uint16_t, (size_t)w * h);
        self->data_obj = MP_OBJ_NULL;
    }
    if (args[ARG_transparent].u_obj != mp_const_none) {
        self->transparent = mp_obj_get_int(args[ARG_transparent].u_obj);
        self->has_transparent = true;
    } else {
        self->transparent = 0;
        self->has_transparent = false;
    }
    uint16_t fill = self->has_transparent ? self->transparent : 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        self->data[i] = fill;
    }
    picogame_canvas_dirty_reset(self);
    return MP_OBJ_FROM_PTR(self);
}

static picogame_canvas_obj_t *cv_self(mp_obj_t o) {
    return MP_OBJ_TO_PTR(o);
}

//|     def clear(self, color: int) -> None: ...
static mp_obj_t canvas_clear(mp_obj_t self_in, mp_obj_t color) {
    picogame_canvas_clear(cv_self(self_in), mp_obj_get_int(color));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(canvas_clear_obj, canvas_clear);

//|     def pixel(self, x: int, y: int, color: int) -> None: ...
static mp_obj_t canvas_pixel(size_t n, const mp_obj_t *a) {
    picogame_canvas_pixel(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]), mp_obj_get_int(a[3]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_pixel_obj, 4, 4, canvas_pixel);

//|     def fill_rect(self, x: int, y: int, w: int, h: int, color: int) -> None: ...
static mp_obj_t canvas_fill_rect(size_t n, const mp_obj_t *a) {
    picogame_canvas_fill_rect(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), mp_obj_get_int(a[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_fill_rect_obj, 6, 6, canvas_fill_rect);

//|     def blit(self, bitmap: Bitmap, x: int, y: int, frame: int = 0,
//|              flip_x: bool = False, flip_y: bool = False) -> None:
//|         """Stamp frame `frame` of `bitmap` into the canvas at (x, y), honouring its transparent
//|         key. The retained way to bake an image (icon, portrait, rendered text) into a panel."""
//|         ...
static mp_obj_t canvas_blit(size_t n, const mp_obj_t *a) {
    picogame_bitmap_obj_t *bm = MP_OBJ_TO_PTR(
        mp_arg_validate_type(a[1], &picogame_bitmap_type, MP_QSTR_bitmap));
    int frame = (n > 4) ? mp_obj_get_int(a[4]) : 0;
    bool fx = (n > 5) ? mp_obj_is_true(a[5]) : false;
    bool fy = (n > 6) ? mp_obj_is_true(a[6]) : false;
    picogame_canvas_blit(cv_self(a[0]), bm, mp_obj_get_int(a[2]), mp_obj_get_int(a[3]), frame, fx, fy);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_blit_obj, 4, 7, canvas_blit);

//|     def rect(self, x: int, y: int, w: int, h: int, color: int) -> None: ...
static mp_obj_t canvas_rect(size_t n, const mp_obj_t *a) {
    picogame_canvas_rect(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), mp_obj_get_int(a[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_rect_obj, 6, 6, canvas_rect);

//|     def line(self, x0: int, y0: int, x1: int, y1: int, color: int) -> None: ...
static mp_obj_t canvas_line(size_t n, const mp_obj_t *a) {
    picogame_canvas_line(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), mp_obj_get_int(a[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_line_obj, 6, 6, canvas_line);

//|     def fill_circle(self, cx: int, cy: int, r: int, color: int) -> None: ...
static mp_obj_t canvas_fill_circle(size_t n, const mp_obj_t *a) {
    picogame_canvas_fill_circle(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_fill_circle_obj, 5, 5, canvas_fill_circle);

//|     def circle(self, cx: int, cy: int, r: int, color: int) -> None: ...
static mp_obj_t canvas_circle(size_t n, const mp_obj_t *a) {
    picogame_canvas_circle(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_circle_obj, 5, 5, canvas_circle);

//|     def ring(self, cx: int, cy: int, r: int, thickness: int, color: int) -> None: ...
static mp_obj_t canvas_ring(size_t n, const mp_obj_t *a) {
    picogame_canvas_ring(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), mp_obj_get_int(a[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_ring_obj, 6, 6, canvas_ring);

//|     def triangle(self, x0: int, y0: int, x1: int, y1: int, x2: int, y2: int, color: int) -> None: ...
static mp_obj_t canvas_triangle(size_t n, const mp_obj_t *a) {
    picogame_canvas_triangle(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), mp_obj_get_int(a[5]), mp_obj_get_int(a[6]), mp_obj_get_int(a[7]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_triangle_obj, 8, 8, canvas_triangle);

//|     def fill_triangle(self, x0: int, y0: int, x1: int, y1: int, x2: int, y2: int, color: int) -> None: ...
static mp_obj_t canvas_fill_triangle(size_t n, const mp_obj_t *a) {
    picogame_canvas_fill_triangle(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), mp_obj_get_int(a[5]), mp_obj_get_int(a[6]), mp_obj_get_int(a[7]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_fill_triangle_obj, 8, 8, canvas_fill_triangle);

//|     def ellipse(self, cx: int, cy: int, rx: int, ry: int, color: int) -> None: ...
static mp_obj_t canvas_ellipse(size_t n, const mp_obj_t *a) {
    picogame_canvas_ellipse(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), mp_obj_get_int(a[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_ellipse_obj, 6, 6, canvas_ellipse);

//|     def fill_ellipse(self, cx: int, cy: int, rx: int, ry: int, color: int) -> None: ...
static mp_obj_t canvas_fill_ellipse(size_t n, const mp_obj_t *a) {
    picogame_canvas_fill_ellipse(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), mp_obj_get_int(a[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_fill_ellipse_obj, 6, 6, canvas_fill_ellipse);

//|     def fill_round_rect(self, x: int, y: int, w: int, h: int, r: int, color: int) -> None: ...
static mp_obj_t canvas_fill_round_rect(size_t n, const mp_obj_t *a) {
    picogame_canvas_fill_round_rect(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), mp_obj_get_int(a[5]), mp_obj_get_int(a[6]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_fill_round_rect_obj, 7, 7, canvas_fill_round_rect);

//|     def frame3d(self, x: int, y: int, w: int, h: int, light: int, dark: int) -> None: ...
static mp_obj_t canvas_frame3d(size_t n, const mp_obj_t *a) {
    picogame_canvas_frame3d(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), mp_obj_get_int(a[5]), mp_obj_get_int(a[6]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_frame3d_obj, 7, 7, canvas_frame3d);

//|     def text(self, x: int, y: int, s: str, fg: int, font: fontio.BuiltinFont, bg: int | None = None) -> None:
//|         """Composite ``s`` into the surface in C, rasterizing each glyph from ``font`` on the fly -
//|         no Python glyph cache, no per-call Bitmap/Sprite (zero retained text RAM, no fragmentation).
//|         If ``bg`` is given the glyph background is filled too; otherwise it is transparent. Inside a
//|         StripDraw callback the ``view`` is a Canvas pointing at the live strip, so ``view.text(...)``
//|         draws immediate-mode HUD/screen text straight into the frame."""
static mp_obj_t canvas_text(size_t n, const mp_obj_t *a) {
    const char *s = mp_obj_str_get_str(a[3]);
    mp_int_t fg = mp_obj_get_int(a[4]);
    const void *font = MP_OBJ_TO_PTR(mp_arg_validate_type(a[5], &fontio_builtinfont_type, MP_QSTR_font));
    bool has_bg = (n >= 7) && (a[6] != mp_const_none);
    uint16_t bg = has_bg ? (uint16_t)mp_obj_get_int(a[6]) : 0;
    picogame_canvas_text(cv_self(a[0]), mp_obj_get_int(a[1]), mp_obj_get_int(a[2]),
        s, (uint16_t)fg, bg, has_bg, font);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_text_obj, 6, 7, canvas_text);

//|     def move(self, x: int, y: int) -> None: ...
static mp_obj_t canvas_move(mp_obj_t self_in, mp_obj_t x_in, mp_obj_t y_in) {
    picogame_canvas_obj_t *self = cv_self(self_in);
    int nx = mp_obj_get_int(x_in), ny = mp_obj_get_int(y_in);
    if (nx == self->x && ny == self->y) {
        return mp_const_none;                        // unchanged -> avoid an avoidable repaint
    }
    // dirty old + new extents so the move repaints both
    picogame_canvas_dirty_union(self, self->x, self->y, self->x + self->w, self->y + self->h);
    self->x = nx;
    self->y = ny;
    picogame_canvas_dirty_union(self, self->x, self->y, self->x + self->w, self->y + self->h);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(canvas_move_obj, canvas_move);

//|     x: int
//|     y: int
//|     """Current pixel position of the canvas top-left (read-only; set with move())."""
//|     width: int
//|     height: int
//|     """Surface size in pixels (read-only)."""
static mp_obj_t canvas_get_x(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(cv_self(self_in)->x);
}
static MP_DEFINE_CONST_FUN_OBJ_1(canvas_get_x_obj, canvas_get_x);
MP_PROPERTY_GETTER(canvas_x_obj, (mp_obj_t)&canvas_get_x_obj);

static mp_obj_t canvas_get_y(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(cv_self(self_in)->y);
}
static MP_DEFINE_CONST_FUN_OBJ_1(canvas_get_y_obj, canvas_get_y);
MP_PROPERTY_GETTER(canvas_y_obj, (mp_obj_t)&canvas_get_y_obj);

static mp_obj_t canvas_get_width(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(cv_self(self_in)->w);
}
static MP_DEFINE_CONST_FUN_OBJ_1(canvas_get_width_obj, canvas_get_width);
MP_PROPERTY_GETTER(canvas_width_obj, (mp_obj_t)&canvas_get_width_obj);

static mp_obj_t canvas_get_height(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(cv_self(self_in)->h);
}
static MP_DEFINE_CONST_FUN_OBJ_1(canvas_get_height_obj, canvas_get_height);
MP_PROPERTY_GETTER(canvas_height_obj, (mp_obj_t)&canvas_get_height_obj);

static const mp_rom_map_elem_t picogame_canvas_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&canvas_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_x), MP_ROM_PTR(&canvas_x_obj) },
    { MP_ROM_QSTR(MP_QSTR_y), MP_ROM_PTR(&canvas_y_obj) },
    { MP_ROM_QSTR(MP_QSTR_width), MP_ROM_PTR(&canvas_width_obj) },
    { MP_ROM_QSTR(MP_QSTR_height), MP_ROM_PTR(&canvas_height_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&canvas_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_rect), MP_ROM_PTR(&canvas_fill_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_blit), MP_ROM_PTR(&canvas_blit_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&canvas_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&canvas_line_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_circle), MP_ROM_PTR(&canvas_fill_circle_obj) },
    { MP_ROM_QSTR(MP_QSTR_circle), MP_ROM_PTR(&canvas_circle_obj) },
    { MP_ROM_QSTR(MP_QSTR_ring), MP_ROM_PTR(&canvas_ring_obj) },
    { MP_ROM_QSTR(MP_QSTR_triangle), MP_ROM_PTR(&canvas_triangle_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_triangle), MP_ROM_PTR(&canvas_fill_triangle_obj) },
    { MP_ROM_QSTR(MP_QSTR_ellipse), MP_ROM_PTR(&canvas_ellipse_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_ellipse), MP_ROM_PTR(&canvas_fill_ellipse_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_round_rect), MP_ROM_PTR(&canvas_fill_round_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_frame3d), MP_ROM_PTR(&canvas_frame3d_obj) },
    { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&canvas_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_move), MP_ROM_PTR(&canvas_move_obj) },
};
static MP_DEFINE_CONST_DICT(picogame_canvas_locals_dict, picogame_canvas_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    picogame_canvas_type,
    MP_QSTR_Canvas,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, picogame_canvas_make_new,
    locals_dict, &picogame_canvas_locals_dict
    );
