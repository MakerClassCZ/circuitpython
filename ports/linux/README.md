# CircuitPython for Linux and Windows

CircuitPython as a program: the supervisor, the VM and the CIRCUITPY drive run in one process,
the terminal is the serial console, and SDL2 provides the display, the buttons and the sound. It
is meant for Linux handhelds (Miyoo, Anbernic and the like) and as a fast way to run
CircuitPython code on a desktop, Linux or Windows. This port is experimental.

## Build

Needs GCC 10 or newer and the SDL2 development files (`libsdl2-dev` on Debian and Ubuntu).

```
make                        # build-desktop/circuitpython
make CC="gcc -m32" BUILD=build-desktop-i386     # a 32-bit build on a 64-bit PC
```

Another SDL2 can be given with `SDL_CFLAGS` and `SDL_LIBS`.

The Windows program is a cross build from Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
and the MinGW development package of SDL2 from [libsdl.org](https://github.com/libsdl-org/SDL/releases):

```
make BUILD=build-windows CROSS_COMPILE=x86_64-w64-mingw32- SDL_DIR=/path/to/SDL2-2.32.8/x86_64-w64-mingw32
```

`build-windows/circuitpython.exe` runs on Windows 10 and 11 with `SDL2.dll` from the same SDL2
package beside it; the C runtime and winpthreads are built in. A crash prints an address that
`x86_64-w64-mingw32-addr2line -f -e build-windows/circuitpython.exe 0x...` resolves.

## Run

```
build-desktop/circuitpython [--drive PATH] [--drive-size MB] [--ram MB]
```

CIRCUITPY is a directory, `./CIRCUITPY` by default. A missing directory is created on first start
with what a blank board has: `code.py`, `settings.toml` and `lib/`. Edit the files with any editor
or copy a project in, and `code.py` reloads after each change, as when a computer saves to a
board's drive. Hidden files such as editor swap files and `.git` do not trigger a reload.

Python sees the directory read-only, as on a board whose drive is connected to a computer.
`storage.remount("/", readonly=False)` in `boot.py` gives it to Python instead: Python can write,
and host edits stop reloading `code.py`. Unlike FAT, file names are case-sensitive, and
`storage.erase_filesystem()` leaves the directory as it is. `storage.map_file()` maps a file of
the directory straight into memory.

On Windows the drive is `.\CIRCUITPY` by default and any path works, `C:\Users\me\game` as well
as a relative one.

`--drive` can also name a FAT image file. A missing path ending in `.img` becomes a new image of
`--drive-size` MB, formatted like a blank board. The host reads and writes an image with mtools or
a loop mount, but only while CircuitPython is not running:

```
mcopy -i CIRCUITPY.img -o code.py ::code.py
mcopy -i CIRCUITPY.img -s -o mygame/* ::/
```

The console works as on a board: Ctrl-C interrupts, Ctrl-D reloads, any key enters the REPL.
Ctrl-\ or closing the window powers off. `microcontroller.reset()` restarts the process, and a
crash restarts it in safe mode with a backtrace on stderr; `addr2line -f -e
build-desktop/circuitpython 0x...` turns its `circuitpython(+0x...)` lines into function names. A
second crash right after such a restart ends the program instead. On Windows a reset starts a new
process and the old one waits for it, so after sixteen resets in a row the program ends.

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

## Debugging

`--debug [PORT]` loads the debug adapter (`debugadapter.py` beside the program) into the VM
before `code.py` and waits for an editor speaking the Debug Adapter Protocol on
`127.0.0.1:PORT` (default 5678). Then `code.py` runs under `sys.settrace`: breakpoints in
`code.py` and in `lib/*.py`, step into, over and out, the call stack, the local and global
variables of each frame, and expressions evaluated in a frame. The display keeps running in its
window while the program is stopped. `vscode/` is the VS Code extension: it starts the program
in a terminal and connects the editor (see `vscode/README.md`). Any other DAP client attaches
the same way.

A program under the debugger runs about three times slower. Python files compiled from source
show their locals; a `.mpy` keeps no variable names. After a reload (a file saved on the drive)
the program waits for the editor again.

## Without a screen

`SDL_VIDEODRIVER=dummy` runs without a display and `SDL_AUDIODRIVER=disk` writes the sound to a
file. `CIRCUITPY_SDL_KEYS` names a file or FIFO of lines `down KEY`, `up KEY` and `sleep MS`
(SDL key names such as `Z` or `Right`) that are played as key presses, so a test can drive a
program headless.

## Not yet

- Native `.mpy` files (turbo): loading them needs executable memory.
- On Windows: `audiomp3` (its decoder's inline assembly is for MSVC) and `os.statvfs()`.
- `wifi` itself, mDNS and BLE: code that calls `wifi.radio` needs `hostnetwork.HostNetwork()`
  instead.
- `busio` and USB.
- Builds and packages for handhelds: the port needs a cross compiler with the device's SDL2
  (PortMaster for Anbernic and others, the OnionOS toolchain for the Miyoo Mini).
