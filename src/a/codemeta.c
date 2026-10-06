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
#include <ghoti.io/runtime-core/allocator.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

/* A base must be a live *reference*: a RAW entry at the slot does not count. */
static bool is_live(const GRCORE_CodeSite * site, int64_t slot) {
  for (size_t i = 0; i < site->live_count; i++) {
    if (site->live[i].value == slot &&
        site->live[i].slot_kind == GRCORE_SLOT_VALUE) {
      return true;
    }
  }
  return false;
}

/* The same question for a whole site at once. `marks` holds a bit per frame
 * slot; the VALUE entries of the site's stack map are set in it, so that each
 * derived pointer's base is a bit test and not a search of the stack map
 * (which made a site with n derived pointers and n live references n squared).
 * Slots were range-checked before this is used. */
static size_t slot_index(int64_t offset) {
  return (size_t)(-offset / 8 - 1);
}

static void mark_slots(const GRCORE_CodeSite * s, uint64_t * marks, bool on) {
  for (size_t i = 0; i < s->live_count; i++) {
    if (s->live[i].slot_kind != GRCORE_SLOT_VALUE) {
      continue;
    }
    size_t k = slot_index(s->live[i].value);
    if (on) {
      marks[k / 64] |= UINT64_C(1) << (k % 64);
    } else {
      marks[k / 64] &= ~(UINT64_C(1) << (k % 64));
    }
  }
}

static GRCORE_Result check_site(const GRCORE_CodeSite * s, uint32_t frame_bytes,
    uint64_t * marks, const char ** out_reason, size_t * loc) {
  *loc = SIZE_MAX;
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
    *loc = i;
    if (l->kind != GRCORE_LOC_FRAME_SLOT) {
      CORRUPT("stack map entry is not a frame slot");
    }
    if (l->slot_kind != GRCORE_SLOT_RAW && l->slot_kind != GRCORE_SLOT_VALUE) {
      CORRUPT("slot kind is not RAW or VALUE");
    }
    if (!slot_ok(l->value, frame_bytes)) {
      CORRUPT("stack map slot is misaligned or outside the frame");
    }
    if ((unsigned)l->representation >= (unsigned)GRCORE_REPR_COUNT) {
      CORRUPT("stack map entry's representation is not one of the representations");
    }
    if (l->representation != GRCORE_REPR_BITS) {
      CORRUPT("stack map entry carries a converting representation");
    }
  }
  *loc = SIZE_MAX;
  if (marks != NULL) {
    mark_slots(s, marks, true);
  }
  for (size_t i = 0; i < s->derived_count; i++) {
    const GRCORE_DerivedPointer * d = &s->derived[i];
    if (!slot_ok(d->slot, frame_bytes) || !slot_ok(d->base_slot, frame_bytes)) {
      if (marks != NULL) {
        mark_slots(s, marks, false);
      }
      CORRUPT("derived pointer slot is misaligned or outside the frame");
    }
    bool live;
    if (marks != NULL) {
      size_t k = slot_index(d->base_slot);
      live = (marks[k / 64] >> (k % 64)) & 1u;
    } else {
      live = is_live(s, d->base_slot);
    }
    if (!live) {
      if (marks != NULL) {
        mark_slots(s, marks, false);
      }
      CORRUPT("derived pointer's base is not a live reference of its site");
    }
  }
  if (marks != NULL) {
    mark_slots(s, marks, false);
  }
  for (size_t i = 0; i < s->frame_state_count; i++) {
    const GRCORE_CodeLocation * l = &s->frame_state[i];
    *loc = i;
    if ((unsigned)l->kind >= (unsigned)GRCORE_LOC_KIND_COUNT) {
      CORRUPT("location kind is not one of the kinds");
    }
    if (l->slot_kind != GRCORE_SLOT_RAW && l->slot_kind != GRCORE_SLOT_VALUE) {
      CORRUPT("slot kind is not RAW or VALUE");
    }
    if (l->kind == GRCORE_LOC_FRAME_SLOT && !slot_ok(l->value, frame_bytes)) {
      CORRUPT("frame state slot is misaligned or outside the frame");
    }
    if ((unsigned)l->representation >= (unsigned)GRCORE_REPR_COUNT) {
      CORRUPT("frame state representation is not one of the representations");
    }
    if (l->representation != GRCORE_REPR_BITS) {
      if (l->slot_kind == GRCORE_SLOT_VALUE) {
        CORRUPT("frame state location is a reference with a converting representation");
      }
      if (l->kind == GRCORE_LOC_DEAD) {
        CORRUPT("frame state location is dead with a converting representation");
      }
    }
  }
  *loc = SIZE_MAX;
  return GRCORE_OK;
}

