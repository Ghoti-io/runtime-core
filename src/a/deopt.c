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
 * Reading and writing a native frame by its metadata.
 */

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/deopt.h>

#include "guest_internal.h"

#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/b/budget.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

GRCORE_Result grcore_deopt_read(const GRCORE_CodeSite * site,
    const void * frame_base, uint64_t * slots, size_t slot_count) {
  if (site == NULL || frame_base == NULL || slots == NULL ||
      slot_count != site->frame_state_count ||
      (slot_count != 0 && site->frame_state == NULL)) {
    return GRCORE_ERR_INVALID;
  }
  for (size_t i = 0; i < slot_count; i++) {
    const GRCORE_CodeLocation * l = &site->frame_state[i];
    switch (l->kind) {
    case GRCORE_LOC_FRAME_SLOT:
      memcpy(&slots[i], (const unsigned char *)frame_base + l->value,
          sizeof slots[i]);
      break;
    case GRCORE_LOC_CONSTANT:
      slots[i] = (uint64_t)l->value;
      break;
    default:
      slots[i] = 0;
      break;
    }
  }
  return GRCORE_OK;
}

GRCORE_Result grcore_deopt_write_back(const GRCORE_CodeSite * site,
    void * frame_base, const uint64_t * slots, size_t slot_count) {
  if (site == NULL || frame_base == NULL || slots == NULL ||
      slot_count != site->frame_state_count ||
      (slot_count != 0 && site->frame_state == NULL)) {
    return GRCORE_ERR_INVALID;
  }
  for (size_t i = 0; i < slot_count; i++) {
    const GRCORE_CodeLocation * l = &site->frame_state[i];
    if (l->kind == GRCORE_LOC_FRAME_SLOT && l->slot_kind == GRCORE_SLOT_VALUE) {
      memcpy((unsigned char *)frame_base + l->value, &slots[i], sizeof slots[i]);
    }
  }
  return GRCORE_OK;
}

/* ---- Representations (AD-27) ----------------------------------------- */

static bool converts(const GRCORE_CodeLocation * l) {
  return l->kind != GRCORE_LOC_DEAD && l->representation != GRCORE_REPR_BITS;
}

/* The word a location names, as grcore_deopt_read reads it. */
static uint64_t word_of(const GRCORE_CodeLocation * l, const void * frame_base) {
  uint64_t w = 0;
  if (l->kind == GRCORE_LOC_FRAME_SLOT) {
    memcpy(&w, (const unsigned char *)frame_base + l->value, sizeof w);
  } else if (l->kind == GRCORE_LOC_CONSTANT) {
    w = (uint64_t)l->value;
  }
  return w;
}

static bool site_ok(const GRCORE_CodeSite * site) {
  return site != NULL && (site->frame_state_count == 0 || site->frame_state != NULL);
}

GRCORE_Result grcore_deopt_read_tagged(const GRCORE_CodeSite * site,
    const void * frame_base, GRCORE_DeoptSlot * out, size_t slot_count) {
  if (!site_ok(site) || frame_base == NULL || out == NULL ||
      slot_count != site->frame_state_count) {
    return GRCORE_ERR_INVALID;
  }
  for (size_t i = 0; i < slot_count; i++) {
    const GRCORE_CodeLocation * l = &site->frame_state[i];
    out[i].word = word_of(l, frame_base);
    out[i].representation =
        l->kind == GRCORE_LOC_DEAD ? GRCORE_REPR_BITS : l->representation;
  }
  return GRCORE_OK;
}

static const char * const kNoConversion[GRCORE_REPR_COUNT] = {
  "no conversion needed",
  "code has an I32 location and the engine descriptor has no convert and reverse",
  "code has an I64 location and the engine descriptor has no convert and reverse",
  "code has an F32 location and the engine descriptor has no convert and reverse",
  "code has an F64 location and the engine descriptor has no convert and reverse",
};

