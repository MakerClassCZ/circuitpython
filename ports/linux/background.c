// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "supervisor/port.h"

#if CIRCUITPY_SDL
#include "sdl_host.h"
#endif
#if CIRCUITPY_AUDIOIO
#include "common-hal/audioio/AudioOut.h"
#endif

void port_background_tick(void) {
}

void port_background_task(void) {
    #if CIRCUITPY_SDL
    linux_sdl_task();
    #endif
    #if CIRCUITPY_AUDIOIO
    audioout_background();
    #endif
}

void port_start_background_tick(void) {
}

void port_finish_background_tick(void) {
}
