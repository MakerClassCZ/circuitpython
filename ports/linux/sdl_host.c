// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <SDL.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "py/mpconfig.h"
#include "supervisor/shared/tick.h"

#include "linux_port.h"
#include "sdl_host.h"

#define FRAME_MS 16

static SDL_Window *_window;
static SDL_Renderer *_renderer;
static SDL_Texture *_texture;
static uint16_t *_pixels;
static int _width;
static int _height;
static int _scale = 2;
static bool _fullscreen;
static uint64_t _last_frame_ms;
static int _screenshot_requested;
static int _screenshot_count;

// Keyboard on a desktop; the arrows and Z X A S sit where a handheld's buttons are.
static int _key_button(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP:
            return LINUX_BUTTON_UP;
        case SDLK_DOWN:
            return LINUX_BUTTON_DOWN;
        case SDLK_LEFT:
            return LINUX_BUTTON_LEFT;
        case SDLK_RIGHT:
            return LINUX_BUTTON_RIGHT;
        case SDLK_z:
            return LINUX_BUTTON_A;
        case SDLK_x:
            return LINUX_BUTTON_B;
        case SDLK_a:
            return LINUX_BUTTON_X;
        case SDLK_s:
            return LINUX_BUTTON_Y;
        case SDLK_RETURN:
            return LINUX_BUTTON_START;
        case SDLK_BACKSPACE:
            return LINUX_BUTTON_SELECT;
        case SDLK_q:
            return LINUX_BUTTON_L;
        case SDLK_w:
            return LINUX_BUTTON_R;
        case SDLK_ESCAPE:
            return LINUX_BUTTON_MENU;
        default:
            return -1;
    }
}

// Game controllers, which is how SDL reports a handheld's own buttons.
static int _pad_button(uint8_t button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return LINUX_BUTTON_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return LINUX_BUTTON_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return LINUX_BUTTON_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return LINUX_BUTTON_RIGHT;
        case SDL_CONTROLLER_BUTTON_A:
            return LINUX_BUTTON_A;
        case SDL_CONTROLLER_BUTTON_B:
            return LINUX_BUTTON_B;
        case SDL_CONTROLLER_BUTTON_X:
            return LINUX_BUTTON_X;
        case SDL_CONTROLLER_BUTTON_Y:
            return LINUX_BUTTON_Y;
        case SDL_CONTROLLER_BUTTON_START:
            return LINUX_BUTTON_START;
        case SDL_CONTROLLER_BUTTON_BACK:
            return LINUX_BUTTON_SELECT;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return LINUX_BUTTON_L;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return LINUX_BUTTON_R;
        case SDL_CONTROLLER_BUTTON_GUIDE:
            return LINUX_BUTTON_MENU;
        default:
            return -1;
    }
}

static void _save_screenshot(void) {
    char name[32];
    const char *path = getenv("CIRCUITPY_SCREENSHOT");
    if (path == NULL) {
        snprintf(name, sizeof(name), "screenshot-%03d.bmp", _screenshot_count++);
        path = name;
    }
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(_pixels, _width, _height, 16,
        _width * 2, SDL_PIXELFORMAT_RGB565);
    if (surface != NULL && SDL_SaveBMP(surface, path) == 0) {
        fprintf(stderr, "circuitpython: screenshot %s\r\n", path);
    }
    SDL_FreeSurface(surface);
}

// CIRCUITPY_SDL_KEYS names a file or FIFO of lines "down KEY", "up KEY" (SDL key names such as
// Z or Right) and "sleep MS"; they arrive as key events, so a test can play a program headless.
static void *_key_script(void *arg) {
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    FILE *f = fopen((const char *)arg, "r");
    if (f == NULL) {
        fprintf(stderr, "circuitpython: cannot open %s\r\n", (const char *)arg);
        return NULL;
    }
    char line[64];
    while (fgets(line, sizeof(line), f) != NULL) {
        char verb[8], name[32];
        if (sscanf(line, "%7s %31[^\r\n]", verb, name) != 2) {
            continue;
        }
        if (strcmp(verb, "sleep") == 0) {
            usleep(atoi(name) * 1000);
            continue;
        }
        SDL_Event event = {0};
        event.type = strcmp(verb, "down") == 0 ? SDL_KEYDOWN : SDL_KEYUP;
        event.key.state = event.type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
        event.key.keysym.sym = SDL_GetKeyFromName(name);
        if (event.key.keysym.sym != SDLK_UNKNOWN) {
            SDL_PushEvent(&event);
            linux_wake_vm();
        }
    }
    fclose(f);
    return NULL;
}

