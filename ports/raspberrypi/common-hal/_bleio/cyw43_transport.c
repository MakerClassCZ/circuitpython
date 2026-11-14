// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimír Smitka
//
// SPDX-License-Identifier: MIT

// _bleio on a board with a CYW43439: devices/ble_hci's host with the chip's Bluetooth core as its
// controller (cyw43_hci.c). The adapter is the board's own: there from the start as
// _bleio.adapter, kept from one run to the next, and turned on at its first use.

#include <string.h>

#include "py/mperrno.h"

#include "py/obj.h"
#include "supervisor/port_heap.h"
#include "shared-bindings/_bleio/__init__.h"
#include "common-hal/_bleio/Adapter.h"
#include "common-hal/_bleio/hci.h"
#include "common-hal/_bleio/hci_transport.h"

#include "cyw43_hci.h"
#include "cyw43_transport.h"

// The packet being handed to the host, which reads it a byte at a time; from the port's heap at
// the first enable, as cyw43_hci.c's buffers are.
#define PACKET_SIZE 1025
static uint8_t *_packet;
static size_t _length, _at;

static size_t cyw43_available(void *ctx) {
    (void)ctx;
    if (_at == _length) {
        _length = _packet != NULL ? cyw43_hci_receive(_packet, PACKET_SIZE) : 0;
        _at = 0;
    }
    return _length - _at;
}

static size_t cyw43_read(void *ctx, uint8_t *data, size_t len, int *errcode) {
    *errcode = 0;
    size_t n = cyw43_available(ctx);
    n = n < len ? n : len;
    memcpy(data, _packet + _at, n);
    _at += n;
    return n;
}

// Always one whole H4 packet from hci.c.
static size_t cyw43_write(void *ctx, const uint8_t *h4, size_t len, int *errcode) {
    (void)ctx;
    *errcode = cyw43_hci_send(h4, len) ? 0 : MP_EIO;
    return *errcode ? 0 : len;
}

static void cyw43_enabled(void *ctx, bool on) {
    (void)ctx;
    _length = _at = 0;
    if (on) {
        // A core that does not come up, or no RAM for the buffers, answers nothing: the HCI Reset
        // that follows times out.
        if (_packet == NULL) {
            _packet = port_malloc(PACKET_SIZE, false);
        }
        cyw43_hci_init();
    } else {
        cyw43_hci_deinit();
    }
}

static const bleio_hci_transport_t cyw43_transport = {
    .available = cyw43_available,
    .read = cyw43_read,
    .write = cyw43_write,
    .set_rts = NULL,
    .get_cts = NULL,
    .enabled = cyw43_enabled,
};

void bleio_cyw43_attach_adapter(void) {
    common_hal_bleio_adapter_construct_hci_transport(&common_hal_bleio_adapter_obj, &cyw43_transport, NULL);
}
