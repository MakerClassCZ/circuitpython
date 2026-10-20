// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// CIRCUITPY is a host directory, served by the supervisor through VfsPosix, or a plain FAT image
// file (the partition, without an MBR) mapped into memory. An empty new image is formatted on
// first boot like a blank board; the host can open it with mtools or a loop mount, and
// storage.map_file reads straight from the mapping.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "py/mphal.h"
#include "supervisor/flash.h"
#include "supervisor/internal_flash.h"

#include "linux_port.h"

static uint8_t *_drive;
static size_t _drive_size;

#if CIRCUITPY_FILESYSTEM_POSIX
// An existing directory, or a missing path that does not end in .img, is a directory drive.
static bool _drive_is_directory(void) {
    static int directory = -1;
    if (directory < 0) {
        const char *path = linux_options.drive_path;
        struct stat st;
        size_t len = strlen(path);
        if (stat(path, &st) == 0) {
            directory = S_ISDIR(st.st_mode);
        } else {
            directory = !(len > 4 && strcmp(path + len - 4, ".img") == 0);
        }
    }
    return directory == 1;
}

const char *supervisor_filesystem_posix_root(void) {
    return _drive_is_directory() ? linux_options.drive_path : NULL;
}
#endif

void supervisor_flash_init(void) {
    if (_drive != NULL) {
        return;
    }
    const char *path = linux_options.drive_path;
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "circuitpython: cannot open drive %s: %s\n", path, strerror(errno));
        linux_power_off(1);
    }
    size_t size = st.st_size;
    if (size == 0) {
        size = linux_options.drive_size;
        if (ftruncate(fd, size) != 0) {
            fprintf(stderr, "circuitpython: cannot size drive %s: %s\n", path, strerror(errno));
            linux_power_off(1);
        }
    }
    size -= size % FILESYSTEM_BLOCK_SIZE;
    _drive = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (_drive == MAP_FAILED) {
        fprintf(stderr, "circuitpython: cannot map drive %s: %s\n", path, strerror(errno));
        linux_power_off(1);
    }
    _drive_size = size;
}

uint32_t supervisor_flash_get_block_size(void) {
    return FILESYSTEM_BLOCK_SIZE;
}

uint32_t supervisor_flash_get_block_count(void) {
    return _drive_size / FILESYSTEM_BLOCK_SIZE;
}

void port_internal_flash_flush(void) {
    if (_drive != NULL) {
        msync(_drive, _drive_size, MS_ASYNC);
    }
}

mp_uint_t supervisor_flash_read_blocks(uint8_t *dest, uint32_t block, uint32_t num_blocks) {
    if ((uint64_t)(block + num_blocks) * FILESYSTEM_BLOCK_SIZE > _drive_size) {
        return 1;
    }
    memcpy(dest, _drive + (size_t)block * FILESYSTEM_BLOCK_SIZE, (size_t)num_blocks * FILESYSTEM_BLOCK_SIZE);
    return 0;
}

mp_uint_t supervisor_flash_write_blocks(const uint8_t *src, uint32_t block, uint32_t num_blocks) {
    if ((uint64_t)(block + num_blocks) * FILESYSTEM_BLOCK_SIZE > _drive_size) {
        return 1;
    }
    memcpy(_drive + (size_t)block * FILESYSTEM_BLOCK_SIZE, src, (size_t)num_blocks * FILESYSTEM_BLOCK_SIZE);
    return 0;
}

void supervisor_flash_release_cache(void) {
}

const uint8_t *port_internal_flash_xip_address(uint32_t block, uint32_t *contiguous) {
    uint32_t count = supervisor_flash_get_block_count();
    if (block >= count) {
        return NULL;
    }
    *contiguous = count - block;
    return _drive + (size_t)block * FILESYSTEM_BLOCK_SIZE;
}
