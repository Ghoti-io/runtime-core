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
 * Activation records (AD-17, AD-21).
 *
 * The records are an array in the stack, grown by copy through the context's
 * counting allocator. A record is named by a serial that is never reused, so
 * a stale reference cannot name a later record the way a stale frame offset
 * can name a later frame.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "guest_internal.h"

#include "../b/context_internal.h"

#include <ghoti.io/runtime-core/b/budget.h>
#include <ghoti.io/runtime-core/b/poll.h>

static bool enters_native_depth(GRCORE_ActivationKind kind) {
  return kind == GRCORE_ACTIVATION_JIT || kind == GRCORE_ACTIVATION_NATIVE ||
      kind == GRCORE_ACTIVATION_REENTRY;
}

bool grcore_activation_absorb_cell(GRCORE_Stack * stack) {
  GRCORE_Context * context = stack->context;
  if (context->walk_cell[0] == 0) {
    return true;
  }
  if (stack->activation_count == 0 ||
      stack->activations[stack->activation_count - 1].kind !=
          GRCORE_ACTIVATION_JIT) {
    /* Compiled code stores the cell only inside a JIT record, and every entry
     * and leave moves it, so this is stale or forged. It is left where it is
     * for the walk to report. */
    return false;
  }
  GRCORE_ActivationRecord * rec =
      &stack->activations[stack->activation_count - 1];
  rec->frame_base = context->walk_cell[0];
  rec->return_address = context->walk_cell[1];
  context->walk_cell[0] = 0;
  context->walk_cell[1] = 0;
  return true;
}

GRCORE_Result grcore_activation_enter(GRCORE_Stack * stack,
    GRCORE_ActivationKind kind, GRCORE_EngineId engine, bool nested,
    const GRCORE_CSegment * segment, GRCORE_ActivationRef * out_ref) {
  if (!grcore_stack_owned(stack) || out_ref == NULL ||
      (unsigned)kind > GRCORE_ACTIVATION_REENTRY ||
      engine > stack->engine_count ||
      (segment != NULL && segment->lo >= segment->hi)) {
    return GRCORE_ERR_INVALID;
  }
  /* A re-entry is a nested activation by definition (AD-5). */
  nested = nested || kind == GRCORE_ACTIVATION_REENTRY;
  GRCORE_Context * context = stack->context;
  /* Everything that can be refused comes first, and the native depth, which is
   * given back if a later step fails, last of the refusable ones. */
  void * grown;
  GRCORE_Result r = grcore_guest_array_reserve(context, stack->activations,
      &stack->activation_capacity, stack->activation_count,
      sizeof *stack->activations, &grown);
  if (r != GRCORE_OK) {
    return r;
  }
  stack->activations = grown;
  if (enters_native_depth(kind)) {
    r = grcore_context_enter_depth(context, GRCORE_DEPTH_NATIVE);
    if (r != GRCORE_OK) {
      return r;
    }
  }
  if (nested) {
    r = grcore_context_nested_enter(context);
    if (r != GRCORE_OK) {
      if (enters_native_depth(kind)) {
        grcore_context_leave_depth(context, GRCORE_DEPTH_NATIVE);
      }
      return r;
    }
  }
  /* A re-entry finds the native-stack limit set by the run it is inside; one
   * that finds none (the host entered on a path the run did not see) sets it
   * from here. */
  bool set_limit = false;
  if (kind == GRCORE_ACTIVATION_REENTRY && context->native_limit == 0 &&
      grcore_context_native_stack_bytes(context) != GRCORE_UNLIMITED) {
    grcore_context_native_limit_here(context);
    set_limit = context->native_limit != 0;
  }
  /* What compiled code recorded belongs to the record that is innermost now,
   * and the one being entered starts with none (AD-28). */
  (void)grcore_activation_absorb_cell(stack);
  GRCORE_ActivationRecord * rec = &stack->activations[stack->activation_count++];
  rec->id = ++stack->activation_serial;
  rec->kind = kind;
  rec->engine = engine;
  rec->nested = nested;
  rec->base_frames = stack->frame_count;
  rec->lo = segment == NULL ? 0 : segment->lo;
  rec->hi = segment == NULL ? 0 : segment->hi;
  rec->frame_base = 0;
  rec->return_address = 0;
  rec->rebuilt = false;
  rec->set_limit = set_limit;
  if (kind == GRCORE_ACTIVATION_JIT) {
    stack->jit_live++;
  }
  out_ref->id = rec->id;
  return GRCORE_OK;
}

