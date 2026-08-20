// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/ring_buffer.h>

#include "py/runtime.h"
#include "py/stream.h"

#include "shared/runtime/interrupt_char.h"

#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Connection.h"
#include "shared-bindings/_bleio/PacketBuffer.h"

#include "supervisor/shared/tick.h"

#include "common-hal/_bleio/Characteristic.h"
#include "common-hal/_bleio/PacketBuffer.h"

// Zephyr's ring_buf is safe for single-producer/single-consumer without
// locks. The GATT callbacks (system workqueue) are the sole producer;
// the CircuitPython VM (main thread) is the sole consumer.

// Forward declarations.
static bool conn_is_valid(bleio_packet_buffer_obj_t *self);
static bool send_pending(bleio_packet_buffer_obj_t *self);

// Called from Zephyr GATT callbacks (system workqueue context).
// Wraps incoming data with a uint16_t length prefix and pushes into ringbuf.
// Tracks the first connection so notifications target the right peer.
void bleio_packet_buffer_extend(bleio_packet_buffer_obj_t *self,
    struct bt_conn *conn, const uint8_t *data, size_t len) {
    // Track the first/current connection for notifications.
    if (conn != NULL) {
        conn_is_valid(self);  // clear stale conn if needed
        if (self->conn == NULL) {
            self->conn = conn;
        }
    }
    if (len > UINT16_MAX) {
        return;
    }

    uint16_t packet_len = (uint16_t)len;
    size_t total = sizeof(uint16_t) + len;

    // If the packet doesn't fit, drop oldest packets to make room.
    while (ring_buf_space_get(&self->ringbuf) < total) {
        uint16_t old_len;
        if (ring_buf_size_get(&self->ringbuf) < sizeof(uint16_t)) {
            // Not enough data for a length prefix, just reset.
            ring_buf_reset(&self->ringbuf);
            break;
        }
        uint32_t peeked = ring_buf_peek(&self->ringbuf, (uint8_t *)&old_len, sizeof(uint16_t));
        if (peeked < sizeof(uint16_t)) {
            ring_buf_reset(&self->ringbuf);
            break;
        }
        // Discard the length prefix
        ring_buf_get(&self->ringbuf, (uint8_t *)&old_len, sizeof(uint16_t));
        // Discard the packet data
        size_t to_discard = old_len;
        if (to_discard > ring_buf_size_get(&self->ringbuf)) {
            to_discard = ring_buf_size_get(&self->ringbuf);
        }
        uint8_t discard_buf[32];
        while (to_discard > 0) {
            size_t chunk = to_discard > sizeof(discard_buf) ? sizeof(discard_buf) : to_discard;
            ring_buf_get(&self->ringbuf, discard_buf, chunk);
            to_discard -= chunk;
        }
    }

    ring_buf_put(&self->ringbuf, (uint8_t *)&packet_len, sizeof(uint16_t));
    ring_buf_put(&self->ringbuf, data, len);
}

void bleio_packet_buffer_set_conn(bleio_packet_buffer_obj_t *self,
    struct bt_conn *conn) {
    self->conn = conn;
}

// Completion callback for bt_gatt_notify_cb — called when the PDU has been
// sent (or the buffer freed). Drains any accumulated pending data.
static void notify_complete_cb(struct bt_conn *conn, void *user_data) {
    bleio_packet_buffer_obj_t *self = (bleio_packet_buffer_obj_t *)user_data;
    self->packet_queued = false;
    send_pending(self);
}

// Returns true if the tracked connection is still connected.
// Clears self->conn if the connection is stale.
static bool conn_is_valid(bleio_packet_buffer_obj_t *self) {
    if (self->conn == NULL) {
        return false;
    }
    struct bt_conn_info info;
    if (bt_conn_get_info(self->conn, &info) != 0 ||
        info.state == BT_CONN_STATE_DISCONNECTED) {
        self->conn = NULL;
        return false;
    }
    return true;
}

// Send the pending outgoing buffer via GATT notify.
// Returns true if sent successfully (or terminal failure).
static bool send_pending(bleio_packet_buffer_obj_t *self) {
    if (self->pending_size == 0) {
        return true;
    }
    if (self->characteristic == NULL ||
        self->characteristic->service == NULL ||
        self->characteristic->service->is_remote) {
        self->pending_size = 0;
        return true;
    }

    bleio_characteristic_obj_t *c = self->characteristic;
    if (!(c->props & CHAR_PROP_NOTIFY) || !c->service->registered) {
        self->pending_size = 0;
        return true;
    }

    struct bt_gatt_notify_params params = {
        .attr = &c->service->attrs[c->value_attr_index],
        .data = self->outgoing_buffer,
        .len = self->pending_size,
        .func = notify_complete_cb,
        .user_data = self,
    };

    // If the tracked connection is stale, clear it.
    conn_is_valid(self);

    int err = bt_gatt_notify_cb(self->conn, &params);
    if (err == 0) {
        self->pending_size = 0;
        self->packet_queued = true;
        return true;
    }
    if (err == -ENOTCONN) {
        // Peer disconnected — clear tracking, discard pending.
        self->conn = NULL;
        self->pending_size = 0;
        return true;
    }
    // -ENOMEM (no TX buffer) — leave pending, caller will retry.
    return false;
}

