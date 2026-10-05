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
 * Engine descriptors and A's per-context state (AD-18, AD-19).
 *
 * The state is created by the first registration and registered under a
 * cardinality-one key, whose destructor frees it through the context's
 * allocator. That is how A attaches to a context B made without B knowing A
 * exists. Everything is allocated through the context's counting allocator,
 * so it is charged to the context.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "guest_internal.h"

#include <ghoti.io/runtime-core/b/budget.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void guest_destroy(GRCORE_Context * context, void * value) {
  GRCORE_Stack * stack = value;
  const GRCORE_Allocator * a = grcore_context_allocator(context);
  a->free_fn(a->ctx, stack->buffer);
  a->free_fn(a->ctx, (void *)stack->engines);
  a->free_fn(a->ctx, stack->activations);
  a->free_fn(a->ctx, stack->scopes);
  a->free_fn(a->ctx, stack);
}

const GRCORE_Key grcore_guest_key = GRCORE_KEY_INIT("runtime-core.guest",
    GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, guest_destroy, NULL,
    grcore_guest_snapshot, grcore_guest_restore, grcore_guest_settle);

GRCORE_Result grcore_guest_alloc_failure(
    const GRCORE_Context * context, uint64_t refusals_before) {
  return grcore_context_memory_refusals(context) > refusals_before
      ? GRCORE_ERR_LIMIT
      : GRCORE_ERR_OOM;
}

bool grcore_decoder_decode(const GRCORE_ConservativeDecoder * decoder,
    uint64_t word, uint64_t * out_address) {
  if (decoder == NULL || out_address == NULL || decoder->mask == 0 ||
      decoder->shift >= 64) {
    return false;
  }
  *out_address = ((word & decoder->mask) >> decoder->shift) + decoder->base;
  return true;
}

GRCORE_Stack * grcore_context_stack(const GRCORE_Context * context) {
  return grcore_context_slot(context, &grcore_guest_key);
}

bool grcore_engine_descriptor_valid(const GRCORE_EngineDescriptor * descriptor) {
  /* The first member is read to learn how much of the rest exists, so a
   * descriptor is judged by that alone (see grcore_key_valid). */
  return descriptor != NULL &&
      descriptor->size >= GRCORE_ENGINE_DESCRIPTOR_MIN_SIZE &&
      descriptor->size % _Alignof(GRCORE_EngineDescriptor) == 0;
}

GRCORE_Result grcore_engine_register(GRCORE_Context * context,
    const GRCORE_EngineDescriptor * descriptor, GRCORE_EngineId * out_id) {
  if (context == NULL || !grcore_engine_descriptor_valid(descriptor) ||
      out_id == NULL || descriptor->name == NULL || descriptor->name[0] == '\0' ||
      !grcore_context_is_owner(context) ||
      grcore_context_state(context) == GRCORE_CONTEXT_RUNNING) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  if (stack != NULL) {
    for (size_t i = 0; i < stack->engine_count; i++) {
      if (stack->engines[i] == descriptor) {
        return GRCORE_ERR_INVALID;
      }
    }
  }
  const GRCORE_Allocator * a = grcore_context_allocator(context);
  uint64_t refusals = grcore_context_memory_refusals(context);
  bool fresh = stack == NULL;
  if (fresh) {
    stack = a->calloc_fn(a->ctx, 1, sizeof *stack);
    if (stack == NULL) {
      return grcore_guest_alloc_failure(context, refusals);
    }
    stack->context = context;
    stack->used = GRCORE_STACK_BASE;
  }
  /* Everything that can fail comes before the first change to a table that
   * already exists, so a refusal leaves it as it was. */
  const GRCORE_EngineDescriptor ** grown = NULL;
  if (stack->engine_count == stack->engine_capacity) {
    size_t capacity = stack->engine_capacity == 0 ? 4 : stack->engine_capacity * 2;
    grown = a->malloc_fn(a->ctx, capacity * sizeof *grown);
    if (grown == NULL) {
      GRCORE_Result r = grcore_guest_alloc_failure(context, refusals);
      if (fresh) {
        a->free_fn(a->ctx, stack);
      }
      return r;
    }
    if (stack->engine_count > 0) {
      memcpy((void *)grown, (const void *)stack->engines,
          stack->engine_count * sizeof *grown);
    }
    stack->engine_capacity = capacity;
  }
  if (fresh) {
    /* The root source goes in first and comes out again if the registration
     * fails, so a refusal leaves no source that points at a freed stack. */
    GRCORE_Result r =
        grcore_context_add_root_source(context, &grcore_guest_root_source, stack);
    if (r == GRCORE_OK) {
      r = grcore_context_register(context, &grcore_guest_key, stack);
      if (r != GRCORE_OK) {
        grcore_context_remove_root_source(
            context, &grcore_guest_root_source, stack);
      }
    }
    if (r != GRCORE_OK) {
      a->free_fn(a->ctx, (void *)grown);
      a->free_fn(a->ctx, stack);
      return r;
    }
  }
  if (grown != NULL) {
    a->free_fn(a->ctx, (void *)stack->engines);
    stack->engines = grown;
  }
  stack->engines[stack->engine_count++] = descriptor;
  *out_id = (GRCORE_EngineId)stack->engine_count;
  return GRCORE_OK;
}

const GRCORE_EngineDescriptor * grcore_engine_descriptor(
    const GRCORE_Context * context, GRCORE_EngineId id) {
  const GRCORE_Stack * stack = grcore_context_stack(context);
  if (stack == NULL || id == 0 || id > stack->engine_count) {
    return NULL;
  }
  return stack->engines[id - 1];
}

size_t grcore_engine_count(const GRCORE_Context * context) {
  const GRCORE_Stack * stack = grcore_context_stack(context);
  return stack == NULL ? 0 : stack->engine_count;
}

GRCORE_Result grcore_engine_inspect(const GRCORE_Context * context,
    GRCORE_EngineId id, GRCORE_SlotKind kind, uint64_t value, char * buffer,
    size_t size, size_t * out_length) {
  const GRCORE_EngineDescriptor * d = grcore_engine_descriptor(context, id);
  if (d == NULL || out_length == NULL || (buffer == NULL && size != 0)) {
    return GRCORE_ERR_INVALID;
  }
  if (d->inspect != NULL) {
    *out_length = d->inspect(context, kind, value, buffer, size);
    return GRCORE_OK;
  }
  int n = snprintf(buffer, size, "0x%" PRIx64, value);
  if (n < 0) {
    return GRCORE_ERR_INTERNAL;
  }
  *out_length = (size_t)n;
  return GRCORE_OK;
}
