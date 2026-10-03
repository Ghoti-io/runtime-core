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
 * The execution context: lifecycle, ownership and keyed state.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "context_internal.h"
#include "group_internal.h"
#include "options_internal.h"

#include <stdint.h>
#include <string.h>

uintptr_t grcore_thread_id(void) {
  static uintptr_t next_id = 0;
  static _Thread_local uintptr_t mine = 0;
  if (mine == 0) {
    mine = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
  }
  return mine;
}

static bool owned_by_caller(const GRCORE_Context * context) {
  return __atomic_load_n(&context->owner, __ATOMIC_ACQUIRE) ==
      grcore_thread_id();
}

bool grcore_context_owned_by_caller(const GRCORE_Context * context) {
  return owned_by_caller(context);
}

/* The poll's scratch block: for every registration one vote and one slot in
 * the verdict-key list and the shuffle order, after the same for the built-in
 * kinds. It is sized with the registration table, so that the poll never
 * allocates, and a growth carries what is already in it across, because a
 * handler may register at-poll. */
typedef struct Scratch {
  const GRCORE_Key ** keys;
  size_t * order;
  GRCORE_Vote * votes;
} Scratch;

static bool scratch_make(
    const GRCORE_Context * c, size_t capacity, Scratch * out) {
  const size_t core = GRCORE_REQUEST_CORE_COUNT;
  if (capacity > (SIZE_MAX / 2) / 32) {
    return false;
  }
  size_t bytes = (core + capacity) * sizeof(void *) +
      capacity * sizeof(size_t) + (core + capacity) * sizeof(GRCORE_Vote);
  const GRCORE_Allocator * a = c->group->allocator;
  void * block = a->malloc_fn(a->ctx, bytes);
  if (block == NULL) {
    return false;
  }
  out->keys = block;
  out->order = (void *)(out->keys + core + capacity);
  out->votes = (void *)(out->order + capacity);
  for (size_t i = 0; i < core + capacity; i++) {
    out->keys[i] = NULL;
    out->votes[i].verdict = GRCORE_VERDICT_CONTINUE;
    out->votes[i].result = GRCORE_ERR_LIMIT;
  }
  for (size_t i = 0; i < capacity; i++) {
    out->order[i] = 0;
  }
  if (c->votes != NULL) {
    size_t old = c->scratch_capacity;
    memcpy(out->keys, c->verdict_keys, (core + old) * sizeof *out->keys);
    memcpy(out->order, c->order, old * sizeof *out->order);
    memcpy(out->votes, c->votes, (core + old) * sizeof *out->votes);
  }
  return true;
}

static void scratch_free(const GRCORE_Context * c, void * block) {
  const GRCORE_Allocator * a = c->group->allocator;
  a->free_fn(a->ctx, block);
}

static void scratch_install(
    GRCORE_Context * c, const Scratch * s, size_t capacity) {
  void * old = (void *)c->verdict_keys;
  c->verdict_keys = s->keys;
  c->order = s->order;
  c->votes = s->votes;
  c->scratch_capacity = capacity;
  if (old != NULL) {
    scratch_free(c, old);
  }
}

GRCORE_Result grcore_context_create(GRCORE_Group * group,
    const GRCORE_Options * options, GRCORE_Context ** out_context) {
  if (group == NULL || out_context == NULL) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_Allocator * a = group->allocator;
  GRCORE_Context * c = a->calloc_fn(a->ctx, 1, sizeof *c);
  if (c == NULL) {
    return GRCORE_ERR_OOM;
  }
  c->group = group;
  GRCORE_Result r = grcore_options_clone(options, a, &c->options);
  if (r != GRCORE_OK) {
    a->free_fn(a->ctx, c);
    return r;
  }
  Scratch scratch;
  if (!scratch_make(c, 0, &scratch)) {
    grcore_options_destroy(c->options);
    a->free_fn(a->ctx, c);
    return GRCORE_ERR_OOM;
  }
  scratch_install(c, &scratch, 0);
  grcore_meter_init(&c->meter);
  grcore_counting_init(&c->counting, &c->meter, a, group->pages);
  c->counting.request_word = &c->request_word;
  grcore_counting_set_limit(&c->counting,
      grcore_options_get_memory_bytes(c->options),
      grcore_options_get_memory_reserve(c->options));
  c->fuel_limit = grcore_options_get_fuel(c->options);
  c->unwind_result = GRCORE_OK;
  c->owner = grcore_thread_id();
  c->config = GRCORE_CONFIG_PARKED_OUTSIDE;
  grcore_group_enter(group);
  *out_context = c;
  return GRCORE_OK;
}