void common_hal_bleio_packet_buffer_construct(
    bleio_packet_buffer_obj_t *self, bleio_characteristic_obj_t *characteristic,
    size_t buffer_size, size_t max_packet_size) {

    self->characteristic = characteristic;
    self->timeout_ms = 0;
    self->max_packet_size = max_packet_size;
    self->conn = NULL;
    self->client = (characteristic->service != NULL && characteristic->service->is_remote);
    self->pending_size = 0;
    self->packet_queued = false;

    // Allocate ring buffer: buffer_size packets, each with 2-byte length prefix
    self->ringbuf_size = buffer_size * (sizeof(uint16_t) + max_packet_size);
    self->ringbuf_data = m_malloc_without_collect(self->ringbuf_size);
    ring_buf_init(&self->ringbuf, self->ringbuf_size, self->ringbuf_data);

    // Allocate outgoing buffer for pending writes
    bleio_characteristic_properties_t props =
        common_hal_bleio_characteristic_get_properties(characteristic);
    if (self->client) {
        // Client-side: we write to remote characteristic
        self->outgoing_buffer = m_malloc_without_collect(max_packet_size);
    } else {
        // Server-side: we notify via local characteristic
        if (props & (CHAR_PROP_NOTIFY | CHAR_PROP_INDICATE)) {
            self->outgoing_buffer = m_malloc_without_collect(max_packet_size);
        } else {
            self->outgoing_buffer = NULL;
        }
    }

    // Set ourselves as the characteristic's observer
    bleio_characteristic_set_observer(characteristic, MP_OBJ_FROM_PTR(self));

    // For client-side characteristics with NOTIFY/INDICATE, subscribe to notifications
    if (self->client && (props & (CHAR_PROP_NOTIFY | CHAR_PROP_INDICATE))) {
        bool do_notify = (props & CHAR_PROP_NOTIFY) != 0;
        bool do_indicate = (props & CHAR_PROP_INDICATE) != 0;
        common_hal_bleio_characteristic_set_cccd(characteristic, do_notify, do_indicate);
    }
}

// Allocation-free version for BLE workflow use (not yet implemented for Zephyr).
void _common_hal_bleio_packet_buffer_construct(
    bleio_packet_buffer_obj_t *self, bleio_characteristic_obj_t *characteristic,
    uint32_t *incoming_buffer, size_t incoming_buffer_size,
    uint32_t *outgoing_buffer1, uint32_t *outgoing_buffer2, size_t max_packet_size,
    ble_event_handler_t *static_handler_entry) {
    (void)self;
    (void)characteristic;
    (void)incoming_buffer;
    (void)incoming_buffer_size;
    (void)outgoing_buffer1;
    (void)outgoing_buffer2;
    (void)max_packet_size;
    (void)static_handler_entry;
    mp_raise_NotImplementedError(NULL);
}

mp_int_t common_hal_bleio_packet_buffer_readinto(bleio_packet_buffer_obj_t *self,
    uint8_t *data, size_t len) {
    // Need at least 2 bytes for the length prefix
    if (ring_buf_size_get(&self->ringbuf) < sizeof(uint16_t)) {
        return 0;
    }

    // Peek at the packet length (don't consume yet)
    uint16_t packet_length;
    uint32_t peeked = ring_buf_peek(&self->ringbuf, (uint8_t *)&packet_length, sizeof(uint16_t));
    if (peeked < sizeof(uint16_t)) {
        return 0;
    }

    mp_int_t ret;
    if (packet_length > len) {
        // Packet is longer than requested. Return negative of overrun value.
        ret = len - packet_length;
        // Discard the packet
        ring_buf_get(&self->ringbuf, (uint8_t *)&packet_length, sizeof(uint16_t));
        if (packet_length <= ring_buf_size_get(&self->ringbuf)) {
            // Discard data in chunks
            uint8_t discard[32];
            size_t remaining = packet_length;
            while (remaining > 0) {
                size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
                ring_buf_get(&self->ringbuf, discard, chunk);
                remaining -= chunk;
            }
        }
    } else {
        // Consume the length prefix
        ring_buf_get(&self->ringbuf, (uint8_t *)&packet_length, sizeof(uint16_t));
        // Read packet data
        ring_buf_get(&self->ringbuf, data, packet_length);
        ret = packet_length;
    }

    return ret;
}

