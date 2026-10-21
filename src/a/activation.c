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
#include <ghoti.io/runtime-core/a/boundary.h>

#include "guest_internal.h"

#include "../b/context_internal.h"

#include <ghoti.io/runtime-core/b/budget.h>
#include <ghoti.io/runtime-core/b/poll.h>

static bool enters_native_depth(GRCORE_ActivationKind kind) {
  return kind == GRCORE_ACTIVATION_JIT || kind == GRCORE_ACTIVATION_NATIVE ||
      kind == GRCORE_ACTIVATION_REENTRY;
}

/* grcore_stack_owned without the call: the same predicate (stack.c's owner_of),
 * made of the same inline load and compare. Every entry and leave makes it. */
static inline bool stack_owned(const GRCORE_Stack * stack) {
  return stack != NULL && grcore_context_owned_inline(stack->context);
}

/* The walk cell moved into the innermost record: grcore_activation_absorb_cell,
 * and the entry's own step, inline in both. */
static inline bool absorb_cell(GRCORE_Stack * stack) {
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

bool grcore_activation_absorb_cell(GRCORE_Stack * stack) {
  return absorb_cell(stack);
}

/* The last steps of every entry, once nothing more can be refused: what
 * compiled code recorded is moved to the record that is innermost now, and the
 * new record is pushed. */
static inline GRCORE_ActivationRef push_record(GRCORE_Stack * stack,
    GRCORE_ActivationKind kind, GRCORE_EngineId engine, bool nested,
    const GRCORE_CSegment * segment, bool set_limit) {
  /* The one being entered starts with no compiled frame (AD-28). */
  (void)absorb_cell(stack);
  /* Every member a record that is not a boundary has is written, whatever the
   * slot held before: no compiled frame yet, not rebuilt, and no boundary state
   * (grcore_boundary_enter sets that, and the members only a boundary has,
   * after this returns). */
  GRCORE_ActivationRecord * rec = &stack->activations[stack->activation_count++];
  const uint64_t id = ++stack->activation_serial;
  rec->id = id;
  rec->kind = kind;
  rec->engine = engine;
  rec->base_frames = stack->frame_count;
  rec->lo = segment == NULL ? 0 : segment->lo;
  rec->hi = segment == NULL ? 0 : segment->hi;
  rec->frame_base = 0;
  rec->return_address = 0;
  rec->nested = nested;
  rec->set_limit = set_limit;
  rec->rebuilt = false;
  rec->boundary = false;
  rec->boundary_resumable = false;
  rec->boundary_upgraded_native = false;
  if (kind == GRCORE_ACTIVATION_JIT) {
    stack->jit_live++;
  }
  return (GRCORE_ActivationRef){id};
}

/* Every entry the common one below does not take: a nested record (a re-entry
 * always), or one that must grow the array first. The arguments are checked.
 * Out of line, so that the common entry keeps no registers for its calls. */
static GRCORE_Result __attribute__((noinline)) enter_general(
    GRCORE_Stack * stack, GRCORE_ActivationKind kind, GRCORE_EngineId engine,
    bool nested, const GRCORE_CSegment * segment,
    GRCORE_ActivationRef * out_ref) {
  GRCORE_Context * context = stack->context;
  /* Everything that can be refused comes first, and the native depth, which is
   * given back if a later step fails, last of the refusable ones. */
  GRCORE_Result r;
  if (stack->activation_count >= stack->activation_capacity) {
    void * grown;
    r = grcore_guest_array_reserve(context, stack->activations,
        &stack->activation_capacity, stack->activation_count,
        sizeof *stack->activations, &grown);
    if (r != GRCORE_OK) {
      return r;
    }
    stack->activations = grown;
  }
  /* The same step grcore_context_enter_depth takes, without its argument
   * checks: the context is the owner's and the kind is a constant. */
  if (enters_native_depth(kind)) {
    r = grcore_context_depth_step_in(context, GRCORE_DEPTH_NATIVE);
    if (r != GRCORE_OK) {
      return r;
    }
  }
  if (nested) {
    r = grcore_context_nested_enter(context);
    if (r != GRCORE_OK) {
      if (enters_native_depth(kind)) {
        (void)grcore_context_depth_step_out(context, GRCORE_DEPTH_NATIVE);
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
  *out_ref = push_record(stack, kind, engine, nested, segment, set_limit);
  return GRCORE_OK;
}

GRCORE_Result grcore_activation_enter(GRCORE_Stack * stack,
    GRCORE_ActivationKind kind, GRCORE_EngineId engine, bool nested,
    const GRCORE_CSegment * segment, GRCORE_ActivationRef * out_ref) {
  if (!stack_owned(stack) || out_ref == NULL ||
      (unsigned)kind > GRCORE_ACTIVATION_REENTRY ||
      engine > stack->engine_count ||
      (segment != NULL && segment->lo >= segment->hi)) {
    return GRCORE_ERR_INVALID;
  }
  /* The common entry, which every call of compiled code from an interpreter
   * makes: not nested, so no nesting count and no native-stack limit (a
   * re-entry is nested by definition, AD-5), into an array with room. Its one
   * refusable step is the native depth, as it is the general entry's last. */
  nested = nested || kind == GRCORE_ACTIVATION_REENTRY;
  if (nested || stack->activation_count >= stack->activation_capacity) {
    return enter_general(stack, kind, engine, nested, segment, out_ref);
  }
  if (enters_native_depth(kind)) {
    GRCORE_Result r =
        grcore_context_depth_step_in(stack->context, GRCORE_DEPTH_NATIVE);
    if (r != GRCORE_OK) {
      return r;
    }
  }
  *out_ref = push_record(stack, kind, engine, false, segment, false);
  return GRCORE_OK;
}

/* What a record's leave undoes beside the native depth and the walk cell: the
 * nesting count, the native-stack limit and the boundary counts. Out of line:
 * none of it is a JIT record's from the interpreter. */
static void __attribute__((noinline)) drop_rare(
    GRCORE_Stack * stack, const GRCORE_ActivationRecord * rec) {
  GRCORE_Context * context = stack->context;
  if (rec->set_limit) {
    /* The limit names the stack this re-entry measured from, which nobody is
     * running on once it is left (a later re-entry may be on another thread). */
    context->native_limit = 0;
  }
  if (rec->nested) {
    /* It refuses only a non-owner or a count of zero. Every caller checked
     * ownership, and this record counted one when it was entered, so a
     * refusal here would be a defect of the count, not a case to handle. The
     * old code ignored it too. */
    (void)grcore_context_nested_leave(context);
  }
  if (rec->boundary_resumable && context->resumable_boundaries) {
    context->resumable_boundaries--;
  }
  if (rec->boundary_upgraded_native) {
    for (size_t i = stack->activation_count; i > 0; i--) {
      GRCORE_ActivationRecord * native = &stack->activations[i - 1];
      if (native->kind == GRCORE_ACTIVATION_NATIVE && native->nested &&
          native->boundary_resumable) {
        native->boundary_resumable = false;
        if (context->resumable_boundaries) {
          context->resumable_boundaries--;
        }
        break;
      }
    }
  }
}

/* Drops the top record of a stack that has one: grcore_activation_drop_top
 * after its checks, and grcore_activation_leave after its own, which the
 * compiler inlines into both. */
static inline void drop_record(GRCORE_Stack * stack) {
  GRCORE_Context * context = stack->context;
  /* Read in place: nothing below writes the array before the release of
   * retired code, the last step, which is the only one that runs callbacks. */
  const GRCORE_ActivationRecord * rec =
      &stack->activations[--stack->activation_count];
  const GRCORE_ActivationKind kind = rec->kind;
  if (kind == GRCORE_ACTIVATION_JIT) {
    /* Its compiled frames are gone, and so is anything compiled code recorded
     * for them: a later walk must not read frames that were left. */
    context->walk_cell[0] = 0;
    context->walk_cell[1] = 0;
  }
  if (enters_native_depth(kind)) {
    (void)grcore_context_depth_step_out(context, GRCORE_DEPTH_NATIVE);
  }
  if (rec->set_limit || rec->nested || rec->boundary_resumable ||
      rec->boundary_upgraded_native) {
    drop_rare(stack, rec);
  }
  if (kind == GRCORE_ACTIVATION_JIT && --stack->jit_live == 0 &&
      grcore_registry_holds_retired(stack->registry)) {
    /* No compiled frame can be on the native stack now, so nothing can return
     * into retired code (AD-28). Nothing retired is the common case, and the
     * test is the release's own, so the call is made only to release. */
    grcore_registry_release_retired(stack);
  }
}

bool grcore_activation_drop_top(GRCORE_Stack * stack) {
  if (stack == NULL || stack->activation_count == 0) {
    return false;
  }
  drop_record(stack);
  return true;
}

GRCORE_Result grcore_activation_leave(
    GRCORE_Stack * stack, GRCORE_ActivationRef ref) {
  if (!stack_owned(stack) || ref.id == 0 ||
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
  drop_record(stack);
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

GRCORE_Result grcore_boundary_enter(GRCORE_Stack *stack,
    GRCORE_EngineId caller,GRCORE_EngineId callee,uintptr_t instance_token,
    bool resumable,GRCORE_BoundaryRef*out_ref){
  if(!grcore_stack_owned(stack)||!out_ref||!instance_token||!caller||!callee||
      caller>stack->engine_count||callee>stack->engine_count||
      (resumable&&stack->context->resumable_boundaries==UINT64_MAX))
    return GRCORE_ERR_INVALID;
  bool upgraded_native=false;
  if(resumable&&stack->activation_count){
    GRCORE_ActivationRecord*top=&stack->activations[stack->activation_count-1];
    if(top->kind==GRCORE_ACTIVATION_NATIVE&&top->nested&&
        !top->boundary_resumable){
      top->boundary_resumable=true;
      stack->context->resumable_boundaries++;
      upgraded_native=true;
    }
  }
  GRCORE_ActivationRef activation={0};
  GRCORE_Result result=grcore_activation_enter(stack,GRCORE_ACTIVATION_REENTRY,
      callee,true,NULL,&activation);
  if(result!=GRCORE_OK){if(upgraded_native){
    stack->activations[stack->activation_count-1].boundary_resumable=false;
    stack->context->resumable_boundaries--;}
    return result;}
  GRCORE_ActivationRecord*record=&stack->activations[stack->activation_count-1];
  record->boundary=true;record->boundary_resumable=resumable;
  record->boundary_upgraded_native=upgraded_native;
  record->boundary_caller=caller;record->boundary_instance=instance_token;
  /* The entry leaves the members only a boundary has as the slot held them. */
  record->foreign_exception_engine=0;record->foreign_exception_identity=0;
  if(resumable)stack->context->resumable_boundaries++;
  out_ref->id=activation.id;return GRCORE_OK;
}

static GRCORE_ActivationRecord *boundary_record(GRCORE_Stack*stack,
    GRCORE_BoundaryRef ref){
  if(!stack||!ref.id)return NULL;
  for(size_t i=stack->activation_count;i>0;i--){
    GRCORE_ActivationRecord*record=&stack->activations[i-1];
    if(record->id==ref.id)return record->boundary?record:NULL;
  }
  return NULL;
}

GRCORE_Result grcore_boundary_leave(GRCORE_Stack*stack,GRCORE_BoundaryRef ref){
  if(!grcore_stack_owned(stack))return GRCORE_ERR_INVALID;
  GRCORE_ActivationRecord*record=boundary_record(stack,ref);
  if(!record)return GRCORE_ERR_INVALID;
  GRCORE_ActivationRef activation={ref.id};
  return grcore_activation_leave(stack,activation);
}

GRCORE_Result grcore_boundary_active_instance(const GRCORE_Stack*stack,
    GRCORE_EngineId engine,uintptr_t*out_instance_token){
  if(!stack||!out_instance_token||engine==0||engine>stack->engine_count)
    return GRCORE_ERR_INVALID;
  for(size_t i=stack->activation_count;i>0;i--){
    const GRCORE_ActivationRecord*record=&stack->activations[i-1];
    if(record->boundary&&record->engine==engine){
      *out_instance_token=record->boundary_instance;return GRCORE_OK;
    }
  }
  return GRCORE_ERR_INVALID;
}

GRCORE_Result grcore_boundary_set_foreign_exception(GRCORE_Stack*stack,
    GRCORE_BoundaryRef ref,GRCORE_EngineId engine,uint64_t identity){
  if(!grcore_stack_owned(stack)||engine==0||engine>stack->engine_count||!identity)
    return GRCORE_ERR_INVALID;
  GRCORE_ActivationRecord*record=boundary_record(stack,ref);
  if(!record)return GRCORE_ERR_INVALID;
  record->foreign_exception_engine=engine;
  record->foreign_exception_identity=identity;return GRCORE_OK;
}

GRCORE_Result grcore_boundary_get_foreign_exception(const GRCORE_Stack*stack,
    GRCORE_BoundaryRef ref,GRCORE_EngineId*out_engine,uint64_t*out_identity){
  if(!stack||!out_engine||!out_identity||!ref.id)return GRCORE_ERR_INVALID;
  for(size_t i=stack->activation_count;i>0;i--){
    const GRCORE_ActivationRecord*record=&stack->activations[i-1];
    if(record->id==ref.id){
      if(!record->boundary||!record->foreign_exception_identity)return GRCORE_ERR_INVALID;
      *out_engine=record->foreign_exception_engine;
      *out_identity=record->foreign_exception_identity;return GRCORE_OK;
    }
  }
  return GRCORE_ERR_INVALID;
}
