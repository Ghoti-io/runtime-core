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
 * The code registry, the entry slots and the retired list (AD-28).
 *
 * The registry is a sorted array of ranges. A range that is unregistered stays
 * in the array, flagged, until the context has no open JIT record, because a
 * frame may still return into it and the walk must still find it there. Slot
 * code is retired through a node the slot allocated when it took the code, so
 * that clearing a slot, which is on the path of discarding code, allocates
 * nothing and cannot fail.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "guest_internal.h"

#include <ghoti.io/runtime-core/b/budget.h>

#include <string.h>

typedef struct RetireNode {
  GRCORE_Code * code;      ///< The reference being held back.
  struct RetireNode * next;
} RetireNode;

typedef struct RegEntry {
  uintptr_t start;
  uintptr_t end;
  GRCORE_Code * code;      ///< The registry's reference.
  const GRCORE_CodeMeta * meta;
  GRCORE_EngineId engine;
  bool retired;
} RegEntry;

typedef struct SlotRec {
  GRCORE_EntrySlot pub;    ///< What compiled code sees; `pub.reserved` is this.
  GRCORE_Code * code;      ///< The slot's reference, or NULL.
  RetireNode * node;       ///< Allocated with `code`; used to retire it.
  struct GRCORE_CodeRegistry * owner;
  struct SlotRec * next;
} SlotRec;

struct GRCORE_CodeRegistry {
  RegEntry * entries;      ///< Sorted by `start`; ranges do not overlap.
  size_t count;
  size_t capacity;
  size_t retired_entries;  ///< Entries flagged `retired`.
  RetireNode * retired;    ///< Slot code waiting to be released.
  size_t retired_nodes;
  SlotRec * slots;
};

/* The first entry whose `end` is above `address`: the only one that can
 * contain it, since ranges do not overlap. */
