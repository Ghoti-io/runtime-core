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
 * The guest stack (AD-8, AD-17, AD-18).
 *
 * A byte buffer of frames, each a 40-byte header and then 64-bit slots. The
 * first eight bytes are never a frame, so an offset of zero can mean "none".
 * Growth allocates a new buffer, copies and frees the old one; `realloc` is
 * avoided on purpose, because it may grow in place and then a test could not
 * tell a stack that tolerates a move from one that merely was not moved.
 *
 * The buffer has no declared type, so a header is only ever copied in and out
 * with `memcpy`. Slots are read through `uint64_t` pointers into it, which the
 * interface says are dead after a push.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "guest_internal.h"

#include "../b/context_internal.h"

#include <ghoti.io/runtime-core/b/budget.h>

#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(GRCORE_FrameHeader) == GRCORE_FRAME_HEADER,
    "the frame header is 40 bytes");

/* The tag a frame at `offset` carries. It is not a defence against a hostile
 * caller; it makes a reference that was never a frame (an offset into the
 * middle of a slot, a number someone typed) fail with high probability. */
static uint32_t tag_for(uint64_t offset) {
  return (uint32_t)((offset * UINT64_C(0x9E3779B97F4A7C15)) >> 32) ^
      UINT32_C(0xA5C3E1F7);
}

static bool owner_of(const GRCORE_Stack * stack) {
  return stack != NULL && grcore_context_is_owner(stack->context);
}

bool grcore_stack_owned(const GRCORE_Stack * stack) {
  return owner_of(stack);
}

GRCORE_Result grcore_guest_array_reserve(GRCORE_Context * context, void * array,
    size_t * capacity, size_t count, size_t element_size, void ** out_array) {
  if (count < *capacity) {
    *out_array = array;
    return GRCORE_OK;
  }
  size_t grown_capacity = *capacity == 0 ? 8 : *capacity * 2;
  if (grown_capacity > SIZE_MAX / element_size) {
    return GRCORE_ERR_OOM;
  }
  const GRCORE_Allocator * a = grcore_context_allocator(context);
  uint64_t refusals = grcore_context_memory_refusals(context);
  void * grown = a->malloc_fn(a->ctx, grown_capacity * element_size);
  if (grown == NULL) {
    return grcore_guest_alloc_failure(context, refusals);
  }
  if (count > 0) {
    memcpy(grown, array, count * element_size);
  }
  a->free_fn(a->ctx, array);
  *out_array = grown;
  *capacity = grown_capacity;
  return GRCORE_OK;
}

bool grcore_stack_read_frame(const GRCORE_Stack * stack, GRCORE_FrameRef frame,
    GRCORE_FrameHeader * out) {
  if (stack == NULL || stack->buffer == NULL) {
    return false;
  }
  size_t offset = frame.offset;
  if (offset < GRCORE_STACK_BASE || offset % 8 != 0 ||
      offset > stack->used || stack->used - offset < GRCORE_FRAME_HEADER) {
    return false;
  }
  GRCORE_FrameHeader h;
  memcpy(&h, stack->buffer + offset, sizeof h);
  if (h.tag != tag_for(offset) || h.slot_count > GRCORE_FRAME_MAX_SLOTS ||
      h.size != GRCORE_FRAME_HEADER + 8u * h.slot_count ||
      stack->used - offset < h.size || h.engine == 0 ||
      h.engine > stack->engine_count) {
    return false;
  }
  *out = h;
  return true;
}

/* Makes the buffer at least `need` bytes, and moves it when `force_move`.
 * A new buffer is allocated before the old one is freed, so a move always
 * changes the address. Nothing changes on failure. */
static GRCORE_Result stack_grow(
    GRCORE_Stack * s, size_t need, bool force_move) {
  if (s->buffer != NULL && need <= s->capacity && !force_move) {
    return GRCORE_OK;
  }
  size_t capacity = s->capacity;
  if (capacity == 0) {
    capacity = GRCORE_STACK_INITIAL_BYTES;
  } else if (need > capacity) {
    if (capacity > SIZE_MAX / 2) {
      return GRCORE_ERR_OOM;
    }
    capacity *= 2;
  }
  while (capacity < need) {
    if (capacity > SIZE_MAX / 2) {
      return GRCORE_ERR_OOM;
    }
    capacity *= 2;
  }
  const GRCORE_Allocator * a = grcore_context_allocator(s->context);
  uint64_t refusals = grcore_context_memory_refusals(s->context);
  unsigned char * grown = a->calloc_fn(a->ctx, 1, capacity);
  if (grown == NULL) {
    return grcore_guest_alloc_failure(s->context, refusals);
  }
  if (s->buffer != NULL) {
    memcpy(grown, s->buffer, s->used);
    a->free_fn(a->ctx, s->buffer);
    s->move_count++;
  }
  s->buffer = grown;
  s->capacity = capacity;
  if (s->used < GRCORE_STACK_BASE) {
    s->used = GRCORE_STACK_BASE;
  }
  return GRCORE_OK;
}

