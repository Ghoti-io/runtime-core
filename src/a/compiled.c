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
  /* What compiled code recorded in the context's cell belongs to the innermost
   * record (a/layout.h). The walk is the owner's, at a poll or in a native, so
   * moving it is not a data race; the context is const here only because
   * reading frames never changes the guest's state. */
  if (stack != NULL && !grcore_activation_absorb_cell((GRCORE_Stack *)stack)) {
    out_walk->failing = true;
    out_walk->reason =
        "compiled code recorded where a walk starts with no JIT activation open";
  }
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

/* Picks the next record outward that carries compiled state and starts a run
 * at it. FRAME means a run was started; END, none is left; BROKEN, its first
 * base is not one. */
static GRCORE_CompiledWalkStatus start_run(
    GRCORE_CompiledWalk * walk, const GRCORE_Stack * stack) {
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
  return walk->base == 0 ? GRCORE_CWALK_END : GRCORE_CWALK_FRAME;
}

/* Yields the frame at `walk->base` and moves to its caller. The one place that
 * reads a frame's words and decides where a run goes on or ends, so the count
 * of a run's frames (made on a copy of the walk) and the walk itself cannot
 * disagree about it. `out` need not be filled in by the caller for a count. */
static GRCORE_CompiledWalkStatus yield_frame(GRCORE_CompiledWalk * walk,
    const GRCORE_Stack * stack, GRCORE_CompiledFrame * out) {
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
  out->frame_base = walk->base;
  out->return_address = walk->return_address;
  out->engine = range.engine;
  out->meta = range.meta;
  out->site = site;
  out->identity = site->identity;
  out->depth = walk->depth++;
  out->record = walk->run_record;
  out->base_frames = rec->base_frames;
  /* The caller: the two words at this frame's base. */
  uintptr_t words[2];
  memcpy(words, (const void *)walk->base, sizeof words);
  walk->last_base = walk->base;
  if (words[0] == GRCORE_COMPILED_CHAIN_END) {
    /* The entry stub's marker: the run ends here, definitively. */
    walk->base = 0;
    walk->return_address = 0;
    return GRCORE_CWALK_FRAME;
  }
  if (!grcore_registry_find(stack, words[1], NULL)) {
    /* Not the entry, because the entry is marked; so this is a word that went
     * wrong, and the frames above are not to be skipped. */
    walk->failing = true;
    walk->reason = "a compiled frame returns into no registered code, and the "
                   "chain-end marker was not met";
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
    GRCORE_CompiledWalkStatus st = start_run(walk, stack);
    if (st != GRCORE_CWALK_FRAME) {
      return st;
    }
    /* How many frames the run has, counted on a copy by the same step, so that
     * each frame can say where in its run it is (and so which guest frame it
     * stands for). */
    GRCORE_CompiledWalk probe = *walk;
    GRCORE_CompiledFrame scratch;
    size_t n = 0;
    while (yield_frame(&probe, stack, &scratch) == GRCORE_CWALK_FRAME) {
      n++;
      if (probe.base == 0 || probe.failing) {
        break;
      }
    }
    walk->run_length = n;
    walk->run_index = 0;
  }
  GRCORE_CompiledWalkStatus st = yield_frame(walk, stack, out_frame);
  if (st == GRCORE_CWALK_FRAME) {
    out_frame->run_depth = walk->run_index++;
    out_frame->run_length = walk->run_length;
  }
  return st;
}

bool grcore_compiled_guest_index(
    const GRCORE_CompiledFrame * frame, size_t * out_index) {
  if (frame->base_frames == 0 || frame->run_depth >= frame->run_length) {
    return false;
  }
  *out_index = frame->base_frames - 1u + (frame->run_length - 1u - frame->run_depth);
  return true;
}

const char * grcore_compiled_walk_reason(const GRCORE_CompiledWalk * walk) {
  return walk != NULL && walk->broken ? walk->reason : NULL;
}
