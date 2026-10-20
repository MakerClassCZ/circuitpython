// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// Plays through an SDL2 audio device. The background task keeps the device's queue topped up
// from the sample, so samples, mixers and synthesizers run on the VM thread as on a board.
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *left_channel;
    mp_obj_t sample;
    uint32_t device;            // SDL_AudioDeviceID, 0 while nothing plays
    uint32_t sample_rate;
    uint8_t channel_count;
    uint8_t bits_per_sample;
    bool samples_signed;
    bool loop;
    bool playing;
    bool paused;
    bool sample_done;           // the sample ran out; playing ends once the queue drains
} audioio_audioout_obj_t;

void audioout_background(void);
void audioout_reset(void);
