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
