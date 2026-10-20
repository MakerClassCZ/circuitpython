// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Command-line options, parsed in linux_main.c.
typedef struct {
    const char *drive_path;     // the CIRCUITPY image
    size_t drive_size;          // bytes, used only when the image is created
    size_t ram_size;            // bytes of RAM for the port heap
    char **argv;                // reset re-executes the process with these
} linux_options_t;

extern linux_options_t linux_options;

// The VM thread's stack, allocated in linux_main.c.
extern uint8_t *linux_stack_bottom;
extern uint8_t *linux_stack_top;

// The VM thread; other threads wake it like an interrupt would.
extern pthread_t linux_vm_thread;

// The process was restarted by reset_cpu() rather than started fresh.
extern bool linux_was_reset;
void linux_wake_vm(void);

// Block until an interrupt signal arrives or timeout_ms passes (-1 = no timeout).
void linux_wait_for_interrupt(int timeout_ms);

void linux_serial_restore(void);
__attribute__((noreturn)) void linux_power_off(int status);
