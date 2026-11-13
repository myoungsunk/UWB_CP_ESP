#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_INTERNAL (1U << 0)
#define MALLOC_CAP_8BIT (1U << 1)
void* heap_caps_malloc(size_t bytes, uint32_t caps);
void heap_caps_free(void* ptr);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
