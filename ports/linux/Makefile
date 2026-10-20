# This file is part of the CircuitPython project: https://circuitpython.org
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
#
# SPDX-License-Identifier: MIT

# CircuitPython as a Linux process: the supervisor, the VM and the CIRCUITPY drive (a FAT image
# file) run in one program, with stdin/stdout as the serial console.

BOARD ?= desktop
CROSS_COMPILE ?=

include ../../py/circuitpy_mkenv.mk

MACHINE := $(shell $(CC) -dumpmachine | cut -d- -f1)

INC += \
	-I. \
	-I../.. \
	-I../../lib/mp-readline \
	-I$(BUILD) \
	-I$(BUILD)/genhdr \
	-I./boards/$(BOARD)

SRC_C += \
	background.c \
	linux_main.c \
	linux_pins.c \
	mphalport.c \
	boards/$(BOARD)/board.c \
	boards/$(BOARD)/pins.c \
	shared/runtime/gchelper_generic.c

ifeq ($(DEBUG),1)
COPT = -O0 -ggdb3
else
COPT = -O2 -g
endif

CFLAGS += $(INC) -Wall -Werror -std=gnu11 $(BASE_CFLAGS) $(COPT) -DLINUX_MACHINE='"$(MACHINE)"'
CFLAGS += -D_GNU_SOURCE -fno-common
# The shared code is written for 32-bit MCUs; these warnings fire on a 64-bit host only.
CFLAGS += -Wno-error=cast-align -Wno-cast-align -Wno-error=unused-parameter -Wno-error=sign-compare

# CircuitPython's main() becomes circuitpython_main(); linux_main.c owns the process entry.
$(BUILD)/main.o: CFLAGS += -Dmain=circuitpython_main

LIBS = -lm -lpthread -lrt
# Drops the shared weak defaults this port overrides (and the TLSF calls inside them).
LDFLAGS += -Wl,--gc-sections
# Function names in the hard fault backtrace.
LDFLAGS += -rdynamic

OBJ += $(PY_O) $(SUPERVISOR_O) $(addprefix $(BUILD)/, $(SRC_C:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_COMMON_HAL_SHARED_MODULE_EXPANDED:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_CIRCUITPY_COMMON:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_MOD:.c=.o))

SRC_QSTR += $(SRC_C) $(SRC_SUPERVISOR) $(SRC_CIRCUITPY_COMMON) \
	$(SRC_COMMON_HAL_SHARED_MODULE_EXPANDED) $(SRC_MOD)
SRC_QSTR_PREPROCESSOR +=

all: $(BUILD)/circuitpython

$(BUILD)/circuitpython: $(OBJ)
	$(STEPECHO) "LINK $@"
	$(Q)echo $^ > $(BUILD)/firmware.objs
	$(Q)$(CC) -o $@ $(LDFLAGS) @$(BUILD)/firmware.objs $(LIBS)
	$(Q)$(SIZE) $@

include $(TOP)/py/mkrules.mk
