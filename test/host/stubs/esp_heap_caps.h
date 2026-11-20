/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stddef.h>
#include <malloc.h>
static inline size_t heap_caps_get_allocated_size(void *p)
{
    return malloc_usable_size(p);
}