GRCORE_Result grcore_deopt_bind(const GRCORE_Context * context,
    GRCORE_EngineId engine, const GRCORE_CodeMeta * meta,
    const char ** out_reason) {
  if (context == NULL || meta == NULL) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_EngineDescriptor * d = grcore_engine_descriptor(context, engine);
  if (d == NULL) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Result r =
      grcore_codemeta_validate_at(meta, meta->code_bytes, out_reason, NULL, NULL);
  if (r != GRCORE_OK) {
    return r;
  }
  bool has = GRCORE_ENGINE_DESCRIPTOR_HAS(d, convert) &&
      GRCORE_ENGINE_DESCRIPTOR_HAS(d, reverse) && d->convert != NULL &&
      d->reverse != NULL;
  if (has) {
    return GRCORE_OK;
  }
  for (size_t i = 0; i < meta->site_count; i++) {
    const GRCORE_CodeSite * s = &meta->sites[i];
    for (size_t k = 0; k < s->frame_state_count; k++) {
      if (converts(&s->frame_state[k])) {
        if (out_reason != NULL) {
          *out_reason = kNoConversion[s->frame_state[k].representation];
        }
        return GRCORE_ERR_UNSUPPORTED;
      }
    }
  }
  return GRCORE_OK;
}

size_t grcore_deopt_converting_count(const GRCORE_CodeSite * site) {
  size_t n = 0;
  if (!site_ok(site)) {
    return 0;
  }
  for (size_t i = 0; i < site->frame_state_count; i++) {
    n += converts(&site->frame_state[i]) ? 1 : 0;
  }
  return n;
}

struct GRCORE_DeoptReservation {
  uint64_t * cells;
  size_t capacity;  /* cells the rebuild may use: what has been reserved and extended */
  size_t allocated; /* cells `cells` holds, at least `capacity` */
  size_t live;      /* cells a collector is to see: the ones a rebuild is filling */
};

static void reservation_enumerate(
    GRCORE_Context * context, void * value, const GRCORE_RootVisitor * visitor) {
  (void)context;
  GRCORE_DeoptReservation * r = value;
  if (visitor->slot == NULL) {
    return;
  }
  for (size_t i = 0; i < r->live; i++) {
    visitor->slot(visitor->user, &r->cells[i]);
  }
}

static const GRCORE_RootSource reservation_source =
    GRCORE_ROOT_SOURCE_INIT("runtime-core.deopt-reservation",
        reservation_enumerate);

GRCORE_Result grcore_deopt_reserve(GRCORE_Context * context, size_t capacity,
    GRCORE_DeoptReservation ** out) {
  if (context == NULL || out == NULL || !grcore_context_is_owner(context) ||
      capacity > SIZE_MAX / sizeof(uint64_t)) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_Allocator * a = grcore_context_allocator(context);
  uint64_t refusals = grcore_context_memory_refusals(context);
  GRCORE_DeoptReservation * r = a->calloc_fn(a->ctx, 1, sizeof *r);
  if (r == NULL) {
    return grcore_context_memory_refusals(context) > refusals ? GRCORE_ERR_LIMIT
                                                              : GRCORE_ERR_OOM;
  }
  if (capacity != 0) {
    r->cells = a->calloc_fn(a->ctx, capacity, sizeof *r->cells);
    if (r->cells == NULL) {
      a->free_fn(a->ctx, r);
      return grcore_context_memory_refusals(context) > refusals ? GRCORE_ERR_LIMIT
                                                                : GRCORE_ERR_OOM;
    }
  }
  r->capacity = capacity;
  r->allocated = capacity;
  GRCORE_Result res =
      grcore_context_add_root_source(context, &reservation_source, r);
  if (res != GRCORE_OK) {
    a->free_fn(a->ctx, r->cells);
    a->free_fn(a->ctx, r);
    return res;
  }
  *out = r;
  return GRCORE_OK;
}

size_t grcore_deopt_reservation_capacity(const GRCORE_DeoptReservation * r) {
  return r == NULL ? 0 : r->capacity;
}

