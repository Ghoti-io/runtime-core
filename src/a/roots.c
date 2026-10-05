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

/* Every VALUE slot of every frame, innermost frame first, as an address the
 * visitor may write through; the engine's `roots` hook after each frame's
 * slots; then one conservative range per activation that recorded a C
 * segment, with the decoder of the engine the activation names. Engine zero
 * has no decoder of its own, so its words are read as the addresses they are.
 * Nothing here pushes or polls, so the buffer does not move. */
static void guest_enumerate(
    GRCORE_Context * context, void * value, const GRCORE_RootVisitor * visitor) {
  GRCORE_Stack * stack = value;
  size_t depth = 0;
  GRCORE_FrameRef ref = {stack->top};
  while (ref.offset != 0) {
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
    visitor->range(visitor->user, &range);
  }
}

const GRCORE_RootSource grcore_guest_root_source =
    GRCORE_ROOT_SOURCE_INIT("runtime-core.guest", guest_enumerate);
