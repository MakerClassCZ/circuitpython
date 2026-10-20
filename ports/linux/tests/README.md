# Tests of the port

Each script takes the program and works in its own directory, so run them from a scratch copy:

    cp -r ports/linux/tests /tmp/cplt && cd /tmp/cplt
    B=/path/to/build-desktop/circuitpython          # or build-windows/circuitpython.exe on Windows
    for t in test_folder test_folder2 test_image test_erase test_dap; do python3 $t.py $B; done
    python3 test_stellar.py $B /path/to/picostellar/CIRCUITPY     # optional, needs the picostellar repo

- `test_folder.py`, `test_folder2.py`: the directory drive (defaults, auto-reload on host edits,
  read-only Python and `storage.remount`, `os` functions, imports, settings.toml, the web
  workflow, reset).
- `test_image.py`: the FAT image drive (`DEMO_IMAGE=path.img` also boots an existing image).
- `test_erase.py`: `storage.erase_filesystem()` leaves a directory alone.
- `test_stellar.py`: picostellar from a directory, with `storage.map_file` mapping the catalogue.
- `test_dap.py`: the debugger over the Debug Adapter Protocol, like an editor: breakpoints,
  stack, scopes, locals, evaluate, step into a library file, step out, step over, Stop.
- `test_wine.py`: the Windows build under Wine on Linux, headless (`WINE=/path/to/wine`,
  `WINEPREFIX` set to a scratch directory). `test_dap.py circuitpython.exe /path/to/wine`
  drives the debugger under Wine too.

The scripts were written on Linux; on Windows they run with Python for Windows against the
`.exe`, minus the Wine one and the Linux-only process limit and memory-map checks. A reset on
Windows starts a child process and waits for it, so a crash at start-up once nested 947
processes and the machine ran out of memory: run a new build with a process limit or a watchful
eye, and when killing leftovers never match the program by a substring that the test's own
command line contains.
