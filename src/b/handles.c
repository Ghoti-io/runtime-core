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
 * The cross-heap handle table (CAP-14). b/handles.h states the contract; this
 * file keeps it. The table knows nothing about what a value word means: it
 * stores 64-bit words, reports each one to its owner's collector through a
 * root source, and hands out generation-checked tokens for them.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "context_internal.h"
#include "group_internal.h"
#include "handles_internal.h"

#include <string.h>
#include <time.h>

/* ---- The table's lock ---------------------------------------------------- */

static void lock(GRCORE_HandleTable * t) {
  GCU_MUTEX_LOCK(t->mutex);
}

static void unlock(GRCORE_HandleTable * t) {
  GCU_MUTEX_UNLOCK(t->mutex);
}

static const GRCORE_Allocator * allocator_of(const GRCORE_HandleTable * t) {
  return &t->group->counting.allocator;
}

/* ---- The tag ------------------------------------------------------------- */

static uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

/* A number that differs between tables and between runs. It is not a secret:
 * the holder check is what keeps a token from being used by a third party. Its
 * job is to refuse a token that belongs to another table or another run. */
static uint32_t make_tag(const void * address) {
  static uint64_t counter;
  struct timespec ts = { 0, 0 };
  (void)timespec_get(&ts, TIME_UTC);
  uint64_t n = __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
  uint64_t x = splitmix64(n ^ (uint64_t)(uintptr_t)address ^
      ((uint64_t)ts.tv_sec << 20) ^ (uint64_t)ts.tv_nsec);
  uint32_t tag = (uint32_t)(x >> 32);
  return tag == 0 ? 1u : tag;
}

/* ---- Lifecycle ----------------------------------------------------------- */

GRCORE_Result grcore_handles_table_init(
    GRCORE_HandleTable * t, GRCORE_Group * group) {
  memset(t, 0, sizeof *t);
  t->group = group;
  t->free_head = GRCORE_HANDLE_NONE;
  t->next_generation = 1;
  t->next_serial = 1;
  t->limit_per_holder = GRCORE_HANDLES_DEFAULT_PER_HOLDER;
  t->limit_total = GRCORE_HANDLES_DEFAULT_TOTAL;
  t->tag = make_tag(t);
  if (GCU_MUTEX_CREATE(t->mutex) != 0) {
    return GRCORE_ERR_OOM;
  }
  t->mutex_ready = true;
  return GRCORE_OK;
}

void grcore_handles_table_destroy(GRCORE_HandleTable * t) {
  const GRCORE_Allocator * a = allocator_of(t);
  /* A group is destroyed with no contexts, and a context's destruction freed
   * its party, so there are none; a party that is still here is freed anyway,
   * so that the group's meter is balanced whatever happened. */
  for (size_t i = 0; i < t->party_capacity; i++) {
    if (t->parties[i] != NULL) {
      a->free_fn(a->ctx, t->parties[i]);
    }
  }
  a->free_fn(a->ctx, t->parties);
  a->free_fn(a->ctx, t->entries);
  if (t->mutex_ready) {
    GCU_MUTEX_DESTROY(t->mutex);
  }
}

GRCORE_HandleTable * grcore_group_handles(GRCORE_Group * group) {
  return group == NULL ? NULL : &group->handles;
}

/* ---- Parties ------------------------------------------------------------- */

/* A new party, in the first free slot. The lock is held. */
static GRCORE_Result party_make(GRCORE_HandleTable * t, GRCORE_HandleParty ** out) {
  const GRCORE_Allocator * a = allocator_of(t);
  size_t slot = t->party_capacity;
  for (size_t i = 0; i < t->party_capacity; i++) {
    if (t->parties[i] == NULL) {
      slot = i;
      break;
    }
  }
  if (slot == t->party_capacity) {
    size_t capacity = t->party_capacity == 0 ? 4 : t->party_capacity * 2;
    if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof *t->parties) {
      return GRCORE_ERR_OOM;
    }
    GRCORE_HandleParty ** grown = a->calloc_fn(a->ctx, capacity, sizeof *grown);
    if (grown == NULL) {
      return GRCORE_ERR_OOM;
    }
    if (t->party_capacity > 0) {
      memcpy(grown, t->parties, t->party_capacity * sizeof *grown);
    }
    a->free_fn(a->ctx, t->parties);
    t->parties = grown;
    t->party_capacity = capacity;
  }
  GRCORE_HandleParty * p = a->calloc_fn(a->ctx, 1, sizeof *p);
  if (p == NULL) {
    return GRCORE_ERR_OOM;
  }
  p->table = t;
  p->serial = t->next_serial++;
  p->slot = (uint32_t)slot;
  p->owned_head = GRCORE_HANDLE_NONE;
  p->held_head = GRCORE_HANDLE_NONE;
  t->parties[slot] = p;
  t->party_count++;
  *out = p;
  return GRCORE_OK;
}

