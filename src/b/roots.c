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
 * Root sources (AD-11, AD-18): the table a collector pulls a context's roots
 * from. It knows nothing about who the sources are.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "context_internal.h"

#include <stdint.h>
#include <string.h>

static bool owner_ok(const GRCORE_Context * c) {
  return c != NULL && grcore_context_owned_by_caller(c) && !c->tearing_down;
}

GRCORE_Result grcore_context_add_root_source(GRCORE_Context * context,
    const GRCORE_RootSource * source, void * value) {
  if (!owner_ok(context) || source == NULL || source->enumerate == NULL) {
    return GRCORE_ERR_INVALID;
  }
  for (size_t i = 0; i < context->root_count; i++) {
    if (context->roots[i].source == source && context->roots[i].value == value) {
      return GRCORE_ERR_INVALID;
    }
  }
  if (context->root_count == context->root_capacity) {
    const GRCORE_Allocator * a = &context->counting.allocator;
    size_t capacity = context->root_capacity == 0 ? 4 : context->root_capacity * 2;
    if (capacity > SIZE_MAX / sizeof(GRCORE_RootEntry)) {
      return GRCORE_ERR_OOM;
    }
    uint64_t refusals = grcore_counting_refusals(&context->counting);
    GRCORE_RootEntry * grown = a->malloc_fn(a->ctx, capacity * sizeof *grown);
    if (grown == NULL) {
      return grcore_counting_refusals(&context->counting) > refusals
          ? GRCORE_ERR_LIMIT
          : GRCORE_ERR_OOM;
    }
    if (context->root_count > 0) {
      memcpy(grown, context->roots, context->root_count * sizeof *grown);
    }
    a->free_fn(a->ctx, context->roots);
    context->roots = grown;
    context->root_capacity = capacity;
  }
  context->roots[context->root_count].source = source;
  context->roots[context->root_count].value = value;
  context->root_count++;
  return GRCORE_OK;
}

GRCORE_Result grcore_context_remove_root_source(
    GRCORE_Context * context, const GRCORE_RootSource * source, void * value) {
  if (!owner_ok(context) || source == NULL) {
    return GRCORE_ERR_INVALID;
  }
  for (size_t i = 0; i < context->root_count; i++) {
    if (context->roots[i].source == source && context->roots[i].value == value) {
      /* The order of the rest is the order they were added in. */
      memmove(&context->roots[i], &context->roots[i + 1],
          (context->root_count - i - 1) * sizeof context->roots[i]);
      context->root_count--;
      if (context->root_count == 0) {
        /* An empty table is not kept: a source added and taken straight back
         * out (A's, when a registration fails) leaves nothing charged. */
        const GRCORE_Allocator * a = &context->counting.allocator;
        a->free_fn(a->ctx, context->roots);
        context->roots = NULL;
        context->root_capacity = 0;
      }
      return GRCORE_OK;
    }
  }
  return GRCORE_ERR_INVALID;
}

size_t grcore_context_root_source_count(const GRCORE_Context * context) {
  return context == NULL ? 0 : context->root_count;
}

GRCORE_Result grcore_context_root_source(const GRCORE_Context * context,
    size_t index, const GRCORE_RootSource ** out_source, void ** out_value) {
  if (context == NULL || !grcore_context_owned_by_caller(context) ||
      index >= context->root_count) {
    return GRCORE_ERR_INVALID;
  }
  if (out_source != NULL) {
    *out_source = context->roots[index].source;
  }
  if (out_value != NULL) {
    *out_value = context->roots[index].value;
  }
  return GRCORE_OK;
}

GRCORE_Result grcore_context_enumerate_roots(
    GRCORE_Context * context, const GRCORE_RootVisitor * visitor) {
  if (context == NULL || visitor == NULL ||
      !grcore_context_owned_by_caller(context)) {
    return GRCORE_ERR_INVALID;
  }
  /* A source must not add or remove one, so the table cannot move under this
   * loop; the count is read once all the same. */
  size_t count = context->root_count;
  for (size_t i = 0; i < count; i++) {
    GRCORE_RootEntry entry = context->roots[i];
    entry.source->enumerate(context, entry.value, visitor);
  }
  return GRCORE_OK;
}
