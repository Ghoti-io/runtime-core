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
 * The abstract frame and the frame walk (AD-18, AD-20).
 */

#include <ghoti.io/runtime-core/macros.h>

#include "guest_internal.h"

#include <ghoti.io/runtime-core/a/deopt.h>
#include <ghoti.io/runtime-core/a/frame.h>

#include <string.h>

/* Looks one compiled frame ahead. A broken chain is recorded, not skipped. */
static void pull_compiled(GRCORE_FrameWalk * walk) {
  switch (grcore_compiled_walk_next(&walk->compiled, &walk->pending)) {
  case GRCORE_CWALK_FRAME:
    walk->has_pending = true;
    break;
  case GRCORE_CWALK_BROKEN:
    walk->broken = true;
    walk->has_pending = false;
    break;
  default:
    walk->has_pending = false;
    break;
  }
}

GRCORE_Result grcore_frame_walk_begin(
    const GRCORE_Context * context, GRCORE_FrameWalk * out_walk) {
  if (context == NULL || out_walk == NULL ||
      !grcore_context_guest_state_readable(context)) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_Stack * stack = grcore_context_stack(context);
  out_walk->context = context;
  out_walk->next = grcore_stack_top(stack);
  out_walk->depth = 0;
  out_walk->remaining = grcore_stack_frame_count(stack);
  out_walk->has_pending = false;
  out_walk->broken = false;
  memset(&out_walk->pending, 0, sizeof out_walk->pending);
  grcore_compiled_walk_begin(context, &out_walk->compiled);
  pull_compiled(out_walk);
  return GRCORE_OK;
}

/* A compiled frame, as an abstract frame (AD-28). It is placed before the guest
 * frames that were on the stack when its record was entered. */
static void compiled_abstract_frame(GRCORE_FrameWalk * walk,
    const GRCORE_Stack * stack, GRCORE_AbstractFrame * out) {
  const GRCORE_CompiledFrame * cf = &walk->pending;
  const GRCORE_EngineDescriptor * d = stack->engines[cf->engine - 1];
  memset(out, 0, sizeof *out);
  out->context = walk->context;
  out->engine = cf->engine;
  out->descriptor = d;
  out->identity = cf->identity;
  if (d->locate != NULL) {
    out->location = d->locate(walk->context, cf->identity.function,
        cf->identity.offset);
  }
  out->slot_count = cf->site->frame_state_count;
  out->depth = walk->depth;
  out->native_base = cf->frame_base;
  out->site = cf->site;
}

bool grcore_frame_walk_broken(
    const GRCORE_FrameWalk * walk, const char ** out_reason) {
  if (walk == NULL || !walk->broken) {
    return false;
  }
  if (out_reason != NULL) {
    *out_reason = grcore_compiled_walk_reason(&walk->compiled);
  }
  return true;
}

bool grcore_frame_walk_next(
    GRCORE_FrameWalk * walk, GRCORE_AbstractFrame * out_frame) {
  if (walk == NULL || out_frame == NULL || walk->broken ||
      !grcore_context_guest_state_readable(walk->context)) {
    return false;
  }
  const GRCORE_Stack * stack = grcore_context_stack(walk->context);
  if (walk->has_pending &&
      (walk->next.offset == 0 || walk->pending.base_frames >= walk->remaining)) {
    compiled_abstract_frame(walk, stack, out_frame);
    walk->depth++;
    pull_compiled(walk);
    return true;
  }
  if (walk->next.offset == 0) {
    return false;
  }
  GRCORE_FrameHeader h;
  if (!grcore_stack_read_frame(stack, walk->next, &h)) {
    return false;
  }
  const GRCORE_EngineDescriptor * d = stack->engines[h.engine - 1];
  GRCORE_AbstractFrame f;
  f.context = walk->context;
  f.engine = h.engine;
  f.descriptor = d;
  f.identity.function = h.function;
  f.identity.offset = h.offset;
  f.location.file = NULL;
  f.location.line = 0;
  if (d->locate != NULL) {
    f.location = d->locate(walk->context, h.function, h.offset);
  }
  f.slot_count = h.slot_count;
  f.depth = walk->depth;
  f.frame = walk->next;
  f.native_base = 0;
  f.site = NULL;
  *out_frame = f;
  walk->next.offset = (size_t)h.prev;
  walk->depth++;
  if (walk->remaining > 0) {
    walk->remaining--;
  }
  return true;
}

bool grcore_stack_hook_frame(const GRCORE_Stack * stack, GRCORE_FrameRef ref,
    size_t depth, GRCORE_AbstractFrame * out, GRCORE_FrameHeader * header) {
  GRCORE_FrameHeader h;
  if (!grcore_stack_read_frame(stack, ref, &h)) {
    return false;
  }
  out->context = stack->context;
  out->engine = h.engine;
  out->descriptor = stack->engines[h.engine - 1];
  out->identity.function = h.function;
  out->identity.offset = h.offset;
  out->location.file = NULL;
  out->location.line = 0;
  out->slot_count = h.slot_count;
  out->depth = depth;
  out->frame = ref;
  out->native_base = 0;
  out->site = NULL;
  *header = h;
  return true;
}

