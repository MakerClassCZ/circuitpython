// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The supervisor on a POSIX host. Interrupts are real-time signals delivered to the VM thread:
// one periodic timer is the 1024 Hz tick, a one-shot timer is the wake-up alarm, and "disabling
// interrupts" blocks both signals.

#include <errno.h>
#include <execinfo.h>
#include <malloc.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "supervisor/background_callback.h"
#include "supervisor/board.h"
#include "supervisor/filesystem.h"
#include "supervisor/port.h"
#include "supervisor/shared/safe_mode.h"
#include "supervisor/shared/tick.h"
#include "supervisor/workflow.h"
#include "shared-bindings/microcontroller/__init__.h"
#include "shared-bindings/rtc/__init__.h"
#if CIRCUITPY_AUDIOIO
#include "common-hal/audioio/AudioOut.h"
#endif
#include "common-hal/microcontroller/Pin.h"

#include "linux_port.h"

#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif

#define TICK_SIGNAL (SIGRTMIN)
#define WAKE_SIGNAL (SIGRTMIN + 1)
#define POLL_SIGNAL (SIGRTMIN + 2)
#define SAVED_WORD_ENV "CIRCUITPY_LINUX_SAVED_WORD"

pthread_t linux_vm_thread;
bool linux_was_reset;

static size_t _ram_used;
static uint32_t _saved_word;
static struct timespec _start;
static timer_t _tick_timer;
static timer_t _wake_timer;
static timer_t _poll_timer;
static volatile sig_atomic_t _woken;
static sigset_t _irq_signals;

static void _on_interrupt(int sig, siginfo_t *info, void *context) {
    (void)info;
    (void)context;
    if (sig == TICK_SIGNAL) {
        supervisor_tick();
    }
    #if CIRCUITPY_SOCKETPOOL
    if (sig == POLL_SIGNAL) {
        supervisor_workflow_request_background();
    }
    #endif
    _woken = 1;
}

// A crash is the host's hard fault: report where on stderr, then restart into safe mode.
static void _on_fault(int sig) {
    static const char msg[] = "\r\ncircuitpython: hard fault, backtrace:\r\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    void *frames[32];
    backtrace_symbols_fd(frames, backtrace(frames, 32), STDERR_FILENO);
    reset_into_safe_mode(SAFE_MODE_HARD_FAULT);
}

static void _set_timer(timer_t timer, uint64_t ns, bool periodic) {
    struct itimerspec spec = {0};
    spec.it_value.tv_sec = ns / 1000000000u;
    spec.it_value.tv_nsec = ns % 1000000000u;
    if (periodic) {
        spec.it_interval = spec.it_value;
    }
    timer_settime(timer, 0, &spec, NULL);
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

    sigemptyset(&_irq_signals);
    sigaddset(&_irq_signals, TICK_SIGNAL);
    sigaddset(&_irq_signals, WAKE_SIGNAL);
    sigaddset(&_irq_signals, POLL_SIGNAL);

    struct sigaction action = {0};
    action.sa_sigaction = _on_interrupt;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    action.sa_mask = _irq_signals;              // one interrupt at a time
    sigaction(TICK_SIGNAL, &action, NULL);
    sigaction(WAKE_SIGNAL, &action, NULL);
    sigaction(POLL_SIGNAL, &action, NULL);

    struct sigevent event = {0};
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_notify_thread_id = syscall(SYS_gettid);
    event.sigev_signo = TICK_SIGNAL;
    if (timer_create(CLOCK_MONOTONIC, &event, &_tick_timer) != 0) {
        return SAFE_MODE_PROGRAMMATIC;
    }
    event.sigev_signo = WAKE_SIGNAL;
    if (timer_create(CLOCK_MONOTONIC, &event, &_wake_timer) != 0) {
        return SAFE_MODE_PROGRAMMATIC;
    }
    event.sigev_signo = POLL_SIGNAL;
    if (timer_create(CLOCK_MONOTONIC, &event, &_poll_timer) != 0) {
        return SAFE_MODE_PROGRAMMATIC;
    }
    pthread_sigmask(SIG_UNBLOCK, &_irq_signals, NULL);
    return SAFE_MODE_NONE;
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

void port_enable_tick(void) {
    _set_timer(_tick_timer, 1000000000u / 1024, true);
}

void port_disable_tick(void) {
    _set_timer(_tick_timer, 0, false);
}

void port_interrupt_after_ticks(uint32_t ticks) {
    uint64_t ns = (uint64_t)ticks * 1000000000u / 1024;
    _set_timer(_wake_timer, ns > 0 ? ns : 1000, false);
}

void linux_request_workflow_poll(uint32_t ms) {
    _set_timer(_poll_timer, (uint64_t)ms * 1000000u, false);
}

void linux_wait_for_interrupt(int timeout_ms) {
    sigset_t old;
    pthread_sigmask(SIG_BLOCK, &_irq_signals, &old);
    if (!_woken && !background_callback_pending()) {
        sigset_t open = old;
        sigdelset(&open, TICK_SIGNAL);
        sigdelset(&open, WAKE_SIGNAL);
        sigdelset(&open, POLL_SIGNAL);
        struct timespec timeout = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000L };
        ppoll(NULL, 0, timeout_ms < 0 ? NULL : &timeout, &open);
    }
    _woken = 0;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

void port_idle_until_interrupt(void) {
    linux_wait_for_interrupt(-1);
}

void linux_wake_vm(void) {
    pthread_kill(linux_vm_thread, WAKE_SIGNAL);
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
