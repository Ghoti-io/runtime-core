/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Runtime-core.
 *
 * Ghoti.io Runtime-core is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Runtime-core is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Context options.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "options_internal.h"

#include <ghoti.io/runtime-core/b/options.h>

#include <stdlib.h>
#include <string.h>

typedef struct Entry {
  const GRCORE_Key * key;
  size_t size;
  void * bytes;
} Entry;

struct GRCORE_Options {
  const GRCORE_Allocator * allocator;
  uint64_t fuel;
  uint64_t memory_bytes;
  uint64_t guest_depth;
  uint64_t native_depth;
  Entry * entries;
  size_t count;
};

GRCORE_Result grcore_options_create(
    const GRCORE_Allocator * allocator, GRCORE_Options ** out_options) {
  if (out_options == NULL) {
    return GRCORE_ERR_INVALID;
  }
  if (allocator == NULL) {
    allocator = grcore_allocator_default();
  }
  GRCORE_Options * o = allocator->malloc_fn(allocator->ctx, sizeof *o);
  if (o == NULL) {
    return GRCORE_ERR_OOM;
  }
  o->allocator = allocator;
  o->fuel = GRCORE_UNLIMITED;
  o->memory_bytes = GRCORE_UNLIMITED;
  o->guest_depth = GRCORE_UNLIMITED;
  o->native_depth = GRCORE_UNLIMITED;
  o->entries = NULL;
  o->count = 0;
  *out_options = o;
  return GRCORE_OK;
}

void grcore_options_destroy(GRCORE_Options * options) {
  if (options == NULL) {
    return;
  }
  const GRCORE_Allocator * a = options->allocator;
  for (size_t i = 0; i < options->count; i++) {
    a->free_fn(a->ctx, options->entries[i].bytes);
  }
  a->free_fn(a->ctx, options->entries);
  a->free_fn(a->ctx, options);
}

#define SETTER(name, field)                                                    \
  GRCORE_Result grcore_options_set_##name(                                     \
      GRCORE_Options * options, uint64_t value) {                              \
    if (options == NULL) {                                                     \
      return GRCORE_ERR_INVALID;                                               \
    }                                                                          \
    options->field = value;                                                    \
    return GRCORE_OK;                                                          \
  }                                                                            \
  uint64_t grcore_options_get_##name(const GRCORE_Options * options) {         \
    return options == NULL ? GRCORE_UNLIMITED : options->field;                \
  }

SETTER(fuel, fuel)
SETTER(memory_bytes, memory_bytes)
SETTER(guest_depth, guest_depth)
SETTER(native_depth, native_depth)

static Entry * find(const GRCORE_Options * options, const GRCORE_Key * key) {
  for (size_t i = 0; i < options->count; i++) {
    if (options->entries[i].key == key) {
      return &options->entries[i];
    }
  }
  return NULL;
}

GRCORE_Result grcore_options_set_keyed(GRCORE_Options * options,
    const GRCORE_Key * key, const void * bytes, size_t size) {
  if (options == NULL || key == NULL || (bytes == NULL && size > 0)) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_Allocator * a = options->allocator;
  Entry * existing = find(options, key);
  if (size == 0) {
    if (existing != NULL) {
      a->free_fn(a->ctx, existing->bytes);
      size_t index = (size_t)(existing - options->entries);
      memmove(existing, existing + 1,
          (options->count - index - 1) * sizeof *existing);
      options->count--;
    }
    return GRCORE_OK;
  }
  void * copy = a->malloc_fn(a->ctx, size);
  if (copy == NULL) {
    return GRCORE_ERR_OOM;
  }
  memcpy(copy, bytes, size);
  if (existing != NULL) {
    a->free_fn(a->ctx, existing->bytes);
    existing->bytes = copy;
    existing->size = size;
    return GRCORE_OK;
  }
  Entry * grown = a->realloc_fn(
      a->ctx, options->entries, (options->count + 1) * sizeof *grown);
  if (grown == NULL) {
    a->free_fn(a->ctx, copy);
    return GRCORE_ERR_OOM;
  }
  options->entries = grown;
  grown[options->count].key = key;
  grown[options->count].size = size;
  grown[options->count].bytes = copy;
  options->count++;
  return GRCORE_OK;
}

GRCORE_Result grcore_options_get_keyed(const GRCORE_Options * options,
    const GRCORE_Key * key, const void ** out_bytes, size_t * out_size) {
  if (options == NULL || key == NULL || out_bytes == NULL ||
      out_size == NULL) {
    return GRCORE_ERR_INVALID;
  }
  const Entry * e = find(options, key);
  *out_bytes = e != NULL ? e->bytes : NULL;
  *out_size = e != NULL ? e->size : 0;
  return GRCORE_OK;
}

GRCORE_Result grcore_options_clone(const GRCORE_Options * source,
    const GRCORE_Allocator * allocator, GRCORE_Options ** out_options) {
  if (out_options == NULL) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Options * o;
  GRCORE_Result r = grcore_options_create(allocator, &o);
  if (r != GRCORE_OK) {
    return r;
  }
  if (source != NULL) {
    o->fuel = source->fuel;
    o->memory_bytes = source->memory_bytes;
    o->guest_depth = source->guest_depth;
    o->native_depth = source->native_depth;
    for (size_t i = 0; i < source->count; i++) {
      r = grcore_options_set_keyed(
          o, source->entries[i].key, source->entries[i].bytes,
          source->entries[i].size);
      if (r != GRCORE_OK) {
        grcore_options_destroy(o);
        return r;
      }
    }
  }
  *out_options = o;
  return GRCORE_OK;
}