/* Takes a party out of the table and frees it. The lock is held. */
static void party_drop(GRCORE_HandleTable * t, GRCORE_HandleParty * p) {
  const GRCORE_Allocator * a = allocator_of(t);
  t->parties[p->slot] = NULL;
  t->party_count--;
  a->free_fn(a->ctx, p);
}

/* The party a holder name refers to, or NULL. The lock is held. */
static GRCORE_HandleParty * party_find(
    const GRCORE_HandleTable * t, GRCORE_HandleHolder h) {
  if (h.serial == 0 || h.tag != t->tag || h.slot >= t->party_capacity) {
    return NULL;
  }
  GRCORE_HandleParty * p = t->parties[h.slot];
  return p != NULL && p->serial == h.serial ? p : NULL;
}

/* The party of a context, making it if the context has none. The lock is
 * held, and the caller owns the context (which is what confines the field to
 * one thread at a time). */
static GRCORE_Result party_of(GRCORE_HandleTable * t, GRCORE_Context * c,
    GRCORE_HandleParty ** out, bool * made) {
  if (c->handle_party != NULL) {
    *out = c->handle_party;
    *made = false;
    return GRCORE_OK;
  }
  GRCORE_Result r = party_make(t, out);
  if (r == GRCORE_OK) {
    c->handle_party = *out;
    *made = true;
  }
  return r;
}

/* ---- Entries and their lists -------------------------------------------- */

static void owned_link(GRCORE_HandleTable * t, GRCORE_HandleParty * p, uint32_t i) {
  GRCORE_HandleEntry * e = &t->entries[i];
  e->owned_prev = GRCORE_HANDLE_NONE;
  e->owned_next = p->owned_head;
  if (p->owned_head != GRCORE_HANDLE_NONE) {
    t->entries[p->owned_head].owned_prev = i;
  }
  p->owned_head = i;
  p->owned_count++;
}

static void owned_unlink(
    GRCORE_HandleTable * t, GRCORE_HandleParty * p, uint32_t i) {
  GRCORE_HandleEntry * e = &t->entries[i];
  if (e->owned_prev != GRCORE_HANDLE_NONE) {
    t->entries[e->owned_prev].owned_next = e->owned_next;
  } else {
    p->owned_head = e->owned_next;
  }
  if (e->owned_next != GRCORE_HANDLE_NONE) {
    t->entries[e->owned_next].owned_prev = e->owned_prev;
  }
  e->owned_prev = e->owned_next = GRCORE_HANDLE_NONE;
  p->owned_count--;
}

static void held_link(GRCORE_HandleTable * t, GRCORE_HandleParty * p, uint32_t i) {
  GRCORE_HandleEntry * e = &t->entries[i];
  e->held_prev = GRCORE_HANDLE_NONE;
  e->held_next = p->held_head;
  if (p->held_head != GRCORE_HANDLE_NONE) {
    t->entries[p->held_head].held_prev = i;
  }
  p->held_head = i;
  p->held_count++;
}

static void held_unlink(
    GRCORE_HandleTable * t, GRCORE_HandleParty * p, uint32_t i) {
  GRCORE_HandleEntry * e = &t->entries[i];
  if (e->held_prev != GRCORE_HANDLE_NONE) {
    t->entries[e->held_prev].held_next = e->held_next;
  } else {
    p->held_head = e->held_next;
  }
  if (e->held_next != GRCORE_HANDLE_NONE) {
    t->entries[e->held_next].held_prev = e->held_prev;
  }
  e->held_prev = e->held_next = GRCORE_HANDLE_NONE;
  p->held_count--;
}

/* Frees an entry that is on no list. The lock is held. */
static void entry_free(GRCORE_HandleTable * t, uint32_t i) {
  GRCORE_HandleEntry * e = &t->entries[i];
  memset(e, 0, sizeof *e);
  e->next_free = t->free_head;
  t->free_head = i;
  t->live--;
}

/* Takes an entry off both of its lists and frees it. The lock is held. */
static void entry_release(GRCORE_HandleTable * t, uint32_t i) {
  GRCORE_HandleEntry * e = &t->entries[i];
  if (e->owner != NULL) {
    owned_unlink(t, e->owner, i);
  } else {
    t->tombstones--;
  }
  held_unlink(t, e->holder, i);
  entry_free(t, i);
}