bool grcore_activation_drop_top(GRCORE_Stack * stack) {
  if (stack == NULL || stack->activation_count == 0) {
    return false;
  }
  GRCORE_ActivationRecord rec = stack->activations[--stack->activation_count];
  if (rec.set_limit) {
    /* The limit names the stack this re-entry measured from, which nobody is
     * running on once it is left (a later re-entry may be on another thread). */
    stack->context->native_limit = 0;
  }
  if (rec.kind == GRCORE_ACTIVATION_JIT) {
    /* Its compiled frames are gone, and so is anything compiled code recorded
     * for them: a later walk must not read frames that were left. */
    stack->context->walk_cell[0] = 0;
    stack->context->walk_cell[1] = 0;
  }
  if (enters_native_depth(rec.kind)) {
    grcore_context_leave_depth(stack->context, GRCORE_DEPTH_NATIVE);
  }
  if (rec.nested) {
    grcore_context_nested_leave(stack->context);
  }
  if (rec.kind == GRCORE_ACTIVATION_JIT && --stack->jit_live == 0) {
    /* No compiled frame can be on the native stack now, so nothing can return
     * into retired code (AD-28). */
    grcore_registry_release_retired(stack);
  }
  return true;
}

GRCORE_Result grcore_activation_leave(
    GRCORE_Stack * stack, GRCORE_ActivationRef ref) {
  if (!grcore_stack_owned(stack) || ref.id == 0 ||
      stack->activation_count == 0) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_ActivationRecord * top =
      &stack->activations[stack->activation_count - 1];
  /* Guest frames pushed since the record was entered must have been popped,
   * with one exception: a JIT record whose compiled frames were rebuilt into
   * their guest frames (AD-28) leaves those frames on the stack for the
   * interpreter to finish, because the compiled frames only returned. */
  if (top->id != ref.id ||
      (top->rebuilt ? stack->frame_count < top->base_frames
                    : stack->frame_count != top->base_frames)) {
    return GRCORE_ERR_INVALID;
  }
  /* A scope opened inside this activation must be closed before it. */
  if (stack->scope_count > 0 &&
      stack->scopes[stack->scope_count - 1].activation_base >=
          stack->activation_count) {
    return GRCORE_ERR_INVALID;
  }
  grcore_activation_drop_top(stack);
  return GRCORE_OK;
}

size_t grcore_activation_count(const GRCORE_Stack * stack) {
  return stack == NULL ? 0 : stack->activation_count;
}

static void fill_info(
    const GRCORE_ActivationRecord * rec, GRCORE_ActivationInfo * out) {
  out->kind = rec->kind;
  out->engine = rec->engine;
  out->nested = rec->nested;
  out->base_frame_count = rec->base_frames;
  out->segment_lo = rec->lo;
  out->segment_hi = rec->hi;
  out->frame_base = rec->frame_base;
  out->return_address = rec->return_address;
}

GRCORE_Result grcore_activation_at(const GRCORE_Stack * stack, size_t index,
    GRCORE_ActivationInfo * out_info) {
  if (stack == NULL || out_info == NULL || index >= stack->activation_count) {
    return GRCORE_ERR_INVALID;
  }
  fill_info(&stack->activations[index], out_info);
  return GRCORE_OK;
}

GRCORE_Result grcore_activation_info(const GRCORE_Stack * stack,
    GRCORE_ActivationRef ref, GRCORE_ActivationInfo * out_info) {
  if (stack == NULL || out_info == NULL || ref.id == 0) {
    return GRCORE_ERR_INVALID;
  }
  for (size_t i = stack->activation_count; i > 0; i--) {
    if (stack->activations[i - 1].id == ref.id) {
      fill_info(&stack->activations[i - 1], out_info);
      return GRCORE_OK;
    }
  }
  return GRCORE_ERR_INVALID;
}

GRCORE_Result grcore_activation_top(
    const GRCORE_Stack * stack, GRCORE_ActivationRef * out_ref) {
  if (stack == NULL || out_ref == NULL || stack->activation_count == 0) {
    return GRCORE_ERR_INVALID;
  }
  out_ref->id = stack->activations[stack->activation_count - 1].id;
  return GRCORE_OK;
}

GRCORE_Result grcore_activation_set_compiled(GRCORE_Stack * stack,
    GRCORE_ActivationRef ref, uintptr_t frame_base, uintptr_t return_address) {
  if (!grcore_stack_owned(stack) || ref.id == 0 ||
      (frame_base == 0 && return_address != 0)) {
    return GRCORE_ERR_INVALID;
  }
  for (size_t i = stack->activation_count; i > 0; i--) {
    GRCORE_ActivationRecord * rec = &stack->activations[i - 1];
    if (rec->id == ref.id) {
      /* Only a JIT record is counted toward the release of retired code, so
       * state on any other kind could outlive the code it names. */
      if (rec->kind != GRCORE_ACTIVATION_JIT) {
        return GRCORE_ERR_INVALID;
      }
      rec->frame_base = frame_base;
      rec->return_address = return_address;
      return GRCORE_OK;
    }
  }
  return GRCORE_ERR_INVALID;
}
