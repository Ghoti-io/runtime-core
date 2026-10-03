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
 * Budget scopes (AD-21). B counts; this remembers where on the stack and among
 * the activations a scope began, which only A can.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "guest_internal.h"

#include <ghoti.io/runtime-core/a/budget_scope.h>
#include <ghoti.io/runtime-core/a/unwind.h>

GRCORE_Result grcore_budget_scope_open(GRCORE_Stack * stack, uint64_t fuel,
    GRCORE_ScopePolicy policy, GRCORE_BudgetScope * out_scope) {
  if (!grcore_stack_owned(stack) || out_scope == NULL ||
      (unsigned)policy > GRCORE_SCOPE_POLICY_PAUSE) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Context * context = stack->context;
  /* Room for the record first, so the fuel scope is not opened and then left
   * with nowhere to be remembered. */
  void * grown;
  GRCORE_Result r = grcore_guest_array_reserve(context, stack->scopes,
      &stack->scope_capacity, stack->scope_count, sizeof *stack->scopes, &grown);
  if (r != GRCORE_OK) {
    return r;
  }
  stack->scopes = grown;
  uint64_t id;
  r = grcore_context_fuel_scope_open(context, fuel, policy, &id);
  if (r != GRCORE_OK) {
    return r;
  }
  GRCORE_ScopeRecord * rec = &stack->scopes[stack->scope_count++];
  rec->id = id;
  rec->frame_base = stack->frame_count;
  rec->activation_base = stack->activation_count;
  out_scope->id = id;
  return GRCORE_OK;
}

bool grcore_budget_scope_drop_top(GRCORE_Stack * stack) {
  if (stack == NULL || stack->scope_count == 0) {
    return false;
  }
  uint64_t id = stack->scopes[--stack->scope_count].id;
  GRCORE_Context * context = stack->context;
  /* A fuel scope opened straight through B inside this one is closed with it. */
  while (grcore_context_fuel_scope_depth(context) > 0 &&
      grcore_context_fuel_scope_top(context) != id) {
    if (grcore_context_fuel_scope_close(
            context, grcore_context_fuel_scope_top(context)) != GRCORE_OK) {
      return true;
    }
  }
  grcore_context_fuel_scope_close(context, id);
  return true;
}

GRCORE_Result grcore_budget_scope_close(
    GRCORE_Stack * stack, GRCORE_BudgetScope scope) {
  if (!grcore_stack_owned(stack) || scope.id == 0 || stack->scope_count == 0) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_ScopeRecord * top = &stack->scopes[stack->scope_count - 1];
  if (top->id != scope.id || stack->frame_count != top->frame_base ||
      stack->activation_count != top->activation_base) {
    return GRCORE_ERR_INVALID;
  }
  /* B refuses an id that is not its innermost, which it is not if a fuel
   * scope was opened straight through B and left open. */
  if (grcore_context_fuel_scope_top(stack->context) != scope.id) {
    return GRCORE_ERR_INVALID;
  }
  grcore_budget_scope_drop_top(stack);
  return GRCORE_OK;
}

GRCORE_Result grcore_budget_scope_unwind(
    GRCORE_Stack * stack, GRCORE_BudgetScope scope, size_t * out_popped) {
  if (!grcore_stack_owned(stack) || scope.id == 0) {
    return GRCORE_ERR_INVALID;
  }
  size_t index = stack->scope_count;
  while (index > 0 && stack->scopes[index - 1].id != scope.id) {
    index--;
  }
  if (index == 0) {
    return GRCORE_ERR_INVALID;
  }
  index--;
  GRCORE_ScopeRecord rec = stack->scopes[index];
  size_t popped;
  GRCORE_Result r = grcore_unwind_frames(stack, rec.frame_base, &popped);
  if (r != GRCORE_OK) {
    return r;
  }
  /* Activations entered since, and scopes inside this one, innermost first;
   * the scope itself goes last. */
  while (stack->scope_count > index + 1 ||
      stack->activation_count > rec.activation_base) {
    bool scope_is_inner = stack->scope_count > index + 1 &&
        (stack->activation_count <= rec.activation_base ||
            stack->scopes[stack->scope_count - 1].activation_base >=
                stack->activation_count);
    if (scope_is_inner) {
      grcore_budget_scope_drop_top(stack);
    } else {
      grcore_activation_drop_top(stack);
    }
  }
  grcore_budget_scope_drop_top(stack);
  if (out_popped != NULL) {
    *out_popped = popped;
  }
  return GRCORE_OK;
}

bool grcore_budget_scope_exhausted(
    const GRCORE_Stack * stack, GRCORE_BudgetScope scope) {
  if (stack == NULL || scope.id == 0) {
    return false;
  }
  for (size_t i = stack->scope_count; i > 0; i--) {
    if (stack->scopes[i - 1].id == scope.id) {
      return grcore_context_fuel_scope_unwinding(stack->context) == scope.id;
    }
  }
  return false;
}

size_t grcore_budget_scope_count(const GRCORE_Stack * stack) {
  return stack == NULL ? 0 : stack->scope_count;
}

GRCORE_Result grcore_budget_scope_innermost(
    const GRCORE_Stack * stack, GRCORE_BudgetScope * out_scope) {
  if (stack == NULL || out_scope == NULL || stack->scope_count == 0) {
    return GRCORE_ERR_INVALID;
  }
  out_scope->id = stack->scopes[stack->scope_count - 1].id;
  return GRCORE_OK;
}
