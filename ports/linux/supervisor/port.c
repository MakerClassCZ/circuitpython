// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The supervisor on a POSIX host: the process, its memory and its clock. Interrupts live in
// interrupts.c.

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <pthread.h>        // clock_gettime
#define malloc_usable_size _msize
#else
#include <execinfo.h>
#include <signal.h>
#endif

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
#define RESTARTS_ENV "CIRCUITPY_LINUX_RESTARTS"
#define FAULTED_ENV "CIRCUITPY_LINUX_FAULTED"
// A Windows reset leaves the parent waiting, so a program that keeps resetting is cut off here.
#define RESTARTS_MAX 16

bool linux_was_reset;
static int _restarts;                   // process generations since the fresh start
static bool _restarted_after_fault;     // this process is the restart a hard fault asked for

static void _set_env(const char *name, const char *value) {
    #ifdef _WIN32
    SetEnvironmentVariableA(name, value);
    _putenv_s(name, value != NULL ? value : "");
    #else
    if (value != NULL) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
    #endif
}

// A fault right after a restart for a fault would restart forever; the second one ends the process.
static void _fault_restart(void) {
    if (_restarted_after_fault) {
        static const char msg[] = "circuitpython: hard fault again after the restart, giving up\r\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        linux_power_off(3);
    }
    _set_env(FAULTED_ENV, "1");
    reset_into_safe_mode(SAFE_MODE_HARD_FAULT);
}

static size_t _ram_used;
static uint32_t _saved_word;
static struct timespec _start;

// A crash is the host's hard fault: report where on stderr, then restart into safe mode.
#ifdef _WIN32
static LONG WINAPI _on_exception(EXCEPTION_POINTERS *info) {
    EXCEPTION_RECORD *record = info->ExceptionRecord;
    // The address as addr2line wants it: at the image base the linker chose, not where the
    // loader put the image.
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *headers = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    uintptr_t linked = headers->OptionalHeader.ImageBase + ((uintptr_t)record->ExceptionAddress - base);
    fprintf(stderr, "\r\ncircuitpython: hard fault, exception 0x%lx at 0x%llx\r\n",
        (unsigned long)record->ExceptionCode, (unsigned long long)linked);
    _fault_restart();
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
static void _on_fault(int sig) {
    static const char msg[] = "\r\ncircuitpython: hard fault, backtrace:\r\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    void *frames[32];
    backtrace_symbols_fd(frames, backtrace(frames, 32), STDERR_FILENO);
    _fault_restart();
}
#endif

safe_mode_t port_init(void) {
    clock_gettime(CLOCK_MONOTONIC, &_start);

    // reset_cpu() re-executes the process; the safe mode word rides along in the environment.
    const char *word = getenv(SAVED_WORD_ENV);
    if (word != NULL) {
        _saved_word = strtoul(word, NULL, 0);
        linux_was_reset = true;
        _set_env(SAVED_WORD_ENV, NULL);
    }
    const char *restarts = getenv(RESTARTS_ENV);
    if (restarts != NULL) {
        _restarts = atoi(restarts);
        _set_env(RESTARTS_ENV, NULL);
    }
    if (getenv(FAULTED_ENV) != NULL) {
        _restarted_after_fault = true;
        _set_env(FAULTED_ENV, NULL);
    }

    #ifdef _WIN32
    SetUnhandledExceptionFilter(_on_exception);
    #else
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
    #endif

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
    _set_env(SAVED_WORD_ENV, word);
    char count[16];
    snprintf(count, sizeof(count), "%d", _restarts + 1);
    _set_env(RESTARTS_ENV, count);
    // The relative paths of the command line are relative to where the process started.
    if (chdir(linux_options.start_dir) != 0) {
        perror("circuitpython: reset");
    }
    #ifdef _WIN32
    // Windows cannot replace a running process: run the same command line again, wait for it and
    // end with its status, so the console session is one program from start to finish. Each
    // generation keeps its parent waiting, hence the limit.
    if (_restarts + 1 > RESTARTS_MAX) {
        fprintf(stderr, "circuitpython: reset: too many restarts, ending\r\n");
        _exit(1);
    }
    char *command = strdup(GetCommandLineA());
    STARTUPINFOA startup = { .cb = sizeof(startup) };
    PROCESS_INFORMATION process;
    if (command != NULL && CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        DWORD status = 1;
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &status);
        _exit(status);
    }
    fprintf(stderr, "circuitpython: reset: error %lu\r\n", (unsigned long)GetLastError());
    #else
    execv("/proc/self/exe", linux_options.argv);
    perror("circuitpython: reset");
    #endif
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