GRCORE_Result grcore_context_destroy(GRCORE_Context * context) {
  if (context == NULL || !owned_by_caller(context) || context->tearing_down ||
      (context->config != GRCORE_CONFIG_PARKED_OUTSIDE &&
          context->config != GRCORE_CONFIG_PAUSED)) {
    return GRCORE_ERR_INVALID;
  }
  /* Everything a destructor could call to change the context is refused from
   * here on, and a registration leaves the table before its destructor runs,
   * so a destructor never finds a value that was already destroyed. */
  context->tearing_down = true;
  /* Reverse registration order (AD-20). The context stays fully valid while
   * its destructors run, so one may read the meter or release memory through
   * the context's allocator. */
  while (context->registration_count > 0) {
    GRCORE_Registration reg =
        context->registrations[--context->registration_count];
    if (reg.key->destroy != NULL) {
      reg.key->destroy(context, reg.value);
    }
  }
  /* After the destructors, which may still post to or read the port. A post
   * that races this finds the context gone and is refused. */
  grcore_context_port_detach(context);
  GRCORE_Group * group = context->group;
  const GRCORE_Allocator * a = group->allocator;
  a->free_fn(a->ctx, context->registrations);
  a->free_fn(a->ctx, context->kind_keys);
  scratch_free(context, (void *)context->verdict_keys);
  grcore_options_destroy(context->options);
  a->free_fn(a->ctx, context);
  grcore_group_leave(group);
  return GRCORE_OK;
}

GRCORE_ContextState grcore_context_state(const GRCORE_Context * context) {
  if (context == NULL) {
    return GRCORE_CONTEXT_PARKED;
  }
  switch (context->config) {
    case GRCORE_CONFIG_RUNNING:
      return GRCORE_CONTEXT_RUNNING;
    case GRCORE_CONFIG_AT_POLL:
      return GRCORE_CONTEXT_AT_POLL;
    case GRCORE_CONFIG_PAUSED:
      return GRCORE_CONTEXT_PAUSED;
    case GRCORE_CONFIG_PARKED_OUTSIDE:
    case GRCORE_CONFIG_PARKED_INSIDE:
    case GRCORE_CONFIG_COUNT:
      break;
  }
  return GRCORE_CONTEXT_PARKED;
}

/* legal[from][to]: the eight edges of AD-20. */
static const bool legal[GRCORE_CONFIG_COUNT][GRCORE_CONFIG_COUNT] = {
    [GRCORE_CONFIG_PARKED_OUTSIDE][GRCORE_CONFIG_RUNNING] = true,
    [GRCORE_CONFIG_RUNNING][GRCORE_CONFIG_AT_POLL] = true,
    [GRCORE_CONFIG_AT_POLL][GRCORE_CONFIG_RUNNING] = true,
    [GRCORE_CONFIG_AT_POLL][GRCORE_CONFIG_PAUSED] = true,
    [GRCORE_CONFIG_PAUSED][GRCORE_CONFIG_RUNNING] = true,
    [GRCORE_CONFIG_RUNNING][GRCORE_CONFIG_PARKED_INSIDE] = true,
    [GRCORE_CONFIG_PARKED_INSIDE][GRCORE_CONFIG_RUNNING] = true,
    [GRCORE_CONFIG_RUNNING][GRCORE_CONFIG_PARKED_OUTSIDE] = true,
};

GRCORE_Result grcore_context_transition(
    GRCORE_Context * context, GRCORE_ContextConfig to) {
  if (context == NULL || (unsigned)to >= GRCORE_CONFIG_COUNT ||
      !owned_by_caller(context) || context->tearing_down || !legal[context->config][to]) {
    return GRCORE_ERR_INVALID;
  }
  context->config = to;
  return GRCORE_OK;
}

GRCORE_Result grcore_context_park(GRCORE_Context * context) {
  return grcore_context_transition(context, GRCORE_CONFIG_PARKED_INSIDE);
}

GRCORE_Result grcore_context_unpark(GRCORE_Context * context) {
  /* PARKED_OUTSIDE to RUNNING is also a legal edge, but it is `run` beginning,
   * not the end of a bracket. */
  if (context == NULL || !owned_by_caller(context) ||
      context->config != GRCORE_CONFIG_PARKED_INSIDE) {
    return GRCORE_ERR_INVALID;
  }
  return grcore_context_transition(context, GRCORE_CONFIG_RUNNING);
}

