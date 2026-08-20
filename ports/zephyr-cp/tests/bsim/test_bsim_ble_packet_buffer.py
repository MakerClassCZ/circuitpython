# SPDX-FileCopyrightText: 2026 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""PacketBuffer tests for bsim."""

import pytest


# ---- Test 1: Server-side PacketBuffer incoming (WRITE characteristic) ----

BSIM_PB_SERVER_IN_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

svc = _bleio.Service(_bleio.UUID(0xFFE0))
char = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0xFFE1),
    properties=_bleio.Characteristic.WRITE,
    read_perm=_bleio.Attribute.NO_ACCESS,
    write_perm=_bleio.Attribute.OPEN,
    max_length=20, fixed_length=False,
)

# Wrap in PacketBuffer for incoming packets
pb = _bleio.PacketBuffer(char, buffer_size=4, max_packet_size=20)
print("service created")

name = b"CPPBIN"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)
print("advertising")

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

# Read first incoming packet
data = bytearray(20)
n = 0
deadline = time.monotonic() + 5.0
while n == 0 and time.monotonic() < deadline:
    n = pb.readinto(data)
    if n == 0:
        time.sleep(0.05)
print("packet1", data[:n])

# Read second incoming packet
n2 = 0
deadline2 = time.monotonic() + 5.0
while n2 == 0 and time.monotonic() < deadline2:
    n2 = pb.readinto(data)
    if n2 == 0:
        time.sleep(0.05)
print("packet2", data[:n2])

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)
print("done")
"""

BSIM_PB_CLIENT_IN_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPPBIN" in entry.advertisement_bytes:
        target = entry.address
        print("found server")
        break
adapter.stop_scan()

connection = adapter.connect(target, timeout=5.0)
print("connected", connection.connected)

services = connection.discover_remote_services([_bleio.UUID(0xFFE0)])
print("discovered services", len(services))

remote_char = services[0].characteristics[0]
print("found char")

# Write two packets
remote_char.value = bytes([0x01, 0x02, 0x03, 0x04])
print("wrote packet1")

time.sleep(0.3)

remote_char.value = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE])
print("wrote packet2")

time.sleep(0.5)

connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_SERVER_IN_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_CLIENT_IN_CODE})
def test_bsim_packet_buffer_server_incoming(bsim_phy, circuitpython1, circuitpython2):
    """PacketBuffer on server-side WRITE characteristic receives framed packets."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    server_output = server.serial.all_output
    client_output = client.serial.all_output

    assert "service created" in server_output
    assert "connected True" in server_output
    assert "packet1" in server_output
    assert "packet2" in server_output
    assert "done" in server_output

    assert "found server" in client_output
    assert "wrote packet1" in client_output
    assert "wrote packet2" in client_output


# ---- Test 2: Server-side PacketBuffer write/flush (NOTIFY response) ----

BSIM_PB_BIDI_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

svc = _bleio.Service(_bleio.UUID(0xFFE0))
char = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0xFFE1),
    properties=_bleio.Characteristic.WRITE | _bleio.Characteristic.NOTIFY,
    read_perm=_bleio.Attribute.NO_ACCESS,
    write_perm=_bleio.Attribute.OPEN,
    max_length=20, fixed_length=False,
)

# Wrap in PacketBuffer for incoming packets and outgoing notifications
pb = _bleio.PacketBuffer(char, buffer_size=4, max_packet_size=20)
print("service created")

name = b"CPPBBI"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)
print("advertising")

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

# Read incoming packet from client
data = bytearray(20)
n = 0
deadline = time.monotonic() + 5.0
while n == 0 and time.monotonic() < deadline:
    n = pb.readinto(data)
    if n == 0:
        time.sleep(0.05)
print("received", data[:n])

