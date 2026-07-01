// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "supervisor/board.h"
#include "shared-module/displayio/__init__.h"   // picogame: board.DISPLAY slot lives in displays[]

// picogame custom-board firmware exposes board.DISPLAY (pins.c) as displays[0], which boot.py
// fills from settings.toml. If boot.py doesn't (missing/invalid config, or no boot.py at all), that
// slot stays NULL-typed and the first use of board.DISPLAY hard-faults. Seed it to a valid None so
// an unfilled slot raises a clean "expected a Display" TypeError instead of locking up the board.
void board_init(void) {
    displays[0].display_base.type = &mp_type_NoneType;
}

// Use the MP_WEAK supervisor/shared/board.c versions of routines not defined here.
