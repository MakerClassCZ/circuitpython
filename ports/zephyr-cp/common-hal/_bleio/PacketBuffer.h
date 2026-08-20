// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>

#include <zephyr/sys/ring_buffer.h>

#include "py/obj.h"
#include "shared-bindings/_bleio/Characteristic.h"

struct bt_conn;

typedef void *ble_event_handler_t;

typedef struct {
    mp_obj_base_t base;
    bleio_characteristic_obj_t *characteristic;
    uint32_t timeout_ms;
    struct ring_buf ringbuf;
    uint8_t *ringbuf_data;
    size_t ringbuf_size;
    size_t max_packet_size;
    // Outgoing pending buffer
    uint8_t *outgoing_buffer;
    uint16_t pending_size;
    bool packet_queued;
    struct bt_conn *conn;
    bool client;
} bleio_packet_buffer_obj_t;

// Called from GATT callbacks (system workqueue context) to push
// data into the PacketBuffer ring buffer with length-prefix framing.
void bleio_packet_buffer_extend(bleio_packet_buffer_obj_t *self,
    struct bt_conn *conn, const uint8_t *data, size_t len);

// Called from CCCD write callback to record the subscribing connection.
void bleio_packet_buffer_set_conn(bleio_packet_buffer_obj_t *self,
    struct bt_conn *conn);
