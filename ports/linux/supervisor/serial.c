// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The serial console is the process's stdin and stdout. A reader thread plays the part of the
// UART receive interrupt: it buffers input, raises KeyboardInterrupt on the interrupt character
// and wakes the VM. The terminal is put in raw mode, so Ctrl-C arrives as a byte like on a UART.

#include <errno.h>
#include <stdlib.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/runtime/interrupt_char.h"
#include "supervisor/shared/serial.h"

#include "linux_port.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <termios.h>
#include <unistd.h>
#endif

#define RX_SIZE 1024
#define CTRL_BACKSLASH 0x1c
// Empty polls in a row before a poll sleeps until input or the next interrupt.
#define IDLE_POLLS 64

static uint8_t _rx[RX_SIZE];
static uint32_t _rx_head;           // written by the reader thread
static uint32_t _rx_tail;           // written by the VM thread
static uint32_t _empty_polls;
static bool _tty;

// Input from the reader thread: the power-off key, the interrupt character, the rest buffered.
static void _received(const uint8_t *buf, size_t n) {
    uint32_t head = __atomic_load_n(&_rx_head, __ATOMIC_RELAXED);
    for (size_t i = 0; i < n; i++) {
        if (_tty && buf[i] == CTRL_BACKSLASH) {
            linux_power_off(0);
        }
        if (buf[i] == mp_interrupt_char) {
            mp_sched_keyboard_interrupt();
            continue;
        }
        uint32_t next = (head + 1) % RX_SIZE;
        if (next == __atomic_load_n(&_rx_tail, __ATOMIC_ACQUIRE)) {
            break;                      // full: drop the rest
        }
        _rx[head] = buf[i];
        head = next;
    }
    __atomic_store_n(&_rx_head, head, __ATOMIC_RELEASE);
    linux_wake_vm();
}

#ifdef _WIN32

static HANDLE _in;
static HANDLE _out;
static HANDLE _reader_thread;
static DWORD _in_mode;
static DWORD _out_mode;
static bool _console_out;

void linux_serial_restore(void) {
    if (_tty) {
        SetConsoleMode(_in, _in_mode);
    }
    if (_console_out) {
        SetConsoleMode(_out, _out_mode);
    }
    if (_reader_thread != NULL) {
        // A restarted process reads the console next; this one must not take its keys.
        CancelSynchronousIo(_reader_thread);
    }
}

static DWORD WINAPI _reader(LPVOID arg) {
    (void)arg;
    uint8_t buf[64];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(_in, buf, sizeof(buf), &n, NULL) || n == 0) {
            return 0;                   // end of input: the console stays output only
        }
        _received(buf, n);
    }
}

void port_serial_init(void) {
    _in = GetStdHandle(STD_INPUT_HANDLE);
    _out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(_in, &_in_mode)) {
        // Raw: no line editing or echo, Ctrl-C as a byte, keys as escape sequences.
        DWORD mode = _in_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        SetConsoleMode(_in, mode | ENABLE_VIRTUAL_TERMINAL_INPUT);
        _tty = true;
    }
    if (GetConsoleMode(_out, &_out_mode)) {
        SetConsoleMode(_out, _out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
        _console_out = true;
    }
    _reader_thread = CreateThread(NULL, 0, _reader, NULL, 0, NULL);
}

void port_serial_write_substring(const char *text, uint32_t len) {
    while (len > 0) {
        DWORD n = 0;
        if (!WriteFile(_out, text, len, &n, NULL)) {
            return;
        }
        text += n;
        len -= n;
    }
}

#else

static struct termios _saved_termios;

void linux_serial_restore(void) {
    if (_tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &_saved_termios);
    }
}

static void *_reader(void *arg) {
    (void)arg;
    uint8_t buf[64];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return NULL;                // end of input: the console stays output only
        }
        _received(buf, n);
    }
}

void port_serial_init(void) {
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &_saved_termios) == 0) {
        struct termios raw = _saved_termios;
        raw.c_iflag &= ~(BRKINT | ICRNL | INLCR | IGNCR | ISTRIP | IXON | INPCK);
        raw.c_oflag &= ~OPOST;              // CircuitPython sends \r\n itself
        raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        _tty = true;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t reader;
    pthread_create(&reader, &attr, _reader, NULL);
}

void port_serial_write_substring(const char *text, uint32_t len) {
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, text, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return;
        }
        text += n;
        len -= n;
    }
}

#endif

void port_serial_early_init(void) {
}

bool port_serial_connected(void) {
    return true;
}

uint32_t port_serial_bytes_available(void) {
    uint32_t head = __atomic_load_n(&_rx_head, __ATOMIC_ACQUIRE);
    uint32_t count = (head + RX_SIZE - _rx_tail) % RX_SIZE;
    if (count > 0) {
        _empty_polls = 0;
    } else if (++_empty_polls >= IDLE_POLLS) {
        // The console input loop polls without pausing; on a host that would spin a core.
        _empty_polls = 0;
        linux_wait_for_interrupt(10);
    }
    return count;
}

char port_serial_read(void) {
    uint32_t head = __atomic_load_n(&_rx_head, __ATOMIC_ACQUIRE);
    if (head == _rx_tail) {
        return -1;
    }
    char c = _rx[_rx_tail];
    __atomic_store_n(&_rx_tail, (_rx_tail + 1) % RX_SIZE, __ATOMIC_RELEASE);
    return c;
}
