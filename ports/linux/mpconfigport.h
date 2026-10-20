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

// The Python call stack: a board's 2 KB would end a traced program early, since the debugger
// adds frames of its own above every call of the user's.
#define CIRCUITPY_PYSTACK_SIZE                  (256 * 1024)

// sys.settrace, the hook a debugger steps and inspects with; a host can afford its cost.
#define MICROPY_PY_SYS_SETTRACE                 (1)

// The register helper's x86-64 assembly is for the System V ABI; Windows uses setjmp instead.
#ifdef _WIN32
#define MICROPY_GCREGS_SETJMP                   (1)
#endif

// CIRCUITPY can be a host directory (VfsPosix) as well as a FAT image (--drive).
#ifndef CIRCUITPY_FILESYSTEM_POSIX
#define CIRCUITPY_FILESYSTEM_POSIX              (1)
#endif
// VfsPosix's statvfs, for os.statvfs() on a directory CIRCUITPY; Windows has no statvfs().
#ifdef _WIN32
#define MICROPY_PY_OS_STATVFS                   (0)
#else
#define MICROPY_PY_OS_STATVFS                   (1)
#endif

// Also includes mpconfigboard.h
#include "py/circuitpy_mpconfig.h"
