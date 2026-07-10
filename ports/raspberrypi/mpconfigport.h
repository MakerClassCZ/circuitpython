// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "hardware/platform_defs.h"

#if PICO_RP2040
#define MICROPY_PY_SYS_PLATFORM             "RP2040"
#endif

#if PICO_RP2350
#define MICROPY_PY_SYS_PLATFORM             "RP2350"

// PSRAM can require more stack space for GC.
#define MICROPY_ALLOC_GC_STACK_SIZE         (128)
#endif

// Setting a non-default value also requires a non-default link.ld
#ifndef CIRCUITPY_FIRMWARE_SIZE
#define CIRCUITPY_FIRMWARE_SIZE (1020 * 1024)
#endif

#define CIRCUITPY_INTERNAL_NVM_SIZE         (4 * 1024)
// This is the XIP address
#define CIRCUITPY_INTERNAL_NVM_START_ADDR   (0x10000000 + CIRCUITPY_FIRMWARE_SIZE)

// picogame ROMFS-XIP asset region (review/romfs-xip-implementation-plan.md): carved BETWEEN the
// NVM sector and the FAT drive, so NVM (game saves) keeps its address across flavors and the
// firmware region is untouched (no linker change). Layout: firmware | NVM 4K | ROMFS | FAT.
// CIRCUITPY_PICOGAME_ROMFS_KB comes from the build (0 = no region, layout identical to before).
#ifndef CIRCUITPY_PICOGAME_ROMFS_KB
#define CIRCUITPY_PICOGAME_ROMFS_KB (0)
#endif
#if CIRCUITPY_PICOGAME_ROMFS_KB > 0
// flash linear offset / XIP bus address / length of the region (4 KB aligned: firmware+NVM are)
#define PICOGAME_ROMFS_XIP_OFFSET (CIRCUITPY_FIRMWARE_SIZE + CIRCUITPY_INTERNAL_NVM_SIZE)
#define PICOGAME_ROMFS_BASE_ADDR  (0x10000000 + PICOGAME_ROMFS_XIP_OFFSET)
#define PICOGAME_ROMFS_LEN        (CIRCUITPY_PICOGAME_ROMFS_KB * 1024)
// The region-safety invariants romfs_program() relies on (exact sector accounting keeps every
// write inside [OFFSET, OFFSET+LEN) ONLY if both ends are 4K-aligned) - enforced at build time:
#if (CIRCUITPY_PICOGAME_ROMFS_KB % 4) != 0
#error "CIRCUITPY_PICOGAME_ROMFS_KB must be a multiple of 4 (4 KB flash sectors)"
#endif
#if (PICOGAME_ROMFS_XIP_OFFSET % 4096) != 0
#error "PICOGAME_ROMFS_XIP_OFFSET must be 4 KB aligned (check CIRCUITPY_FIRMWARE_SIZE / NVM size)"
#endif
// the region implies ROMFS filesystem support (the vfs_rom extmod is self-gated on this);
// IOCTL stays 0: CP lacks MicroPython's rom_ioctl port glue - deploy is pg.romfs_program instead
#ifndef MICROPY_VFS_ROM
#define MICROPY_VFS_ROM (1)
#endif
#ifndef MICROPY_VFS_ROM_IOCTL
#define MICROPY_VFS_ROM_IOCTL (0)
#endif
#endif

// This is the flash linear address
#define CIRCUITPY_CIRCUITPY_DRIVE_START_ADDR (CIRCUITPY_FIRMWARE_SIZE + CIRCUITPY_INTERNAL_NVM_SIZE + CIRCUITPY_PICOGAME_ROMFS_KB * 1024)
#define CIRCUITPY_DEFAULT_STACK_SIZE        (24 * 1024)

#define MICROPY_USE_INTERNAL_PRINTF         (1)

#define CIRCUITPY_PROCESSOR_COUNT           (2)

// For RP2 boards we use a custom way to read BOOTSEL
#define CIRCUITPY_BOOT_BUTTON_NO_GPIO       (1)

#if CIRCUITPY_USB_HOST
#define CIRCUITPY_USB_HOST_INSTANCE 1
#endif

// This also includes mpconfigboard.h.
#include "py/circuitpy_mpconfig.h"

#if CIRCUITPY_CYW43
#define MICROPY_PY_LWIP_ENTER   cyw43_arch_lwip_begin();
#define MICROPY_PY_LWIP_REENTER MICROPY_PY_LWIP_ENTER
#define MICROPY_PY_LWIP_EXIT    cyw43_arch_lwip_end();
#endif

// Protect the background queue with a lock because both cores may modify it.
#include "pico/critical_section.h"
extern critical_section_t background_queue_lock;
#define CALLBACK_CRITICAL_BEGIN (critical_section_enter_blocking(&background_queue_lock))
#define CALLBACK_CRITICAL_END (critical_section_exit(&background_queue_lock))

// Turn some macros into compile-time constants, using enum.
// Some nested macros expand across multiple lines, which is not
// handled by the MP_REGISTER_ROOT_POINTER processing in makeqstrdefs.py.
enum {
    enum_NUM_DMA_CHANNELS = NUM_DMA_CHANNELS,
    enum_NUM_PWM_SLICES = NUM_PWM_SLICES,
};
