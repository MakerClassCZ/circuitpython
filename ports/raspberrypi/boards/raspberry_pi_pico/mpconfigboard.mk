USB_VID = 0x239A
USB_PID = 0x80F4
USB_PRODUCT = "Pico"
USB_MANUFACTURER = "Raspberry Pi"

CHIP_VARIANT = RP2040
CHIP_FAMILY = rp2

EXTERNAL_FLASH_DEVICES = "W25Q16JVxQ"

CIRCUITPY__EVE = 1
CIRCUITPY_PICODVI = 1

# picogame custom-board build: native engine + fast DMA display backend. board.DISPLAY is
# left empty here and built by boot.py from settings.toml (see picogame-custom_boards).
CIRCUITPY_PICOGAME = 1
CIRCUITPY_PICOGAME_FAST_DISPLAY = 1
