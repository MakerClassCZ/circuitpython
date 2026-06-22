// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "py/objproperty.h"
#include "py/objtuple.h"
#include "py/objlist.h"
#include "shared-bindings/picogame/Scene.h"
#include "shared-bindings/picogame/__init__.h"
#include "shared-bindings/picogame/Sprite.h"
#include "shared-bindings/picogame/Tilemap.h"
#include "shared-bindings/picogame/Particles.h"
#include "shared-bindings/picogame/Canvas.h"
#include "shared-bindings/busdisplay/BusDisplay.h"
#if CIRCUITPY_PICOGAME_FAST_DISPLAY
#include "shared-bindings/picogame/Display.h"
#endif
#include "shared-module/picogame/Scene.h"
#include "shared-module/picogame/__init__.h"
#include "shared-module/picogame/Tilemap.h"
#include "shared-module/picogame/Particles.h"
#include "shared-module/picogame/Canvas.h"
#if CIRCUITPY_PICOGAME_FAST_DISPLAY
#include "common-hal/picogame/Display.h"
#endif

#define SCENE_INIT_CAP 8
#define PICOGAME_MAX_DIRTY_RECTS 6   // separate regions repainted per refresh

//| class Scene:
//|     """Retained-mode scene with dirty-rectangle rendering. Add sprites and
//|     tilemaps once (tilemaps first = bottom layer), mutate them each frame,
//|     then call :py:meth:`refresh` - only the changed region is repainted.
//|     Backed by a fast :py:class:`Display`."""
//|
//|     def __init__(
//|         self,
//|         display: Display,
//|         buffer_a: WriteableBuffer,
//|         buffer_b: WriteableBuffer,
//|         *,
//|         background: int = 0,
//|     ) -> None: ...
static mp_obj_t picogame_scene_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_display, ARG_buffer_a, ARG_buffer_b, ARG_background,
           ARG_top, ARG_bottom, ARG_left, ARG_right };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_display, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_buffer_a, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_buffer_b, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_background, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_top, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },      // reserved border insets:
        { MP_QSTR_bottom, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },   // the scene renders only the
        { MP_QSTR_left, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },     // inner play rect; the app
        { MP_QSTR_right, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },    // owns the border around it
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Accept either the fast picogame.Display (DMA, where available) or a plain
    // busdisplay (rendered via the portable bus.send fallback -> cross-port).
    mp_obj_t disp = args[ARG_display].u_obj;
    bool fast = false;
    #if CIRCUITPY_PICOGAME_FAST_DISPLAY
    if (mp_obj_is_type(disp, &picogame_display_type)) {
        fast = true;
    } else
    #endif
    if (!mp_obj_is_type(disp, &busdisplay_busdisplay_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected a Display"));
    }
    mp_buffer_info_t tmp;
    mp_get_buffer_raise(args[ARG_buffer_a].u_obj, &tmp, MP_BUFFER_WRITE);
    mp_get_buffer_raise(args[ARG_buffer_b].u_obj, &tmp, MP_BUFFER_WRITE);

    picogame_scene_obj_t *self = mp_obj_malloc(picogame_scene_obj_t, type);
    self->display = disp;
    self->fast = fast;
    self->buf_a = args[ARG_buffer_a].u_obj;
    self->buf_b = args[ARG_buffer_b].u_obj;
    self->background = args[ARG_background].u_int;
    self->count = 0;
    self->cap = SCENE_INIT_CAP;
    self->items = m_new(mp_obj_t, SCENE_INIT_CAP);
    self->kinds = m_new(uint8_t, SCENE_INIT_CAP);
    self->snap = m_new(picogame_snapshot_t, SCENE_INIT_CAP);
    self->cleared = false;
    self->ox = 0;
    self->oy = 0;
    self->top = args[ARG_top].u_int;
    self->bottom = args[ARG_bottom].u_int;
    self->left = args[ARG_left].u_int;
    self->right = args[ARG_right].u_int;
    mp_obj_t zeros[4] = {
        MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0),
        MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0),
    };
    self->dirty_rect = mp_obj_new_list(4, zeros);   // reused every refresh
    return MP_OBJ_FROM_PTR(self);
}

