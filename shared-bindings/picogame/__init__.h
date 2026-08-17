// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"


#ifndef CIRCUITPY_PICOGAME_ROMFS
#define CIRCUITPY_PICOGAME_ROMFS (0)
#endif
#ifndef CIRCUITPY_PICOGAME_XIP_MAP
#define CIRCUITPY_PICOGAME_XIP_MAP (0)
#endif
#ifndef PICOGAME_FAT_PROBE
#define PICOGAME_FAT_PROBE (0)      // bench-only FAT layout/repack probes (make PICOGAME_FAT_PROBE=1)
#endif
#if CIRCUITPY_PICOGAME_ROMFS
// ROMFS asset-region flash writer, implemented PORT-side (common-hal/picogame/romfs.c).
// Erases + programs ONE flash sector at `flash_offset` (linear address, sector-aligned,
// inside the ROMFS region only - the caller bounds-checks) from a full sector buffer.
// The port supplies the XIP-safe dance (IRQs off, audio DMA paused, cache flushed).
#define PICOGAME_ROMFS_SECTOR (4096)
void common_hal_picogame_romfs_write_sector(uint32_t flash_offset, const uint8_t *buf);
// Erase one sector WITHOUT programming (used to invalidate the header before a rewrite).
void common_hal_picogame_romfs_erase_sector(uint32_t flash_offset);
#endif

uint8_t picogame_kind_of(mp_obj_t o);