/* A free entry: from the free list, else a new slot, growing the array. The
 * lock is held. */
static GRCORE_Result entry_take(GRCORE_HandleTable * t, uint32_t * out) {
  if (t->free_head != GRCORE_HANDLE_NONE) {
    *out = t->free_head;
    t->free_head = t->entries[*out].next_free;
    return GRCORE_OK;
  }
  if (t->entry_used == t->entry_capacity) {
    if (t->entry_capacity >= GRCORE_HANDLES_MAX_ENTRIES) {
      return GRCORE_ERR_LIMIT;
    }
    const GRCORE_Allocator * a = allocator_of(t);
    size_t capacity = t->entry_capacity == 0 ? 16 : t->entry_capacity * 2;
    if (capacity > GRCORE_HANDLES_MAX_ENTRIES) {
      capacity = GRCORE_HANDLES_MAX_ENTRIES;
    }
    GRCORE_HandleEntry * grown = a->malloc_fn(a->ctx, capacity * sizeof *grown);
    if (grown == NULL) {
      return GRCORE_ERR_OOM;
    }
    if (t->entry_used > 0) {
      memcpy(grown, t->entries, t->entry_used * sizeof *grown);
    }
    a->free_fn(a->ctx, t->entries);
    t->entries = grown;
    t->entry_capacity = capacity;
  }
  *out = (uint32_t)t->entry_used++;
  return GRCORE_OK;
}

/* The entry a token names, or GRCORE_HANDLE_NONE. The lock is held. */
static uint32_t entry_find(const GRCORE_HandleTable * t, GRCORE_HandleToken k) {
  if (k.tag != t->tag || k.generation == 0 || k.index >= t->entry_used) {
    return GRCORE_HANDLE_NONE;
  }
  return t->entries[k.index].generation == k.generation ? k.index
                                                        : GRCORE_HANDLE_NONE;
}

/* ---- Roots --------------------------------------------------------------- */

/* The root source of a context that owns entries. The value is the party. The
 * table's mutex is held across the visitor, which is why a visitor must not
 * call the table; a visitor that rewrites the word in place is the case it is
 * for. */
static void enumerate_roots(
    GRCORE_Context * context, void * value, const GRCORE_RootVisitor * visitor) {
  (void)context;
  GRCORE_HandleParty * p = value;
  if (visitor->slot == NULL) {
    return;
  }
  GRCORE_HandleTable * t = p->table;
  lock(t);
  for (uint32_t i = p->owned_head; i != GRCORE_HANDLE_NONE;
      i = t->entries[i].owned_next) {
    visitor->slot(visitor->user, &t->entries[i].value);
  }
  unlock(t);
}

static const GRCORE_RootSource kHandleRoots =
    GRCORE_ROOT_SOURCE_INIT("runtime-core.handles", enumerate_roots);

/* ---- Callers ------------------------------------------------------------- */

static bool caller_ok(const GRCORE_HandleTable * t, const GRCORE_Context * c) {
  return t != NULL && c != NULL && c->group == t->group &&
      grcore_context_owned_by_caller(c) && !c->tearing_down;
}

/* ---- Enrolling and pinning ---------------------------------------------- */

GRCORE_Result grcore_handles_enroll(GRCORE_HandleTable * t,
    GRCORE_Context * holder, GRCORE_HandleHolder * out) {
  if (out == NULL || !caller_ok(t, holder)) {
    return GRCORE_ERR_INVALID;
  }
  lock(t);
  GRCORE_HandleParty * p;
  bool made;
  GRCORE_Result r = party_of(t, holder, &p, &made);
  if (r == GRCORE_OK) {
    out->serial = p->serial;
    out->slot = p->slot;
    out->tag = t->tag;
  }
  unlock(t);
  return r;
}

bool grcore_handles_holder_alive(GRCORE_HandleTable * t, GRCORE_HandleHolder h) {
  if (t == NULL) {
    return false;
  }
  lock(t);
  bool alive = party_find(t, h) != NULL;
  unlock(t);
  return alive;
}

