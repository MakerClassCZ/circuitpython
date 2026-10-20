// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

// The one window of the process, through SDL2: SDL picks X11, Wayland, KMS/DRM or fbdev, and
// SDL_VIDEODRIVER=dummy runs without a screen.

// Start SDL subsystems (SDL_INIT_VIDEO, SDL_INIT_AUDIO, ...); false and a message if SDL cannot.
bool linux_sdl_init(uint32_t subsystems);

// How a new window appears; set before the first show.
void linux_sdl_configure(int scale, bool fullscreen);

// Show a width x height RGB565 picture (native byte order) from now on. False if SDL has no video.
bool linux_sdl_show(uint16_t *pixels, int width, int height);

// The picture went away.
void linux_sdl_hide(void);

// Put the picture on screen now.
void linux_sdl_present(void);

// Events, and a new frame at most 60 times a second. Called from the background task.
void linux_sdl_task(void);

// Save the next frame as a BMP (CIRCUITPY_SCREENSHOT names the file). Safe from any thread.
void linux_sdl_request_screenshot(void);
