// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"


#ifndef CIRCUITPY_PICOGAME_XIP_MAP
#define CIRCUITPY_PICOGAME_XIP_MAP (0)
#endif
#ifndef PICOGAME_FAT_PROBE
#define PICOGAME_FAT_PROBE (0)      // bench-only FAT layout/repack probes (make PICOGAME_FAT_PROBE=1)
#endif

uint8_t picogame_kind_of(mp_obj_t o);