GRCORE_Result grcore_handles_pin(GRCORE_HandleTable * t, GRCORE_Context * owner,
    GRCORE_HandleHolder holder, uint64_t value, GRCORE_HandleToken * out) {
  if (out == NULL || !caller_ok(t, owner)) {
    return GRCORE_ERR_INVALID;
  }
  lock(t);
  GRCORE_Result r = GRCORE_OK;
  GRCORE_HandleParty * hp = party_find(t, holder);
  if (hp == NULL) {
    r = GRCORE_ERR_INVALID;
  } else if (hp->held_count >= t->limit_per_holder || t->live >= t->limit_total ||
      t->next_generation == UINT64_MAX) {
    r = GRCORE_ERR_LIMIT;
  }
  GRCORE_HandleParty * op = NULL;
  bool made = false;
  if (r == GRCORE_OK) {
    r = party_of(t, owner, &op, &made);
  }
  if (r == GRCORE_OK && !op->root_registered) {
    r = grcore_context_add_root_source(owner, &kHandleRoots, op);
    if (r == GRCORE_OK) {
      op->root_registered = true;
    } else if (made) {
      /* Nothing else refers to a party made by this call. */
      owner->handle_party = NULL;
      party_drop(t, op);
      op = NULL;
    }
  }
  uint32_t i = GRCORE_HANDLE_NONE;
  if (r == GRCORE_OK) {
    r = entry_take(t, &i);
  }
  if (r == GRCORE_OK) {
    GRCORE_HandleEntry * e = &t->entries[i];
    memset(e, 0, sizeof *e);
    e->value = value;
    e->generation = t->next_generation++;
    e->owner = op;
    e->owner_serial = op->serial;
    e->holder = hp;
    e->holder_serial = hp->serial;
    owned_link(t, op, i);
    held_link(t, hp, i);
    t->live++;
    out->generation = e->generation;
    out->index = i;
    out->tag = t->tag;
  }
  unlock(t);
  return r;
}

/* ---- Asking about a token ----------------------------------------------- */

/* How the asking context stands to an entry. The lock is held. */
typedef struct Standing {
  bool holder;
  bool owner;
} Standing;

static Standing standing(const GRCORE_HandleEntry * e, const GRCORE_Context * c) {
  Standing s = { false, false };
  const GRCORE_HandleParty * p = c->handle_party;
  if (p != NULL) {
    s.holder = e->holder == p;
    s.owner = e->owner == p;
  }
  return s;
}

GRCORE_HandleStatus grcore_handles_resolve(GRCORE_HandleTable * t,
    GRCORE_HandleToken token, GRCORE_Context * owner, uint64_t * out_value) {
  if (!caller_ok(t, owner)) {
    return GRCORE_HANDLE_BAD_CALL;
  }
  lock(t);
  GRCORE_HandleStatus status = GRCORE_HANDLE_STALE;
  uint32_t i = entry_find(t, token);
  if (i != GRCORE_HANDLE_NONE) {
    const GRCORE_HandleEntry * e = &t->entries[i];
    if (standing(e, owner).owner) {
      status = GRCORE_HANDLE_LIVE;
      if (out_value != NULL) {
        *out_value = e->value;
      }
    } else {
      status = GRCORE_HANDLE_FOREIGN;
    }
  }
  unlock(t);
  return status;
}

GRCORE_HandleStatus grcore_handles_inspect(GRCORE_HandleTable * t,
    GRCORE_HandleToken token, GRCORE_Context * asker, uint64_t * out_owner,
    uint64_t * out_holder) {
  if (!caller_ok(t, asker)) {
    return GRCORE_HANDLE_BAD_CALL;
  }
  lock(t);
  GRCORE_HandleStatus status = GRCORE_HANDLE_STALE;
  uint32_t i = entry_find(t, token);
  if (i != GRCORE_HANDLE_NONE) {
    const GRCORE_HandleEntry * e = &t->entries[i];
    Standing s = standing(e, asker);
    if (s.holder || s.owner) {
      status = e->owner != NULL ? GRCORE_HANDLE_LIVE : GRCORE_HANDLE_TOMBSTONE;
      if (out_owner != NULL) {
        *out_owner = e->owner_serial;
      }
      if (out_holder != NULL) {
        *out_holder = e->holder_serial;
      }
    } else {
      status = GRCORE_HANDLE_FOREIGN;
    }
  }
  unlock(t);
  return status;
}

GRCORE_HandleStatus grcore_handles_release(GRCORE_HandleTable * t,
    GRCORE_HandleToken token, GRCORE_Context * asker) {
  if (!caller_ok(t, asker)) {
    return GRCORE_HANDLE_BAD_CALL;
  }
  lock(t);
  GRCORE_HandleStatus status = GRCORE_HANDLE_STALE;
  uint32_t i = entry_find(t, token);
  if (i != GRCORE_HANDLE_NONE) {
    const GRCORE_HandleEntry * e = &t->entries[i];
    Standing s = standing(e, asker);
    if (s.holder || s.owner) {
      status = e->owner != NULL ? GRCORE_HANDLE_LIVE : GRCORE_HANDLE_TOMBSTONE;
      entry_release(t, i);
    } else {
      status = GRCORE_HANDLE_FOREIGN;
    }
  }
  unlock(t);
  return status;
}

