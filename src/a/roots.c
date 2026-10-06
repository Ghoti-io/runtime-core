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
 * A's root source over the guest stack and the activation records (AD-17,
 * AD-18). It is registered with B by the first engine registration, and a
 * collector reaches it only through B's root types.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "guest_internal.h"

#include <stdio.h>
#include <stdlib.h>

/* A chain of compiled frames that cannot be walked has no safe answer here:
 * root enumeration returns no result, and a frame left out is a root missed,
 * which a collector turns into a live object freed (AD-17, AD-28). So it stops
 * the process, saying why. */
static void abort_broken_chain(const char * reason) {
  fprintf(stderr,
      "runtime-core: the chain of compiled frames is broken (%s); a skipped "
      "frame would be a missed root, so root enumeration stops\n",
      reason != NULL ? reason : "unknown");
  abort();
}

/* Steps the walk, aborting on a broken chain. True with a frame. */
static bool next_compiled(GRCORE_CompiledWalk * walk, GRCORE_CompiledFrame * out) {
  GRCORE_CompiledWalkStatus st = grcore_compiled_walk_next(walk, out);
  if (st == GRCORE_CWALK_BROKEN) {
    abort_broken_chain(grcore_compiled_walk_reason(walk));
  }
  return st == GRCORE_CWALK_FRAME;
}

/* The words of a compiled frame the site's stack map names, as addresses the
 * visitor may write through. Nothing else in the frame is reported: a raw word
 * that looks like a reference is not one (AD-27). */
static void visit_compiled(
    const GRCORE_CompiledFrame * cf, const GRCORE_RootVisitor * visitor) {
  if (visitor->slot == NULL) {
    return;
  }
  for (size_t i = 0; i < cf->site->live_count; i++) {
    const GRCORE_CodeLocation * loc = &cf->site->live[i];
    if (loc->kind != GRCORE_LOC_FRAME_SLOT) {
      continue;
    }
    uint64_t * slot =
        (uint64_t *)(void *)(cf->frame_base + (uintptr_t)(int64_t)loc->value);
    visitor->slot(visitor->user, slot);
  }
}

static bool any_compiled_state(const GRCORE_Stack * stack) {
  for (size_t i = 0; i < stack->activation_count; i++) {
    if (stack->activations[i].frame_base != 0) {
      return true;
    }
  }
  return false;
}

/* Reports `whole` as ranges that leave out every compiled frame: a compiled
 * frame is read by its stack map, and a conservative scan of it would report
 * the raw words that look like references. A frame's extent is from its base
 * less the code's frame size up through its two saved words. The walk yields
 * frames from the lowest address up, so one pass over them cuts the range. */
static void visit_range(GRCORE_Context * context, const GRCORE_Stack * stack,
    const GRCORE_ConservativeRange * whole, const GRCORE_RootVisitor * visitor) {
  if (!any_compiled_state(stack)) {
    visitor->range(visitor->user, whole);
    return;
  }
  GRCORE_ConservativeRange piece = *whole;
  uint64_t cur = whole->lo;
  GRCORE_CompiledWalk walk;
  GRCORE_CompiledFrame cf;
  grcore_compiled_walk_begin(context, &walk);
  while (next_compiled(&walk, &cf)) {
    uint64_t lo = cf.frame_base >= cf.meta->frame_bytes
        ? cf.frame_base - cf.meta->frame_bytes
        : 0;
    uint64_t hi = cf.frame_base + 2u * sizeof(uintptr_t);
    if (hi <= cur || lo >= whole->hi) {
      continue;
    }
    if (lo > cur) {
      piece.lo = cur;
      piece.hi = lo;
      visitor->range(visitor->user, &piece);
    }
    cur = hi;
  }
  if (cur < whole->hi) {
    piece.lo = cur;
    piece.hi = whole->hi;
    visitor->range(visitor->user, &piece);
  }
}

/* Every VALUE slot of every frame, innermost frame first, as an address the
 * visitor may write through; the engine's `roots` hook after each frame's
 * slots; then one conservative range per activation that recorded a C
 * segment, with the decoder of the engine the activation names. Engine zero
 * has no decoder of its own, so its words are read as the addresses they are.
 *
 * Compiled frames (AD-28) are reported precisely, by their stack maps, in
 * place among the guest frames: a compiled frame of a record comes before the
 * guest frames that were on the stack when the record was entered. A range
 * leaves out the compiled frames it contains. A broken chain aborts.
 * Nothing here pushes or polls, so the buffer does not move. */
static void guest_enumerate(
    GRCORE_Context * context, void * value, const GRCORE_RootVisitor * visitor) {
  GRCORE_Stack * stack = value;
  size_t depth = 0;
  size_t remaining = stack->frame_count;
  GRCORE_FrameRef ref = {stack->top};
  GRCORE_CompiledWalk walk;
  GRCORE_CompiledFrame cf;
  grcore_compiled_walk_begin(context, &walk);
  bool have = next_compiled(&walk, &cf);
  while (true) {
    while (have && cf.base_frames >= remaining) {
      visit_compiled(&cf, visitor);
      have = next_compiled(&walk, &cf);
    }
    if (ref.offset == 0) {
      break;
    }
    GRCORE_AbstractFrame frame;
    GRCORE_FrameHeader h;
    if (!grcore_stack_hook_frame(stack, ref, depth, &frame, &h)) {
      break;
    }
    const GRCORE_EngineDescriptor * d = frame.descriptor;
    if (visitor->slot != NULL && d->slot_kind != NULL) {
      for (size_t i = 0; i < h.slot_count; i++) {
        if (d->slot_kind(&frame, i) == GRCORE_SLOT_VALUE) {
          uint64_t * slot = (uint64_t *)(void *)(stack->buffer + ref.offset +
              GRCORE_FRAME_HEADER + 8u * i);
          visitor->slot(visitor->user, slot);
        }
      }
    }
    if (grcore_engine_roots(d) != NULL) {
      grcore_engine_roots(d)(context, &frame, visitor);
    }
    ref.offset = (size_t)h.prev;
    depth++;
    if (remaining > 0) {
      remaining--;
    }
  }
  while (have) {
    visit_compiled(&cf, visitor);
    have = next_compiled(&walk, &cf);
  }
  if (visitor->range == NULL) {
    return;
  }
  for (size_t i = stack->activation_count; i > 0; i--) {
    const GRCORE_ActivationRecord * rec = &stack->activations[i - 1];
    if (rec->hi <= rec->lo) {
      continue;
    }
    GRCORE_ConservativeRange range;
    range.lo = rec->lo;
    range.hi = rec->hi;
    if (rec->engine == 0) {
      range.mask = UINT64_MAX;
      range.shift = 0;
      range.base = 0;
    } else {
      const GRCORE_ConservativeDecoder * dec =
          &stack->engines[rec->engine - 1]->decoder;
      range.mask = dec->mask;
      range.shift = dec->shift;
      range.base = dec->base;
    }
    visit_range(context, stack, &range, visitor);
  }
}

const GRCORE_RootSource grcore_guest_root_source =
    GRCORE_ROOT_SOURCE_INIT("runtime-core.guest", guest_enumerate);
