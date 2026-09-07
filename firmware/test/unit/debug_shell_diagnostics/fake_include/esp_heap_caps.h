#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_DEFAULT (1U << 12)
#define MALLOC_CAP_INTERNAL (1U << 11)
#define MALLOC_CAP_DMA (1U << 3)
#define MALLOC_CAP_SPIRAM (1U << 10)

size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