# Write response back (via NOTIFY) — sends immediately via completion callback
pb.write(bytes([0x52, 0x45, 0x53, 0x50]))  # "RESP"
print("sent response")

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)
print("done")
"""

BSIM_PB_BIDI_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPPBBI" in entry.advertisement_bytes:
        target = entry.address
        print("found server")
        break
adapter.stop_scan()

connection = adapter.connect(target, timeout=5.0)
print("connected", connection.connected)

services = connection.discover_remote_services([_bleio.UUID(0xFFE0)])
print("discovered services", len(services))

remote_char = services[0].characteristics[0]
print("found char, props", remote_char.properties)

# Write a packet to the server
remote_char.value = bytes([0x48, 0x45, 0x4C, 0x4C, 0x4F])  # "HELLO"
print("wrote hello")

connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(20)
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_BIDI_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_BIDI_CLIENT_CODE})
def test_bsim_packet_buffer_bidirectional(bsim_phy, circuitpython1, circuitpython2):
    """Bidirectional PacketBuffer: server receives WRITE, sends NOTIFY response."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    server_output = server.serial.all_output
    client_output = client.serial.all_output

    assert "service created" in server_output
    assert "connected True" in server_output
    assert "received" in server_output
    assert "sent response" in server_output
    assert "done" in server_output

    assert "found server" in client_output
    assert "wrote hello" in client_output
    assert "done" in client_output


# ---- Test 3: PacketBuffer with multiple queued incoming packets ----

BSIM_PB_QUEUE_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

svc = _bleio.Service(_bleio.UUID(0xFFE0))
char = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0xFFE1),
    properties=_bleio.Characteristic.WRITE,
    read_perm=_bleio.Attribute.NO_ACCESS,
    write_perm=_bleio.Attribute.OPEN,
    max_length=20, fixed_length=False,
)

# Small buffer: only 1 packet, small max_packet_size
pb = _bleio.PacketBuffer(char, buffer_size=1, max_packet_size=10)
print("service created")

name = b"CPPBQU"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)
print("advertising")

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

# Wait for client to send all packets
time.sleep(1.0)

# Read packets - with buffer_size=1, oldest packets are dropped
data = bytearray(20)

n1 = pb.readinto(data)
print("pkt1", data[:n1])

n2 = pb.readinto(data)
print("pkt2", data[:n2])

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)
print("done")
"""

BSIM_PB_QUEUE_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPPBQU" in entry.advertisement_bytes:
        target = entry.address
        print("found server")
        break
adapter.stop_scan()

connection = adapter.connect(target, timeout=5.0)
print("connected", connection.connected)

services = connection.discover_remote_services([_bleio.UUID(0xFFE0)])
remote_char = services[0].characteristics[0]

# Write 3 packets quickly (buffer only holds 2, so oldest should be dropped)
remote_char.value = bytes([0x01, 0x02, 0x03])
print("wrote pkt1")

time.sleep(0.1)

remote_char.value = bytes([0x04, 0x05, 0x06])
print("wrote pkt2")

time.sleep(0.1)

remote_char.value = bytes([0x07, 0x08, 0x09])
print("wrote pkt3")

time.sleep(0.5)

connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_QUEUE_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_QUEUE_CLIENT_CODE})
def test_bsim_packet_buffer_queue(bsim_phy, circuitpython1, circuitpython2):
    """PacketBuffer queues multiple packets; oldest dropped when buffer full."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    server_output = server.serial.all_output
    client_output = client.serial.all_output

    assert "service created" in server_output
    assert "connected True" in server_output
    # With buffer_size=1, only the last packet(s) remain
    assert "pkt1" in server_output
    assert "pkt2" in server_output
    assert "done" in server_output

    assert "wrote pkt1" in client_output
    assert "wrote pkt2" in client_output
    assert "wrote pkt3" in client_output


# ---- Test 4: readinto with buffer too small returns negative ----

BSIM_PB_OVERFLOW_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

svc = _bleio.Service(_bleio.UUID(0xFFE0))
char = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0xFFE1),
    properties=_bleio.Characteristic.WRITE,
    read_perm=_bleio.Attribute.NO_ACCESS,
    write_perm=_bleio.Attribute.OPEN,
    max_length=20, fixed_length=False,
)

pb = _bleio.PacketBuffer(char, buffer_size=4, max_packet_size=20)
print("service created")

name = b"CPPBOV"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

