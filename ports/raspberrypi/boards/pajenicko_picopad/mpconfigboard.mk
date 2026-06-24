USB_VID = 0x2E8A
USB_PID = 0x1063

USB_PRODUCT = "PicoPad"
USB_MANUFACTURER = "Pajenicko s.r.o."

CHIP_VARIANT = RP2040
CHIP_FAMILY = rp2

EXTERNAL_FLASH_DEVICES = "W25Q16JVxQ"

CIRCUITPY_KEYPAD = 1
CIRCUITPY_AUDIOIO = 1
CIRCUITPY_AUDIOEFFECTS = 0

# picogame: the native 2D game engine (import picogame). FAST_DISPLAY = the DMA/strip ST7789 backend.
CIRCUITPY_PICOGAME = 1
CIRCUITPY_PICOGAME_FAST_DISPLAY = 1
# RGB444 (12-bit COLMOD) compiled out: the ST7789 supports it, but on this CPU-balanced panel the
# per-strip pack >= the SPI byte saving (measured net loss). Set to 1 to compile it in.
CIRCUITPY_PICOGAME_RGB444 = 0

# Wi-Fi/CYW43 kept (Pico W): status LED + future multiplayer.
CIRCUITPY_CYW43 = 1
CIRCUITPY_SSL = 1
CIRCUITPY_HASHLIB = 1
CIRCUITPY_WEB_WORKFLOW = 1
CIRCUITPY_MDNS = 1
CIRCUITPY_SOCKETPOOL = 1
CIRCUITPY_WIFI = 1

# Trimmed for the engine build:
#  - native _stage: ugame/stage compatibility is provided in Python by picogame-stage, so the C
#    module (and the picosystem frozen ugame.py that needs it) is redundant.
#  - PICODVI / _EVE: no HDMI/DVI or FT8xx use on this device.
#  - qrio: QR DECODE needs a camera the PicoPad lacks (generation via adafruit_miniqr is unaffected);
#    also drops its ~32 KB quirc backend.
CIRCUITPY_STAGE = 0
CIRCUITPY_PICODVI = 0
CIRCUITPY__EVE = 0
CIRCUITPY_QRIO = 0

# Optimization: the rp2 default is -O3, but on this Cortex-M0+ (no SIMD/FPU, 16 KB XIP cache) most of
# what -O3 adds over -O2 is bloat this code can't use (the vectorizer, ipa-cp-clone, heavy loop
# unroll/peel/version passes). Measured on-device: -O2 plus just the five cheap loop passes below
# matches full -O3 engine speed (the picogame hot path stays within +-1% across all kernels) while
# using ~150 KB LESS flash. gc.o/vm.o stay -O3 via SUPEROPT, so the interpreter core is unaffected.
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
