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
 * The unwinder (AD-5, AD-18). It edits data and calls engine hooks, and runs
 * no guest code.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "guest_internal.h"

#include <ghoti.io/runtime-core/a/unwind.h>
#include <ghoti.io/runtime-core/b/budget.h>

GRCORE_Result grcore_unwind_frames(
    GRCORE_Stack * stack, size_t target, size_t * popped) {
  size_t n = 0;
  while (stack->frame_count > target) {
    GRCORE_AbstractFrame frame;
    GRCORE_FrameHeader h;
    GRCORE_FrameRef top = {stack->top};
    if (!grcore_stack_hook_frame(stack, top, 0, &frame, &h)) {
      return GRCORE_ERR_INTERNAL;
    }
    if (frame.descriptor->unwind != NULL) {
      frame.descriptor->unwind(stack->context, &frame);
    }
    if (grcore_stack_pop(stack) != GRCORE_OK) {
      return GRCORE_ERR_INTERNAL;
    }
    n++;
  }
  *popped = n;
  return GRCORE_OK;
}

/* Drops activations and scopes down to the given counts, innermost first. A
 * scope opened after an activation was entered is dropped before it, and one
 * opened before it stays, so the two arrays unwind as the one stack of
 * boundaries they are. */
static void drop_boundaries(
    GRCORE_Stack * stack, size_t activations, size_t scopes) {
  while (stack->activation_count > activations || stack->scope_count > scopes) {
    bool scope_is_inner = stack->scope_count > scopes &&
        (stack->activation_count <= activations ||
            stack->scopes[stack->scope_count - 1].activation_base >=
                stack->activation_count);
    if (scope_is_inner) {
      grcore_budget_scope_drop_top(stack);
    } else {
      grcore_activation_drop_top(stack);
    }
  }
}

GRCORE_Result grcore_unwind_to_activation(
    GRCORE_Stack * stack, GRCORE_ActivationRef ref, size_t * out_popped) {
  if (!grcore_stack_owned(stack) || ref.id == 0) {
    return GRCORE_ERR_INVALID;
  }
  size_t index = stack->activation_count;
  while (index > 0 && stack->activations[index - 1].id != ref.id) {
    index--;
  }
  if (index == 0) {
    return GRCORE_ERR_INVALID;
  }
  index--; /* the target */
  size_t popped;
  GRCORE_Result r =
      grcore_unwind_frames(stack, stack->activations[index].base_frames, &popped);
  if (r != GRCORE_OK) {
    return r;
  }
  /* The scopes opened while the target or something deeper was innermost
   * have an activation base above the target's index. */
  size_t keep_scopes = stack->scope_count;
  while (keep_scopes > 0 && stack->scopes[keep_scopes - 1].activation_base > index) {
    keep_scopes--;
  }
  drop_boundaries(stack, index + 1, keep_scopes);
  if (out_popped != NULL) {
    *out_popped = popped;
  }
  return GRCORE_OK;
}

GRCORE_Result grcore_unwind_all(GRCORE_Stack * stack, size_t * out_popped) {
  if (!grcore_stack_owned(stack)) {
    return GRCORE_ERR_INVALID;
  }
  size_t popped;
  GRCORE_Result r = grcore_unwind_frames(stack, 0, &popped);
  if (r != GRCORE_OK) {
    return r;
  }
  drop_boundaries(stack, 0, 0);
  /* A fuel scope opened straight through B belongs to no record of A's, and
   * is still part of the run that is over. */
  GRCORE_Context * context = stack->context;
  while (grcore_context_fuel_scope_depth(context) > 0) {
    if (grcore_context_fuel_scope_close(
            context, grcore_context_fuel_scope_top(context)) != GRCORE_OK) {
      break;
    }
  }
  if (out_popped != NULL) {
    *out_popped = popped;
  }
  return GRCORE_OK;
}