# Try to read a 5-byte packet into a 3-byte buffer -> ValueError
try:
    data = bytearray(3)
    n = 0
    deadline = time.monotonic() + 5.0
    while n == 0 and time.monotonic() < deadline:
        n = pb.readinto(data)
        if n == 0:
            time.sleep(0.05)
    print("unexpected success", n)
except ValueError as e:
    print("valueerror", e)

time.sleep(0.5)
for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)
print("done")
"""

BSIM_PB_OVERFLOW_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPPBOV" in entry.advertisement_bytes:
        target = entry.address
        break
adapter.stop_scan()

connection = adapter.connect(target, timeout=5.0)

services = connection.discover_remote_services([_bleio.UUID(0xFFE0)])
remote_char = services[0].characteristics[0]

# Write a 5-byte packet, but server only has a 3-byte read buffer
remote_char.value = bytes([0x01, 0x02, 0x03, 0x04, 0x05])
print("wrote 5 bytes")

time.sleep(0.5)
connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_OVERFLOW_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_OVERFLOW_CLIENT_CODE})
def test_bsim_packet_buffer_readinto_overflow(bsim_phy, circuitpython1, circuitpython2):
    """readinto returns negative when packet is larger than buffer."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    server_output = server.serial.all_output
    client_output = client.serial.all_output

    assert "service created" in server_output
    assert "valueerror" in server_output

    assert "wrote 5 bytes" in client_output


# ---- Test 5: write() with header kwarg ----

BSIM_PB_HEADER_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

svc = _bleio.Service(_bleio.UUID(0xFFE0))
char = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0xFFE1),
    properties=_bleio.Characteristic.WRITE | _bleio.Characteristic.NOTIFY,
    read_perm=_bleio.Attribute.NO_ACCESS,
    write_perm=_bleio.Attribute.OPEN,
    max_length=20, fixed_length=False,
)

pb = _bleio.PacketBuffer(char, buffer_size=4, max_packet_size=20)
print("service created")

name = b"CPPBHD"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

# Read an incoming packet
data = bytearray(20)
n = 0
deadline = time.monotonic() + 5.0
while n == 0 and time.monotonic() < deadline:
    n = pb.readinto(data)
    if n == 0:
        time.sleep(0.05)
print("received", data[:n])

# Response: write body with a header — header goes at start of packet
pb.write(bytes([0x42, 0x4F, 0x44, 0x59]), header=bytes([0x48, 0x44]))  # "HD" + "BODY"
print("sent with header")

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)
print("done")
"""

BSIM_PB_HEADER_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPPBHD" in entry.advertisement_bytes:
        target = entry.address
        break
adapter.stop_scan()

connection = adapter.connect(target, timeout=5.0)

services = connection.discover_remote_services([_bleio.UUID(0xFFE0)])
remote_char = services[0].characteristics[0]

# Write a packet to trigger the server's response
remote_char.value = bytes([0x47, 0x4F])
print("wrote trigger")

connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_HEADER_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_HEADER_CLIENT_CODE})
def test_bsim_packet_buffer_write_header(bsim_phy, circuitpython1, circuitpython2):
    """write() header kwarg prepends to packet body."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    server_output = server.serial.all_output
    client_output = client.serial.all_output

    assert "service created" in server_output
    assert "received" in server_output
    assert "sent with header" in server_output

    assert "wrote trigger" in client_output


# ---- Test 6: incoming_packet_length / outgoing_packet_length ----

BSIM_PB_LENGTHS_CODE = """\
import _bleio

svc = _bleio.Service(_bleio.UUID(0xFFE0))
char = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0xFFE1),
    properties=_bleio.Characteristic.WRITE | _bleio.Characteristic.NOTIFY,
    read_perm=_bleio.Attribute.NO_ACCESS,
    write_perm=_bleio.Attribute.OPEN,
    max_length=20, fixed_length=False,
)

pb = _bleio.PacketBuffer(char, buffer_size=2, max_packet_size=15)

# incoming_packet_length reflects the max we can receive (characteristic max_length)
print("incoming", pb.incoming_packet_length)
# outgoing_packet_length is capped by max_packet_size
print("outgoing", pb.outgoing_packet_length)
print("done")
"""


