// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// A directory CIRCUITPY reloads code.py when the host changes a file in it, as a board does when
// a computer writes to its drive. Hidden files and directories (editor swap files, .git) do not
// count, nor does boot_out.txt, nor anything while Python itself may write the drive.

#define _XOPEN_SOURCE_EXTENDED 1

#include "py/mpconfig.h"

#if CIRCUITPY_FILESYSTEM_POSIX

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "supervisor/filesystem.h"
#include "supervisor/shared/reload.h"

#include "linux_port.h"

static void _changed(void) {
    supervisor_vfs_t *circuitpy = filesystem_circuitpy();
    if (circuitpy != NULL && !filesystem_is_writable_by_python(circuitpy)) {
        autoreload_trigger();
        linux_wake_vm();
    }
}

static void _start_thread(void *(*main)(void *), void *arg) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, main, arg) == 0) {
        pthread_detach(thread);
    }
}

#ifdef _WIN32

#include <windows.h>

static HANDLE _dir = INVALID_HANDLE_VALUE;

// Any component of the relative path starting with '.'.
static bool _hidden(const wchar_t *name, size_t len) {
    bool start = true;
    for (size_t i = 0; i < len; i++) {
        if (start && name[i] == L'.') {
            return true;
        }
        start = name[i] == L'\\' || name[i] == L'/';
    }
    return false;
}

static void *_watch(void *arg) {
    (void)arg;
    static uint8_t buf[64 * 1024] __attribute__((aligned(4)));
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    for (;;) {
        DWORD bytes = 0;
        if (!ReadDirectoryChangesW(_dir, buf, sizeof(buf), TRUE, filter, &bytes, NULL, NULL)) {
            return NULL;
        }
        bool changed = bytes == 0;      // too many changes for the buffer: something changed
        for (FILE_NOTIFY_INFORMATION *event = (FILE_NOTIFY_INFORMATION *)buf; bytes > 0;) {
            size_t len = event->FileNameLength / sizeof(wchar_t);
            if (!_hidden(event->FileName, len) &&
                !(len == 12 && _wcsnicmp(event->FileName, L"boot_out.txt", 12) == 0)) {
                changed = true;
            }
            if (event->NextEntryOffset == 0) {
                break;
            }
            event = (FILE_NOTIFY_INFORMATION *)((uint8_t *)event + event->NextEntryOffset);
        }
        if (changed) {
            _changed();
        }
    }
}

void linux_watch_start(const char *root) {
    if (_dir != INVALID_HANDLE_VALUE) {
        return;
    }
    _dir = CreateFileA(root, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (_dir == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "circuitpython: cannot watch %s\r\n", root);
        return;
    }
    _start_thread(_watch, NULL);
}

#else

#include <ftw.h>
#include <sys/inotify.h>

#define WATCH_MASK (IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF)
#define WATCHES_MAX 1024

static int _fd = -1;
static int _root_wd = -1;
static char *_paths[WATCHES_MAX];       // watch descriptor -> directory path

static void _watch_directory(const char *path) {
    int wd = inotify_add_watch(_fd, path, WATCH_MASK | IN_ONLYDIR);
    if (wd >= 0 && wd < WATCHES_MAX && _paths[wd] == NULL) {
        _paths[wd] = strdup(path);
    }
    if (_root_wd < 0) {
        _root_wd = wd;
    }
}

static int _visit(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)st;
    if (type != FTW_D) {
        return FTW_CONTINUE;
    }
    if (ftw->level > 0 && path[ftw->base] == '.') {
        return FTW_SKIP_SUBTREE;
    }
    _watch_directory(path);
    return FTW_CONTINUE;
}

static void *_watch(void *arg) {
    nftw((const char *)arg, _visit, 16, FTW_ACTIONRETVAL | FTW_PHYS);
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(_fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return NULL;
        }
        bool changed = false;
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *event = (struct inotify_event *)p;
            p += sizeof(*event) + event->len;
            if (event->len == 0 || event->name[0] == '.') {
                continue;
            }
            if (event->wd == _root_wd && strcmp(event->name, "boot_out.txt") == 0) {
                continue;
            }
            if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO)) &&
                event->wd >= 0 && event->wd < WATCHES_MAX && _paths[event->wd] != NULL) {
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s/%s", _paths[event->wd], event->name);
                nftw(path, _visit, 16, FTW_ACTIONRETVAL | FTW_PHYS);
            }
            changed = true;
        }
        if (changed) {
            _changed();
        }
    }
}

void linux_watch_start(const char *root) {
    if (_fd >= 0) {
        return;
    }
    _fd = inotify_init1(IN_CLOEXEC);
    if (_fd < 0) {
        perror("circuitpython: inotify");
        return;
    }
    _start_thread(_watch, (void *)root);
}

#endif

#endif