GRCORE_Result grcore_deopt_reservation_extend(GRCORE_Context * context,
    GRCORE_DeoptReservation * r, size_t cells) {
  if (context == NULL || r == NULL || !grcore_context_is_owner(context) ||
      cells > SIZE_MAX - r->capacity ||
      r->capacity + cells > SIZE_MAX / sizeof(uint64_t)) {
    return GRCORE_ERR_INVALID;
  }
  if (cells == 0) {
    return GRCORE_OK;
  }
  size_t want = r->capacity + cells;
  if (want > r->allocated) {
    /* Grown by doubling, so a chain of fifty calls is a handful of
     * allocations. A collector reads only `live` cells, which this does not
     * change, so a move of the array is not seen. */
    size_t grown = r->allocated * 2 > want ? r->allocated * 2 : want;
    if (grown > SIZE_MAX / sizeof(uint64_t)) {
      grown = want;
    }
    const GRCORE_Allocator * a = grcore_context_allocator(context);
    uint64_t refusals = grcore_context_memory_refusals(context);
    uint64_t * cellsp =
        a->realloc_fn(a->ctx, r->cells, grown * sizeof *r->cells);
    if (cellsp == NULL) {
      return grcore_context_memory_refusals(context) > refusals ? GRCORE_ERR_LIMIT
                                                                : GRCORE_ERR_OOM;
    }
    r->cells = cellsp;
    r->allocated = grown;
  }
  r->capacity = want;
  return GRCORE_OK;
}

void grcore_deopt_reservation_retract(GRCORE_DeoptReservation * r, size_t cells) {
  if (r == NULL) {
    return;
  }
  r->capacity = cells > r->capacity ? 0 : r->capacity - cells;
}

GRCORE_Result grcore_deopt_release(
    GRCORE_Context * context, GRCORE_DeoptReservation * reservation) {
  if (context == NULL || !grcore_context_is_owner(context)) {
    return GRCORE_ERR_INVALID;
  }
  if (reservation == NULL) {
    return GRCORE_OK;
  }
  GRCORE_Result r =
      grcore_context_remove_root_source(context, &reservation_source, reservation);
  if (r != GRCORE_OK) {
    return r; /* still registered, so still readable: free nothing */
  }
  const GRCORE_Allocator * a = grcore_context_allocator(context);
  if (reservation->cells != NULL) {
    a->free_fn(a->ctx, reservation->cells);
  }
  a->free_fn(a->ctx, reservation);
  return GRCORE_OK;
}

GRCORE_Result grcore_deopt_rebuild(GRCORE_Context * context,
    GRCORE_EngineId engine, const GRCORE_CodeSite * site, const void * frame_base,
    GRCORE_DeoptReservation * reservation, uint64_t * slots, size_t slot_count) {
  if (context == NULL || !site_ok(site) || frame_base == NULL ||
      reservation == NULL || slots == NULL ||
      slot_count != site->frame_state_count ||
      !grcore_context_is_owner(context)) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_EngineDescriptor * d = grcore_engine_descriptor(context, engine);
  if (d == NULL) {
    return GRCORE_ERR_INVALID;
  }
  size_t need = grcore_deopt_converting_count(site);
  if (need > reservation->capacity) {
    return GRCORE_ERR_INVALID; /* a short reservation is a defect, caught here */
  }
  if (need != 0 &&
      (!GRCORE_ENGINE_DESCRIPTOR_HAS(d, convert) || d->convert == NULL)) {
    return GRCORE_ERR_UNSUPPORTED;
  }
  /* Everything that can be refused has been. From here on nothing fails. */
  /* Phase 0 and 1: the words that are not converted, and null for the rest. */
  for (size_t i = 0; i < slot_count; i++) {
    const GRCORE_CodeLocation * l = &site->frame_state[i];
    /* word_of is zero for a DEAD location. */
    slots[i] = converts(l) ? 0 : word_of(l, frame_base);
  }
  /* Phase 2: each raw value into a root, which the collector sees. The cells
   * are null before the first conversion, so a root source reading them at any
   * moment reads null or a converted value, never a raw word. */
  if (need != 0) {
    memset(reservation->cells, 0, need * sizeof *reservation->cells);
  }
  reservation->live = need;
  size_t k = 0;
  for (size_t i = 0; i < slot_count; i++) {
    const GRCORE_CodeLocation * l = &site->frame_state[i];
    if (converts(l)) {
      d->convert(context, l->representation, word_of(l, frame_base),
          &reservation->cells[k++]);
    }
  }
  /* Phase 3: only now are the slots written. */
  k = 0;
  for (size_t i = 0; i < slot_count; i++) {
    if (converts(&site->frame_state[i])) {
      slots[i] = reservation->cells[k++];
    }
  }
  reservation->live = 0;
  return GRCORE_OK;
}