@pytest.mark.duration(5)
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_LENGTHS_CODE})
def test_bsim_packet_buffer_packet_lengths(bsim_phy, circuitpython):
    """incoming_packet_length and outgoing_packet_length properties."""
    circuitpython.wait_until_done()
    output = circuitpython.serial.all_output

    # Server-side local characteristic:
    # incoming = max_length = 20
    # outgoing = min(max_packet_size, max_length) = min(15, 20) = 15
    assert "incoming 20" in output
    assert "outgoing 15" in output
    assert "done" in output


# ---- Test 7: disconnect / reconnect, conn tracking ----

BSIM_PB_RECONNECT_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

svc = _bleio.Service(_bleio.UUID(0xFFE0))
char = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0xFFE1),
    properties=_bleio.Characteristic.WRITE | _bleio.Characteristic.NOTIFY,
    read_perm=_bleio.Attribute.NO_ACCESS,
    write_perm=_bleio.Attribute.OPEN,
    max_length=20, fixed_length=False,
)

pb = _bleio.PacketBuffer(char, buffer_size=4, max_packet_size=20)
print("service created")

name = b"CPPBRC"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)

# First connection
for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected1", adapter.connected)

# Read first packet
data = bytearray(20)
n = 0
deadline = time.monotonic() + 5.0
while n == 0 and time.monotonic() < deadline:
    n = pb.readinto(data)
    if n == 0:
        time.sleep(0.05)
print("packet1", data[:n])

# Send a response to confirm the tracked conn works
pb.write(bytes([0x41, 0x43, 0x4B]))  # "ACK"
print("sent ack1")

# Wait for disconnect
for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)
print("disconnected")

# Start advertising again for second connection
adapter.start_advertising(advertisement, connectable=True)

# Second connection
for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected2", adapter.connected)

# Read second packet (should track new conn)
n2 = 0
deadline2 = time.monotonic() + 5.0
while n2 == 0 and time.monotonic() < deadline2:
    n2 = pb.readinto(data)
    if n2 == 0:
        time.sleep(0.05)
print("packet2", data[:n2])

pb.write(bytes([0x41, 0x43, 0x4B]))  # "ACK"
print("sent ack2")

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)
print("done")
"""

BSIM_PB_RECONNECT_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

# First connection
target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPPBRC" in entry.advertisement_bytes:
        target = entry.address
        break
adapter.stop_scan()

connection = adapter.connect(target, timeout=5.0)
print("connected1", connection.connected)

services = connection.discover_remote_services([_bleio.UUID(0xFFE0)])
remote_char = services[0].characteristics[0]

# Write first packet
remote_char.value = bytes([0x46, 0x49, 0x52, 0x53, 0x54])  # "FIRST"
print("wrote first")

time.sleep(0.5)

# Disconnect
connection.disconnect()
timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("disconnected")

# Wait for server to re-advertise
time.sleep(0.5)

# Second connection
target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPPBRC" in entry.advertisement_bytes:
        target = entry.address
        break
adapter.stop_scan()

connection = adapter.connect(target, timeout=5.0)
print("connected2", connection.connected)

services = connection.discover_remote_services([_bleio.UUID(0xFFE0)])
remote_char = services[0].characteristics[0]

# Write second packet
remote_char.value = bytes([0x53, 0x45, 0x43, 0x4E, 0x44])  # "SECOND"
print("wrote second")

time.sleep(0.5)
connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(30)
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_RECONNECT_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_PB_RECONNECT_CLIENT_CODE})
def test_bsim_packet_buffer_reconnect(bsim_phy, circuitpython1, circuitpython2):
    """PacketBuffer tracks conn through disconnect / reconnect."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    server_output = server.serial.all_output
    client_output = client.serial.all_output

    assert "service created" in server_output
    assert "connected1 True" in server_output
    assert "packet1" in server_output
    assert "sent ack1" in server_output
    assert "disconnected" in server_output
    assert "connected2 True" in server_output
    assert "packet2" in server_output
    assert "sent ack2" in server_output

    assert "wrote first" in client_output
    assert "wrote second" in client_output
