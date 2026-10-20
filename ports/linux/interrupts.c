// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// Interrupts on a host. A timer thread plays the interrupt controller: it runs the 1024 Hz tick
// and the one-shot alarms, and "disabling interrupts" holds the lock the thread takes around
// each of them. The VM idles on a condition variable that the thread, the console reader and the
// drive watcher signal, as an interrupt wakes an MCU. All of it is pthreads, which MinGW provides
// on Windows too.

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "supervisor/background_callback.h"
#include "supervisor/port.h"
#include "supervisor/shared/safe_mode.h"
#include "supervisor/shared/tick.h"
#include "supervisor/workflow.h"

#include "linux_port.h"

#define NS_PER_S 1000000000ULL
#define TICK_NS (NS_PER_S / 1024)

// Held while interrupts are disabled, and by the timer thread around each interrupt.
static pthread_mutex_t _irq_lock;

// The alarms, in CLOCK_MONOTONIC nanoseconds; 0 = not armed.
static pthread_mutex_t _timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t _timer_cond = PTHREAD_COND_INITIALIZER;
static bool _tick_enabled;
static uint64_t _next_tick_ns;
static uint64_t _wake_ns;
static uint64_t _poll_ns;

// Set by whoever wakes the VM, cleared by the VM as it leaves idle.
static pthread_mutex_t _wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t _wake_cond = PTHREAD_COND_INITIALIZER;
static bool _woken;

static uint64_t _now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * NS_PER_S + now.tv_nsec;
}

// pthread_cond_timedwait takes a CLOCK_REALTIME deadline.
static struct timespec _realtime_after(uint64_t ns) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    uint64_t nsec = (uint64_t)t.tv_nsec + ns;
    t.tv_sec += nsec / NS_PER_S;
    t.tv_nsec = nsec % NS_PER_S;
    return t;
}

// Nested disable/enable pairs work as on an MCU, and the lock exists before main() runs.
__attribute__((constructor)) static void _init_irq_lock(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&_irq_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

void linux_irq_disable(void) {
    pthread_mutex_lock(&_irq_lock);
}

void linux_irq_enable(void) {
    if (pthread_mutex_unlock(&_irq_lock) != 0) {
        reset_into_safe_mode(SAFE_MODE_INTERRUPT_ERROR);   // enabled more often than disabled
    }
}

void linux_wake_vm(void) {
    pthread_mutex_lock(&_wake_lock);
    _woken = true;
    pthread_cond_signal(&_wake_cond);
    pthread_mutex_unlock(&_wake_lock);
}

void linux_wait_for_interrupt(int timeout_ms) {
    pthread_mutex_lock(&_wake_lock);
    if (!_woken && !background_callback_pending()) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&_wake_cond, &_wake_lock);
        } else {
            struct timespec deadline = _realtime_after((uint64_t)timeout_ms * 1000000ULL);
            pthread_cond_timedwait(&_wake_cond, &_wake_lock, &deadline);
        }
    }
    _woken = false;
    pthread_mutex_unlock(&_wake_lock);
}

void port_idle_until_interrupt(void) {
    linux_wait_for_interrupt(-1);
}

// The next alarm among those armed, or 0; due alarms are taken.
static uint64_t _due(uint64_t *slot, uint64_t now, bool *fired, uint64_t next) {
    if (*slot == 0) {
        return next;
    }
    if (*slot <= now) {
        *fired = true;
        *slot = 0;
        return next;
    }
    return next == 0 || *slot < next ? *slot : next;
}

static void *_timer_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&_timer_lock);
    for (;;) {
        uint64_t now = _now_ns();
        uint64_t next = 0;
        bool tick = false;
        bool wake = false;
        bool poll = false;
        if (_tick_enabled) {
            if (_next_tick_ns <= now) {
                tick = true;
                _next_tick_ns += TICK_NS;
                if (_next_tick_ns <= now) {
                    _next_tick_ns = now + TICK_NS;      // after a stall, skip the missed ticks
                }
            }
            next = _next_tick_ns;
        }
        next = _due(&_wake_ns, now, &wake, next);
        next = _due(&_poll_ns, now, &poll, next);
        if (tick || wake || poll) {
            pthread_mutex_unlock(&_timer_lock);
            if (tick) {
                linux_irq_disable();
                supervisor_tick();
                linux_irq_enable();
            }
            #if CIRCUITPY_SOCKETPOOL
            if (poll) {
                linux_irq_disable();
                supervisor_workflow_request_background();
                linux_irq_enable();
            }
            #endif
            linux_wake_vm();
            pthread_mutex_lock(&_timer_lock);
            continue;
        }
        if (next == 0) {
            pthread_cond_wait(&_timer_cond, &_timer_lock);
        } else {
            struct timespec deadline = _realtime_after(next - now);
            pthread_cond_timedwait(&_timer_cond, &_timer_lock, &deadline);
        }
    }
    return NULL;
}

bool linux_interrupts_init(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int err = pthread_create(&thread, &attr, _timer_main, NULL);
    pthread_attr_destroy(&attr);
    return err == 0;
}

static void _arm(uint64_t *slot, uint64_t ns) {
    pthread_mutex_lock(&_timer_lock);
    *slot = _now_ns() + (ns > 0 ? ns : 1);
    pthread_cond_signal(&_timer_cond);
    pthread_mutex_unlock(&_timer_lock);
}

void port_enable_tick(void) {
    pthread_mutex_lock(&_timer_lock);
    _tick_enabled = true;
    _next_tick_ns = _now_ns() + TICK_NS;
    pthread_cond_signal(&_timer_cond);
    pthread_mutex_unlock(&_timer_lock);
}

void port_disable_tick(void) {
    pthread_mutex_lock(&_timer_lock);
    _tick_enabled = false;
    pthread_mutex_unlock(&_timer_lock);
}

void port_interrupt_after_ticks(uint32_t ticks) {
    _arm(&_wake_ns, (uint64_t)ticks * NS_PER_S / 1024);
}

void linux_request_workflow_poll(uint32_t ms) {
    _arm(&_poll_ns, (uint64_t)ms * 1000000ULL);
}