static void _start_key_script(void) {
    const char *path = getenv("CIRCUITPY_SDL_KEYS");
    if (path == NULL) {
        return;
    }
    pthread_t thread;
    pthread_create(&thread, NULL, _key_script, (void *)path);
    pthread_detach(thread);
}

void linux_sdl_configure(int scale, bool fullscreen) {
    _scale = scale > 0 ? scale : 1;
    _fullscreen = fullscreen;
}

bool linux_sdl_init(uint32_t subsystems) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");      // the port owns SIGINT and SIGTERM
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");    // whole pixels
    if (SDL_WasInit(subsystems) == subsystems) {
        return true;
    }
    if (SDL_InitSubSystem(subsystems) != 0) {
        fprintf(stderr, "circuitpython: %s\r\n", SDL_GetError());
        return false;
    }
    return true;
}

static bool _open(int width, int height) {
    if (_window != NULL) {
        return true;
    }
    if (!linux_sdl_init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) {
        return false;
    }
    Uint32 flags = SDL_WINDOW_RESIZABLE | (_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    _window = SDL_CreateWindow("CircuitPython - " MICROPY_HW_BOARD_NAME, SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, width * _scale, height * _scale, flags);
    if (_window != NULL) {
        _renderer = SDL_CreateRenderer(_window, -1, 0);
        if (_renderer == NULL) {
            _renderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_SOFTWARE);
        }
    }
    if (_renderer == NULL) {
        fprintf(stderr, "circuitpython: no display: %s\r\n", SDL_GetError());
        if (_window != NULL) {
            SDL_DestroyWindow(_window);
            _window = NULL;
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    SDL_ShowCursor(SDL_DISABLE);
    _start_key_script();
    return true;
}

bool linux_sdl_show(uint16_t *pixels, int width, int height) {
    if (!_open(width, height)) {
        return false;
    }
    if (_texture != NULL && (width != _width || height != _height)) {
        SDL_DestroyTexture(_texture);
        _texture = NULL;
    }
    if (_texture == NULL) {
        _texture = SDL_CreateTexture(_renderer, SDL_PIXELFORMAT_RGB565,
            SDL_TEXTUREACCESS_STREAMING, width, height);
        if (_texture == NULL) {
            fprintf(stderr, "circuitpython: no display: %s\r\n", SDL_GetError());
            return false;
        }
    }
    SDL_RenderSetLogicalSize(_renderer, width, height);
    SDL_RenderSetIntegerScale(_renderer, SDL_TRUE);
    bool was_showing = _pixels != NULL;
    _pixels = pixels;
    _width = width;
    _height = height;
    if (!was_showing) {
        // Frames and events are handled in the background task, which runs with the tick.
        supervisor_enable_tick();
    }
    linux_sdl_present();
    return true;
}

void linux_sdl_hide(void) {
    if (_pixels == NULL) {
        return;
    }
    _pixels = NULL;
    supervisor_disable_tick();
    SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
    SDL_RenderClear(_renderer);
    SDL_RenderPresent(_renderer);
}

void linux_sdl_present(void) {
    if (_pixels == NULL) {
        return;
    }
    SDL_UpdateTexture(_texture, NULL, _pixels, _width * 2);
    SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
    SDL_RenderClear(_renderer);
    SDL_RenderCopy(_renderer, _texture, NULL, NULL);
    SDL_RenderPresent(_renderer);
    _last_frame_ms = supervisor_ticks_ms64();
    if (__atomic_exchange_n(&_screenshot_requested, 0, __ATOMIC_ACQ_REL)) {
        _save_screenshot();
    }
}

void linux_sdl_request_screenshot(void) {
    __atomic_store_n(&_screenshot_requested, 1, __ATOMIC_RELEASE);
}

void linux_sdl_task(void) {
    if (_window == NULL) {
        return;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        int button = -1;
        bool pressed = false;
        switch (event.type) {
            case SDL_QUIT:
                linux_power_off(0);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (event.key.repeat) {
                    break;
                }
                pressed = event.type == SDL_KEYDOWN;
                if (pressed && event.key.keysym.sym == SDLK_F12) {
                    linux_sdl_request_screenshot();
                    break;
                }
                button = _key_button(event.key.keysym.sym);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                SDL_GameControllerOpen(event.cdevice.which);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                pressed = event.type == SDL_CONTROLLERBUTTONDOWN;
                button = _pad_button(event.cbutton.button);
                break;
            default:
                break;
        }
        if (button >= 0) {
            linux_button_set(button, pressed);
        }
    }
    if (_pixels != NULL && supervisor_ticks_ms64() - _last_frame_ms >= FRAME_MS) {
        linux_sdl_present();
    }
}
