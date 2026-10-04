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
 * The code-metadata validator and lookup (AD-17). A parser of a table it was
 * handed, so every index is checked before it is used.
 */

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/codemeta.h>

#include <stdbool.h>

#define CORRUPT(why) \
  do { \
    if (out_reason != NULL) { \
      *out_reason = (why); \
    } \
    return GRCORE_ERR_CORRUPT; \
  } while (0)

static bool slot_ok(int64_t offset, uint32_t frame_bytes) {
  /* The frame is [-frame_bytes, 0) below the base; a slot is 8 bytes. */
  return offset % 8 == 0 && offset >= -(int64_t)frame_bytes && offset <= -8;
}

static bool is_live(const GRCORE_CodeSite * site, int64_t slot) {
  for (size_t i = 0; i < site->live_count; i++) {
    if (site->live[i].value == slot) {
      return true;
    }
  }
  return false;
}

static GRCORE_Result check_site(const GRCORE_CodeSite * s, uint32_t frame_bytes,
    const char ** out_reason) {
  if ((unsigned)s->kind >= (unsigned)GRCORE_SITE_KIND_COUNT) {
    CORRUPT("site kind is not one of the kinds");
  }
  if ((s->live == NULL && s->live_count != 0) ||
      (s->derived == NULL && s->derived_count != 0) ||
      (s->frame_state == NULL && s->frame_state_count != 0)) {
    CORRUPT("array is NULL with a non-zero count");
  }
  for (size_t i = 0; i < s->live_count; i++) {
    const GRCORE_CodeLocation * l = &s->live[i];
    if (l->kind != GRCORE_LOC_FRAME_SLOT) {
      CORRUPT("stack map entry is not a frame slot");
    }
    if (l->slot_kind != GRCORE_SLOT_RAW && l->slot_kind != GRCORE_SLOT_VALUE) {
      CORRUPT("slot kind is not RAW or VALUE");
    }
    if (!slot_ok(l->value, frame_bytes)) {
      CORRUPT("stack map slot is misaligned or outside the frame");
    }
  }
  for (size_t i = 0; i < s->derived_count; i++) {
    const GRCORE_DerivedPointer * d = &s->derived[i];
    if (!slot_ok(d->slot, frame_bytes) || !slot_ok(d->base_slot, frame_bytes)) {
      CORRUPT("derived pointer slot is misaligned or outside the frame");
    }
    if (!is_live(s, d->base_slot)) {
      CORRUPT("derived pointer's base is not a live reference of its site");
    }
  }
  for (size_t i = 0; i < s->frame_state_count; i++) {
    const GRCORE_CodeLocation * l = &s->frame_state[i];
    if ((unsigned)l->kind >= (unsigned)GRCORE_LOC_KIND_COUNT) {
      CORRUPT("location kind is not one of the kinds");
    }
    if (l->slot_kind != GRCORE_SLOT_RAW && l->slot_kind != GRCORE_SLOT_VALUE) {
      CORRUPT("slot kind is not RAW or VALUE");
    }
    if (l->kind == GRCORE_LOC_FRAME_SLOT && !slot_ok(l->value, frame_bytes)) {
      CORRUPT("frame state slot is misaligned or outside the frame");
    }
  }
  return GRCORE_OK;
}

GRCORE_Result grcore_codemeta_validate(
    const GRCORE_CodeMeta * meta, size_t code_bytes, const char ** out_reason) {
  if (meta == NULL) {
    return GRCORE_ERR_INVALID;
  }
  if (meta->version != GRCORE_CODEMETA_FORMAT_VERSION) {
    CORRUPT("wrong format version");
  }
  if (meta->code_bytes != code_bytes) {
    CORRUPT("table's code size is not the code's size");
  }
  if (meta->sites == NULL && meta->site_count != 0) {
    CORRUPT("array is NULL with a non-zero count");
  }
  for (size_t i = 0; i < meta->site_count; i++) {
    const GRCORE_CodeSite * s = &meta->sites[i];
    if (s->code_offset >= meta->code_bytes) {
      CORRUPT("site offset is past the code");
    }
    if (i > 0 && s->code_offset <= meta->sites[i - 1].code_offset) {
      CORRUPT("site offsets are not strictly increasing");
    }
    GRCORE_Result r = check_site(s, meta->frame_bytes, out_reason);
    if (r != GRCORE_OK) {
      return r;
    }
    /* One function, one interpreter frame size: compare with the first
     * earlier site that names the same function. */
    for (size_t j = 0; j < i; j++) {
      if (meta->sites[j].identity.function == s->identity.function) {
        if (meta->sites[j].frame_state_count != s->frame_state_count) {
          CORRUPT("two sites of one function disagree on the slot count");
        }
        break;
      }
    }
  }
  return GRCORE_OK;
}

const GRCORE_CodeSite * grcore_codemeta_find(
    const GRCORE_CodeMeta * meta, uint32_t code_offset) {
  if (meta == NULL || meta->sites == NULL) {
    return NULL;
  }
  size_t lo = 0;
  size_t hi = meta->site_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    uint32_t at = meta->sites[mid].code_offset;
    if (at == code_offset) {
      return &meta->sites[mid];
    }
    if (at < code_offset) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return NULL;
}
