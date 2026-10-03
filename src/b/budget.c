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
 * Fuel, memory and depth (AD-21).
 *
 * Fuel and memory are enforced through the request word: running out raises a
 * derived request, and the poll turns it into a verdict. The derived bits are
 * levels, so they are recomputed from the budgets rather than trusted to have
 * been set or cleared at the right moment.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "context_internal.h"

#include <stdint.h>
#include <string.h>

#define FUEL_BIT (UINT64_C(1) << GRCORE_REQUEST_FUEL)
#define MEMORY_BIT (UINT64_C(1) << GRCORE_REQUEST_MEMORY)

static bool memory_over(const GRCORE_Context * c) {
  uint64_t limit = grcore_counting_limit(&c->counting);
  return limit != GRCORE_UNLIMITED &&
      grcore_meter_in_use(&c->meter) > limit;
}

void grcore_context_refresh_derived(GRCORE_Context * context) {
  if (grcore_fuel_ceiling_exhausted(context) ||
      grcore_fuel_scope_exhausted(context)) {
    __atomic_fetch_or(&context->request_word, FUEL_BIT, __ATOMIC_RELEASE);
  } else {
    __atomic_fetch_and(&context->request_word, ~FUEL_BIT, __ATOMIC_ACQ_REL);
  }
  if (memory_over(context)) {
    __atomic_fetch_or(&context->request_word, MEMORY_BIT, __ATOMIC_RELEASE);
  } else {
    context->reclaim_tried = false;
    __atomic_fetch_and(&context->request_word, ~MEMORY_BIT, __ATOMIC_ACQ_REL);
  }
}

bool grcore_context_charge_fuel(GRCORE_Context * context, uint64_t amount) {
  if (context == NULL) {
    return false;
  }
  uint64_t used = context->fuel_used;
  used = amount > UINT64_MAX - used ? UINT64_MAX : used + amount;
  context->fuel_used = used;
  /* The scope's clock is exclusive: only the innermost one runs. */
  if (context->fuel_scope_count > 0) {
    GRCORE_FuelScope * s = &context->fuel_scopes[context->fuel_scope_count - 1];
    s->used = amount > UINT64_MAX - s->used ? UINT64_MAX : s->used + amount;
  }
  if (grcore_fuel_ceiling_exhausted(context) ||
      grcore_fuel_scope_exhausted(context)) {
    __atomic_fetch_or(&context->request_word, FUEL_BIT, __ATOMIC_RELEASE);
    return true;
  }
  return false;
}

uint64_t grcore_context_fuel_used(const GRCORE_Context * context) {
  return context == NULL ? 0 : context->fuel_used;
}

uint64_t grcore_context_fuel_remaining(const GRCORE_Context * context) {
  if (context == NULL || context->fuel_limit == GRCORE_UNLIMITED) {
    return GRCORE_UNLIMITED;
  }
  return context->fuel_used >= context->fuel_limit
      ? 0
      : context->fuel_limit - context->fuel_used;
}

uint64_t grcore_context_fuel_limit(const GRCORE_Context * context) {
  return context == NULL ? GRCORE_UNLIMITED : context->fuel_limit;
}

/* A budget may be changed when no guest code is executing: not in the middle
 * of a run, and not in a host call made from inside one. */
static bool budget_settable(const GRCORE_Context * c) {
  return c != NULL && grcore_context_owned_by_caller(c) && !c->tearing_down &&
      (c->config == GRCORE_CONFIG_PARKED_OUTSIDE ||
          c->config == GRCORE_CONFIG_AT_POLL ||
          c->config == GRCORE_CONFIG_PAUSED);
}

GRCORE_Result grcore_context_set_fuel(
    GRCORE_Context * context, uint64_t limit) {
  if (!budget_settable(context)) {
    return GRCORE_ERR_INVALID;
  }
  context->fuel_limit = limit;
  grcore_context_refresh_derived(context);
  return GRCORE_OK;
}

uint64_t grcore_context_memory_limit(const GRCORE_Context * context) {
  return context == NULL ? GRCORE_UNLIMITED
                         : grcore_counting_limit(&context->counting);
}

GRCORE_Result grcore_context_set_memory_bytes(
    GRCORE_Context * context, uint64_t bytes) {
  if (!budget_settable(context)) {
    return GRCORE_ERR_INVALID;
  }
  grcore_counting_set_limit(&context->counting, bytes,
      grcore_counting_reserve(&context->counting));
  grcore_context_refresh_derived(context);
  return GRCORE_OK;
}

uint64_t grcore_context_memory_reserve(const GRCORE_Context * context) {
  return context == NULL ? GRCORE_DEFAULT_MEMORY_RESERVE
                         : grcore_counting_reserve(&context->counting);
}

uint64_t grcore_context_memory_refusals(const GRCORE_Context * context) {
  return context == NULL ? 0 : grcore_counting_refusals(&context->counting);
}

GRCORE_Result grcore_context_enter_depth(
    GRCORE_Context * context, GRCORE_DepthKind kind) {
  if (context == NULL || (unsigned)kind > GRCORE_DEPTH_NATIVE) {
    return GRCORE_ERR_INVALID;
  }
  uint64_t limit = kind == GRCORE_DEPTH_GUEST
      ? grcore_options_get_guest_depth(context->options)
      : grcore_options_get_native_depth(context->options);
  if (context->depth[kind] >= limit) {
    return GRCORE_ERR_LIMIT;
  }
  context->depth[kind]++;
  return GRCORE_OK;
}

GRCORE_Result grcore_context_leave_depth(
    GRCORE_Context * context, GRCORE_DepthKind kind) {
  if (context == NULL || (unsigned)kind > GRCORE_DEPTH_NATIVE ||
      context->depth[kind] == 0) {
    return GRCORE_ERR_INVALID;
  }
  context->depth[kind]--;
  return GRCORE_OK;
}

