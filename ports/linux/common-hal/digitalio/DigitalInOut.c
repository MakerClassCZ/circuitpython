// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/digitalio/DigitalInOut.h"
#include "shared-bindings/microcontroller/Pin.h"

static linux_pin_state_t *_state(digitalio_digitalinout_obj_t *self) {
    return &linux_pin_state[self->pin->number];
}

digitalinout_result_t common_hal_digitalio_digitalinout_construct(
    digitalio_digitalinout_obj_t *self, const mcu_pin_obj_t *pin) {
    common_hal_mcu_pin_claim(pin);
    self->pin = pin;
    self->open_drain = false;
    _state(self)->output = false;
    _state(self)->pull = PULL_NONE;
    return DIGITALINOUT_OK;
}

void common_hal_digitalio_digitalinout_never_reset(digitalio_digitalinout_obj_t *self) {
    common_hal_never_reset_pin(self->pin);
}

bool common_hal_digitalio_digitalinout_deinited(digitalio_digitalinout_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_digitalio_digitalinout_deinit(digitalio_digitalinout_obj_t *self) {
    if (common_hal_digitalio_digitalinout_deinited(self)) {
        return;
    }
    common_hal_reset_pin(self->pin);
    self->pin = NULL;
}

digitalinout_result_t common_hal_digitalio_digitalinout_switch_to_input(
    digitalio_digitalinout_obj_t *self, digitalio_pull_t pull) {
    _state(self)->output = false;
    _state(self)->pull = pull;
    return DIGITALINOUT_OK;
}

digitalinout_result_t common_hal_digitalio_digitalinout_switch_to_output(
    digitalio_digitalinout_obj_t *self, bool value, digitalio_drive_mode_t drive_mode) {
    self->open_drain = drive_mode == DRIVE_MODE_OPEN_DRAIN;
    _state(self)->output = true;
    _state(self)->out_value = value;
    return DIGITALINOUT_OK;
}

digitalio_direction_t common_hal_digitalio_digitalinout_get_direction(
    digitalio_digitalinout_obj_t *self) {
    return _state(self)->output ? DIRECTION_OUTPUT : DIRECTION_INPUT;
}

void common_hal_digitalio_digitalinout_set_value(digitalio_digitalinout_obj_t *self, bool value) {
    _state(self)->out_value = value;
}

bool common_hal_digitalio_digitalinout_get_value(digitalio_digitalinout_obj_t *self) {
    linux_pin_state_t *state = _state(self);
    if (state->output) {
        return state->out_value;
    }
    if (state->driven) {
        return state->driven_level;
    }
    return state->pull == PULL_UP;
}

digitalinout_result_t common_hal_digitalio_digitalinout_set_drive_mode(
    digitalio_digitalinout_obj_t *self, digitalio_drive_mode_t drive_mode) {
    self->open_drain = drive_mode == DRIVE_MODE_OPEN_DRAIN;
    return DIGITALINOUT_OK;
}

digitalio_drive_mode_t common_hal_digitalio_digitalinout_get_drive_mode(
    digitalio_digitalinout_obj_t *self) {
    return self->open_drain ? DRIVE_MODE_OPEN_DRAIN : DRIVE_MODE_PUSH_PULL;
}

digitalinout_result_t common_hal_digitalio_digitalinout_set_pull(
    digitalio_digitalinout_obj_t *self, digitalio_pull_t pull) {
    _state(self)->pull = pull;
    return DIGITALINOUT_OK;
}

digitalio_pull_t common_hal_digitalio_digitalinout_get_pull(digitalio_digitalinout_obj_t *self) {
    return _state(self)->output ? PULL_NONE : _state(self)->pull;
}
