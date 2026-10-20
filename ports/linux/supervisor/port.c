// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The supervisor on a POSIX host: the process, its memory and its clock. Interrupts live in
// interrupts.c.

#include <errno.h>
#include <execinfo.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "supervisor/board.h"
#include "supervisor/filesystem.h"
#include "supervisor/port.h"
#include "supervisor/shared/safe_mode.h"
#include "supervisor/shared/tick.h"
#include "shared-bindings/microcontroller/__init__.h"
#include "shared-bindings/rtc/__init__.h"
#if CIRCUITPY_AUDIOIO
#include "common-hal/audioio/AudioOut.h"
#endif
#include "common-hal/microcontroller/Pin.h"

#include "linux_port.h"

#define SAVED_WORD_ENV "CIRCUITPY_LINUX_SAVED_WORD"

bool linux_was_reset;

static size_t _ram_used;
static uint32_t _saved_word;
static struct timespec _start;

// A crash is the host's hard fault: report where on stderr, then restart into safe mode.
static void _on_fault(int sig) {
    static const char msg[] = "\r\ncircuitpython: hard fault, backtrace:\r\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    void *frames[32];
    backtrace_symbols_fd(frames, backtrace(frames, 32), STDERR_FILENO);
    reset_into_safe_mode(SAFE_MODE_HARD_FAULT);
}

safe_mode_t port_init(void) {
    clock_gettime(CLOCK_MONOTONIC, &_start);

    // reset_cpu() re-executes the process; the safe mode word rides along in the environment.
    const char *word = getenv(SAVED_WORD_ENV);
    if (word != NULL) {
        _saved_word = strtoul(word, NULL, 0);
        linux_was_reset = true;
        unsetenv(SAVED_WORD_ENV);
    }

    struct sigaction fault = {0};
    fault.sa_handler = _on_fault;
    fault.sa_flags = SA_RESETHAND | SA_NODEFER;
    sigaction(SIGSEGV, &fault, NULL);
    sigaction(SIGBUS, &fault, NULL);
    sigaction(SIGILL, &fault, NULL);
    sigaction(SIGFPE, &fault, NULL);
    sigset_t faults;
    sigemptyset(&faults);
    sigaddset(&faults, SIGSEGV);
    sigaddset(&faults, SIGBUS);
    sigaddset(&faults, SIGILL);
    sigaddset(&faults, SIGFPE);
    pthread_sigmask(SIG_UNBLOCK, &faults, NULL);

    return linux_interrupts_init() ? SAFE_MODE_NONE : SAFE_MODE_PROGRAMMATIC;
}

void reset_port(void) {
    #if CIRCUITPY_RTC
    rtc_reset();
    #endif
    #if CIRCUITPY_AUDIOIO
    audioout_reset();
    #endif
    reset_all_pins();
    #if CIRCUITPY_FILESYSTEM_POSIX
    // Once CIRCUITPY is up, a directory drive starts reloading on host edits.
    if (filesystem_present() && supervisor_filesystem_posix_root() != NULL) {
        linux_watch_start(linux_options.drive_path);
    }
    #endif
}

void reset_cpu(void) {
    linux_serial_restore();
    char word[16];
    snprintf(word, sizeof(word), "%u", (unsigned)_saved_word);
    setenv(SAVED_WORD_ENV, word, 1);
    // The relative paths of the command line are relative to where the process started.
    if (chdir(linux_options.start_dir) != 0) {
        perror("circuitpython: reset");
    }
    execv("/proc/self/exe", linux_options.argv);
    perror("circuitpython: reset");
    _exit(1);
}

// There is no bootloader to go to: entering it ends the process.
void reset_to_bootloader(void) {
    linux_power_off(0);
}

uint32_t *port_stack_get_limit(void) {
    return (uint32_t *)linux_stack_bottom;
}

uint32_t *port_stack_get_top(void) {
    return (uint32_t *)linux_stack_top;
}

// The port heap is the system allocator, limited to --ram bytes so a program sees the memory a
// board of that size would have. The VM heap is carved from it like on a board.
uint32_t *port_heap_get_bottom(void) {
    return NULL;
}

uint32_t *port_heap_get_top(void) {
    return NULL;
}

void port_heap_init(void) {
}

void *port_malloc(size_t size, bool dma_capable) {
    if (_ram_used + size > linux_options.ram_size) {
        return NULL;
    }
    void *block = malloc(size);
    if (block != NULL) {
        _ram_used += malloc_usable_size(block);
    }
    return block;
}

void port_free(void *ptr) {
    if (ptr != NULL) {
        _ram_used -= malloc_usable_size(ptr);
        free(ptr);
    }
}

void *port_realloc(void *ptr, size_t size, bool dma_capable) {
    size_t old = ptr != NULL ? malloc_usable_size(ptr) : 0;
    if (size > old && _ram_used + size - old > linux_options.ram_size) {
        return NULL;
    }
    void *block = realloc(ptr, size);
    if (block != NULL) {
        _ram_used = _ram_used - old + malloc_usable_size(block);
    }
    return block;
}

size_t port_heap_get_largest_free_size(void) {
    return _ram_used < linux_options.ram_size ? linux_options.ram_size - _ram_used : 0;
}

void port_set_saved_word(uint32_t value) {
    _saved_word = value;
}

uint32_t port_get_saved_word(void) {
    return _saved_word;
}

uint64_t port_get_raw_ticks(uint8_t *subticks) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t sec = now.tv_sec - _start.tv_sec;
    int64_t nsec = now.tv_nsec - _start.tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000;
    }
    uint64_t sub = (uint64_t)sec * 32768 + (uint64_t)nsec * 32768 / 1000000000;
    if (subticks != NULL) {
        *subticks = sub & 31;
    }
    return sub >> 5;
}

void port_gc_collect(void) {
}

bool port_boot_button_pressed(void) {
    return false;
}

void linux_power_off(int status) {
    linux_serial_restore();
    exit(status);
}