GRCORE_Result grcore_deopt_write_back_checked(
    GRCORE_Context * context, GRCORE_EngineId engine, const GRCORE_CodeSite * site,
    void * frame_base, const uint64_t * slots, size_t slot_count,
    GRCORE_DeoptOutcome * out_outcome, size_t * out_misfit) {
  if (out_misfit != NULL) {
    *out_misfit = SIZE_MAX;
  }
  if (context == NULL || !site_ok(site) || frame_base == NULL || slots == NULL ||
      slot_count != site->frame_state_count || out_outcome == NULL) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_EngineDescriptor * d = grcore_engine_descriptor(context, engine);
  if (d == NULL) {
    return GRCORE_ERR_INVALID;
  }
  bool need = false;
  for (size_t i = 0; i < slot_count; i++) {
    const GRCORE_CodeLocation * l = &site->frame_state[i];
    need = need || (converts(l) && l->kind == GRCORE_LOC_FRAME_SLOT);
  }
  if (need && (!GRCORE_ENGINE_DESCRIPTOR_HAS(d, reverse) || d->reverse == NULL)) {
    return GRCORE_ERR_UNSUPPORTED;
  }
  /* Every value is tested before any word is written. `reverse` is pure, so
   * the second call below gives the answer of this one. */
  for (size_t i = 0; i < slot_count; i++) {
    const GRCORE_CodeLocation * l = &site->frame_state[i];
    if (converts(l) && l->kind == GRCORE_LOC_FRAME_SLOT) {
      uint64_t probe;
      if (!d->reverse(context, l->representation, slots[i], &probe)) {
        *out_outcome = GRCORE_DEOPT_EXIT_AT_SITE;
        if (out_misfit != NULL) {
          *out_misfit = i;
        }
        return GRCORE_OK;
      }
    }
  }
  for (size_t i = 0; i < slot_count; i++) {
    const GRCORE_CodeLocation * l = &site->frame_state[i];
    if (l->kind != GRCORE_LOC_FRAME_SLOT) {
      continue;
    }
    if (converts(l)) {
      uint64_t raw;
      if (!d->reverse(context, l->representation, slots[i], &raw)) {
        return GRCORE_ERR_INTERNAL; /* it fitted a moment ago: not pure */
      }
      memcpy((unsigned char *)frame_base + l->value, &raw, sizeof raw);
    } else if (l->slot_kind == GRCORE_SLOT_VALUE) {
      memcpy((unsigned char *)frame_base + l->value, &slots[i], sizeof slots[i]);
    }
  }
  *out_outcome = GRCORE_DEOPT_WRITTEN;
  return GRCORE_OK;
}

/* ---- A chain of compiled frames, rebuilt in one piece (AD-28) ------------ */

/* The guest frames a run stands for, innermost compiled frame first, are read
 * by walking the guest stack's links from the top after skipping the frames
 * above the run's innermost one, so no array is needed and nothing can fail
 * for want of one. */
