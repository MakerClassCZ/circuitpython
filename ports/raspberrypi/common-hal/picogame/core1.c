// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// core1 fork-join helper (PROBE): run the second half of a row/column-splittable picogame C kernel
// on the RP2040's idle second core, inside ONE synchronous C call - core0 executes [lo, mid) itself
// and spin-joins, so Python never runs while core1 works (no GC/exception window; everything core1
// reads is pinned by the calling frame). First consumer: Canvas.mode7 rows; raycast columns and
// (on framebuffer targets) strip compositing can reuse the same hook if the probe pays.
//
// Safety notes:
//  - the worker loop lives in RAM (__not_in_flash_func); job code (mode7_rows) is in flash, so a
//    flash WRITE while a job is mid-flight is a probe-known hazard (don't save files while a core1
//    bench runs). Lockout-victim integration is deliberately NOT used here: it owns the SIO FIFO
//    via an IRQ handler and clashes with any other FIFO use - the production version must either
//    move jobs to RAM or hook CP's flash layer to join core1 first;
//  - picogame_core1_reset() is called from reset_port() so soft reload / Ctrl-C never leaves a
//    stale core1 running user-era code.

#include "py/mpconfig.h"
#include "shared-module/picogame/__init__.h"   // prototypes + picogame_job_t for BOTH branches

#if CIRCUITPY_PICOGAME_FAST_DISPLAY

#include "pico/multicore.h"

static picogame_job_t volatile s_fn;
static void *volatile s_arg;
static volatile int s_lo, s_hi;
static volatile bool s_busy;
static bool s_launched;
static bool s_enabled;

// Dispatch over a plain RAM mailbox, NOT the SIO FIFO: multicore lockout owns the FIFO via an IRQ
// handler, and mixing a blocking pop with it races jobs against lockout words (first probe rev did
// exactly that - the job got eaten, core0 spun in the join forever and the VM/serial froze). The
// FIFO is only touched by the SDK's launch handshake, before this loop starts.
static volatile bool s_go;

static void __not_in_flash_func(core1_worker)(void) {
    while (true) {
        while (!s_go) {                         // idle spin in RAM (probe; SEV/WFE polish can come later)
        }
        picogame_job_t fn = s_fn;
        s_go = false;
        fn((void *)s_arg, s_lo, s_hi);
        s_busy = false;                         // M0+ is in-order, no data cache: plain store suffices
    }
}

static bool par_split(picogame_job_t fn, void *arg, int lo, int hi) {
    if (!s_enabled || hi - lo < 8) {            // tiny jobs: launch/join overhead beats the win
        return false;
    }
    if (s_busy) {                               // worker holds an async frame: run this one serially
        return false;
    }
    if (!s_launched) {
        multicore_launch_core1(core1_worker);
        s_launched = true;
    }
    int mid = lo + ((hi - lo) >> 1);
    s_fn = fn;
    s_arg = arg;
    s_lo = mid;
    s_hi = hi;
    s_busy = true;
    s_go = true;
    fn(arg, lo, mid);                           // core0 renders the front half meanwhile
    while (s_busy) {
    }
    return true;
}

void picogame_core1_set_enabled(bool on) {
    s_enabled = on;
    picogame_par_split = on ? par_split : NULL;
}

// Fire-and-forget submission (the async-refresh probe): run `fn(arg, 0, 0)` on core1 and RETURN -
// the caller overlaps Python with it and joins later (next refresh / flash fence / reset). Reuses
// the same mailbox as par_split; par_split degrades to serial while an async job is in flight.
bool picogame_core1_submit(picogame_job_t fn, void *arg) {
    if (s_busy) {                               // previous frame still going: caller must join first
        return false;
    }
    if (!s_launched) {
        multicore_launch_core1(core1_worker);
        s_launched = true;
    }
    s_fn = fn;
    s_arg = arg;
    s_lo = 0;
    s_hi = 0;
    s_busy = true;
    s_go = true;
    return true;
}

void picogame_core1_join(void) {
    while (s_busy) {
    }
}

// Flash fence: park core1 before any flash erase/program. Jobs are submitted ONLY by core0, and
// core0 is inside the flash path while this runs - so waiting out the current job is sufficient:
// afterwards the worker spins in its RAM loop and never touches XIP. A stuck job (engine bug)
// falls back to a hard core1 reset after ~100 ms rather than risking an erase under a running
// XIP fetch. Called from port_internal_flash_flush() and the NVM writers.
void picogame_core1_flash_fence(void) {
    if (!s_launched) {
        return;
    }
    for (uint32_t spin = 0; s_busy; spin++) {
        if (spin > 12500000u) {                 // ~100 ms @125 MHz: job wedged -> reset, don't gamble
            multicore_reset_core1();
            s_launched = false;
            s_busy = false;
            picogame_par_split = NULL;
            s_enabled = false;
            return;
        }
    }
}

void picogame_core1_reset(void) {
    picogame_par_split = NULL;
    s_enabled = false;
    if (s_launched) {
        multicore_reset_core1();
        s_launched = false;
    }
}

#else

void picogame_core1_set_enabled(bool on) {
    (void)on;
}
void picogame_core1_reset(void) {
}
void picogame_core1_flash_fence(void) {
}
bool picogame_core1_submit(picogame_job_t fn, void *arg) {
    (void)fn;
    (void)arg;
    return false;
}
void picogame_core1_join(void) {
}

#endif