// On a full repaint, sync sprite snapshots to current and drain the layer
// dirties (tilemap/particles/canvas) so they don't re-report a stale region.
static void snapshot_sync(picogame_scene_obj_t *self) {
    int a, b, c, d;
    for (uint16_t i = 0; i < self->count; i++) {
        uint8_t kind = self->kinds[i] & PICOGAME_KIND_MASK;
        if (kind == PICOGAME_KIND_TILEMAP) {
            picogame_tilemap_take_dirty(MP_OBJ_TO_PTR(self->items[i]), &a, &b, &c, &d);
        } else if (kind == PICOGAME_KIND_PARTICLES) {
            picogame_particles_take_dirty(MP_OBJ_TO_PTR(self->items[i]), &a, &b, &c, &d);
        } else if (kind == PICOGAME_KIND_CANVAS) {
            picogame_canvas_take_dirty(MP_OBJ_TO_PTR(self->items[i]), &a, &b, &c, &d);
        } else if (kind == PICOGAME_KIND_STRIPDRAW) {
            // Immediate-mode layer: no retained state to snapshot/drain.
        } else {
            picogame_sprite_obj_t *s = MP_OBJ_TO_PTR(self->items[i]);
            picogame_bitmap_obj_t *bm = s->bitmap;
            int ax1, ay1, ax2, ay2;
            picogame_sprite_aabb(s, &ax1, &ay1, &ax2, &ay2);
            self->snap[i].x = ax1;
            self->snap[i].y = ay1;
            self->snap[i].w = ax2 - ax1;
            self->snap[i].h = ay2 - ay1;
            self->snap[i].bitmap = (void *)bm;
            self->snap[i].frame = s->frame;
            self->snap[i].flags = s->flags;
            self->snap[i].scale = s->scale;
            self->snap[i].angle = s->angle;
            self->snap[i].seq = s->seq;
            self->snap[i].dither = s->dither;
            self->snap[i].flash_color = s->flash_color;
        }
    }
}

//|     def add(self, item: Union[Sprite, Tilemap], *, fixed: bool = False) -> Union[Sprite, Tilemap]:
//|         """Add a sprite/tilemap/particles/canvas (drawn next refresh; insertion
//|         order is bottom-to-top). fixed=True pins the item to the screen (it ignores
//|         the view offset) - use it for HUD / score / dialog over a scrolling world.
//|         Returns the added item, so you can write ``spr = scene.add(Sprite(...))``."""
//|         ...
static void scene_add_one(picogame_scene_obj_t *self, mp_obj_t item_in, bool fixed) {
    uint8_t kind;
    if (mp_obj_is_type(item_in, &picogame_sprite_type)) {
        kind = PICOGAME_KIND_SPRITE;
    } else if (mp_obj_is_type(item_in, &picogame_tilemap_type)) {
        kind = PICOGAME_KIND_TILEMAP;
    } else if (mp_obj_is_type(item_in, &picogame_particles_type)) {
        kind = PICOGAME_KIND_PARTICLES;
    } else if (mp_obj_is_type(item_in, &picogame_canvas_type)) {
        kind = PICOGAME_KIND_CANVAS;
    } else if (mp_obj_is_type(item_in, &picogame_stripdraw_type)) {
        kind = PICOGAME_KIND_STRIPDRAW;
    } else {
        mp_raise_TypeError(MP_ERROR_TEXT("expected a Sprite, Tilemap, Particles, Canvas or StripDraw"));
    }
    if (fixed) {
        kind |= PICOGAME_KIND_FIXED;
    }
    if (self->count >= self->cap) {
        if (self->cap >= 0x8000) {                 // next doubling overflows uint16_t -> m_renew(0) shrink
            mp_raise_RuntimeError(MP_ERROR_TEXT("scene full"));
        }
        uint16_t new_cap = self->cap * 2;
        self->items = m_renew(mp_obj_t, self->items, self->cap, new_cap);
        self->kinds = m_renew(uint8_t, self->kinds, self->cap, new_cap);
        self->snap = m_renew(picogame_snapshot_t, self->snap, self->cap, new_cap);
        self->cap = new_cap;
    }
    self->items[self->count] = item_in;
    self->kinds[self->count] = kind;
    // Snapshot starts "invisible" so a new sprite is detected as changed and drawn.
    self->snap[self->count].x = 0;
    self->snap[self->count].y = 0;
    self->snap[self->count].w = 0;
    self->snap[self->count].h = 0;
    self->snap[self->count].bitmap = NULL;
    self->snap[self->count].frame = 0;
    self->snap[self->count].flags = 0;
    // init the rest of the diffed fields too (m_renew doesn't zero) so the first dirty-diff
    // doesn't compare against garbage scale/angle/seq/dither
    self->snap[self->count].scale = 0;
    self->snap[self->count].angle = 0;
    self->snap[self->count].seq = 0;
    self->snap[self->count].dither = 0;
    self->snap[self->count].flash_color = 0;
    self->count++;
}

static mp_obj_t picogame_scene_add(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_item, ARG_fixed };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_item, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_fixed, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    scene_add_one(MP_OBJ_TO_PTR(pos_args[0]), args[ARG_item].u_obj, args[ARG_fixed].u_bool);
    return args[ARG_item].u_obj;   // constructive: return the added item for `x = scene.add(...)`
}
static MP_DEFINE_CONST_FUN_OBJ_KW(picogame_scene_add_obj, 2, picogame_scene_add);

