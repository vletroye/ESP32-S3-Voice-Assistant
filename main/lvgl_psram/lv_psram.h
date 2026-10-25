/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * LVGL memory (objects, styles, label texts, layers) in PSRAM. LVGL uses malloc() by default, and the
 * small allocations of malloc() go to the internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), which is
 * kept for task stacks, DMA and the network. Included by LVGL (CONFIG_LV_MEM_CUSTOM_INCLUDE), the
 * functions are selected in the top-level CMakeLists.txt. The draw buffers are allocated separately (DMA).
 */

#pragma once

#include <stddef.h>
#include "esp_heap_caps.h"

static inline void *lv_psram_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline void *lv_psram_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline void lv_psram_free(void *ptr)
{
    heap_caps_free(ptr);
}
