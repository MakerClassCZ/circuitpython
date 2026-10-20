// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The process entry: parse the options, give the VM a thread with a stack of known bounds and
// turn termination signals into a clean exit that restores the terminal.

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "linux_port.h"
#include "sdl_host.h"

int circuitpython_main(void);

#define MIB (1024 * 1024)
#define VM_STACK_SIZE (2 * MIB)
#define STACK_GUARD (64 * 1024)

linux_options_t linux_options = {
    .drive_path = "CIRCUITPY.img",
    .drive_size = 16 * MIB,
    .ram_size = 16 * MIB,
};

uint8_t *linux_stack_bottom;
uint8_t *linux_stack_top;

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [--drive IMAGE] [--drive-size MB] [--ram MB]\n"
        "  --drive IMAGE     the CIRCUITPY drive, a FAT image (default CIRCUITPY.img, created if missing)\n"
        "  --drive-size MB   size of a newly created drive (default 16)\n"
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

static void *vm_main(void *arg) {
    (void)arg;
    circuitpython_main();
    linux_power_off(0);
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

    // Every thread starts with all signals blocked: the VM thread unblocks its interrupt signals
    // in port_init() and this thread waits for the termination ones.
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    uint8_t *stack = mmap(NULL, VM_STACK_SIZE + STACK_GUARD, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        perror("circuitpython: stack");
        return 1;
    }
    mprotect(stack, STACK_GUARD, PROT_NONE);
    linux_stack_bottom = stack + STACK_GUARD;
    linux_stack_top = linux_stack_bottom + VM_STACK_SIZE;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, linux_stack_bottom, VM_STACK_SIZE);
    if (pthread_create(&linux_vm_thread, &attr, vm_main, NULL) != 0) {
        perror("circuitpython: thread");
        return 1;
    }

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
