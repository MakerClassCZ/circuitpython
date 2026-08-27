# ROMFS asset region — parked, not deleted

**Status (2026-08-27):** works, measured, and **has no consumer.** Parked on this branch so it can
be revived without archaeology; removed from the integration branch to stop it costing conflicts on
every upstream catch-up.

## What it is

`pg.romfs_program(path)` writes a ROMFS image into the **tail slack of the firmware region** — the
space between the 4 KB-aligned end of the built image and the end of `CIRCUITPY_FIRMWARE_SIZE` — and
`pg.romfs_mount()` exposes it so bitmaps blit straight out of XIP flash at **zero heap**. The FAT
drive never moves, so one universal firmware serves every board and nothing needs reformatting.

## Why it was parked

- **Nothing uses it.** The one game that needs 0-heap flash assets (Pictor, 138 KB of row strips)
  ships on `pg.xip_map()` instead, with a graceful fallback: no XIP → drop the middle parallax layer.
  `xip_map` won on ergonomics — assets stay ordinary files you drag onto CIRCUITPY and can edit.
- **Capacity stopped being ours to control.** It is whatever the build leaves over: 160 KB on a
  PicoPad, 72 KB on a Fruit Jam (2026-08-27). Since the engine landed upstream (#11199), every
  feature Adafruit adds to CircuitPython shrinks it without us deciding anything.
- **A firmware update orphans the image** — the header magic check then reports it absent and
  `romfs_program` has to run again.
- Its remaining unique advantage, immunity to FAT fragmentation, is covered instead by the
  `fat_layout` / `fat_max_free_run` / `repack` trio, which makes `xip_map` robust rather than adding
  a second mechanism for the same job. Measured on a lived-in drive: 94 of 96 files were contiguous.

## Everything that makes it up (the revival checklist)

| Where | What |
|---|---|
| `ports/raspberrypi/common-hal/picogame/romfs.c` | the port-side sector writer (the whole file) |
| `ports/raspberrypi/Makefile` | `ifeq ($(CIRCUITPY_PICOGAME_ROMFS),1)` → `SRC_C += common-hal/picogame/romfs.c` |
| `py/circuitpy_mpconfig.mk` | the `CIRCUITPY_PICOGAME_ROMFS` flag block (0/1; ~3.6 KB flash on, ~30 B off) |
| `shared-bindings/picogame/__init__.c` | `romfs_program`, `romfs_mount`, `romfs_region` bindings + `ROMFS_SUPPORTED` |
| `shared-bindings/picogame/__init__.h` | `picogame_romfs_base/len/present/erase_sector/program/mount` declarations |
| `repos/picogame/tools/build_romfs.py` | host-side image builder |
| `repos/picogame-final/games/assets-pictor.romfs{,.manifest}` | a real 57 KB image to test against |

Reviving = rebase this branch onto the then-current `upstream/main` and re-check the flag wiring;
the code is self-contained apart from those two build files.
