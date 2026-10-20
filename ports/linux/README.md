# CircuitPython for Linux

CircuitPython as a Linux program: the supervisor, the VM and the CIRCUITPY drive run in one
process, the terminal is the serial console, and SDL2 provides the display, the buttons and the
sound. It is meant for Linux handhelds (Miyoo, Anbernic and the like) and as a fast way to run
CircuitPython code on a desktop. This port is experimental.

## Build

Needs GCC 10 or newer and the SDL2 development files (`libsdl2-dev` on Debian and Ubuntu).

```
make                        # build-desktop/circuitpython
make CC="gcc -m32" BUILD=build-desktop-i386     # a 32-bit build on a 64-bit PC
```

Another SDL2 can be given with `SDL_CFLAGS` and `SDL_LIBS`.

## Run

```
build-desktop/circuitpython [--drive CIRCUITPY.img] [--drive-size MB] [--ram MB]
```

On first start the drive image is created and formatted like a blank board: `code.py`,
`settings.toml`, `lib/` and `boot_out.txt`. The image is a plain FAT volume, so the host reads and
writes it with mtools or a loop mount:

```
mcopy -i CIRCUITPY.img -o code.py ::code.py
mcopy -i CIRCUITPY.img -s -o mygame/* ::/
```

Write to the image only while CircuitPython is not running: the program has it mounted, as a board
has its flash. While it runs, edit files through the web workflow (below), which also reloads
`code.py` after a change.

The console works as on a board: Ctrl-C interrupts, Ctrl-D reloads, any key enters the REPL.
Ctrl-\ powers off. `microcontroller.reset()` restarts the process, and a crash restarts it in safe
mode with a backtrace on stderr; `addr2line -f -e build-desktop/circuitpython 0x...` turns its
`circuitpython(+0x...)` lines into function names.

## Display, buttons and sound

`board.DISPLAY` is a `framebufferio.FramebufferDisplay` over an `sdl.Framebuffer`, so the console
appears on screen and `displayio`, `terminalio` and `picogame` draw into it. SDL2 picks the
window system on a desktop and KMS/DRM or fbdev on a handheld. `settings.toml` sets it up:

| key | default | meaning |
|---|---|---|
| `CIRCUITPY_DISPLAY_WIDTH`, `CIRCUITPY_DISPLAY_HEIGHT` | 320, 240 | the picture in pixels |
| `CIRCUITPY_DISPLAY_SCALE` | 2 | window size as a multiple of the picture |
| `CIRCUITPY_DISPLAY_FULLSCREEN` | false | fill the screen, whole-pixel scaling |
| `CIRCUITPY_DISPLAY_ROTATION` | 0 | 0, 90, 180 or 270 |

The buttons are pins with the PicoPad's names, `board.SW_UP` to `board.SW_MENU`, low while
pressed, so `keypad.Keys` and code written for the PicoPad work unchanged. They follow a game
controller, which is how SDL reports a handheld's own buttons, or the keyboard:

| button | key | button | key |
|---|---|---|---|
| UP, DOWN, LEFT, RIGHT | arrows | START | Enter |
| A, B | Z, X | SELECT | Backspace |
| X, Y | A, S | L, R | Q, W |
| MENU | Esc | | |

`audioio.AudioOut(board.SPEAKER)` plays through SDL2 audio: samples, `audiomixer` and `synthio`.

F12 or `kill -USR1` saves a screenshot, `screenshot-NNN.bmp` or the file named by
`CIRCUITPY_SCREENSHOT`.

## Network

The host's network is a network interface like `wifi.radio`:

```
import hostnetwork, socketpool, ssl
pool = socketpool.SocketPool(hostnetwork.HostNetwork())
context = ssl.create_default_context()     # for adafruit_requests and other TLS code
```

TCP, UDP, DNS and TLS with the usual root certificates work. An accepted socket starts
non-blocking, as on the ESP32 port.

With `CIRCUITPY_WEB_API_PASSWORD` in `settings.toml` the web workflow serves the drive, the
serial console and the file editor at `http://127.0.0.1:8080` when
`CIRCUITPY_WEB_API_PORT = 8080` is set too (port 80 needs root). It listens on every interface,
as a board on Wi-Fi does, so other machines on the network reach it with the password.

## Without a screen

`SDL_VIDEODRIVER=dummy` runs without a display and `SDL_AUDIODRIVER=disk` writes the sound to a
file. `CIRCUITPY_SDL_KEYS` names a file or FIFO of lines `down KEY`, `up KEY` and `sleep MS`
(SDL key names such as `Z` or `Right`) that are played as key presses, so a test can drive a
program headless.

## Not yet

- Native `.mpy` files (turbo): loading them needs executable memory.
- Files written into the image from the host while CircuitPython runs are not noticed; use the
  web workflow.
- `wifi` itself, mDNS and BLE: code that calls `wifi.radio` needs `hostnetwork.HostNetwork()`
  instead.
- `busio` and USB.
- Builds and packages for handhelds: the port needs a cross compiler with the device's SDL2
  (PortMaster for Anbernic and others, the OnionOS toolchain for the Miyoo Mini).
