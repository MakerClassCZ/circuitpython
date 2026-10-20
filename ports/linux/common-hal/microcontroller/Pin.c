// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/digitalio/Pull.h"

static uint32_t _claimed;
static uint32_t _never_reset;

static void _reset(uint8_t number) {
    linux_pin_state_t *state = &linux_pin_state[number];
    state->output = false;
    state->out_value = false;
    state->pull = PULL_NONE;
    _claimed &= ~(1u << number);
}

void reset_all_pins(void) {
    for (uint8_t i = 0; i < LINUX_PIN_COUNT; i++) {
        if (!(_never_reset & (1u << i))) {
            _reset(i);
        }
    }
}

void common_hal_reset_pin(const mcu_pin_obj_t *pin) {
    if (pin != NULL) {
        _never_reset &= ~(1u << pin->number);
        _reset(pin->number);
    }
}

void common_hal_mcu_pin_reset_number(uint8_t pin_no) {
    if (pin_no < LINUX_PIN_COUNT) {
        _never_reset &= ~(1u << pin_no);
        _reset(pin_no);
    }
}

void common_hal_never_reset_pin(const mcu_pin_obj_t *pin) {
    _never_reset |= 1u << pin->number;
}

bool common_hal_mcu_pin_is_free(const mcu_pin_obj_t *pin) {
    return !(_claimed & (1u << pin->number));
}

uint8_t common_hal_mcu_pin_number(const mcu_pin_obj_t *pin) {
    return pin->number;
}

void common_hal_mcu_pin_claim(const mcu_pin_obj_t *pin) {
    _claimed |= 1u << pin->number;
}
