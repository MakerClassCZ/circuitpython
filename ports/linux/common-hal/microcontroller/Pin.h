// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// Pins are virtual: a pin keeps its level in linux_pins.c, and the console's buttons drive the
// level of the pins the board names after them, active low like a button to ground.
typedef struct {
    mp_obj_base_t base;
    uint8_t number;
} mcu_pin_obj_t;

#define LINUX_PIN_COUNT 16

extern const mcu_pin_obj_t pin_GPIO0;
extern const mcu_pin_obj_t pin_GPIO1;
extern const mcu_pin_obj_t pin_GPIO2;
extern const mcu_pin_obj_t pin_GPIO3;
extern const mcu_pin_obj_t pin_GPIO4;
extern const mcu_pin_obj_t pin_GPIO5;
extern const mcu_pin_obj_t pin_GPIO6;
extern const mcu_pin_obj_t pin_GPIO7;
extern const mcu_pin_obj_t pin_GPIO8;
extern const mcu_pin_obj_t pin_GPIO9;
extern const mcu_pin_obj_t pin_GPIO10;
extern const mcu_pin_obj_t pin_GPIO11;
extern const mcu_pin_obj_t pin_GPIO12;
extern const mcu_pin_obj_t pin_GPIO13;
extern const mcu_pin_obj_t pin_GPIO14;
extern const mcu_pin_obj_t pin_GPIO15;

typedef struct {
    bool output;
    bool out_value;
    uint8_t pull;           // digitalio_pull_t
    bool driven;            // something outside drives the pin (a button)
    bool driven_level;
} linux_pin_state_t;

extern linux_pin_state_t linux_pin_state[LINUX_PIN_COUNT];

// A host button sets the level a pin reads.
void linux_pin_drive(uint8_t number, bool level);

void reset_all_pins(void);
void common_hal_reset_pin(const mcu_pin_obj_t *pin);
uint8_t common_hal_mcu_pin_number(const mcu_pin_obj_t *pin);
void common_hal_mcu_pin_claim(const mcu_pin_obj_t *pin);
