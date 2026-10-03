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
 * Context groups.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "group_internal.h"

GRCORE_Result grcore_group_create(const GRCORE_Allocator * allocator,
    const GRCORE_PageProvider * pages, GRCORE_Group ** out_group) {
  if (out_group == NULL) {
    return GRCORE_ERR_INVALID;
  }
  if (allocator == NULL) {
    allocator = grcore_allocator_default();
  }
  if (pages == NULL) {
    pages = grcore_page_provider_default();
  }
  if (pages->page_size == 0 || pages->map == NULL || pages->unmap == NULL) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Group * g = allocator->malloc_fn(allocator->ctx, sizeof *g);
  if (g == NULL) {
    return GRCORE_ERR_OOM;
  }
  g->allocator = allocator;
  g->pages = pages;
  g->contexts = 0;
  g->ports = 0;
  grcore_meter_init(&g->meter);
  grcore_counting_init(&g->counting, &g->meter, allocator, pages);
  *out_group = g;
  return GRCORE_OK;
}

GRCORE_Result grcore_group_destroy(GRCORE_Group * group) {
  if (group == NULL || grcore_group_context_count(group) != 0 ||
      grcore_group_port_count(group) != 0) {
    return GRCORE_ERR_INVALID;
  }
  group->allocator->free_fn(group->allocator->ctx, group);
  return GRCORE_OK;
}

size_t grcore_group_context_count(const GRCORE_Group * group) {
  return group == NULL ? 0 : __atomic_load_n(&group->contexts, __ATOMIC_ACQUIRE);
}

size_t grcore_group_port_count(const GRCORE_Group * group) {
  return group == NULL ? 0 : __atomic_load_n(&group->ports, __ATOMIC_ACQUIRE);
}

void grcore_group_port_enter(GRCORE_Group * group) {
  __atomic_add_fetch(&group->ports, 1, __ATOMIC_ACQ_REL);
}

void grcore_group_port_leave(GRCORE_Group * group) {
  __atomic_sub_fetch(&group->ports, 1, __ATOMIC_ACQ_REL);
}

void grcore_group_enter(GRCORE_Group * group) {
  __atomic_add_fetch(&group->contexts, 1, __ATOMIC_ACQ_REL);
}

void grcore_group_leave(GRCORE_Group * group) {
  __atomic_sub_fetch(&group->contexts, 1, __ATOMIC_ACQ_REL);
}

const GRCORE_Allocator * grcore_group_allocator(const GRCORE_Group * group) {
  return group == NULL ? NULL : &group->counting.allocator;
}

const GRCORE_PageProvider * grcore_group_page_provider(
    const GRCORE_Group * group) {
  return group == NULL ? NULL : &group->counting.pages;
}

uint64_t grcore_group_memory_in_use(const GRCORE_Group * group) {
  return group == NULL ? 0 : grcore_meter_in_use(&group->meter);
}

uint64_t grcore_group_memory_peak(const GRCORE_Group * group) {
  return group == NULL ? 0 : grcore_meter_peak(&group->meter);
}

uint64_t grcore_group_memory_blocks(const GRCORE_Group * group) {
  return group == NULL ? 0 : grcore_meter_blocks(&group->meter);
}
