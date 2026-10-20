// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <signal.h>
#include <time.h>

#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "common-hal/microcontroller/Pin.h"
#include "common-hal/microcontroller/Processor.h"
#include "shared-bindings/microcontroller/__init__.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/microcontroller/Processor.h"
#include "supervisor/port.h"
#include "supervisor/shared/safe_mode.h"

void common_hal_mcu_delay_us(uint32_t delay) {
    struct timespec t = { delay / 1000000, (delay % 1000000) * 1000L };
    while (nanosleep(&t, &t) != 0) {
    }
}

// Interrupts are the tick and wake signals; disabling them blocks the signals on this thread.
static volatile uint32_t _nesting;
static sigset_t _saved_mask;

void common_hal_mcu_disable_interrupts(void) {
    sigset_t irq, old;
    sigemptyset(&irq);
    sigaddset(&irq, SIGRTMIN);
    sigaddset(&irq, SIGRTMIN + 1);
    sigaddset(&irq, SIGRTMIN + 2);
    pthread_sigmask(SIG_BLOCK, &irq, &old);
    if (_nesting++ == 0) {
        _saved_mask = old;
    }
}

void common_hal_mcu_enable_interrupts(void) {
    if (_nesting == 0) {
        reset_into_safe_mode(SAFE_MODE_INTERRUPT_ERROR);
    }
    if (--_nesting == 0) {
        pthread_sigmask(SIG_SETMASK, &_saved_mask, NULL);
    }
}

static bool _next_reset_to_bootloader;

void common_hal_mcu_on_next_reset(mcu_runmode_t runmode) {
    if (runmode == RUNMODE_SAFE_MODE) {
        safe_mode_on_next_reset(SAFE_MODE_PROGRAMMATIC);
    }
    _next_reset_to_bootloader = runmode == RUNMODE_BOOTLOADER;
}

void common_hal_mcu_reset(void) {
    if (_next_reset_to_bootloader) {
        reset_to_bootloader();
    }
    reset_cpu();
}

const mcu_processor_obj_t common_hal_mcu_processor_obj = {
    .base = {
        .type = &mcu_processor_type,
    },
};

static const mp_rom_map_elem_t mcu_pin_global_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_GPIO0), MP_ROM_PTR(&pin_GPIO0) },
    { MP_ROM_QSTR(MP_QSTR_GPIO1), MP_ROM_PTR(&pin_GPIO1) },
    { MP_ROM_QSTR(MP_QSTR_GPIO2), MP_ROM_PTR(&pin_GPIO2) },
    { MP_ROM_QSTR(MP_QSTR_GPIO3), MP_ROM_PTR(&pin_GPIO3) },
    { MP_ROM_QSTR(MP_QSTR_GPIO4), MP_ROM_PTR(&pin_GPIO4) },
    { MP_ROM_QSTR(MP_QSTR_GPIO5), MP_ROM_PTR(&pin_GPIO5) },
    { MP_ROM_QSTR(MP_QSTR_GPIO6), MP_ROM_PTR(&pin_GPIO6) },
    { MP_ROM_QSTR(MP_QSTR_GPIO7), MP_ROM_PTR(&pin_GPIO7) },
    { MP_ROM_QSTR(MP_QSTR_GPIO8), MP_ROM_PTR(&pin_GPIO8) },
    { MP_ROM_QSTR(MP_QSTR_GPIO9), MP_ROM_PTR(&pin_GPIO9) },
    { MP_ROM_QSTR(MP_QSTR_GPIO10), MP_ROM_PTR(&pin_GPIO10) },
    { MP_ROM_QSTR(MP_QSTR_GPIO11), MP_ROM_PTR(&pin_GPIO11) },
    { MP_ROM_QSTR(MP_QSTR_GPIO12), MP_ROM_PTR(&pin_GPIO12) },
    { MP_ROM_QSTR(MP_QSTR_GPIO13), MP_ROM_PTR(&pin_GPIO13) },
    { MP_ROM_QSTR(MP_QSTR_GPIO14), MP_ROM_PTR(&pin_GPIO14) },
    { MP_ROM_QSTR(MP_QSTR_GPIO15), MP_ROM_PTR(&pin_GPIO15) },
};
MP_DEFINE_CONST_DICT(mcu_pin_globals, mcu_pin_global_dict_table);
