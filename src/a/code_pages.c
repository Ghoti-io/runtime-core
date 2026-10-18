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
 */

/**
 * @file code_pages.c
 *
 * Budgeted module-shared executable pages over a context group's counted
 * provider.
 */

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/code_pages.h>
#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/b/group.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct GRCORE_CodePages {
  GRCORE_Group * group;
  const GRCORE_Allocator * allocator;
  const GRCORE_PageProvider * backing;
  GRCORE_PageProvider provider;
  uint64_t cap;
  uint64_t in_use;
  uint64_t peak;
  size_t references;
};

static void code_pages_retain_raw(GRCORE_CodePages * pages) {
  __atomic_add_fetch(&pages->references, 1, __ATOMIC_RELAXED);
}

GRCORE_CodePages * grcore_code_pages_retain(
    GRCORE_CodePages * pages) {
  if (pages != NULL) code_pages_retain_raw(pages);
  return pages;
}

void grcore_code_pages_release(
    GRCORE_CodePages * pages) {
  if (pages == NULL || __atomic_sub_fetch(&pages->references, 1,
      __ATOMIC_ACQ_REL) != 0) return;
  const GRCORE_Allocator * allocator = pages->allocator;
  GRCORE_Group * group = pages->group;
  allocator->free_fn(allocator->ctx, pages);
  (void)grcore_group_destroy(group);
}

static void * code_pages_map(void *ctx, size_t size) {
  GRCORE_CodePages * pages = ctx;
  if (size == 0 || size % pages->provider.page_size != 0) return NULL;

  uint64_t current = __atomic_load_n(&pages->in_use, __ATOMIC_ACQUIRE);
  for (;;) {
    if ((uint64_t)size > pages->cap - current) return NULL;
    uint64_t next = current + (uint64_t)size;
    if (__atomic_compare_exchange_n(&pages->in_use, &current, next, true,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      uint64_t peak = __atomic_load_n(&pages->peak, __ATOMIC_RELAXED);
      while (peak < next && !__atomic_compare_exchange_n(&pages->peak, &peak,
          next, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
      code_pages_retain_raw(pages);
      void * mapping = pages->backing->map(pages->backing->ctx, size);
      if (mapping != NULL) return mapping;
      __atomic_sub_fetch(&pages->in_use, (uint64_t)size, __ATOMIC_ACQ_REL);
      grcore_code_pages_release(pages);
      return NULL;
    }
  }
}

static void code_pages_unmap(void *ctx, void *mapping, size_t size) {
  GRCORE_CodePages * pages = ctx;
  pages->backing->unmap(pages->backing->ctx, mapping, size);
  __atomic_sub_fetch(&pages->in_use, (uint64_t)size, __ATOMIC_ACQ_REL);
  grcore_code_pages_release(pages);
}

static bool code_pages_protect(void *ctx, void *mapping, size_t size,
    GRCORE_PageAccess access) {
  GRCORE_CodePages * pages = ctx;
  return grcore_page_protect(pages->backing, mapping, size, access) ==
      GRCORE_OK;
}

GRCORE_Result grcore_code_pages_create(const GRCORE_Allocator * base_allocator,
    uint64_t byte_cap, GRCORE_CodePages **out_pages) {
  if (out_pages == NULL) return GRCORE_ERR_INVALID;
  GRCORE_Group * group = NULL;
  GRCORE_Result created = grcore_group_create(base_allocator, NULL, &group);
  if (created != GRCORE_OK) return created;
  const GRCORE_Allocator * allocator = grcore_group_allocator(group);
  const GRCORE_PageProvider * backing = grcore_group_page_provider(group);
  if (allocator == NULL || backing == NULL || backing->page_size == 0 ||
      backing->map == NULL || backing->unmap == NULL) {
    (void)grcore_group_destroy(group);
    return GRCORE_ERR_INVALID;
  }
  GRCORE_CodePages * pages = allocator->calloc_fn(allocator->ctx, 1,
      sizeof(*pages));
  if (pages == NULL) {
    (void)grcore_group_destroy(group);
    return GRCORE_ERR_OOM;
  }
  pages->group = group;
  pages->allocator = allocator;
  pages->backing = backing;
  pages->cap = byte_cap;
  pages->references = 1;
  pages->provider = (GRCORE_PageProvider)GRCORE_PAGE_PROVIDER_INIT(
      pages, backing->page_size, code_pages_map, code_pages_unmap,
      GRCORE_PAGE_PROVIDER_HAS(backing, protect) && backing->protect != NULL
          ? code_pages_protect : NULL);
  *out_pages = pages;
  return GRCORE_OK;
}

const GRCORE_PageProvider * grcore_code_pages_provider(
    const GRCORE_CodePages * pages) {
  return pages == NULL ? NULL : &pages->provider;
}

uint64_t grcore_code_pages_in_use(
    const GRCORE_CodePages * pages) {
  return pages == NULL ? 0 : __atomic_load_n(&pages->in_use, __ATOMIC_ACQUIRE);
}

uint64_t grcore_code_pages_peak(
    const GRCORE_CodePages * pages) {
  return pages == NULL ? 0 : __atomic_load_n(&pages->peak, __ATOMIC_ACQUIRE);
}

uint64_t grcore_code_pages_cap(
    const GRCORE_CodePages * pages) {
  return pages == NULL ? 0 : pages->cap;
}

const GRCORE_Allocator * grcore_code_pages_allocator(
    const GRCORE_CodePages * pages) {
  return pages == NULL ? NULL : pages->allocator;
}

uint64_t grcore_code_pages_accounted_bytes(
    const GRCORE_CodePages * pages) {
  return pages == NULL ? 0 : grcore_group_memory_in_use(pages->group);
}
