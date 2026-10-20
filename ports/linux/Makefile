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

INC += \
	-I. \
	-I../.. \
	-I../../lib/mp-readline \
	-I$(BUILD) \
	-I$(BUILD)/genhdr \
	-I./boards/$(BOARD)

SRC_C += \
	background.c \
	debug_hook.c \
	interrupts.c \
	linux_main.c \
	linux_pins.c \
	linux_watch.c \
	mphalport.c \
	boards/$(BOARD)/board.c \
	boards/$(BOARD)/pins.c \
	shared/runtime/gchelper_generic.c

# A cross build for Windows: make CROSS_COMPILE=x86_64-w64-mingw32- SDL_DIR=.../SDL2-2.x/x86_64-w64-mingw32
ifneq (,$(findstring mingw,$(CROSS_COMPILE)))
WINDOWS = 1
EXE = .exe
endif

ifeq ($(CIRCUITPY_SDL),1)
ifeq ($(WINDOWS),1)
# The port has its own main(); SDL's console build needs neither SDL_main nor -mwindows.
SDL_CFLAGS ?= -I$(SDL_DIR)/include/SDL2 -DSDL_MAIN_HANDLED
SDL_LIBS ?= -L$(SDL_DIR)/lib -lSDL2
else
SDL_CFLAGS ?= $(shell sdl2-config --cflags)
SDL_LIBS ?= $(shell pkg-config --exists sdl2 && pkg-config --libs sdl2 || echo -l:libSDL2-2.0.so.0)
endif
SRC_C += \
	bindings/sdl/__init__.c \
	bindings/sdl/Framebuffer.c \
	common-hal/sdl/Framebuffer.c \
	sdl_host.c
INC += $(SDL_CFLAGS)
endif

ifeq ($(DEBUG),1)
COPT = -O0 -ggdb3
else
COPT = -O2 -g
endif

CFLAGS += $(INC) -Wall -Werror -std=gnu11 $(BASE_CFLAGS) $(COPT)
# clang (llvm-mingw, macOS) has neither -fsingle-precision-constant nor GCC's inlining parameter;
# the shared code writes its float constants as floats anyway.
ifneq (,$(findstring clang,$(shell $(CC) --version 2>/dev/null)))
CLANG = 1
CFLAGS := $(filter-out -fsingle-precision-constant --param max-inline-insns-single=500 -Wimplicit-fallthrough=2,$(CFLAGS))
# Warning classes clang reads more widely than GCC, which the GCC builds keep checking.
CFLAGS += -Wno-expansion-to-defined -Wno-unreachable-code -Wno-double-promotion -Wno-literal-conversion \
	-Wno-c23-extensions -Wno-unused-variable -Wno-unused-but-set-variable
endif
ifeq ($(WINDOWS),1)
CFLAGS += -D_WIN32_WINNT=0x0A00
endif
CFLAGS += -D_GNU_SOURCE -fno-common
# The shared code is written for 32-bit MCUs; these warnings fire on a 64-bit host only.
CFLAGS += -Wno-error=cast-align -Wno-cast-align -Wno-error=unused-parameter -Wno-error=sign-compare

ifeq ($(CIRCUITPY_HOSTNETWORK),1)
SRC_C += \
	bindings/hostnetwork/__init__.c \
	bindings/hostnetwork/HostNetwork.c \
	common-hal/hostnetwork/HostNetwork.c
endif