GRCORE_Result grcore_stack_push(GRCORE_Stack * stack, GRCORE_EngineId engine,
    size_t slot_count, GRCORE_FrameRef * out_frame) {
  if (!owner_of(stack) || out_frame == NULL || engine == 0 ||
      engine > stack->engine_count || slot_count > GRCORE_FRAME_MAX_SLOTS) {
    return GRCORE_ERR_INVALID;
  }
  size_t size = GRCORE_FRAME_HEADER + 8u * slot_count;
  GRCORE_Result r = grcore_context_enter_depth(stack->context, GRCORE_DEPTH_GUEST);
  if (r != GRCORE_OK) {
    return r;
  }
  if (size > SIZE_MAX - stack->used) {
    grcore_context_leave_depth(stack->context, GRCORE_DEPTH_GUEST);
    return GRCORE_ERR_OOM;
  }
  r = stack_grow(stack, stack->used + size, stack->always_move);
  if (r != GRCORE_OK) {
    grcore_context_leave_depth(stack->context, GRCORE_DEPTH_GUEST);
    return r;
  }
  size_t offset = stack->used;
  GRCORE_FrameHeader h = {(uint64_t)stack->top, 0, 0, (uint32_t)size,
      (uint32_t)engine, (uint32_t)slot_count, tag_for(offset)};
  memcpy(stack->buffer + offset, &h, sizeof h);
  memset(stack->buffer + offset + sizeof h, 0, 8u * slot_count);
  stack->used = offset + size;
  stack->top = offset;
  stack->frame_count++;
  out_frame->offset = offset;
  return GRCORE_OK;
}

GRCORE_Result grcore_stack_pop(GRCORE_Stack * stack) {
  GRCORE_FrameHeader h;
  if (!owner_of(stack) || stack->top == 0 ||
      !grcore_stack_read_frame(stack, (GRCORE_FrameRef){stack->top}, &h)) {
    return GRCORE_ERR_INVALID;
  }
  stack->used = stack->top;
  stack->top = (size_t)h.prev;
  stack->frame_count--;
  grcore_context_leave_depth(stack->context, GRCORE_DEPTH_GUEST);
  return GRCORE_OK;
}

GRCORE_FrameRef grcore_stack_top(const GRCORE_Stack * stack) {
  GRCORE_FrameRef none = {0};
  if (stack == NULL) {
    return none;
  }
  GRCORE_FrameRef top = {stack->top};
  return top;
}

GRCORE_FrameRef grcore_stack_caller(
    const GRCORE_Stack * stack, GRCORE_FrameRef frame) {
  GRCORE_FrameRef none = {0};
  GRCORE_FrameHeader h;
  if (!grcore_stack_read_frame(stack, frame, &h)) {
    return none;
  }
  GRCORE_FrameRef caller = {(size_t)h.prev};
  return caller;
}

bool grcore_stack_frame_valid(
    const GRCORE_Stack * stack, GRCORE_FrameRef frame) {
  GRCORE_FrameHeader h;
  return grcore_stack_read_frame(stack, frame, &h);
}

size_t grcore_stack_frame_count(const GRCORE_Stack * stack) {
  return stack == NULL ? 0 : stack->frame_count;
}

size_t grcore_stack_bytes_used(const GRCORE_Stack * stack) {
  return stack == NULL || stack->buffer == NULL
      ? 0
      : stack->used - GRCORE_STACK_BASE;
}

size_t grcore_stack_bytes_capacity(const GRCORE_Stack * stack) {
  return stack == NULL ? 0 : stack->capacity;
}

uint64_t grcore_stack_move_count(const GRCORE_Stack * stack) {
  return stack == NULL ? 0 : stack->move_count;
}

GRCORE_Result grcore_stack_reserve(GRCORE_Stack * stack, size_t bytes) {
  if (!owner_of(stack)) {
    return GRCORE_ERR_INVALID;
  }
  size_t base = stack->buffer == NULL ? GRCORE_STACK_BASE : stack->used;
  if (bytes > SIZE_MAX - base) {
    return GRCORE_ERR_OOM;
  }
  return stack_grow(stack, base + bytes, false);
}

GRCORE_Result grcore_stack_set_always_move(GRCORE_Stack * stack, bool enabled) {
  if (!owner_of(stack)) {
    return GRCORE_ERR_INVALID;
  }
  stack->always_move = enabled;
  return GRCORE_OK;
}

GRCORE_Result grcore_stack_slot_count(const GRCORE_Stack * stack,
    GRCORE_FrameRef frame, size_t * out_count) {
  GRCORE_FrameHeader h;
  if (out_count == NULL || !grcore_stack_read_frame(stack, frame, &h)) {
    return GRCORE_ERR_INVALID;
  }
  *out_count = h.slot_count;
  return GRCORE_OK;
}

