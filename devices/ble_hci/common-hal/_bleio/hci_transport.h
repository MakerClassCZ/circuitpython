// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimír Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A port whose board has its Bluetooth controller built in sets this to 1 and gives _bleio its
// own link to it. Without it, the link is the program's UART, as with an AirLift.
#ifndef CIRCUITPY_BLEIO_HCI_TRANSPORT
#define CIRCUITPY_BLEIO_HCI_TRANSPORT (0)
#endif

#if CIRCUITPY_BLEIO_HCI_TRANSPORT

// The link between the HCI host in hci.c and its controller, shaped as busio.UART's calls are.
// Every callback runs on the VM's thread.
typedef struct {
    size_t (*available)(void *ctx);
    // Never waits. *errcode EAGAIN when nothing could be read although bytes are waiting.
    size_t (*read)(void *ctx, uint8_t *data, size_t len, int *errcode);
    size_t (*write)(void *ctx, const uint8_t *data, size_t len, int *errcode);
    // RTS: true holds the controller off. May be NULL.
    void (*set_rts)(void *ctx, bool value);
    // CTS: true while the controller cannot take a packet. May be NULL.
    bool (*get_cts)(void *ctx);
    // The adapter turned on (before its HCI Reset) or off (after it). May be NULL.
    void (*enabled)(void *ctx, bool on);
} bleio_hci_transport_t;

typedef struct _bleio_adapter_obj_t bleio_adapter_obj_t;
// A port's own adapter: there from the start, kept from one run to the next, on at first use.
void common_hal_bleio_adapter_construct_hci_transport(bleio_adapter_obj_t *self,
    const bleio_hci_transport_t *transport, void *ctx);

// The program's UART with its RTS and CTS pins; its ctx is the adapter.
extern const bleio_hci_transport_t bleio_hci_uart_transport;

#endif
