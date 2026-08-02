// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// Defined in shared-bindings/picogame/__init__.c (consolidated with Bitmap/Sprite).
extern const mp_obj_type_t picogame_stripdraw_type;

#if CIRCUITPY_PICOGAME_ROMFS_KB > 0
// ROMFS asset-region flash writer, implemented PORT-side (common-hal/picogame/romfs.c).
// Erases + programs ONE flash sector at `flash_offset` (linear address, sector-aligned,
// inside the ROMFS region only - the caller bounds-checks) from a full sector buffer.
// The port supplies the XIP-safe dance (IRQs off, audio DMA paused, cache flushed).
#define PICOGAME_ROMFS_SECTOR (4096)
void common_hal_picogame_romfs_write_sector(uint32_t flash_offset, const uint8_t *buf);
// Erase one sector WITHOUT programming (used to invalidate the header before a rewrite).
void common_hal_picogame_romfs_erase_sector(uint32_t flash_offset);
#endif