/* ---- A context ends ------------------------------------------------------ */

void grcore_handles_context_destroyed(GRCORE_Context * c) {
  GRCORE_HandleParty * p = c->handle_party;
  if (p == NULL) {
    return;
  }
  GRCORE_HandleTable * t = p->table;
  lock(t);
  /* What it owned is gone: the entry stays, as a tombstone, and roots nothing.
   * The context's root source goes with the context. */
  for (uint32_t i = p->owned_head; i != GRCORE_HANDLE_NONE;) {
    GRCORE_HandleEntry * e = &t->entries[i];
    uint32_t next = e->owned_next;
    e->owner = NULL;
    e->value = 0;
    e->owned_prev = e->owned_next = GRCORE_HANDLE_NONE;
    t->tombstones++;
    i = next;
  }
  p->owned_head = GRCORE_HANDLE_NONE;
  p->owned_count = 0;
  /* What it held is released, tombstones and an entry it owned itself too. */
  for (uint32_t i = p->held_head; i != GRCORE_HANDLE_NONE;) {
    uint32_t next = t->entries[i].held_next;
    entry_release(t, i);
    i = next;
  }
  party_drop(t, p);
  if (t->party_count == 0) {
    /* No context takes part any more, so no entry can be in use (every entry
     * has a live holder). The arrays are given back, so that a group whose
     * contexts are all gone has nothing charged to it. */
    const GRCORE_Allocator * a = allocator_of(t);
    a->free_fn(a->ctx, t->parties);
    a->free_fn(a->ctx, t->entries);
    t->parties = NULL;
    t->party_capacity = 0;
    t->entries = NULL;
    t->entry_used = 0;
    t->entry_capacity = 0;
    t->free_head = GRCORE_HANDLE_NONE;
  }
  unlock(t);
  c->handle_party = NULL;
}

/* ---- Limits and counts --------------------------------------------------- */

GRCORE_Result grcore_handles_set_limits(
    GRCORE_HandleTable * t, size_t per_holder, size_t total) {
  if (t == NULL || per_holder == 0 || total == 0 ||
      (total != GRCORE_HANDLES_UNLIMITED && total > GRCORE_HANDLES_MAX_ENTRIES)) {
    return GRCORE_ERR_INVALID;
  }
  lock(t);
  t->limit_per_holder = per_holder;
  t->limit_total =
      total == GRCORE_HANDLES_UNLIMITED ? GRCORE_HANDLES_MAX_ENTRIES : total;
  unlock(t);
  return GRCORE_OK;
}

size_t grcore_handles_limit_per_holder(GRCORE_HandleTable * t) {
  if (t == NULL) {
    return 0;
  }
  lock(t);
  size_t n = t->limit_per_holder;
  unlock(t);
  return n;
}

size_t grcore_handles_limit_total(GRCORE_HandleTable * t) {
  if (t == NULL) {
    return 0;
  }
  lock(t);
  size_t n = t->limit_total;
  unlock(t);
  return n;
}

size_t grcore_handles_count(GRCORE_HandleTable * t) {
  if (t == NULL) {
    return 0;
  }
  lock(t);
  size_t n = t->live;
  unlock(t);
  return n;
}

size_t grcore_handles_tombstone_count(GRCORE_HandleTable * t) {
  if (t == NULL) {
    return 0;
  }
  lock(t);
  size_t n = t->tombstones;
  unlock(t);
  return n;
}

size_t grcore_handles_held_count(GRCORE_HandleTable * t, GRCORE_Context * c) {
  if (t == NULL || c == NULL || c->group != t->group ||
      !grcore_context_owned_by_caller(c)) {
    return 0;
  }
  lock(t);
  size_t n = c->handle_party == NULL ? 0 : c->handle_party->held_count;
  unlock(t);
  return n;
}

size_t grcore_handles_owned_count(GRCORE_HandleTable * t, GRCORE_Context * c) {
  if (t == NULL || c == NULL || c->group != t->group ||
      !grcore_context_owned_by_caller(c)) {
    return 0;
  }
  lock(t);
  size_t n = c->handle_party == NULL ? 0 : c->handle_party->owned_count;
  unlock(t);
  return n;
}
