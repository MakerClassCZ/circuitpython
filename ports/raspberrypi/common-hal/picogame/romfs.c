// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// picogame ROMFS asset region: the port-side flash writer for pg.romfs_program().
// One sector at a time, wrapped in the same XIP-safe dance the CIRCUITPY FAT driver
// uses (supervisor_flash_pre_write: IRQs off + audio DMA paused; the pico-sdk flash
// calls flush the XIP cache themselves, so readback-verify via XIP sees fresh data).

#include "py/mpconfig.h"

#if CIRCUITPY_PICOGAME_ROMFS_KB > 0

#include "shared-bindings/picogame/__init__.h"
#include "supervisor/internal_flash.h"
#include "hardware/flash.h"

void common_hal_picogame_romfs_write_sector(uint32_t flash_offset, const uint8_t *buf) {
    supervisor_flash_pre_write();
    flash_range_erase(flash_offset, PICOGAME_ROMFS_SECTOR);
    flash_range_program(flash_offset, buf, PICOGAME_ROMFS_SECTOR);
    supervisor_flash_post_write();
}

#endif // CIRCUITPY_PICOGAME_ROMFS_KB > 0
