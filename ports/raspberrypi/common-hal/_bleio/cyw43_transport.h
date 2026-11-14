// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimír Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

// Binds the board's own adapter, over the CYW43439's Bluetooth core, to _bleio.adapter.
void bleio_cyw43_attach_adapter(void);
