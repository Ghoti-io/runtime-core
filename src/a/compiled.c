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
 * The precise walk of compiled frames (AD-17, AD-28). See `a/compiled.h` for
 * the layout contract and the checks.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "guest_internal.h"

#include <string.h>

/* A frame is its base word and the return-address word after it. */
#define FRAME_WORDS_BYTES (2u * sizeof(uintptr_t))

GRCORE_Result grcore_compiled_walk_begin(
    const GRCORE_Context * context, GRCORE_CompiledWalk * out_walk) {
  if (context == NULL || out_walk == NULL) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_Stack * stack = grcore_context_stack(context);
  memset(out_walk, 0, sizeof *out_walk);
  out_walk->context = context;
  out_walk->next_record = stack == NULL ? 0 : stack->activation_count;
  return GRCORE_OK;
}

/* Why `base` cannot be the base of a frame whose callee's base is `floor`, or
 * NULL if it can. `floor` is zero for the first frame of the walk. */
static const char * base_fault(const GRCORE_ActivationRecord * rec,
    uintptr_t base, uintptr_t floor) {
  if (base % sizeof(uintptr_t) != 0) {
    return "a frame base is not word-aligned";
  }
  if (base <= floor) {
    return "a frame base is not above the base of the frame it returns to";
  }
  if (rec->hi > rec->lo &&
      (base < rec->lo || rec->hi < FRAME_WORDS_BYTES ||
          base > rec->hi - FRAME_WORDS_BYTES)) {
    return "a frame base is outside the activation record's segment";
  }
  return NULL;
}

GRCORE_CompiledWalkStatus grcore_compiled_walk_next(
    GRCORE_CompiledWalk * walk, GRCORE_CompiledFrame * out_frame) {
  if (walk == NULL || out_frame == NULL) {
    return GRCORE_CWALK_END;
  }
  if (walk->failing) {
    walk->broken = true;
  }
  if (walk->broken) {
    return GRCORE_CWALK_BROKEN;
  }
  const GRCORE_Stack * stack = grcore_context_stack(walk->context);
  if (stack == NULL) {
    return GRCORE_CWALK_END;
  }
  if (walk->base == 0) {
    /* Between runs: the next record outward that carries compiled state. */
    while (walk->next_record > 0 && walk->base == 0) {
      const GRCORE_ActivationRecord * rec =
          &stack->activations[--walk->next_record];
      if (rec->frame_base == 0) {
        continue;
      }
      const char * fault = base_fault(rec, rec->frame_base, walk->last_base);
      if (fault != NULL) {
        walk->broken = true;
        walk->reason = fault;
        return GRCORE_CWALK_BROKEN;
      }
      walk->base = rec->frame_base;
      walk->return_address = rec->return_address;
      walk->run_record = walk->next_record;
    }
    if (walk->base == 0) {
      return GRCORE_CWALK_END;
    }
  }
  const GRCORE_ActivationRecord * rec = &stack->activations[walk->run_record];
  GRCORE_CodeRange range;
  if (!grcore_registry_find(stack, walk->return_address, &range)) {
    walk->broken = true;
    walk->reason = "a compiled frame's return address is in no registered code";
    return GRCORE_CWALK_BROKEN;
  }
  uintptr_t offset = walk->return_address - range.start;
  const GRCORE_CodeSite * site =
      offset > UINT32_MAX ? NULL : grcore_codemeta_find(range.meta, (uint32_t)offset);
  if (site == NULL) {
    walk->broken = true;
    walk->reason = "a compiled frame's return address is at no site of its code";
    return GRCORE_CWALK_BROKEN;
  }
  out_frame->frame_base = walk->base;
  out_frame->return_address = walk->return_address;
  out_frame->engine = range.engine;
  out_frame->meta = range.meta;
  out_frame->site = site;
  out_frame->identity = site->identity;
  out_frame->depth = walk->depth++;
  out_frame->record = walk->run_record;
  out_frame->base_frames = rec->base_frames;
  /* The caller: the two words at this frame's base. A return address in no
   * registered code is the entry from the interpreter, which ends the run. */
  uintptr_t words[2];
  memcpy(words, (const void *)walk->base, sizeof words);
  walk->last_base = walk->base;
  if (!grcore_registry_find(stack, words[1], NULL)) {
    walk->base = 0;
    walk->return_address = 0;
    return GRCORE_CWALK_FRAME;
  }
  const char * fault = base_fault(rec, words[0], walk->base);
  if (fault != NULL) {
    walk->failing = true;
    walk->reason = fault;
    return GRCORE_CWALK_FRAME;
  }
  walk->base = words[0];
  walk->return_address = words[1];
  return GRCORE_CWALK_FRAME;
}

const char * grcore_compiled_walk_reason(const GRCORE_CompiledWalk * walk) {
  return walk != NULL && walk->broken ? walk->reason : NULL;
}
