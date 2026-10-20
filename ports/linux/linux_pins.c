// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/microcontroller/Pin.h"

#include "linux_port.h"

#define PIN(n) const mcu_pin_obj_t pin_GPIO##n = { .base = { .type = &mcu_pin_type }, .number = n }
PIN(0);
PIN(1);
PIN(2);
PIN(3);
PIN(4);
PIN(5);
PIN(6);
PIN(7);
PIN(8);
PIN(9);
PIN(10);
PIN(11);
PIN(12);
PIN(13);
PIN(14);
PIN(15);

linux_pin_state_t linux_pin_state[LINUX_PIN_COUNT];

void linux_pin_drive(uint8_t number, bool level) {
    if (number < LINUX_PIN_COUNT) {
        linux_pin_state[number].driven = true;
        linux_pin_state[number].driven_level = level;
    }
}

void linux_button_set(int button, bool pressed) {
    linux_pin_drive(button, !pressed);
}

void linux_buttons_release_all(void) {
    for (int i = 0; i < LINUX_BUTTON_COUNT; i++) {
        linux_button_set(i, false);
    }
}
