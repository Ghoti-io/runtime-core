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
 * The counting allocator and page provider.
 *
 * Each allocation carries a header the size of `max_align_t`, which keeps the
 * payload aligned and records its size, so a free or a realloc can credit
 * exactly what was charged. A base allocation that fails changes no counter.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "account_internal.h"

#include <ghoti.io/runtime-core/b/request.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HEADER (sizeof(max_align_t))

_Static_assert(sizeof(max_align_t) >= sizeof(size_t),
    "the header must hold a size");

static void note_peak(GRCORE_Meter * m, uint64_t value) {
  uint64_t peak = __atomic_load_n(&m->peak, __ATOMIC_RELAXED);
  while (value > peak &&
      !__atomic_compare_exchange_n(
          &m->peak, &peak, value, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

static void meter_add(GRCORE_Meter * m, uint64_t bytes, uint64_t blocks) {
  uint64_t now = __atomic_add_fetch(&m->in_use, bytes, __ATOMIC_RELAXED);
  __atomic_add_fetch(&m->blocks, blocks, __ATOMIC_RELAXED);
  note_peak(m, now);
}

static void meter_sub(GRCORE_Meter * m, uint64_t bytes, uint64_t blocks) {
  __atomic_sub_fetch(&m->in_use, bytes, __ATOMIC_RELAXED);
  __atomic_sub_fetch(&m->blocks, blocks, __ATOMIC_RELAXED);
}

void grcore_meter_init(GRCORE_Meter * meter) {
  memset(meter, 0, sizeof *meter);
}

uint64_t grcore_meter_in_use(const GRCORE_Meter * meter) {
  return __atomic_load_n(&meter->in_use, __ATOMIC_RELAXED);
}

uint64_t grcore_meter_peak(const GRCORE_Meter * meter) {
  return __atomic_load_n(&meter->peak, __ATOMIC_RELAXED);
}

uint64_t grcore_meter_blocks(const GRCORE_Meter * meter) {
  return __atomic_load_n(&meter->blocks, __ATOMIC_RELAXED);
}

/* Whether charging `add` more bytes would pass the budget and its reserve.
 * Nothing is charged for a refusal. The check and the charge are not one
 * atomic step, so two threads allocating at once through one context can
 * overshoot the reserve by what they race over; a context has one owner, so
 * its own allocations never do (AD-6). */
static bool refuse(GRCORE_Counting * c, uint64_t add) {
  uint64_t limit = __atomic_load_n(&c->limit, __ATOMIC_RELAXED);
  if (limit == UINT64_MAX) {
    return false;
  }
  uint64_t reserve = __atomic_load_n(&c->reserve, __ATOMIC_RELAXED);
  uint64_t cap = reserve > UINT64_MAX - limit ? UINT64_MAX : limit + reserve;
  uint64_t now = __atomic_load_n(&c->meter->in_use, __ATOMIC_RELAXED);
  if (add > cap || now > cap - add) {
    __atomic_add_fetch(&c->refusals, 1, __ATOMIC_RELAXED);
    return true;
  }
  return false;
}

/* After a charge: past the budget raises the memory request, which the next
 * slow poll turns into a verdict. */
static void note_over(GRCORE_Counting * c) {
  uint64_t limit = __atomic_load_n(&c->limit, __ATOMIC_RELAXED);
  if (c->request_word != NULL && limit != UINT64_MAX &&
      __atomic_load_n(&c->meter->in_use, __ATOMIC_RELAXED) > limit) {
    __atomic_fetch_or(c->request_word,
        UINT64_C(1) << GRCORE_REQUEST_MEMORY, __ATOMIC_RELEASE);
  }
}

static void * counted_finish(GRCORE_Counting * c, unsigned char * raw,
    size_t size) {
  if (raw == NULL) {
    return NULL;
  }
  memcpy(raw, &size, sizeof size);
  meter_add(c->meter, size, 1);
  note_over(c);
  return raw + HEADER;
}

static void * counted_malloc(void * ctx, size_t size) {
  GRCORE_Counting * c = ctx;
  if (size > SIZE_MAX - HEADER) {
    return NULL;
  }
  if (refuse(c, size)) {
    return NULL;
  }
  const GRCORE_Allocator * b = c->base_allocator;
  return counted_finish(c, b->malloc_fn(b->ctx, size + HEADER), size);
}

static void * counted_calloc(void * ctx, size_t nitems, size_t size) {
  GRCORE_Counting * c = ctx;
  if (size != 0 && nitems > (SIZE_MAX - HEADER) / size) {
    return NULL;
  }
  if (refuse(c, (uint64_t)nitems * size)) {
    return NULL;
  }
  const GRCORE_Allocator * b = c->base_allocator;
  return counted_finish(
      c, b->calloc_fn(b->ctx, 1, nitems * size + HEADER), nitems * size);
}

static void * counted_realloc(void * ctx, void * ptr, size_t size) {
  GRCORE_Counting * c = ctx;
  if (ptr == NULL) {
    return counted_malloc(ctx, size);
  }
  if (size > SIZE_MAX - HEADER) {
    return NULL;
  }
  const GRCORE_Allocator * b = c->base_allocator;
  unsigned char * raw = (unsigned char *)ptr - HEADER;
  size_t old;
  memcpy(&old, raw, sizeof old);
  if (size > old && refuse(c, size - old)) {
    return NULL;
  }
  unsigned char * grown = b->realloc_fn(b->ctx, raw, size + HEADER);
  if (grown == NULL) {
    return NULL;
  }
  memcpy(grown, &size, sizeof size);
  if (size >= old) {
    meter_add(c->meter, size - old, 0);
    note_over(c);
  } else {
    meter_sub(c->meter, old - size, 0);
  }
  return grown + HEADER;
}

static void counted_free(void * ctx, void * ptr) {
  GRCORE_Counting * c = ctx;
  if (ptr == NULL) {
    return;
  }
  unsigned char * raw = (unsigned char *)ptr - HEADER;
  size_t old;
  memcpy(&old, raw, sizeof old);
  meter_sub(c->meter, old, 1);
  c->base_allocator->free_fn(c->base_allocator->ctx, raw);
}

static void * counted_map(void * ctx, size_t size) {
  GRCORE_Counting * c = ctx;
  if (size == 0 || c->pages.page_size == 0 ||
      size % c->pages.page_size != 0) {
    return NULL;
  }
  if (refuse(c, size)) {
    return NULL;
  }
  void * p = c->base_pages->map(c->base_pages->ctx, size);
  if (p != NULL) {
    meter_add(c->meter, size, 1);
    note_over(c);
  }
  return p;
}

static void counted_unmap(void * ctx, void * ptr, size_t size) {
  GRCORE_Counting * c = ctx;
  if (ptr == NULL) {
    return;
  }
  c->base_pages->unmap(c->base_pages->ctx, ptr, size);
  meter_sub(c->meter, size, 1);
}

void grcore_counting_init(GRCORE_Counting * counting, GRCORE_Meter * meter,
    const GRCORE_Allocator * base_allocator,
    const GRCORE_PageProvider * base_pages) {
  counting->base_allocator =
      base_allocator != NULL ? base_allocator : grcore_allocator_default();
  counting->base_pages =
      base_pages != NULL ? base_pages : grcore_page_provider_default();
  counting->meter = meter;
  counting->limit = UINT64_MAX;
  counting->reserve = 0;
  counting->refusals = 0;
  counting->request_word = NULL;
  counting->allocator.ctx = counting;
  counting->allocator.malloc_fn = counted_malloc;
  counting->allocator.calloc_fn = counted_calloc;
  counting->allocator.realloc_fn = counted_realloc;
  counting->allocator.free_fn = counted_free;
  counting->pages.ctx = counting;
  counting->pages.page_size = counting->base_pages->page_size;
  counting->pages.map = counted_map;
  counting->pages.unmap = counted_unmap;
}

void grcore_counting_set_limit(
    GRCORE_Counting * counting, uint64_t limit, uint64_t reserve) {
  __atomic_store_n(&counting->reserve, reserve, __ATOMIC_RELAXED);
  __atomic_store_n(&counting->limit, limit, __ATOMIC_RELAXED);
}

uint64_t grcore_counting_limit(const GRCORE_Counting * counting) {
  return __atomic_load_n(&counting->limit, __ATOMIC_RELAXED);
}

uint64_t grcore_counting_reserve(const GRCORE_Counting * counting) {
  return __atomic_load_n(&counting->reserve, __ATOMIC_RELAXED);
}

uint64_t grcore_counting_refusals(const GRCORE_Counting * counting) {
  return __atomic_load_n(&counting->refusals, __ATOMIC_RELAXED);
}
