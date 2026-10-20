// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Command-line options, parsed in linux_main.c.
typedef struct {
    const char *drive_path;     // CIRCUITPY: a directory or a FAT image, absolute
    const char *start_dir;      // the working directory at start; VfsPosix changes it
    size_t drive_size;          // bytes, used only when the image is created
    size_t ram_size;            // bytes of RAM for the port heap
    int debug_port;             // --debug: the debug adapter's TCP port, 0 = none
    const char *exe_dir;        // the program's own directory, where debugadapter.py lives
    char **argv;                // reset re-executes the process with these
} linux_options_t;

extern linux_options_t linux_options;

// The VM's part of the main thread's stack, set up in linux_main.c.
extern uint8_t *linux_stack_bottom;
extern uint8_t *linux_stack_top;

// The process was restarted by reset_cpu() rather than started fresh.
extern bool linux_was_reset;

// Interrupts (interrupts.c): the timer thread, its lock, and waking the VM out of idle.
bool linux_interrupts_init(void);
void linux_irq_disable(void);
void linux_irq_enable(void);
void linux_wake_vm(void);

// Run the workflow's background poll after ms, from a timer interrupt.
void linux_request_workflow_poll(uint32_t ms);

// Block until an interrupt signal arrives or timeout_ms passes (-1 = no timeout).
void linux_wait_for_interrupt(int timeout_ms);

// The console's buttons, in board pin order (GPIO0 = UP ... GPIO12 = MENU).
enum {
    LINUX_BUTTON_UP, LINUX_BUTTON_DOWN, LINUX_BUTTON_LEFT, LINUX_BUTTON_RIGHT,
    LINUX_BUTTON_A, LINUX_BUTTON_B, LINUX_BUTTON_X, LINUX_BUTTON_Y,
    LINUX_BUTTON_START, LINUX_BUTTON_SELECT, LINUX_BUTTON_L, LINUX_BUTTON_R, LINUX_BUTTON_MENU,
    LINUX_BUTTON_COUNT
};

// A button pulls its pin low while pressed, like a button to ground.
void linux_button_set(int button, bool pressed);
void linux_buttons_release_all(void);

// Watch the CIRCUITPY directory and auto-reload when the host changes a file in it; once.
void linux_watch_start(const char *root);

void linux_serial_restore(void);
__attribute__((noreturn)) void linux_power_off(int status);