# TLS: mbedtls 4.x as the RP2 port builds it (see ports/raspberrypi/Makefile for the reasons
# behind the exclusions and the generated PSA wrappers).
ifeq ($(CIRCUITPY_SSL),1)
mbedtls_sources = $(patsubst $(TOP)/%,%,$(filter-out $(2),$(wildcard $(TOP)/$(1)/*.c)))
SRC_MBEDTLS := \
	$(call mbedtls_sources,lib/mbedtls/library,\
		$(TOP)/lib/mbedtls/library/mbedtls_config.c \
		$(TOP)/lib/mbedtls/library/net_sockets.c \
		$(TOP)/lib/mbedtls/library/pkcs7.c) \
	$(call mbedtls_sources,lib/mbedtls/tf-psa-crypto/core,\
		$(TOP)/lib/mbedtls/tf-psa-crypto/core/psa_its_file.c \
		$(TOP)/lib/mbedtls/tf-psa-crypto/core/tf_psa_crypto_config.c) \
	$(call mbedtls_sources,lib/mbedtls/tf-psa-crypto/drivers/builtin/src) \
	$(call mbedtls_sources,lib/mbedtls/tf-psa-crypto/extras) \
	$(call mbedtls_sources,lib/mbedtls/tf-psa-crypto/platform) \
	$(call mbedtls_sources,lib/mbedtls/tf-psa-crypto/utilities)
SRC_C += $(SRC_MBEDTLS) lib/mbedtls_config/mbedtls_port.c lib/mbedtls_config/crt_bundle.c
CFLAGS += \
	-isystem $(TOP)/lib/mbedtls/include \
	-isystem $(TOP)/lib/mbedtls/tf-psa-crypto/include \
	-isystem $(TOP)/lib/mbedtls/tf-psa-crypto/drivers/builtin/include \
	-isystem $(TOP)/lib/mbedtls/library \
	-isystem $(TOP)/lib/mbedtls/tf-psa-crypto/core \
	-isystem $(TOP)/lib/mbedtls/tf-psa-crypto/dispatch \
	-isystem $(TOP)/lib/mbedtls/tf-psa-crypto/drivers/builtin/src \
	-isystem $(TOP)/lib/mbedtls/tf-psa-crypto/extras \
	-isystem $(TOP)/lib/mbedtls/tf-psa-crypto/platform \
	-isystem $(TOP)/lib/mbedtls/tf-psa-crypto/utilities \
	-DMBEDTLS_CONFIG_FILE='"$(TOP)/lib/mbedtls_config/mbedtls_config.h"' \
	-DTF_PSA_CRYPTO_CONFIG_FILE='"$(TOP)/lib/mbedtls_config/tf_psa_crypto_config.h"'

$(BUILD)/x509_crt_bundle.S: $(TOP)/lib/certificates/data/roots-full.pem $(TOP)/tools/gen_crt_bundle.py
	$(Q)$(PYTHON) $(TOP)/tools/gen_crt_bundle.py -i $< -o $@ --asm
ifeq ($(WINDOWS),1)
NOEXECSTACK =
else
NOEXECSTACK = -Wa,--noexecstack
endif
$(BUILD)/x509_crt_bundle.o: $(BUILD)/x509_crt_bundle.S
	$(STEPECHO) "CC $<"
	$(Q)$(CC) $(CFLAGS) $(NOEXECSTACK) -c -o $@ $<
OBJ_MBEDTLS := $(BUILD)/x509_crt_bundle.o

MBEDTLS_GEN := $(BUILD)/tf-psa-crypto
MBEDTLS_WRAPPERS_H := $(MBEDTLS_GEN)/psa_crypto_driver_wrappers.h
MBEDTLS_WRAPPERS_C := $(MBEDTLS_GEN)/psa_crypto_driver_wrappers_no_static.c
$(MBEDTLS_WRAPPERS_C): $(MBEDTLS_WRAPPERS_H)
	@true
$(MBEDTLS_WRAPPERS_H): $(TOP)/lib/mbedtls/tf-psa-crypto/scripts/generate_driver_wrappers.py
	$(STEPECHO) "GEN $@"
	$(Q)mkdir -p $(MBEDTLS_GEN)
	$(Q)PYTHONPATH=$(TOP)/lib/mbedtls/framework/scripts $(PYTHON) $< $(MBEDTLS_GEN)
OBJ_MBEDTLS += $(MBEDTLS_WRAPPERS_C:.c=.o)
CFLAGS += -I$(MBEDTLS_GEN)
$(patsubst %.c,$(BUILD)/%.o,$(SRC_MBEDTLS)) $(OBJ_MBEDTLS): $(MBEDTLS_WRAPPERS_H)
ifeq ($(CLANG),1)
$(patsubst %.c,$(BUILD)/%.o,$(SRC_MBEDTLS)) $(OBJ_MBEDTLS): CFLAGS += \
	-Wno-array-bounds \
	-Wno-undef \
	-Wno-uninitialized-const-pointer
else
$(patsubst %.c,$(BUILD)/%.o,$(SRC_MBEDTLS)) $(OBJ_MBEDTLS): CFLAGS += \
	-Wno-array-bounds \
	-Wno-suggest-attribute=format \
	-Wno-undef \
	-Wno-unused-but-set-variable
endif
endif

# CircuitPython's main() becomes circuitpython_main(); linux_main.c owns the process entry.
$(BUILD)/main.o: CFLAGS += -Dmain=circuitpython_main

ifeq ($(WINDOWS),1)
# winpthreads linked in, so the program needs no DLL of the toolchain's beside it.
LIBS = -lm $(shell $(CC) -print-file-name=libwinpthread.a) -lws2_32 -lbcrypt -lwinmm $(SDL_LIBS)
# The VM runs on the main thread, whose stack size is fixed here.
LDFLAGS += -Wl,--stack,8388608
else
LIBS = -lm -lpthread -lrt $(SDL_LIBS)
endif
# Drops the shared weak defaults this port overrides (and the TLSF calls inside them).
LDFLAGS += -Wl,--gc-sections

OBJ += $(PY_O) $(SUPERVISOR_O) $(addprefix $(BUILD)/, $(SRC_C:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_COMMON_HAL_SHARED_MODULE_EXPANDED:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_CIRCUITPY_COMMON:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_MOD:.c=.o))
OBJ += $(OBJ_MBEDTLS)

# mbedtls has no qstrs, and its headers do not take the qstr preprocessing flags.
SRC_QSTR += $(filter-out $(SRC_MBEDTLS),$(SRC_C)) $(SRC_SUPERVISOR) $(SRC_CIRCUITPY_COMMON) \
	$(SRC_COMMON_HAL_SHARED_MODULE_EXPANDED) $(SRC_MOD)
SRC_QSTR_PREPROCESSOR +=

all: $(BUILD)/circuitpython$(EXE) $(BUILD)/debugadapter.py

# The debug adapter lives beside the program (see debug_hook.c).
$(BUILD)/debugadapter.py: python/debugadapter.py
	$(Q)cp $< $@

$(BUILD)/circuitpython$(EXE): $(OBJ)
	$(STEPECHO) "LINK $@"
	$(Q)echo $^ > $(BUILD)/firmware.objs
	$(Q)$(CC) -o $@ $(LDFLAGS) @$(BUILD)/firmware.objs $(LIBS)
	$(Q)$(SIZE) $@

include $(TOP)/py/mkrules.mk
