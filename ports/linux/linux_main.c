// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The process entry: parse the options, run the VM on the main thread (SDL wants its window
// and events there on some systems) with a stack of known bounds, and turn termination signals
// into a clean exit that restores the terminal.

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>
#include <limits.h>
#include <sys/resource.h>
#include <unistd.h>

#include "linux_port.h"
#include "sdl_host.h"

int circuitpython_main(void);

#define MIB (1024 * 1024)
#define VM_STACK_SIZE (2 * MIB)
#define STACK_MARGIN (256 * 1024)       // left to the kernel's guard gap and to signal frames
#define PAGE (4096)

linux_options_t linux_options = {
    .drive_path = "CIRCUITPY",
    .drive_size = 16 * MIB,
    .ram_size = 16 * MIB,
};

uint8_t *linux_stack_bottom;
uint8_t *linux_stack_top;

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [--drive PATH] [--drive-size MB] [--ram MB]\n"
        "  --drive PATH      CIRCUITPY: a directory (default ./CIRCUITPY), or a FAT image file;\n"
        "                    a missing PATH is created, as an image if it ends in .img\n"
        "  --drive-size MB   size of a newly created image (default 16)\n"
        "  --ram MB          heap for CircuitPython (default 16)\n"
        "Ctrl-\\ on the console powers off.\n", prog);
}

static size_t parse_mib(const char *s, const char *prog) {
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v == 0 || v > 4096) {
        usage(prog);
        exit(2);
    }
    return (size_t)v * MIB;
}

// Map the stack below this frame now, page by page from the top: CircuitPython writes a canary at
// the stack limit at once, and an older kernel faults on an access that far below the stack
// pointer instead of growing the stack. Returns the lowest address mapped.
__attribute__((noinline)) static uintptr_t grow_stack(size_t bytes) {
    volatile uint8_t *probe = alloca(bytes);
    for (size_t offset = PAGE; offset <= bytes; offset += PAGE) {
        probe[bytes - offset] = 0;
    }
    return (uintptr_t)probe;
}

// Termination signals end up here, so the VM thread never sees them.
static void *signal_main(void *arg) {
    (void)arg;
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGHUP);
    sigaddset(&stop, SIGQUIT);
    sigaddset(&stop, SIGUSR1);
    for (;;) {
        int sig = 0;
        if (sigwait(&stop, &sig) != 0) {
            continue;
        }
        if (sig == SIGUSR1) {
            // kill -USR1 saves the next frame as a screenshot
            linux_sdl_request_screenshot();
            continue;
        }
        linux_power_off(128 + sig);
    }
}

int main(int argc, char **argv) {
    linux_options.argv = argv;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--drive") == 0 && i + 1 < argc) {
            linux_options.drive_path = argv[++i];
        } else if (strcmp(argv[i], "--drive-size") == 0 && i + 1 < argc) {
            linux_options.drive_size = parse_mib(argv[++i], argv[0]);
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            linux_options.ram_size = parse_mib(argv[++i], argv[0]);
        } else {
            usage(argv[0]);
            return strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }

    // VfsPosix moves the working directory into CIRCUITPY, so keep paths absolute.
    static char start_dir[PATH_MAX];
    static char drive_path[PATH_MAX];
    if (getcwd(start_dir, sizeof(start_dir)) == NULL) {
        perror("circuitpython: working directory");
        return 1;
    }
    linux_options.start_dir = start_dir;
    if (linux_options.drive_path[0] != '/') {
        int len = snprintf(drive_path, sizeof(drive_path), "%s/%s", start_dir, linux_options.drive_path);
        if (len < 0 || (size_t)len >= sizeof(drive_path)) {
            fprintf(stderr, "circuitpython: drive path too long\n");
            return 2;
        }
        linux_options.drive_path = drive_path;
    }

    // Every thread starts with all signals blocked: port_init() unblocks the fault signals on this
    // thread, and the signal thread waits for the termination ones.
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    pthread_t signal_thread;
    if (pthread_create(&signal_thread, NULL, signal_main, NULL) != 0) {
        perror("circuitpython: signal thread");
        return 1;
    }

    // The VM gets VM_STACK_SIZE of the main stack, less if the stack limit is lower.
    size_t stack_size = VM_STACK_SIZE;
    struct rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY &&
        limit.rlim_cur < stack_size + 2 * STACK_MARGIN) {
        stack_size = limit.rlim_cur > 3 * STACK_MARGIN ? limit.rlim_cur - 2 * STACK_MARGIN : STACK_MARGIN;
    }
    linux_stack_top = (uint8_t *)__builtin_frame_address(0);
    linux_stack_bottom = (uint8_t *)(grow_stack(stack_size) + PAGE);
    circuitpython_main();
    linux_power_off(0);
}
