// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/board/__init__.h"
#include "shared-module/displayio/__init__.h"

#include "linux_port.h"

// board.exit(status=0) ends the program: the way out of code or the REPL on a host.
static mp_obj_t desktop_exit(size_t n_args, const mp_obj_t *args) {
    linux_power_off(n_args > 0 ? mp_obj_get_int(args[0]) : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(desktop_exit_obj, 0, 1, desktop_exit);

// The buttons use the PicoPad's names, so games written for it run unchanged.
static const mp_rom_map_elem_t board_module_globals_table[] = {
    CIRCUITPYTHON_BOARD_DICT_STANDARD_ITEMS

    { MP_ROM_QSTR(MP_QSTR_SW_UP), MP_ROM_PTR(&pin_GPIO0) },
    { MP_ROM_QSTR(MP_QSTR_SW_DOWN), MP_ROM_PTR(&pin_GPIO1) },
    { MP_ROM_QSTR(MP_QSTR_SW_LEFT), MP_ROM_PTR(&pin_GPIO2) },
    { MP_ROM_QSTR(MP_QSTR_SW_RIGHT), MP_ROM_PTR(&pin_GPIO3) },
    { MP_ROM_QSTR(MP_QSTR_SW_A), MP_ROM_PTR(&pin_GPIO4) },
    { MP_ROM_QSTR(MP_QSTR_SW_B), MP_ROM_PTR(&pin_GPIO5) },
    { MP_ROM_QSTR(MP_QSTR_SW_X), MP_ROM_PTR(&pin_GPIO6) },
    { MP_ROM_QSTR(MP_QSTR_SW_Y), MP_ROM_PTR(&pin_GPIO7) },
    { MP_ROM_QSTR(MP_QSTR_SW_START), MP_ROM_PTR(&pin_GPIO8) },
    { MP_ROM_QSTR(MP_QSTR_SW_SELECT), MP_ROM_PTR(&pin_GPIO9) },
    { MP_ROM_QSTR(MP_QSTR_SW_L), MP_ROM_PTR(&pin_GPIO10) },
    { MP_ROM_QSTR(MP_QSTR_SW_R), MP_ROM_PTR(&pin_GPIO11) },
    { MP_ROM_QSTR(MP_QSTR_SW_MENU), MP_ROM_PTR(&pin_GPIO12) },

    // audioio.AudioOut(board.SPEAKER) plays through the host's sound output.
    { MP_ROM_QSTR(MP_QSTR_SPEAKER), MP_ROM_PTR(&pin_GPIO13) },

    { MP_ROM_QSTR(MP_QSTR_DISPLAY), MP_ROM_PTR(&displays[0].framebuffer_display) },

    { MP_ROM_QSTR(MP_QSTR_exit), MP_ROM_PTR(&desktop_exit_obj) },
};
MP_DEFINE_CONST_DICT(board_module_globals, board_module_globals_table);
