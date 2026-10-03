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

#include <ghoti.io/runtime-core/a/frame.h>

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
  return GRCORE_OK;
}

bool grcore_frame_walk_next(
    GRCORE_FrameWalk * walk, GRCORE_AbstractFrame * out_frame) {
  if (walk == NULL || out_frame == NULL || walk->next.offset == 0 ||
      !grcore_context_guest_state_readable(walk->context)) {
    return false;
  }
  const GRCORE_Stack * stack = grcore_context_stack(walk->context);
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
  *out_frame = f;
  walk->next.offset = (size_t)h.prev;
  walk->depth++;
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

GRCORE_Result grcore_frame_slot(const GRCORE_AbstractFrame * frame,
    size_t index, GRCORE_SlotKind * out_kind, uint64_t * out_value) {
  const GRCORE_Stack * stack = readable_stack(frame);
  uint64_t value;
  if (stack == NULL || index >= frame->slot_count ||
      grcore_stack_slot_get(stack, frame->frame, index, &value) != GRCORE_OK) {
    return GRCORE_ERR_INVALID;
  }
  if (out_kind != NULL) {
    *out_kind = frame->descriptor->slot_kind != NULL
        ? frame->descriptor->slot_kind(frame, index)
        : GRCORE_SLOT_RAW;
  }
  if (out_value != NULL) {
    *out_value = value;
  }
  return GRCORE_OK;
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
  if (readable_stack(frame) == NULL ||
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
  GRCORE_ScopeInfo info;
  GRCORE_Result r = frame->descriptor->scopes.scope(frame, index, &info);
  if (r == GRCORE_OK) {
    *out_scope = info;
  }
  return r;
}

GRCORE_Result grcore_frame_variable(const GRCORE_AbstractFrame * frame,
    size_t scope, size_t index, GRCORE_Variable * out_variable) {
  GRCORE_ScopeInfo info;
  if (out_variable == NULL ||
      grcore_frame_scope(frame, scope, &info) != GRCORE_OK ||
      index >= info.variable_count ||
      frame->descriptor->scopes.variable == NULL) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Variable variable;
  GRCORE_Result r =
      frame->descriptor->scopes.variable(frame, scope, index, &variable);
  if (r == GRCORE_OK) {
    *out_variable = variable;
  }
  return r;
}