typedef struct ChainCursor {
  GRCORE_CompiledWalk walk;
  GRCORE_FrameRef guest;
  size_t extra;
} ChainCursor;

/* The next frame of the run, with its guest frame. False at the end of the run.
 * `n` is the run's length; `record` the record it belongs to. */
static bool chain_next(ChainCursor * c, const GRCORE_Stack * stack, size_t record,
    size_t n, GRCORE_CompiledFrame * cf, GRCORE_FrameHeader * h,
    GRCORE_FrameRef * guest) {
  if (grcore_compiled_walk_next(&c->walk, cf) != GRCORE_CWALK_FRAME ||
      cf->record != record || cf->run_depth >= n) {
    return false;
  }
  *guest = c->guest;
  if (!grcore_stack_read_frame(stack, *guest, h)) {
    return false;
  }
  c->guest.offset = (size_t)h->prev;
  return true;
}

/* Starts a pass: the walk, and the guest frame of the innermost compiled
 * frame. */
static void chain_begin(ChainCursor * c, GRCORE_Context * context,
    const GRCORE_Stack * stack, size_t extra) {
  grcore_compiled_walk_begin(context, &c->walk);
  c->extra = extra;
  c->guest.offset = stack->top;
  GRCORE_FrameHeader h;
  for (size_t i = 0; i < extra; i++) {
    if (!grcore_stack_read_frame(stack, c->guest, &h)) {
      break;
    }
    c->guest.offset = (size_t)h.prev;
  }
}

static uint64_t * guest_slots(const GRCORE_Stack * stack, GRCORE_FrameRef ref) {
  return (uint64_t *)(void *)(stack->buffer + ref.offset + GRCORE_FRAME_HEADER);
}

