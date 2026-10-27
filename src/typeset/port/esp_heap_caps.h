#pragma once

#include <stddef.h>

#define MALLOC_CAP_8BIT 1

inline size_t heap_caps_get_largest_free_block(int) { return (size_t)64 * 1024 * 1024; }
