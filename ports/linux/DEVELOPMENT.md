# Developing the port

Notes for whoever continues this port on another machine. The user-facing documentation is
`README.md`; this file is where things are and what bit us.

## Branch and layout

Branch `linux-port` on top of adafruit/circuitpython `main` (246a7abe44). Everything of the port
is under `ports/linux/`; the shared code carries small `CIRCUITPY-CHANGE` edits for the host
(clang, MinGW, `supervisor_vfs` host directories, `sys.settrace` with `frame.f_locals`, the
`port_before_user_code()` hook). Commits carry no Co-Authored-By or similar trailers.

| where | what |
|---|---|
| `supervisor/`, `linux_main.c`, `interrupts.c` | the process: options, stack, the timer thread that plays the interrupt controller, console, drive image, reset, crash handling |
| `linux_watch.c` | reload on host edits of a directory drive (inotify, ReadDirectoryChangesW) |
| `host_socket.h`, `common-hal/socketpool`, `bindings/hostnetwork` | the host's sockets with one face (BSD, Winsock) |
| `sdl_host.c`, `bindings/sdl`, `common-hal/sdl`, `common-hal/audioio` | display, buttons and sound through SDL2 |
| `debug_hook.c`, `python/debugadapter.py`, `vscode/` | the debugger: `--debug PORT`, the adapter inside the VM, the editor extension |
| `tests/` | the test scripts (see `tests/README.md`) |
| `tools/bootstrap.sh` | submodules and the Windows toolchain on a fresh machine |
| `tools/package-windows.sh` | the zip for Windows users |

The shared changes outside the port, in case of a rebase: `py/circuitpy_mpconfig.h` (alloca on
Windows, clang passes the GCC version check, `CIRCUITPY_FILESYSTEM_POSIX`), `py/circuitpy_defns.mk`
untouched but its GCC-only flags are filtered in the Makefile for clang, `supervisor/filesystem.h`
and `supervisor/shared/filesystem.c` (the host-directory variant beside FAT and littlefs),
`supervisor/shared/port.c` (TLSF defaults only with TLSF, `port_before_user_code`), `main.c`
(the hook), `shared-module/storage/__init__.c` (`map_file` by mmap or MapViewOfFile),
`shared-module/terminalio/Terminal.c` (no nested function), `supervisor/shared/web_workflow/web_workflow.c`
(no `strchrnul`), `extmod/vfs_posix*.c/.h` (public struct with a blockdev prefix, static init,
file descriptor accessor, no VfsPosix stdio), `py/emitbc.c`, `py/emitglue.h`, `py/profile.c`,
`py/objcell.c` (local names for `f_locals`), `lib/tjpgd/src/tjpgd.h` (`_MSC_VER`).

## Build

`tools/bootstrap.sh` fetches the submodules and prints the exact commands. Linux: `make`
(gcc >= 10, libsdl2-dev). Windows: a cross build with llvm-mingw, see README. After changing
`mpconfigport.h` or `mpconfigport.mk`, delete the build directory: the generated qstr and module
tables are not rebuilt on a configuration change. clang warns about more than GCC; the Makefile
filters the GCC-only flags and silences the classes the GCC builds keep checking.

A native build on Windows: `tools/bootstrap-msys2.sh` in an MSYS2 UCRT64 shell installs GCC,
SDL2, Python and make, then `make BUILD=build-windows SDL_DIR=/ucrt64`. The Makefile takes
`OS=Windows_NT` as a Windows build and GCC keeps its own flags (the clang filters do not apply).
This path has not been tried; expect some friction in the shared Makefiles. An agent running
in Git Bash can run the MSYS2 shell for the build:
`C:/msys64/msys2_shell.cmd -ucrt64 -defterm -no-start -here -c "make ..."`.

Windows is only tested under Wine here (`tests/test_wine.py`, `tests/test_dap.py ... wine`) and
on the user's PC. Wine: a portable build (Kron4ek) works; `SDL_VIDEODRIVER=dummy` opens no window
under Wine, the program says so and runs on.

## Things that bit

- **A reset on Windows is a new process the old one waits for.** A crash at start-up restarted
  forever, 947 nested processes exhausted the memory and the OOM killer took the whole session
  down, twice. Now a second hard fault right after a restart ends the program and Windows stops
  nesting after sixteen resets. Still: run Windows tests under a process-count limit (the scripts
  do) and kill leftovers by PID, never by a substring the test's own command line contains.
- The VM's filesystem is the drive: `sys.path` cannot reach the host, so the debug adapter is
  read from beside the program in C and compiled into a module.
- A local variable changed after `nlr_push()` is stale after an exception (`setjmp` semantics):
  free such buffers on the success path only.
- MicroPython has no `del bytearray[slice]`; `bytearray.extend(memoryview)` works.
- `supervisor_tick()` is for scheduling only; SDL and audio work runs once per tick from
  `port_background_tick()`, never from `port_background_task()` (every VM hook, 30x slower).
- Python sees a directory drive read-only unless `boot.py` remounts it, as a board with USB does;
  `main.c` hands the drive to Python when there is no USB, so the directory variant forces the
  "host writable" flag.
- The debugger adds Python frames above every call of the user's: the host has a 256 KB
  pystack, a board's 2 KB ended picostellar early.

## Next: GPIO through an FT232H (and other transports)

The plan: a backend for `digitalio` and `busio` that talks to the FT232H's MPSSE engine over USB,
chosen with `--board ft232h`, so `board.D4`, `busio.SPI(board.SCK, board.MOSI, board.MISO)` and
`busio.I2C(board.SCL, board.SDA)` work as on a microcontroller and CircuitPython libraries run
unchanged. Transport: libusb-1.0 loaded at run time (dlopen / LoadLibrary, so the program runs
without it): Linux needs a udev rule and `libusb_detach_kernel_driver` for `ftdi_sio`; Windows
needs `libusb-1.0.dll` beside the program and the WinUSB driver from Zadig, as Blinka does.
FTDI's D2XX (the stock Windows driver) is a possible second transport. MPSSE: FTDI AN_108 (the
commands), AN_135 (SPI), AN_113 (I2C by open-drain emulation). Pins: ADBUS0-2 SCK/MOSI/MISO
(SK/DO/DI, also SCL/SDA), ADBUS3 CS, D4-D7 = ADBUS4-7, C0-C7 = ACBUS0-7. No PWM, analog or UART in
MPSSE mode. The same backend layer can later take MCP2221 (HID), a Pico running U2IF, and
`gpiochip`/`i2c-dev`/`spidev` on a Raspberry Pi or a handheld. With the libusb transport, CP's
`usb.core` (shared-bindings/usb/core) can get a host implementation almost for free.