//|     def add_all(self, items: Iterable[Union[Sprite, Tilemap]]) -> None:
//|         """Add several sprites/tilemaps at once (bottom-to-top in order)."""
//|         ...
static mp_obj_t picogame_scene_add_all(mp_obj_t self_in, mp_obj_t iterable) {
    picogame_scene_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t iter = mp_getiter(iterable, NULL);
    mp_obj_t item;
    while ((item = mp_iternext(iter)) != MP_OBJ_STOP_ITERATION) {
        scene_add_one(self, item, false);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(picogame_scene_add_all_obj, picogame_scene_add_all);

//|     def invalidate(self) -> None:
//|         """Force a full-screen repaint on the next refresh (e.g. on scene change)."""
//|         ...
static mp_obj_t picogame_scene_invalidate(mp_obj_t self_in) {
    ((picogame_scene_obj_t *)MP_OBJ_TO_PTR(self_in))->cleared = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(picogame_scene_invalidate_obj, picogame_scene_invalidate);

//|     def set_view(self, ox: int, oy: int) -> None:
//|         """Set the view offset = screen position of the scene origin. Use a
//|         constant offset to centre a small game, or update it each frame to
//|         scroll (which repaints the whole screen)."""
//|         ...
static mp_obj_t picogame_scene_set_view(mp_obj_t self_in, mp_obj_t ox_in, mp_obj_t oy_in) {
    picogame_scene_obj_t *self = MP_OBJ_TO_PTR(self_in);
    int ox = mp_obj_get_int(ox_in);
    int oy = mp_obj_get_int(oy_in);
    if (ox != self->ox || oy != self->oy) {
        self->ox = ox;
        self->oy = oy;
        self->cleared = false;   // the whole view shifted -> full repaint next refresh
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(picogame_scene_set_view_obj, picogame_scene_set_view);

//|     view: Tuple[int, int]
//|     """The current view offset (ox, oy) as set by set_view() (read-only)."""
static mp_obj_t picogame_scene_get_view(mp_obj_t self_in) {
    picogame_scene_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t t[2] = { MP_OBJ_NEW_SMALL_INT(self->ox), MP_OBJ_NEW_SMALL_INT(self->oy) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(picogame_scene_get_view_obj, picogame_scene_get_view);
MP_PROPERTY_GETTER(picogame_scene_view_obj, (mp_obj_t)&picogame_scene_get_view_obj);

//|     display: Union[Display, busdisplay.BusDisplay]
//|     """The backend this Scene was built with (a picogame.Display or a busdisplay),
//|     read-only - handy for one-off picogame.render() / Display.render() calls."""
static mp_obj_t picogame_scene_get_display(mp_obj_t self_in) {
    return ((picogame_scene_obj_t *)MP_OBJ_TO_PTR(self_in))->display;
}
static MP_DEFINE_CONST_FUN_OBJ_1(picogame_scene_get_display_obj, picogame_scene_get_display);
MP_PROPERTY_GETTER(picogame_scene_display_obj, (mp_obj_t)&picogame_scene_get_display_obj);

//|     def refresh(self) -> Optional[list]:
//|         """Diff against the previous frame and repaint only the changed region(s).
//|         Returns the bounding dirty rect as a REUSED list [x1, y1, x2, y2] (read it
//|         immediately; it's overwritten next call), or None if nothing changed."""
//|         ...
static mp_obj_t picogame_scene_refresh(mp_obj_t self_in) {
    picogame_scene_obj_t *self = MP_OBJ_TO_PTR(self_in);

    // Resolve the underlying busdisplay from either backend.
    busdisplay_busdisplay_obj_t *bd;
    #if CIRCUITPY_PICOGAME_FAST_DISPLAY
    picogame_display_obj_t *disp = NULL;
    if (self->fast) {
        disp = MP_OBJ_TO_PTR(self->display);
        bd = disp->display;
    } else
    #endif
    {
        bd = MP_OBJ_TO_PTR(self->display);
    }
    int w = bd->core.width;
    int h = bd->core.height;

    picogame_rect_t rects[PICOGAME_MAX_DIRTY_RECTS];
    int nr;
    if (self->cleared) {
        nr = picogame_scene_compute_dirty_rects(self->items, self->kinds, self->snap,
            self->count, w, h, self->ox, self->oy, rects, PICOGAME_MAX_DIRTY_RECTS);
        if (nr == 0) {
            return mp_const_none;
        }
    } else {
        rects[0].x1 = 0;
        rects[0].y1 = 0;
        rects[0].x2 = w;
        rects[0].y2 = h;
        nr = 1;
        snapshot_sync(self);
        self->cleared = true;
    }

    // Clip every dirty rect to the play rect [left, w-right) x [top, h-bottom); the
    // reserved border is the app's, so the scene never paints into it. Drop empty rects.
    int pa_x1 = self->left;
    int pa_x2 = w - self->right;
    int pa_y1 = self->top;
    int pa_y2 = h - self->bottom;
    int kept = 0;
    for (int i = 0; i < nr; i++) {
        int rx1 = rects[i].x1 < pa_x1 ? pa_x1 : rects[i].x1;
        int rx2 = rects[i].x2 > pa_x2 ? pa_x2 : rects[i].x2;
        int ry1 = rects[i].y1 < pa_y1 ? pa_y1 : rects[i].y1;
        int ry2 = rects[i].y2 > pa_y2 ? pa_y2 : rects[i].y2;
        if (rx1 >= rx2 || ry1 >= ry2) {
            continue;
        }
        rects[kept].x1 = rx1;
        rects[kept].y1 = ry1;
        rects[kept].x2 = rx2;
        rects[kept].y2 = ry2;
        kept++;
    }
    nr = kept;
    if (nr == 0) {
        return mp_const_none;
    }

    mp_buffer_info_t a, b;
    mp_get_buffer_raise(self->buf_a, &a, MP_BUFFER_WRITE);
    mp_get_buffer_raise(self->buf_b, &b, MP_BUFFER_WRITE);
    #if CIRCUITPY_PICOGAME_FAST_DISPLAY
    size_t buf_pixels = (a.len < b.len ? a.len : b.len) / 2;  // fast path double-buffers
    #endif

    // Render each dirty rect independently; return their bounding union (kept for
    // the existing "dirty WxH" debug prints).
    int ux1 = w, uy1 = h, ux2 = 0, uy2 = 0;
    for (int i = 0; i < nr; i++) {
        #if CIRCUITPY_PICOGAME_FAST_DISPLAY
        if (self->fast) {
            common_hal_picogame_display_render(disp, self->items, self->kinds, self->count,
                (uint16_t *)a.buf, (uint16_t *)b.buf, buf_pixels,
                rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2,
                self->background, self->ox, self->oy);
        } else
        #endif
        {
            // Portable single-buffer bus.send path (any CircuitPython port).
            picogame_render_region(bd, self->items, self->kinds, self->count,
                (uint16_t *)a.buf, a.len / 2,
                rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2,
                self->background, self->ox, self->oy);
        }
        if (rects[i].x1 < ux1) {
            ux1 = rects[i].x1;
        }
        if (rects[i].y1 < uy1) {
            uy1 = rects[i].y1;
        }
        if (rects[i].x2 > ux2) {
            ux2 = rects[i].x2;
        }
        if (rects[i].y2 > uy2) {
            uy2 = rects[i].y2;
        }
    }

    // Update the reusable list in place - no per-frame tuple allocation.
    mp_obj_list_store(self->dirty_rect, MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(ux1));
    mp_obj_list_store(self->dirty_rect, MP_OBJ_NEW_SMALL_INT(1), MP_OBJ_NEW_SMALL_INT(uy1));
    mp_obj_list_store(self->dirty_rect, MP_OBJ_NEW_SMALL_INT(2), MP_OBJ_NEW_SMALL_INT(ux2));
    mp_obj_list_store(self->dirty_rect, MP_OBJ_NEW_SMALL_INT(3), MP_OBJ_NEW_SMALL_INT(uy2));
    return self->dirty_rect;
}
static MP_DEFINE_CONST_FUN_OBJ_1(picogame_scene_refresh_obj, picogame_scene_refresh);

static const mp_rom_map_elem_t picogame_scene_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_add), MP_ROM_PTR(&picogame_scene_add_obj) },
    { MP_ROM_QSTR(MP_QSTR_add_all), MP_ROM_PTR(&picogame_scene_add_all_obj) },
    { MP_ROM_QSTR(MP_QSTR_refresh), MP_ROM_PTR(&picogame_scene_refresh_obj) },
    { MP_ROM_QSTR(MP_QSTR_invalidate), MP_ROM_PTR(&picogame_scene_invalidate_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_view), MP_ROM_PTR(&picogame_scene_set_view_obj) },
    { MP_ROM_QSTR(MP_QSTR_view), MP_ROM_PTR(&picogame_scene_view_obj) },
    { MP_ROM_QSTR(MP_QSTR_display), MP_ROM_PTR(&picogame_scene_display_obj) },
};
static MP_DEFINE_CONST_DICT(picogame_scene_locals_dict, picogame_scene_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    picogame_scene_type,
    MP_QSTR_Scene,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, picogame_scene_make_new,
    locals_dict, &picogame_scene_locals_dict
    );