/* Each function's interpreter slot count, found by function in an open
 * addressing table, so that "agree with the first earlier site of this
 * function" is a lookup and not a walk over every earlier site. */
typedef struct FnCount {
  uint64_t function;
  size_t count;
  bool used;
} FnCount;

static size_t fn_home(uint64_t function, size_t mask) {
  uint64_t x = function;
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  return (size_t)x & mask;
}

GRCORE_Result grcore_codemeta_validate(
    const GRCORE_CodeMeta * meta, size_t code_bytes, const char ** out_reason) {
  return grcore_codemeta_validate_at(meta, code_bytes, out_reason, NULL, NULL);
}

GRCORE_Result grcore_codemeta_validate_at(const GRCORE_CodeMeta * meta,
    size_t code_bytes, const char ** out_reason, size_t * out_site,
    size_t * out_location) {
  if (out_site != NULL) {
    *out_site = SIZE_MAX;
  }
  if (out_location != NULL) {
    *out_location = SIZE_MAX;
  }
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

  /* Working memory for the two lookups. If it cannot be had, the checks fall
   * back to searching, which gives the same answers more slowly. */
  const GRCORE_Allocator * a = grcore_allocator_default();
  size_t mark_words = (size_t)meta->frame_bytes / 8 / 64 + 1;
  uint64_t * marks = a->calloc_fn(a->ctx, mark_words, sizeof *marks);
  FnCount * functions = NULL;
  size_t mask = 0;
  if (meta->site_count != 0 && meta->site_count < SIZE_MAX / 4 / sizeof *functions) {
    size_t cap = 16;
    while (cap < meta->site_count * 2) {
      cap <<= 1;
    }
    functions = a->calloc_fn(a->ctx, cap, sizeof *functions);
    mask = cap - 1;
  }

  GRCORE_Result result = GRCORE_OK;
  const char * why = NULL;
  size_t bad_site = SIZE_MAX;
  size_t bad_loc = SIZE_MAX;
  for (size_t i = 0; i < meta->site_count && result == GRCORE_OK; i++) {
    const GRCORE_CodeSite * s = &meta->sites[i];
    if (s->code_offset >= meta->code_bytes) {
      why = "site offset is past the code";
      result = GRCORE_ERR_CORRUPT;
      break;
    }
    if (i > 0 && s->code_offset <= meta->sites[i - 1].code_offset) {
      why = "site offsets are not strictly increasing";
      result = GRCORE_ERR_CORRUPT;
      break;
    }
    result = check_site(s, meta->frame_bytes, marks, &why, &bad_loc);
    if (result != GRCORE_OK) {
      bad_site = i;
      break;
    }
    /* One function, one interpreter frame size: compare with the first
     * earlier site that names the same function. */
    if (functions != NULL) {
      size_t h = fn_home(s->identity.function, mask);
      while (functions[h].used && functions[h].function != s->identity.function) {
        h = (h + 1) & mask;
      }
      if (!functions[h].used) {
        functions[h].used = true;
        functions[h].function = s->identity.function;
        functions[h].count = s->frame_state_count;
      } else if (functions[h].count != s->frame_state_count) {
        why = "two sites of one function disagree on the slot count";
        result = GRCORE_ERR_CORRUPT;
      }
    } else {
      for (size_t j = 0; j < i; j++) {
        if (meta->sites[j].identity.function == s->identity.function) {
          if (meta->sites[j].frame_state_count != s->frame_state_count) {
            why = "two sites of one function disagree on the slot count";
            result = GRCORE_ERR_CORRUPT;
          }
          break;
        }
      }
    }
  }
  a->free_fn(a->ctx, marks);
  a->free_fn(a->ctx, functions);
  if (result != GRCORE_OK && out_reason != NULL) {
    *out_reason = why;
  }
  if (result != GRCORE_OK && out_site != NULL) {
    *out_site = bad_site;
  }
  if (result != GRCORE_OK && out_location != NULL) {
    *out_location = bad_loc;
  }
  return result;
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
