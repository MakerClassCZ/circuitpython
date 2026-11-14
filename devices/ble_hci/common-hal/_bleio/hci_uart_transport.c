// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimír Smitka
//
// SPDX-License-Identifier: MIT

// The program's UART as a transport, on a port that has its own: _bleio.Adapter(uart=...) there.

#include "common-hal/_bleio/hci_transport.h"

#if CIRCUITPY_BLEIO_HCI_TRANSPORT

#include "common-hal/_bleio/Adapter.h"
#include "shared-bindings/busio/UART.h"
#include "shared-bindings/digitalio/DigitalInOut.h"

static size_t uart_available(void *ctx) {
    bleio_adapter_obj_t *adapter = ctx;
    return common_hal_busio_uart_rx_characters_available(adapter->hci_uart);
}

static size_t uart_read(void *ctx, uint8_t *data, size_t len, int *errcode) {
    bleio_adapter_obj_t *adapter = ctx;
    return common_hal_busio_uart_read(adapter->hci_uart, data, len, errcode);
}

static size_t uart_write(void *ctx, const uint8_t *data, size_t len, int *errcode) {
    bleio_adapter_obj_t *adapter = ctx;
    return common_hal_busio_uart_write(adapter->hci_uart, data, len, errcode);
}

static void uart_set_rts(void *ctx, bool value) {
    bleio_adapter_obj_t *adapter = ctx;
    common_hal_digitalio_digitalinout_set_value(adapter->rts_digitalinout, value);
}

static bool uart_get_cts(void *ctx) {
    bleio_adapter_obj_t *adapter = ctx;
    return common_hal_digitalio_digitalinout_get_value(adapter->cts_digitalinout);
}

const bleio_hci_transport_t bleio_hci_uart_transport = {
    .available = uart_available,
    .read = uart_read,
    .write = uart_write,
    .set_rts = uart_set_rts,
    .get_cts = uart_get_cts,
    .enabled = NULL,
};

#endif
