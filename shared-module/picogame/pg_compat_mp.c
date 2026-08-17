// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT
//
// pg_compat_mp.c — MicroPython-only implementations of the CircuitPython core-API delta
// declared in pg_compat.h. On CircuitPython these come from py/argcheck.c / objexcept /
// obj.c, so this file is compiled ONLY in a MicroPython build (it is never added to the
// CircuitPython source list, and the `#if !defined(CIRCUITPY)` guard makes it inert there).
// Bodies are the exact CircuitPython behaviour (from py/argcheck.c).
#if !defined(CIRCUITPY)

#include <stdarg.h>   // BEFORE py/obj.h: MicroPython guards mp_obj_new_exception_msg_vlist
                      // behind `#ifdef va_start`, so va_start must already be defined.
#include "py/runtime.h"
#include "py/obj.h"
#include "shared-module/picogame/pg_compat.h"

// CP varg raisers (MP has mp_obj_new_exception_msg_vlist; CP wraps it as these two names).
NORETURN void mp_raise_ValueError_varg(mp_rom_error_text_t fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mp_obj_t exc = mp_obj_new_exception_msg_vlist(&mp_type_ValueError, fmt, args);
    va_end(args);
    nlr_raise(exc);
}

NORETURN void mp_raise_TypeError_varg(mp_rom_error_text_t fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mp_obj_t exc = mp_obj_new_exception_msg_vlist(&mp_type_TypeError, fmt, args);
    va_end(args);
    nlr_raise(exc);
}

// CP argcheck helpers (verbatim behaviour from CircuitPython py/argcheck.c).
mp_int_t mp_arg_validate_int_min(mp_int_t i, mp_int_t min, qstr arg_name) {
    if (i < min) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be >= %d"), arg_name, min);
    }
    return i;
}

mp_int_t mp_arg_validate_int_range(mp_int_t i, mp_int_t min, mp_int_t max, qstr arg_name) {
    if (i < min || i > max) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be %d-%d"), arg_name, min, max);
    }
    return i;
}

mp_obj_t mp_arg_validate_type(mp_obj_t obj, const mp_obj_type_t *type, qstr arg_name) {
    if (!mp_obj_is_type(obj, type)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q, not %q"),
            arg_name, type->name, mp_obj_get_type(obj)->name);
    }
    return obj;
}

// CP's zeroed-bytearray helper on top of MP's public bytearray-by-ref (m_new0 gives
// GC-managed, zero-initialised storage).
mp_obj_t mp_obj_new_bytearray_of_zeros(size_t n) {
    byte *buf = m_new0(byte, n);
    return mp_obj_new_bytearray_by_ref(n, buf);
}

#endif // !CIRCUITPY