GRCORE_Result grcore_context_acquire(GRCORE_Context * context) {
  if (context == NULL) {
    return GRCORE_ERR_INVALID;
  }
  uintptr_t expected = 0;
  if (context->tearing_down) {
    return GRCORE_ERR_INVALID;
  }
  if (!__atomic_compare_exchange_n(&context->owner, &expected,
          grcore_thread_id(), 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return GRCORE_ERR_INVALID;
  }
  return GRCORE_OK;
}

GRCORE_Result grcore_context_release(GRCORE_Context * context) {
  if (context == NULL || !owned_by_caller(context) || context->tearing_down ||
      (context->config != GRCORE_CONFIG_PARKED_OUTSIDE &&
          context->config != GRCORE_CONFIG_PAUSED)) {
    return GRCORE_ERR_INVALID;
  }
  __atomic_store_n(&context->owner, 0, __ATOMIC_RELEASE);
  return GRCORE_OK;
}

bool grcore_context_is_owner(const GRCORE_Context * context) {
  return context != NULL && owned_by_caller(context);
}

bool grcore_context_guest_state_readable(const GRCORE_Context * context) {
  return context != NULL && owned_by_caller(context) &&
      (context->config == GRCORE_CONFIG_AT_POLL ||
          context->config == GRCORE_CONFIG_PAUSED);
}

GRCORE_Result grcore_context_register(
    GRCORE_Context * context, const GRCORE_Key * key, void * value) {
  if (context == NULL || key == NULL || value == NULL ||
      !owned_by_caller(context) || context->tearing_down ||
      context->config == GRCORE_CONFIG_RUNNING ||
      (key->cardinality != GRCORE_CARDINALITY_ONE &&
          key->cardinality != GRCORE_CARDINALITY_MANY) ||
      (unsigned)key->phase > GRCORE_PHASE_YIELD) {
    return GRCORE_ERR_INVALID;
  }
  if (key->cardinality == GRCORE_CARDINALITY_ONE &&
      grcore_context_slot(context, key) != NULL) {
    return GRCORE_ERR_INVALID;
  }
  if (context->registration_count == context->registration_capacity) {
    const GRCORE_Allocator * a = context->group->allocator;
    size_t capacity =
        context->registration_capacity == 0 ? 4 : context->registration_capacity * 2;
    /* The registration table and the poll's scratch block grow together or
     * not at all: a poll must never find a registration it has no vote slot
     * for. */
    Scratch scratch;
    if (!scratch_make(context, capacity, &scratch)) {
      return GRCORE_ERR_OOM;
    }
    GRCORE_Registration * grown = a->realloc_fn(
        a->ctx, context->registrations, capacity * sizeof *grown);
    if (grown == NULL) {
      scratch_free(context, scratch.keys);
      return GRCORE_ERR_OOM;
    }
    context->registrations = grown;
    context->registration_capacity = capacity;
    scratch_install(context, &scratch, capacity);
  }
  context->registrations[context->registration_count].key = key;
  context->registrations[context->registration_count].value = value;
  context->registration_count++;
  return GRCORE_OK;
}

void * grcore_context_slot(
    const GRCORE_Context * context, const GRCORE_Key * key) {
  if (context == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < context->registration_count; i++) {
    if (context->registrations[i].key == key) {
      return context->registrations[i].value;
    }
  }
  return NULL;
}

size_t grcore_context_registration_count(const GRCORE_Context * context) {
  return context == NULL ? 0 : context->registration_count;
}

GRCORE_Result grcore_context_registration(const GRCORE_Context * context,
    size_t index, const GRCORE_Key ** out_key, void ** out_value) {
  if (context == NULL || index >= context->registration_count) {
    return GRCORE_ERR_INVALID;
  }
  if (out_key != NULL) {
    *out_key = context->registrations[index].key;
  }
  if (out_value != NULL) {
    *out_value = context->registrations[index].value;
  }
  return GRCORE_OK;
}

GRCORE_Group * grcore_context_group(const GRCORE_Context * context) {
  return context == NULL ? NULL : context->group;
}

const GRCORE_Allocator * grcore_context_allocator(
    const GRCORE_Context * context) {
  return context == NULL ? NULL : &context->counting.allocator;
}

const GRCORE_PageProvider * grcore_context_page_provider(
    const GRCORE_Context * context) {
  return context == NULL ? NULL : &context->counting.pages;
}

const GRCORE_Options * grcore_context_options(const GRCORE_Context * context) {
  return context == NULL ? NULL : context->options;
}

uint64_t grcore_context_fuel(const GRCORE_Context * context) {
  return grcore_options_get_fuel(grcore_context_options(context));
}

uint64_t grcore_context_memory_bytes(const GRCORE_Context * context) {
  return grcore_options_get_memory_bytes(grcore_context_options(context));
}

uint64_t grcore_context_guest_depth(const GRCORE_Context * context) {
  return grcore_options_get_guest_depth(grcore_context_options(context));
}

uint64_t grcore_context_native_depth(const GRCORE_Context * context) {
  return grcore_options_get_native_depth(grcore_context_options(context));
}

uint64_t grcore_context_memory_in_use(const GRCORE_Context * context) {
  return context == NULL ? 0 : grcore_meter_in_use(&context->meter);
}

uint64_t grcore_context_memory_peak(const GRCORE_Context * context) {
  return context == NULL ? 0 : grcore_meter_peak(&context->meter);
}

uint64_t grcore_context_memory_blocks(const GRCORE_Context * context) {
  return context == NULL ? 0 : grcore_meter_blocks(&context->meter);
}