mp_int_t common_hal_bleio_packet_buffer_write(bleio_packet_buffer_obj_t *self,
    const uint8_t *data, size_t len, uint8_t *header, size_t header_len) {
    if (self->outgoing_buffer == NULL) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Writes not supported on Characteristic"));
    }

    mp_int_t outgoing_packet_length =
        common_hal_bleio_packet_buffer_get_outgoing_packet_length(self);
    if (outgoing_packet_length < 0) {
        return -1;
    }

    mp_int_t total_len = len + header_len;
    if (total_len > outgoing_packet_length) {
        mp_raise_ValueError_varg(
            MP_ERROR_TEXT("Total data to write is larger than %q"),
            MP_QSTR_outgoing_packet_length);
    }
    if (total_len > (mp_int_t)self->max_packet_size) {
        mp_raise_ValueError_varg(
            MP_ERROR_TEXT("Total data to write is larger than %q"),
            MP_QSTR_max_packet_size);
    }

    // If no room to append, wait until pending is sent.
    if (len + self->pending_size > (size_t)outgoing_packet_length) {
        while (self->pending_size != 0 &&
               !mp_hal_is_interrupted()) {
            RUN_BACKGROUND_TASKS;
        }
    }
    if (mp_hal_is_interrupted()) {
        return -1;
    }

    size_t num_bytes_written = 0;

    if (self->pending_size == 0) {
        memcpy(self->outgoing_buffer, header, header_len);
        self->pending_size += header_len;
        num_bytes_written += header_len;
    }
    memcpy(self->outgoing_buffer + self->pending_size, data, len);
    self->pending_size += len;
    num_bytes_written += len;

    // Send immediately if no write is queued.
    if (!self->packet_queued) {
        send_pending(self);
    }
    return num_bytes_written;
}

mp_int_t common_hal_bleio_packet_buffer_get_incoming_packet_length(
    bleio_packet_buffer_obj_t *self) {
    if (self->characteristic == NULL) {
        return -1;
    }

    if (self->characteristic->service != NULL &&
        self->characteristic->service->is_remote &&
        self->characteristic->service->connection != mp_const_none &&
        (common_hal_bleio_characteristic_get_properties(self->characteristic) &
         (CHAR_PROP_INDICATE | CHAR_PROP_NOTIFY))) {
        // We are receiving from a remote service via NOTIFY/INDICATE.
        bleio_connection_obj_t *connection =
            MP_OBJ_TO_PTR(self->characteristic->service->connection);
        if (connection != NULL && connection->connection != NULL &&
            common_hal_bleio_connection_get_connected(connection)) {
            return common_hal_bleio_connection_get_max_packet_length(connection->connection);
        }
        return -1;
    }
    return self->characteristic->max_length;
}

mp_int_t common_hal_bleio_packet_buffer_get_outgoing_packet_length(
    bleio_packet_buffer_obj_t *self) {
    if (self->characteristic == NULL) {
        return -1;
    }

    if (self->characteristic->service != NULL &&
        !self->characteristic->service->is_remote &&
        (common_hal_bleio_characteristic_get_properties(self->characteristic) &
         (CHAR_PROP_INDICATE | CHAR_PROP_NOTIFY))) {
        // We are sending to a client via NOTIFY/INDICATE.
        // Use max_packet_size since we don't track MTU dynamically here.
        return MIN(self->max_packet_size, self->characteristic->max_length);
    }
    // Writing to remote characteristic or local without NOTIFY
    return MIN(self->characteristic->max_length, self->max_packet_size);
}

void common_hal_bleio_packet_buffer_flush(bleio_packet_buffer_obj_t *self) {
    // With the completion callback, writes drain automatically.
    // flush() just waits for any queued data to be sent.
    while (self->pending_size > 0 &&
           !mp_hal_is_interrupted()) {
        RUN_BACKGROUND_TASKS;
        if (!send_pending(self)) {
            // Couldn't send — wait and retry.
            RUN_BACKGROUND_TASKS;
        }
    }
}

bool common_hal_bleio_packet_buffer_deinited(bleio_packet_buffer_obj_t *self) {
    return self->characteristic == NULL;
}

void common_hal_bleio_packet_buffer_deinit(bleio_packet_buffer_obj_t *self) {
    if (common_hal_bleio_packet_buffer_deinited(self)) {
        return;
    }
    bleio_characteristic_clear_observer(self->characteristic);
    self->characteristic = NULL;
    // Free ringbuf_data allocated with m_malloc_without_collect
    m_free(self->ringbuf_data);
    self->ringbuf_data = NULL;
    // Free outgoing buffer
    m_free(self->outgoing_buffer);
    self->outgoing_buffer = NULL;
}

bool common_hal_bleio_packet_buffer_connected(bleio_packet_buffer_obj_t *self) {
    if (common_hal_bleio_packet_buffer_deinited(self)) {
        return false;
    }
    // Check if the characteristic's connection is still active.
    if (self->characteristic->service != NULL &&
        self->characteristic->service->is_remote) {
        if (self->characteristic->service->connection == mp_const_none) {
            return false;
        }
        bleio_connection_obj_t *connection =
            MP_OBJ_TO_PTR(self->characteristic->service->connection);
        return common_hal_bleio_connection_get_connected(connection);
    }
    return true;
}
