USB_VID = 0x2E8A
USB_PID = 0x000B
USB_PRODUCT = "Pico 2"
USB_MANUFACTURER = "Raspberry Pi"

CHIP_VARIANT = RP2350
CHIP_PACKAGE = A
CHIP_FAMILY = rp2

EXTERNAL_FLASH_DEVICES = "W25Q32JVxQ"

CIRCUITPY__EVE = 1

# picogame custom-board build: native engine + fast DMA display backend. board.DISPLAY is
# left empty here and built by boot.py from settings.toml (see picogame-custom_boards).
CIRCUITPY_PICOGAME = 1
CIRCUITPY_PICOGAME_FAST_DISPLAY = 1
