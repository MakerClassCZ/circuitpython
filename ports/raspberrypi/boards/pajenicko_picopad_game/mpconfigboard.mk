USB_VID = 0x2E8A
USB_PID = 0x1063

USB_PRODUCT = "PicoPad GameEngine"
USB_MANUFACTURER = "Pajenicko s.r.o."

CHIP_VARIANT = RP2040
CHIP_FAMILY = rp2

EXTERNAL_FLASH_DEVICES = "W25Q16JVxQ"

CIRCUITPY_KEYPAD = 1
CIRCUITPY_AUDIOIO = 1

# _stage C module ON too: lets the original python-ugame/stage games run alongside picogame,
# enabling a head-to-head benchmark (original stage vs the picogame stage shim) on the SAME device.
CIRCUITPY_STAGE = 1

# Experimental dual-core-capable game engine (milestone 1: core0 renderer).
CIRCUITPY_PICOGAME = 1
CIRCUITPY_PICOGAME_FAST_DISPLAY = 1
# RGB444 (12-bit COLMOD) compiled IN: measured 2026-08-02 on the refresh-floor bench, the 12-bit
# path cuts the full-screen SPI floor 23.6 -> 18.65 ms (-21 %) now that compose (incl. the pack)
# hides under the shorter send. Games opt in at RUNTIME via picogame_game.setup(rgb444="auto") -
# it wins on C-composed transfer-bound scenes, loses where a Python StripDraw dominates compose.
CIRCUITPY_PICOGAME_RGB444 = 1

# NOTE: the firmware is kept a general-purpose CircuitPython build (ulab, synthio,
# vectorio, bitmaptools, etc. all left ON). If flash ever gets tight, these
# game-IRRELEVANT, non-Wi-Fi modules can be disabled (~150-200 KB; measured 2026-07:
# ULAB ~109 KB and AUDIOMP3 ~38 KB are the big two, the rest are small):
#   CIRCUITPY_ULAB, AUDIOMP3, JPEGIO, GIFIO, VECTORIO, BITMAPTOOLS, BITMAPFILTER,
#   RAINBOWIO  (PICODVI/_EVE/QRIO already off - see below for qrio's rationale).
# Do NOT disable the GAME-CRITICAL set: SYNTHIO + AUDIOPWMIO + AUDIOMIXER + AUDIOCORE
# (picogame_synth/picogame_audio = ALL game music & SFX) and KEYPAD (picogame_input's
# device backend). An earlier version of this list wrongly offered SYNTHIO/KEYPAD/
# AUDIOIO for disabling - it pre-dated the 2026-07 game-audio work.

# USB host OFF: the rp2 port default (=1) costs 23.7 KB of static RAM (PIO-USB EP pool,
# EP SW buffers, USBH/HID/hub/MSC-host structs - measured 2026-07-24) and the PicoPad has
# no host connector; RP2040's native USB is the CircuitPython DEVICE port (CIRCUITPY/REPL),
# host would need PIO-USB on two free expansion GPIOs = a special build, not this one.
# Same "not a capability of this device" rationale as CIRCUITPY_QRIO below.
CIRCUITPY_USB_HOST = 0

# Keep Wi-Fi/CYW43 so the board's CYW pins (status LED, etc.) stay valid.
CIRCUITPY_CYW43 = 1
CIRCUITPY_SSL = 1
CIRCUITPY_HASHLIB = 1
CIRCUITPY_WEB_WORKFLOW = 1
CIRCUITPY_MDNS = 1
CIRCUITPY_SOCKETPOOL = 1
CIRCUITPY_WIFI = 1

# Free flash for the engine: drop HDMI/DVI and the FT8xx EVE driver.
CIRCUITPY_PICODVI = 0
CIRCUITPY__EVE = 0

# qrio is QR DECODE only -- it reads QR codes from a camera image buffer. The PicoPad has no camera,
# so the decoder can never get an input here: it is not a capability of this device (NOT a feature cut
# of the general-purpose build). Disabling it also drops its quirc backend, ~32 KB total (measured).
# QR *generation* is unaffected: that is adafruit_miniqr, a pure-Python lib with no dependency on qrio.
CIRCUITPY_QRIO = 0

# Optimization: the rp2 port default is -O3, but on this Cortex-M0+ (no SIMD/FPU, 16 KB XIP cache)
# most of what -O3 adds over -O2 is bloat our code can't use -- the vectorizer (no SIMD), ipa-cp-clone,
# and heavy loop unroll/peel/version passes. Measured on-device: -O2 plus just the five cheap loop
# passes below MATCHES full -O3 engine speed (picogame_bench_hotpath within +-1% across all kernels;
# the -O2-only transpose +17% / plain +5% penalty vanishes) while using ~154 KB LESS flash than -O3
# (87.8% vs 97.8% of the 1.5 MB region). gc.o/vm.o stay -O3 via SUPEROPT regardless, so the
# interpreter core is unaffected. See the bench matrix in final/bench-firmware/.
OPTIMIZATION_FLAGS = -O2 -funswitch-loops -fpredictive-commoning -fgcse-after-reload -ftree-partial-pre -fsplit-paths

CFLAGS += \
    -DCYW43_PIN_WL_DYNAMIC=0 \
	-DCYW43_DEFAULT_PIN_WL_HOST_WAKE=24 \
	-DCYW43_DEFAULT_PIN_WL_REG_ON=23 \
	-DCYW43_DEFAULT_PIN_WL_CLOCK=29 \
	-DCYW43_DEFAULT_PIN_WL_DATA_IN=24 \
	-DCYW43_DEFAULT_PIN_WL_DATA_OUT=24 \
	-DCYW43_DEFAULT_PIN_WL_CS=25 \
	-DCYW43_WL_GPIO_COUNT=3 \
	-DCYW43_WL_GPIO_LED_PIN=0

# Must be accompanied by a linker script change
CFLAGS += -DCIRCUITPY_FIRMWARE_SIZE='(1536 * 1024)'