GRCORE_Result grcore_stack_slot_get(const GRCORE_Stack * stack,
    GRCORE_FrameRef frame, size_t index, uint64_t * out_value) {
  GRCORE_FrameHeader h;
  if (!owner_of(stack) || out_value == NULL ||
      !grcore_stack_read_frame(stack, frame, &h) || index >= h.slot_count) {
    return GRCORE_ERR_INVALID;
  }
  memcpy(out_value, stack->buffer + frame.offset + GRCORE_FRAME_HEADER + 8u * index,
      sizeof *out_value);
  return GRCORE_OK;
}

GRCORE_Result grcore_stack_slot_set(GRCORE_Stack * stack,
    GRCORE_FrameRef frame, size_t index, uint64_t value) {
  GRCORE_FrameHeader h;
  if (!owner_of(stack) || !grcore_stack_read_frame(stack, frame, &h) ||
      index >= h.slot_count) {
    return GRCORE_ERR_INVALID;
  }
  memcpy(stack->buffer + frame.offset + GRCORE_FRAME_HEADER + 8u * index, &value,
      sizeof value);
  return GRCORE_OK;
}

uint64_t * grcore_stack_slots(GRCORE_Stack * stack, GRCORE_FrameRef frame) {
  GRCORE_FrameHeader h;
  if (!owner_of(stack) || !grcore_stack_read_frame(stack, frame, &h) ||
      h.slot_count == 0) {
    return NULL;
  }
  /* The buffer is a calloc'd block and every offset is a multiple of eight
   * from its start, so the address is aligned for a uint64_t. */
  return (uint64_t *)(void *)(stack->buffer + frame.offset + GRCORE_FRAME_HEADER);
}

GRCORE_Result grcore_stack_engine(const GRCORE_Stack * stack,
    GRCORE_FrameRef frame, GRCORE_EngineId * out_engine) {
  GRCORE_FrameHeader h;
  if (out_engine == NULL || !grcore_stack_read_frame(stack, frame, &h)) {
    return GRCORE_ERR_INVALID;
  }
  *out_engine = h.engine;
  return GRCORE_OK;
}

GRCORE_Result grcore_stack_set_identity(
    GRCORE_Stack * stack, GRCORE_FrameRef frame, GRCORE_PollIdentity identity) {
  GRCORE_FrameHeader h;
  if (!owner_of(stack) || !grcore_stack_read_frame(stack, frame, &h)) {
    return GRCORE_ERR_INVALID;
  }
  memcpy(stack->buffer + frame.offset + offsetof(GRCORE_FrameHeader, function),
      &identity.function, sizeof identity.function);
  memcpy(stack->buffer + frame.offset + offsetof(GRCORE_FrameHeader, offset),
      &identity.offset, sizeof identity.offset);
  return GRCORE_OK;
}

GRCORE_Result grcore_stack_identity(const GRCORE_Stack * stack,
    GRCORE_FrameRef frame, GRCORE_PollIdentity * out_identity) {
  GRCORE_FrameHeader h;
  if (out_identity == NULL || !grcore_stack_read_frame(stack, frame, &h)) {
    return GRCORE_ERR_INVALID;
  }
  out_identity->function = h.function;
  out_identity->offset = h.offset;
  return GRCORE_OK;
}

GRCORE_Verdict grcore_stack_poll(
    GRCORE_Context * context, uint64_t function, uint64_t offset) {
  GRCORE_Stack * stack = grcore_context_stack(context);
  if (stack == NULL || stack->top == 0 || !grcore_context_is_owner(context)) {
    return grcore_poll(context, (GRCORE_Location){NULL, 0});
  }
  /* The identity is recorded on every poll, so the frame always says where
   * the engine last polled. */
  GRCORE_FrameRef top = {stack->top};
  /* `top` is a live frame (the stack maintains it), so the words go straight
   * in without the validation the public setter does. */
  memcpy(stack->buffer + stack->top + offsetof(GRCORE_FrameHeader, function),
      &function, sizeof function);
  memcpy(stack->buffer + stack->top + offsetof(GRCORE_FrameHeader, offset),
      &offset, sizeof offset);
  /* The fast path is grcore_poll's own: one load, nothing else, so the
   * descriptor's `locate` is not called unless a request is pending. */
  if (__builtin_expect(
          __atomic_load_n(&context->request_word, __ATOMIC_RELAXED) == 0, 1)) {
    return GRCORE_VERDICT_CONTINUE;
  }
  GRCORE_Location location = {NULL, 0};
  GRCORE_FrameHeader h;
  if (grcore_stack_read_frame(stack, top, &h)) {
    const GRCORE_EngineDescriptor * d = stack->engines[h.engine - 1];
    if (d->locate != NULL) {
      location = d->locate(context, function, offset);
    }
  }
  return grcore_poll(context, location);
}

GRCORE_Result grcore_context_poll_identity(
    const GRCORE_Context * context, GRCORE_PollIdentity * out_identity) {
  if (context == NULL || out_identity == NULL ||
      !grcore_context_guest_state_readable(context)) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_Stack * stack = grcore_context_stack(context);
  if (stack == NULL || stack->top == 0) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_FrameRef top = {stack->top};
  return grcore_stack_identity(stack, top, out_identity);
}
