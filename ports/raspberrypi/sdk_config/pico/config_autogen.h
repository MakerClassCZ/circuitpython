#pragma once

#include "include/cmsis/rename_exceptions.h"

#include "pico-sdk-configboard.h"

// The CYW43's Bluetooth firmware loader takes its buffers with cyw43_malloc, newlib's malloc by
// default. newlib's heap grows from `end`, which is where the port's own heap (TLSF) starts: the
// first malloc wrote over it. Take them from the port's heap instead.
#if defined(CIRCUITPY_BLEIO_CYW43) && CIRCUITPY_BLEIO_CYW43 && !defined(__ASSEMBLER__)
#include <stdbool.h>
#include <stddef.h>
void *port_malloc(size_t size, bool dma_capable);
void port_free(void *ptr);
#define cyw43_malloc(size) port_malloc(size, true)
#define cyw43_free(ptr) port_free(ptr)
#endif
