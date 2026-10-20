// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

// The VM runs on its own thread with a stack of this size (linux_main.c).
#define CIRCUITPY_DEFAULT_STACK_SIZE            (1024 * 1024)

// The object representation packs floats into 32-bit words; on a 64-bit host use the plain one.
#if UINTPTR_MAX > 0xffffffffu
#define MICROPY_OBJ_REPR                        (MICROPY_OBJ_REPR_A)
#endif

// The processor the binary was built for.
#if defined(__x86_64__)
#define MICROPY_HW_MCU_NAME                     "x86_64"
#elif defined(__i386__)
#define MICROPY_HW_MCU_NAME                     "i386"
#elif defined(__aarch64__)
#define MICROPY_HW_MCU_NAME                     "aarch64"
#elif defined(__arm__)
#define MICROPY_HW_MCU_NAME                     "arm"
#elif defined(__riscv)
#define MICROPY_HW_MCU_NAME                     "riscv"
#else
#define MICROPY_HW_MCU_NAME                     "unknown"
#endif

// Also includes mpconfigboard.h
#include "py/circuitpy_mpconfig.h"