uint64_t grcore_context_depth(
    const GRCORE_Context * context, GRCORE_DepthKind kind) {
  if (context == NULL || (unsigned)kind > GRCORE_DEPTH_NATIVE) {
    return 0;
  }
  return context->depth[kind];
}

/* ---- Fuel scopes (AD-21) ---------------------------------------------- */

static bool scope_owner(const GRCORE_Context * c) {
  return c != NULL && grcore_context_owned_by_caller(c) && !c->tearing_down;
}

static GRCORE_FuelScope * scope_find(const GRCORE_Context * c, uint64_t id) {
  if (c == NULL || id == 0) {
    return NULL;
  }
  /* Ids rise with depth, so the match is usually the innermost. */
  for (size_t i = c->fuel_scope_count; i > 0; i--) {
    if (c->fuel_scopes[i - 1].id == id) {
      return &c->fuel_scopes[i - 1];
    }
  }
  return NULL;
}

GRCORE_Result grcore_context_fuel_scope_open(GRCORE_Context * context,
    uint64_t budget, GRCORE_ScopePolicy policy, uint64_t * out_id) {
  if (!scope_owner(context) || out_id == NULL ||
      (unsigned)policy > GRCORE_SCOPE_POLICY_PAUSE) {
    return GRCORE_ERR_INVALID;
  }
  if (context->fuel_scope_count == context->fuel_scope_capacity) {
    const GRCORE_Allocator * a = &context->counting.allocator;
    size_t capacity =
        context->fuel_scope_capacity == 0 ? 8 : context->fuel_scope_capacity * 2;
    if (capacity > SIZE_MAX / sizeof(GRCORE_FuelScope)) {
      return GRCORE_ERR_OOM;
    }
    uint64_t refusals = grcore_counting_refusals(&context->counting);
    GRCORE_FuelScope * grown = a->malloc_fn(a->ctx, capacity * sizeof *grown);
    if (grown == NULL) {
      return grcore_counting_refusals(&context->counting) > refusals
          ? GRCORE_ERR_LIMIT
          : GRCORE_ERR_OOM;
    }
    if (context->fuel_scope_count > 0) {
      memcpy(grown, context->fuel_scopes,
          context->fuel_scope_count * sizeof *grown);
    }
    a->free_fn(a->ctx, context->fuel_scopes);
    context->fuel_scopes = grown;
    context->fuel_scope_capacity = capacity;
  }
  GRCORE_FuelScope * s = &context->fuel_scopes[context->fuel_scope_count++];
  s->id = ++context->fuel_scope_serial;
  s->budget = budget;
  s->used = 0;
  s->policy = policy;
  *out_id = s->id;
  grcore_context_refresh_derived(context);
  return GRCORE_OK;
}

GRCORE_Result grcore_context_fuel_scope_close(
    GRCORE_Context * context, uint64_t id) {
  if (!scope_owner(context) || id == 0 || context->fuel_scope_count == 0 ||
      context->fuel_scopes[context->fuel_scope_count - 1].id != id) {
    return GRCORE_ERR_INVALID;
  }
  context->fuel_scope_count--;
  if (context->scoped_unwind == id) {
    /* The engine has unwound to this scope's boundary: the unwind is over, and
     * `run` must not mistake it for the terminal kind. */
    context->scoped_unwind = 0;
    if (context->last_verdict == GRCORE_VERDICT_UNWIND) {
      context->last_verdict = GRCORE_VERDICT_CONTINUE;
    }
    context->unwind_result = GRCORE_OK;
    /* The reason for the unwind is over with it. */
    context->verdict_key_count = 0;
    context->pause_location.file = NULL;
    context->pause_location.line = 0;
  }
  grcore_context_refresh_derived(context);
  return GRCORE_OK;
}

GRCORE_Result grcore_context_fuel_scope_set_budget(
    GRCORE_Context * context, uint64_t id, uint64_t budget) {
  GRCORE_FuelScope * s = scope_find(context, id);
  if (s == NULL || !budget_settable(context)) {
    return GRCORE_ERR_INVALID;
  }
  s->budget = budget;
  grcore_context_refresh_derived(context);
  return GRCORE_OK;
}

GRCORE_Result grcore_context_fuel_scope_used(
    const GRCORE_Context * context, uint64_t id, uint64_t * out_used) {
  const GRCORE_FuelScope * s = scope_find(context, id);
  if (s == NULL || out_used == NULL) {
    return GRCORE_ERR_INVALID;
  }
  *out_used = s->used;
  return GRCORE_OK;
}

GRCORE_Result grcore_context_fuel_scope_remaining(
    const GRCORE_Context * context, uint64_t id, uint64_t * out_remaining) {
  const GRCORE_FuelScope * s = scope_find(context, id);
  if (s == NULL || out_remaining == NULL) {
    return GRCORE_ERR_INVALID;
  }
  *out_remaining = s->budget == GRCORE_UNLIMITED ? GRCORE_UNLIMITED
      : s->used >= s->budget                     ? 0
                                                 : s->budget - s->used;
  return GRCORE_OK;
}

uint64_t grcore_context_fuel_scope_depth(const GRCORE_Context * context) {
  return context == NULL ? 0 : context->fuel_scope_count;
}

uint64_t grcore_context_fuel_scope_top(const GRCORE_Context * context) {
  return context == NULL || context->fuel_scope_count == 0
      ? 0
      : context->fuel_scopes[context->fuel_scope_count - 1].id;
}

uint64_t grcore_context_fuel_scope_unwinding(const GRCORE_Context * context) {
  return context == NULL ? 0 : context->scoped_unwind;
}
