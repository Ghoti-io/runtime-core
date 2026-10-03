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

#define FUEL_BIT (UINT64_C(1) << GRCORE_REQUEST_FUEL)
#define MEMORY_BIT (UINT64_C(1) << GRCORE_REQUEST_MEMORY)

static bool memory_over(const GRCORE_Context * c) {
  uint64_t limit = grcore_counting_limit(&c->counting);
  return limit != GRCORE_UNLIMITED &&
      grcore_meter_in_use(&c->meter) > limit;
}

void grcore_context_refresh_derived(GRCORE_Context * context) {
  if (context->fuel_used > context->fuel_limit) {
    __atomic_fetch_or(&context->request_word, FUEL_BIT, __ATOMIC_RELEASE);
  } else {
    __atomic_fetch_and(&context->request_word, ~FUEL_BIT, __ATOMIC_ACQ_REL);
  }
  if (memory_over(context)) {
    __atomic_fetch_or(&context->request_word, MEMORY_BIT, __ATOMIC_RELEASE);
  } else {
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
  if (used > context->fuel_limit) {
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
