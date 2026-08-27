// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "shared-module/picogame/__init__.h"   // picogame_framebuffer_obj_t

#if CIRCUITPY_PICOGAME_FRAMEBUFFER
extern const mp_obj_type_t picogame_framebuffer_type;

// Shared internal-SRAM compose strips owned by Framebuffer.c. Returns the current strip (and
// arms the core1 split partner), or NULL when none is allocated - the caller then uses the
// Framebuffer's own heap buffer.
uint16_t *picogame_fb_scratch_use(void);
#endif