/* The stack a frame's slots are read from, when the frame may be read now. */
static const GRCORE_Stack * readable_stack(const GRCORE_AbstractFrame * frame) {
  if (frame == NULL || frame->context == NULL || frame->descriptor == NULL ||
      !grcore_context_guest_state_readable(frame->context)) {
    return NULL;
  }
  return grcore_context_stack(frame->context);
}

GRCORE_Result grcore_frame_slot_tagged(const GRCORE_AbstractFrame * frame,
    size_t index, GRCORE_SlotKind * out_kind, uint64_t * out_value,
    GRCORE_Representation * out_representation) {
  const GRCORE_Stack * stack = readable_stack(frame);
  if (stack == NULL || index >= frame->slot_count) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_SlotKind kind;
  uint64_t value;
  GRCORE_Representation representation = GRCORE_REPR_BITS;
  if (frame->native_base != 0) {
    /* A compiled frame: the word as the native frame has it, by its site. */
    if (frame->site == NULL) {
      return GRCORE_ERR_INVALID;
    }
    GRCORE_CodeSite one = *frame->site;
    one.frame_state = &frame->site->frame_state[index];
    one.frame_state_count = 1;
    GRCORE_DeoptSlot slot;
    if (grcore_deopt_read_tagged(&one, (const void *)frame->native_base, &slot, 1) !=
        GRCORE_OK) {
      return GRCORE_ERR_INVALID;
    }
    value = slot.word;
    representation = slot.representation;
    const GRCORE_CodeLocation * l = &one.frame_state[0];
    kind = l->kind == GRCORE_LOC_DEAD ? GRCORE_SLOT_RAW : l->slot_kind;
  } else {
    if (grcore_stack_slot_get(stack, frame->frame, index, &value) != GRCORE_OK) {
      return GRCORE_ERR_INVALID;
    }
    kind = frame->descriptor->slot_kind != NULL
        ? frame->descriptor->slot_kind(frame, index)
        : GRCORE_SLOT_RAW;
  }
  if (out_kind != NULL) {
    *out_kind = kind;
  }
  if (out_value != NULL) {
    *out_value = value;
  }
  if (out_representation != NULL) {
    *out_representation = representation;
  }
  return GRCORE_OK;
}

GRCORE_Result grcore_frame_slot(const GRCORE_AbstractFrame * frame,
    size_t index, GRCORE_SlotKind * out_kind, uint64_t * out_value) {
  return grcore_frame_slot_tagged(frame, index, out_kind, out_value, NULL);
}

GRCORE_Result grcore_frame_inspect(const GRCORE_AbstractFrame * frame,
    size_t index, char * buffer, size_t size, size_t * out_length) {
  GRCORE_SlotKind kind;
  uint64_t value;
  GRCORE_Result r = grcore_frame_slot(frame, index, &kind, &value);
  if (r != GRCORE_OK) {
    return r;
  }
  return grcore_engine_inspect(
      frame->context, frame->engine, kind, value, buffer, size, out_length);
}

size_t grcore_frame_scope_count(const GRCORE_AbstractFrame * frame) {
  if (readable_stack(frame) == NULL || frame->native_base != 0 ||
      frame->descriptor->scopes.scope_count == NULL) {
    return 0;
  }
  return frame->descriptor->scopes.scope_count(frame);
}

GRCORE_Result grcore_frame_scope(const GRCORE_AbstractFrame * frame,
    size_t index, GRCORE_ScopeInfo * out_scope) {
  if (out_scope == NULL || index >= grcore_frame_scope_count(frame) ||
      frame->descriptor->scopes.scope == NULL) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_ScopeInfo info = {GRCORE_SCOPE_LOCAL, NULL, 0};
  GRCORE_Result r = frame->descriptor->scopes.scope(frame, index, &info);
  if (r == GRCORE_OK) {
    *out_scope = info;
  }
  return r;
}

GRCORE_Result grcore_frame_variable(const GRCORE_AbstractFrame * frame,
    size_t scope, size_t index, GRCORE_Variable * out_variable) {
  GRCORE_ScopeInfo info = {GRCORE_SCOPE_LOCAL, NULL, 0};
  if (out_variable == NULL ||
      grcore_frame_scope(frame, scope, &info) != GRCORE_OK ||
      index >= info.variable_count ||
      frame->descriptor->scopes.variable == NULL) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Variable variable = {NULL, GRCORE_SLOT_RAW, 0};
  GRCORE_Result r =
      frame->descriptor->scopes.variable(frame, scope, index, &variable);
  if (r == GRCORE_OK) {
    *out_variable = variable;
  }
  return r;
}
