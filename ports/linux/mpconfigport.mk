# This file is part of the CircuitPython project: https://circuitpython.org
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
#
# SPDX-License-Identifier: MIT

# A hosted build: libc and libm come from the system, the compiler is the host's.
CIRCUITPY_LIBC_STRING0 = 0
INTERNAL_LIBM = 0
LONGINT_IMPL = MPZ
CIRCUITPY_MIN_GCC_VERSION = 10
CIRCUITPY_BUILD_EXTENSIONS = elf

# CIRCUITPY is a FAT image file, mapped into memory (supervisor/internal_flash.c).
INTERNAL_FLASH_FILESYSTEM = 1

# stdin/stdout is the serial console; there is no USB device side.
CIRCUITPY_PORT_SERIAL = 1
CIRCUITPY_USB_DEVICE = 0

# No hardware behind these on a desktop or a handheld.
CIRCUITPY_ANALOGIO = 0
CIRCUITPY_AUDIOBUSIO = 0
CIRCUITPY_AUDIOIO = 0
CIRCUITPY_BLEIO_HCI = 0
CIRCUITPY_BUSIO = 0
CIRCUITPY_BUSDEVICE = 0
CIRCUITPY_COUNTIO = 0
CIRCUITPY_FREQUENCYIO = 0
CIRCUITPY_I2CTARGET = 0
CIRCUITPY_NEOPIXEL_WRITE = 0
CIRCUITPY_NVM = 0
CIRCUITPY_PULSEIO = 0
CIRCUITPY_PWMIO = 0
CIRCUITPY_ROTARYIO = 0
CIRCUITPY_SDCARDIO = 0
CIRCUITPY_TOUCHIO = 0
CIRCUITPY_PIXELBUF = 0
CIRCUITPY_PARALLELDISPLAYBUS = 0

# The display is an SDL2 window or screen (bindings/sdl); the SPI and I2C display buses have no
# hardware here.
CIRCUITPY_SDL = 1
CIRCUITPY_FOURWIRE = 0
CIRCUITPY_I2CDISPLAYBUS = 0
CIRCUITPY_SHARPDISPLAY = 0

# The port heap is the system allocator with a budget (supervisor/port.c); the bundled TLSF is
# 32-bit only.
CIRCUITPY_LIB_TLSF = 0