GRCORE_Result grcore_compiled_rebuild(GRCORE_Context * context,
    GRCORE_DeoptReservation * reservation, size_t keep_frames,
    size_t * out_frames) {
  if (out_frames != NULL) {
    *out_frames = 0;
  }
  if (context == NULL || !grcore_context_is_owner(context)) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  if (stack == NULL) {
    return GRCORE_OK;
  }
  /* The first frame says which run this is and how long. */
  GRCORE_CompiledWalk probe;
  GRCORE_CompiledFrame first;
  grcore_compiled_walk_begin(context, &probe);
  switch (grcore_compiled_walk_next(&probe, &first)) {
  case GRCORE_CWALK_END:
    return GRCORE_OK;
  case GRCORE_CWALK_BROKEN:
    return GRCORE_ERR_CORRUPT;
  default:
    break;
  }
  const size_t n = first.run_length;
  const size_t record = first.record;
  /* The whole run must walk, and end cleanly, before anything else is judged: a
   * chain that cannot be trusted is not rebuilt from. */
  for (size_t k = 1; k < n; k++) {
    GRCORE_CompiledFrame again;
    if (grcore_compiled_walk_next(&probe, &again) != GRCORE_CWALK_FRAME) {
      return GRCORE_ERR_CORRUPT;
    }
  }
  if (probe.failing || probe.broken) {
    return GRCORE_ERR_CORRUPT;
  }
  const size_t base = first.base_frames;
  if (base == 0 || stack->frame_count < base - 1u + n) {
    return GRCORE_ERR_INVALID; /* no guest frames to rebuild into */
  }
  const size_t extra = stack->frame_count - (base - 1u + n);

  /* Check everything first. */
  size_t need = 0;
  size_t rebuilt = 0;
  bool unconvertible = false;
  ChainCursor c;
  chain_begin(&c, context, stack, extra);
  GRCORE_CompiledFrame cf;
  GRCORE_FrameHeader h;
  GRCORE_FrameRef guest = {0};
  memset(&h, 0, sizeof h);
  for (size_t d = 0; d < n; d++) {
    if (!chain_next(&c, stack, record, n, &cf, &h, &guest)) {
      return GRCORE_ERR_CORRUPT;
    }
    size_t index;
    if (!grcore_compiled_guest_index(&cf, &index) || index >= stack->frame_count ||
        h.engine != cf.engine || h.function != cf.identity.function ||
        h.slot_count != cf.site->frame_state_count) {
      return GRCORE_ERR_INVALID;
    }
    if (index < keep_frames) {
      rebuilt++;
      size_t here = grcore_deopt_converting_count(cf.site);
      need += here;
      if (here != 0) {
        const GRCORE_EngineDescriptor * ed = grcore_engine_descriptor(context, cf.engine);
        if (ed == NULL || !GRCORE_ENGINE_DESCRIPTOR_HAS(ed, convert) ||
            ed->convert == NULL) {
          unconvertible = true;
        }
      }
    }
  }
  if (need > (reservation == NULL ? 0 : reservation->capacity)) {
    return GRCORE_ERR_INVALID; /* a short reservation is a defect, caught here */
  }
  if (unconvertible) {
    return GRCORE_ERR_UNSUPPORTED;
  }

  /* Everything that can be refused has been. From here on nothing fails.
   * Phase 1: the words that are not converted, null in the slots that are. */
  chain_begin(&c, context, stack, extra);
  for (size_t k = 0; k < n; k++) {
    (void)chain_next(&c, stack, record, n, &cf, &h, &guest);
    size_t index = SIZE_MAX;
    (void)grcore_compiled_guest_index(&cf, &index);
    if (index >= keep_frames) {
      continue;
    }
    uint64_t * slots = guest_slots(stack, guest);
    for (size_t i = 0; i < cf.site->frame_state_count; i++) {
      const GRCORE_CodeLocation * l = &cf.site->frame_state[i];
      if (l->kind == GRCORE_LOC_DEAD) {
        continue; /* the guest frame keeps its own word */
      }
      slots[i] = converts(l) ? 0 : word_of(l, (const void *)cf.frame_base);
    }
  }
  /* Phase 2: each raw value into a root the collector sees. The cells are null
   * before the first conversion, so a reader sees null or a converted value,
   * never a raw word. */
  if (need != 0) {
    memset(reservation->cells, 0, need * sizeof *reservation->cells);
    reservation->live = need;
    chain_begin(&c, context, stack, extra);
    size_t cell = 0;
    for (size_t k = 0; k < n; k++) {
      (void)chain_next(&c, stack, record, n, &cf, &h, &guest);
      size_t index = SIZE_MAX;
      (void)grcore_compiled_guest_index(&cf, &index);
      if (index >= keep_frames) {
        continue;
      }
      for (size_t i = 0; i < cf.site->frame_state_count; i++) {
        const GRCORE_CodeLocation * l = &cf.site->frame_state[i];
        if (converts(l)) {
          grcore_engine_descriptor(context, cf.engine)->convert(context,
              l->representation,
              word_of(l, (const void *)cf.frame_base), &reservation->cells[cell++]);
        }
      }
    }
    /* Phase 3: only now are the converted slots written. */
    chain_begin(&c, context, stack, extra);
    cell = 0;
    for (size_t k = 0; k < n; k++) {
      (void)chain_next(&c, stack, record, n, &cf, &h, &guest);
      size_t index = SIZE_MAX;
      (void)grcore_compiled_guest_index(&cf, &index);
      if (index >= keep_frames) {
        continue;
      }
      uint64_t * slots = guest_slots(stack, guest);
      for (size_t i = 0; i < cf.site->frame_state_count; i++) {
        if (converts(&cf.site->frame_state[i])) {
          slots[i] = reservation->cells[cell++];
        }
      }
    }
    reservation->live = 0;
  }
  /* The frames are rebuilt; the native frames only return from here. */
  stack->activations[record].frame_base = 0;
  stack->activations[record].return_address = 0;
  stack->activations[record].rebuilt = true;
  if (out_frames != NULL) {
    *out_frames = rebuilt;
  }
  return GRCORE_OK;
}