static size_t first_ending_above(const GRCORE_CodeRegistry * r, uintptr_t address) {
  size_t lo = 0, hi = r->count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (r->entries[mid].end > address) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

static void remove_entry(GRCORE_CodeRegistry * r, size_t index) {
  memmove(&r->entries[index], &r->entries[index + 1],
      (r->count - index - 1) * sizeof r->entries[0]);
  r->count--;
}

/* The registry of the context's stack, made if `create`. */
static GRCORE_Result registry_of(
    GRCORE_Context * context, bool create, GRCORE_CodeRegistry ** out) {
  if (context == NULL || !grcore_context_is_owner(context)) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  if (stack == NULL) {
    return GRCORE_ERR_INVALID;
  }
  if (stack->registry == NULL && create) {
    const GRCORE_Allocator * a = grcore_context_allocator(context);
    uint64_t refusals = grcore_context_memory_refusals(context);
    stack->registry = a->calloc_fn(a->ctx, 1, sizeof *stack->registry);
    if (stack->registry == NULL) {
      return grcore_guest_alloc_failure(context, refusals);
    }
  }
  *out = stack->registry;
  return GRCORE_OK;
}

GRCORE_Result grcore_code_register(GRCORE_Context * context,
    GRCORE_EngineId engine, GRCORE_Code * code, uintptr_t start, size_t size,
    const GRCORE_CodeMeta * meta) {
  /* Every refusal that needs no memory comes first, so that a refused call
   * allocates nothing (the registry itself is made last). */
  if (context == NULL || !grcore_context_is_owner(context)) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_Stack * stack = grcore_context_stack(context);
  if (stack == NULL || code == NULL || meta == NULL || size == 0 ||
      start + size < start || engine == 0 || engine > stack->engine_count) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Result res = grcore_codemeta_validate(meta, size, NULL);
  if (res != GRCORE_OK) {
    return res;
  }
  uintptr_t end = start + size;
  GRCORE_CodeRegistry * r = stack->registry;
  size_t at = 0;
  if (r != NULL) {
    at = first_ending_above(r, start);
    if (at < r->count && r->entries[at].start < end) {
      return GRCORE_ERR_INVALID; /* overlaps the range at `at` */
    }
  }
  res = registry_of(context, true, &r);
  if (res != GRCORE_OK) {
    return res;
  }
  void * grown;
  res = grcore_guest_array_reserve(
      context, r->entries, &r->capacity, r->count, sizeof r->entries[0], &grown);
  if (res != GRCORE_OK) {
    return res;
  }
  r->entries = grown;
  memmove(&r->entries[at + 1], &r->entries[at],
      (r->count - at) * sizeof r->entries[0]);
  RegEntry * e = &r->entries[at];
  e->start = start;
  e->end = end;
  e->code = grcore_code_retain(code);
  e->meta = meta;
  e->engine = engine;
  e->retired = false;
  r->count++;
  return GRCORE_OK;
}

GRCORE_Result grcore_code_unregister(GRCORE_Context * context, uintptr_t start) {
  GRCORE_CodeRegistry * r;
  GRCORE_Result res = registry_of(context, false, &r);
  if (res != GRCORE_OK) {
    return res;
  }
  if (r == NULL) {
    return GRCORE_ERR_INVALID;
  }
  size_t at = first_ending_above(r, start);
  if (at >= r->count || r->entries[at].start != start || r->entries[at].retired) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  if (stack->jit_live == 0) {
    grcore_code_release(r->entries[at].code);
    remove_entry(r, at);
  } else {
    r->entries[at].retired = true;
    r->retired_entries++;
  }
  return GRCORE_OK;
}

bool grcore_registry_find(
    const GRCORE_Stack * stack, uintptr_t address, GRCORE_CodeRange * out) {
  const GRCORE_CodeRegistry * r = stack == NULL ? NULL : stack->registry;
  if (r == NULL) {
    return false;
  }
  size_t at = first_ending_above(r, address);
  if (at >= r->count || r->entries[at].start > address) {
    return false;
  }
  if (out != NULL) {
    const RegEntry * e = &r->entries[at];
    out->start = e->start;
    out->end = e->end;
    out->engine = e->engine;
    out->code = e->code;
    out->meta = e->meta;
    out->retired = e->retired;
  }
  return true;
}

bool grcore_code_lookup(const GRCORE_Context * context, uintptr_t address,
    GRCORE_CodeRange * out_range) {
  return context != NULL &&
      grcore_registry_find(grcore_context_stack(context), address, out_range);
}

size_t grcore_code_registered_count(const GRCORE_Context * context) {
  const GRCORE_Stack * stack = context == NULL ? NULL : grcore_context_stack(context);
  return stack == NULL || stack->registry == NULL
      ? 0
      : stack->registry->count - stack->registry->retired_entries;
}

size_t grcore_code_retired_count(const GRCORE_Context * context) {
  const GRCORE_Stack * stack = context == NULL ? NULL : grcore_context_stack(context);
  return stack == NULL || stack->registry == NULL
      ? 0
      : stack->registry->retired_entries + stack->registry->retired_nodes;
}

/* ---- Entry slots -------------------------------------------------------- */

GRCORE_Result grcore_entry_slot_create(
    GRCORE_Context * context, GRCORE_EntrySlot ** out_slot) {
  GRCORE_CodeRegistry * r;
  GRCORE_Result res = registry_of(context, true, &r);
  if (res != GRCORE_OK) {
    return res;
  }
  if (out_slot == NULL) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_Allocator * a = grcore_context_allocator(context);
  uint64_t refusals = grcore_context_memory_refusals(context);
  SlotRec * s = a->calloc_fn(a->ctx, 1, sizeof *s);
  if (s == NULL) {
    return grcore_guest_alloc_failure(context, refusals);
  }
  s->pub.reserved = s;
  s->owner = r;
  s->next = r->slots;
  r->slots = s;
  *out_slot = &s->pub;
  return GRCORE_OK;
}

/* Gives up a reference that a compiled frame may still return into: released
 * now if none can, otherwise parked on the retired list under `node`. */
static void retire(GRCORE_Stack * stack, GRCORE_Code * code, RetireNode * node) {
  GRCORE_CodeRegistry * r = stack->registry;
  if (stack->jit_live == 0) {
    grcore_code_release(code);
    const GRCORE_Allocator * a = grcore_context_allocator(stack->context);
    a->free_fn(a->ctx, node);
    return;
  }
  node->code = code;
  node->next = r->retired;
  r->retired = node;
  r->retired_nodes++;
}

static SlotRec * own_slot(GRCORE_CodeRegistry * r, GRCORE_EntrySlot * slot) {
  SlotRec * s = slot == NULL ? NULL : slot->reserved;
  return r != NULL && s != NULL && s->owner == r && &s->pub == slot ? s : NULL;
}

GRCORE_Result grcore_entry_slot_set(GRCORE_Context * context,
    GRCORE_EntrySlot * slot, GRCORE_Code * code, uintptr_t entry) {
  GRCORE_CodeRegistry * r;
  GRCORE_Result res = registry_of(context, false, &r);
  if (res != GRCORE_OK) {
    return res;
  }
  SlotRec * s = own_slot(r, slot);
  if (s == NULL || code == NULL || entry == 0) {
    return GRCORE_ERR_INVALID;
  }
  /* The one step that can be refused comes first: the node that will retire
   * this code later. */
  const GRCORE_Allocator * a = grcore_context_allocator(context);
  uint64_t refusals = grcore_context_memory_refusals(context);
  RetireNode * node = a->calloc_fn(a->ctx, 1, sizeof *node);
  if (node == NULL) {
    return grcore_guest_alloc_failure(context, refusals);
  }
  GRCORE_Code * old = s->code;
  RetireNode * old_node = s->node;
  s->code = grcore_code_retain(code);
  s->node = node;
  s->pub.entry = entry;
  if (old != NULL) {
    retire(grcore_context_stack(context), old, old_node);
  }
  return GRCORE_OK;
}

GRCORE_Result grcore_entry_slot_clear(
    GRCORE_Context * context, GRCORE_EntrySlot * slot) {
  GRCORE_CodeRegistry * r;
  GRCORE_Result res = registry_of(context, false, &r);
  if (res != GRCORE_OK) {
    return res;
  }
  SlotRec * s = own_slot(r, slot);
  if (s == NULL) {
    return GRCORE_ERR_INVALID;
  }
  s->pub.entry = 0;
  if (s->code != NULL) {
    GRCORE_Code * code = s->code;
    RetireNode * node = s->node;
    s->code = NULL;
    s->node = NULL;
    retire(grcore_context_stack(context), code, node);
  }
  return GRCORE_OK;
}

GRCORE_Code * grcore_entry_slot_code(const GRCORE_EntrySlot * slot) {
  if (slot == NULL) {
    return NULL;
  }
  const SlotRec * s = slot->reserved;
  return s == NULL ? NULL : s->code;
}

/* ---- Release ------------------------------------------------------------ */

void grcore_registry_release_retired(GRCORE_Stack * stack) {
  GRCORE_CodeRegistry * r = stack->registry;
  if (r == NULL || stack->jit_live != 0) {
    return;
  }
  const GRCORE_Allocator * a = grcore_context_allocator(stack->context);
  /* Detach first: a release callback runs arbitrary code. */
  RetireNode * list = r->retired;
  r->retired = NULL;
  r->retired_nodes = 0;
  while (list != NULL) {
    RetireNode * next = list->next;
    grcore_code_release(list->code);
    a->free_fn(a->ctx, list);
    list = next;
  }
  /* Take the retired ranges out of the array before any release callback
   * runs: swap the live ones down in order, which leaves the retired ones, with
   * their references, past the new end. */
  size_t old_count = r->count, kept = 0;
  for (size_t i = 0; i < old_count; i++) {
    if (!r->entries[i].retired) {
      RegEntry t = r->entries[kept];
      r->entries[kept++] = r->entries[i];
      r->entries[i] = t;
    }
  }
  r->count = kept;
  r->retired_entries = 0;
  for (size_t i = kept; i < old_count; i++) {
    grcore_code_release(r->entries[i].code);
  }
}

void grcore_registry_destroy(GRCORE_Stack * stack) {
  GRCORE_CodeRegistry * r = stack->registry;
  if (r == NULL) {
    return;
  }
  const GRCORE_Allocator * a = grcore_context_allocator(stack->context);
  stack->registry = NULL;
  while (r->retired != NULL) {
    RetireNode * next = r->retired->next;
    grcore_code_release(r->retired->code);
    a->free_fn(a->ctx, r->retired);
    r->retired = next;
  }
  for (size_t i = 0; i < r->count; i++) {
    grcore_code_release(r->entries[i].code);
  }
  while (r->slots != NULL) {
    SlotRec * next = r->slots->next;
    if (r->slots->code != NULL) {
      grcore_code_release(r->slots->code);
      a->free_fn(a->ctx, r->slots->node);
    }
    a->free_fn(a->ctx, r->slots);
    r->slots = next;
  }
  a->free_fn(a->ctx, r->entries);
  a->free_fn(a->ctx, r);
}
