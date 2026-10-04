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
 * Context snapshots (AD-12, AD-19, AD-20): the snapshot object, the walk over
 * a context's keys that makes one, and the restore that applies one.
 *
 * The core knows only keys and bytes. Each key with hooks writes one blob; a
 * restore hands each blob back to the destination's key of the same name.
 * Atomicity is the structure of the restore, not the hooks' care: a CHECK pass
 * that changes nothing, an APPLY pass whose effects an ABANDON settle undoes,
 * a PREPARE pass that proves COMMIT cannot fail, and then COMMIT.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "context_internal.h"

#include <ghoti.io/runtime-core/b/snapshot.h>

#include <stdint.h>
#include <string.h>

typedef struct Blob {
  char * name;
  unsigned char * data;
  size_t size;
} Blob;

struct GRCORE_Snapshot {
  GRCORE_Allocator allocator; ///< A copy: the snapshot outlives its caller's.
  size_t refs;                ///< `__atomic` builtins only.
  bool paused;
  int line;                   ///< The source's pause line; zero if unknown.
  Blob * blobs;
  size_t blob_count;
  size_t blob_capacity;
  size_t total;               ///< Bytes in all the blobs.
};

struct GRCORE_SnapshotWriter {
  GRCORE_Snapshot * snapshot;
  Blob * blob;
  size_t capacity;
};

struct GRCORE_SnapshotReader {
  const unsigned char * data;
  size_t size;
  size_t at;
  bool failed;
};

/* ---- the snapshot object ------------------------------------------------ */

static void snapshot_free(GRCORE_Snapshot * s) {
  GRCORE_Allocator a = s->allocator;
  for (size_t i = 0; i < s->blob_count; i++) {
    a.free_fn(a.ctx, s->blobs[i].name);
    a.free_fn(a.ctx, s->blobs[i].data);
  }
  a.free_fn(a.ctx, s->blobs);
  a.free_fn(a.ctx, s);
}

GRCORE_Snapshot * grcore_snapshot_retain(GRCORE_Snapshot * snapshot) {
  if (snapshot != NULL) {
    __atomic_add_fetch(&snapshot->refs, 1, __ATOMIC_RELAXED);
  }
  return snapshot;
}

void grcore_snapshot_release(GRCORE_Snapshot * snapshot) {
  if (snapshot == NULL) {
    return;
  }
  if (__atomic_sub_fetch(&snapshot->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    snapshot_free(snapshot);
  }
}

size_t grcore_snapshot_refcount(const GRCORE_Snapshot * snapshot) {
  return snapshot == NULL
      ? 0
      : __atomic_load_n(&snapshot->refs, __ATOMIC_ACQUIRE);
}

size_t grcore_snapshot_size(const GRCORE_Snapshot * snapshot) {
  return snapshot == NULL ? 0 : snapshot->total;
}

bool grcore_snapshot_was_paused(const GRCORE_Snapshot * snapshot) {
  return snapshot != NULL && snapshot->paused;
}

size_t grcore_snapshot_blob_count(const GRCORE_Snapshot * snapshot) {
  return snapshot == NULL ? 0 : snapshot->blob_count;
}

const char * grcore_snapshot_blob_name(
    const GRCORE_Snapshot * snapshot, size_t index) {
  if (snapshot == NULL || index >= snapshot->blob_count) {
    return NULL;
  }
  return snapshot->blobs[index].name;
}

GRCORE_Result grcore_snapshot_blob(const GRCORE_Snapshot * snapshot,
    const char * name, const void ** out_data, size_t * out_size) {
  if (snapshot == NULL || name == NULL || out_data == NULL ||
      out_size == NULL) {
    return GRCORE_ERR_INVALID;
  }
  for (size_t i = 0; i < snapshot->blob_count; i++) {
    if (strcmp(snapshot->blobs[i].name, name) == 0) {
      *out_data = snapshot->blobs[i].data;
      *out_size = snapshot->blobs[i].size;
      return GRCORE_OK;
    }
  }
  return GRCORE_ERR_INVALID;
}

/* ---- the writer --------------------------------------------------------- */

GRCORE_Result grcore_snapshot_writer_write(
    GRCORE_SnapshotWriter * writer, const void * data, size_t size) {
  if (writer == NULL || (data == NULL && size != 0)) {
    return GRCORE_ERR_INVALID;
  }
  if (size == 0) {
    return GRCORE_OK;
  }
  GRCORE_Snapshot * s = writer->snapshot;
  Blob * b = writer->blob;
  if (size > GRCORE_SNAPSHOT_MAX_BYTES ||
      s->total > GRCORE_SNAPSHOT_MAX_BYTES - size) {
    return GRCORE_ERR_LIMIT;
  }
  if (b->size + size > writer->capacity) {
    size_t capacity = writer->capacity == 0 ? 256 : writer->capacity;
    while (capacity < b->size + size) {
      if (capacity > SIZE_MAX / 2) {
        return GRCORE_ERR_OOM;
      }
      capacity *= 2;
    }
    unsigned char * grown =
        s->allocator.realloc_fn(s->allocator.ctx, b->data, capacity);
    if (grown == NULL) {
      return GRCORE_ERR_OOM;
    }
    b->data = grown;
    writer->capacity = capacity;
  }
  memcpy(b->data + b->size, data, size);
  b->size += size;
  s->total += size;
  return GRCORE_OK;
}

GRCORE_Result grcore_snapshot_writer_u64(
    GRCORE_SnapshotWriter * writer, uint64_t value) {
  return grcore_snapshot_writer_write(writer, &value, sizeof value);
}

GRCORE_Result grcore_snapshot_writer_string(
    GRCORE_SnapshotWriter * writer, const char * text) {
  size_t length = text == NULL ? 0 : strlen(text);
  GRCORE_Result r = grcore_snapshot_writer_u64(writer, length);
  if (r != GRCORE_OK) {
    return r;
  }
  /* The terminator is written too, so a reader can hand back a C string. */
  return grcore_snapshot_writer_write(
      writer, text == NULL ? "" : text, length + 1);
}

size_t grcore_snapshot_writer_size(const GRCORE_SnapshotWriter * writer) {
  return writer == NULL ? 0 : writer->blob->size;
}

const GRCORE_Allocator * grcore_snapshot_writer_allocator(
    const GRCORE_SnapshotWriter * writer) {
  return writer == NULL ? NULL : &writer->snapshot->allocator;
}

/* ---- the reader --------------------------------------------------------- */

GRCORE_Result grcore_snapshot_reader_view(
    GRCORE_SnapshotReader * reader, size_t size, const void ** out_data) {
  if (reader == NULL || out_data == NULL) {
    return GRCORE_ERR_INVALID;
  }
  if (reader->failed || size > reader->size - reader->at) {
    reader->failed = true;
    return GRCORE_ERR_CORRUPT;
  }
  *out_data = reader->data + reader->at;
  reader->at += size;
  return GRCORE_OK;
}

GRCORE_Result grcore_snapshot_reader_read(
    GRCORE_SnapshotReader * reader, void * out, size_t size) {
  const void * view;
  if (out == NULL && size != 0) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Result r = grcore_snapshot_reader_view(reader, size, &view);
  if (r == GRCORE_OK && size != 0) {
    memcpy(out, view, size);
  }
  return r;
}

GRCORE_Result grcore_snapshot_reader_u64(
    GRCORE_SnapshotReader * reader, uint64_t * out_value) {
  if (out_value == NULL) {
    return GRCORE_ERR_INVALID;
  }
  return grcore_snapshot_reader_read(reader, out_value, sizeof *out_value);
}

GRCORE_Result grcore_snapshot_reader_string(GRCORE_SnapshotReader * reader,
    const char ** out_text, size_t * out_length) {
  uint64_t length;
  const void * view;
  if (out_text == NULL) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Result r = grcore_snapshot_reader_u64(reader, &length);
  if (r != GRCORE_OK) {
    return r;
  }
  if (length >= reader->size - reader->at) {
    reader->failed = true;
    return GRCORE_ERR_CORRUPT;
  }
  r = grcore_snapshot_reader_view(reader, (size_t)length + 1, &view);
  if (r != GRCORE_OK) {
    return r;
  }
  if (((const char *)view)[length] != '\0') {
    reader->failed = true;
    return GRCORE_ERR_CORRUPT;
  }
  *out_text = view;
  if (out_length != NULL) {
    *out_length = (size_t)length;
  }
  return GRCORE_OK;
}

size_t grcore_snapshot_reader_remaining(const GRCORE_SnapshotReader * reader) {
  return reader == NULL || reader->failed ? 0 : reader->size - reader->at;
}

/* ---- taking ------------------------------------------------------------- */

static bool hooked(const GRCORE_Key * key) {
  return key->snapshot != NULL;
}

GRCORE_Result grcore_context_snapshot(GRCORE_Context * context,
    const GRCORE_Allocator * allocator, GRCORE_Snapshot ** out_snapshot) {
  if (context == NULL || out_snapshot == NULL ||
      !grcore_context_owned_by_caller(context) || context->tearing_down ||
      (context->config != GRCORE_CONFIG_PAUSED &&
          context->config != GRCORE_CONFIG_PARKED_OUTSIDE) ||
      context->fuel_scope_count != 0 || context->nested != 0) {
    return GRCORE_ERR_INVALID;
  }
  /* Everything that can be refused for the keys' shape is refused before
   * anything is allocated: an incomplete set of hooks, a key with no name or
   * more than one registration, two blobs of one name. */
  for (size_t i = 0; i < context->registration_count; i++) {
    const GRCORE_Key * k = context->registrations[i].key;
    if (!hooked(k)) {
      continue;
    }
    if (k->restore == NULL || k->settle == NULL || k->name == NULL ||
        k->cardinality != GRCORE_CARDINALITY_ONE) {
      return GRCORE_ERR_INVALID;
    }
    for (size_t j = 0; j < i; j++) {
      const GRCORE_Key * other = context->registrations[j].key;
      if (hooked(other) && strcmp(other->name, k->name) == 0) {
        return GRCORE_ERR_INVALID;
      }
    }
  }
  const GRCORE_Allocator * a =
      allocator != NULL ? allocator : grcore_allocator_default();
  GRCORE_Snapshot * s = a->calloc_fn(a->ctx, 1, sizeof *s);
  if (s == NULL) {
    return GRCORE_ERR_OOM;
  }
  s->allocator = *a;
  s->refs = 1;
  s->paused = context->config == GRCORE_CONFIG_PAUSED;
  s->line = s->paused ? context->pause_location.line : 0;
  GRCORE_Result r = GRCORE_OK;
  for (size_t i = 0; i < context->registration_count && r == GRCORE_OK; i++) {
    const GRCORE_Key * k = context->registrations[i].key;
    if (!hooked(k)) {
      continue;
    }
    if (s->blob_count == s->blob_capacity) {
      size_t capacity = s->blob_capacity == 0 ? 4 : s->blob_capacity * 2;
      Blob * grown = a->realloc_fn(a->ctx, s->blobs, capacity * sizeof *grown);
      if (grown == NULL) {
        r = GRCORE_ERR_OOM;
        break;
      }
      s->blobs = grown;
      s->blob_capacity = capacity;
    }
    size_t length = strlen(k->name);
    Blob * b = &s->blobs[s->blob_count];
    b->name = a->malloc_fn(a->ctx, length + 1);
    if (b->name == NULL) {
      r = GRCORE_ERR_OOM;
      break;
    }
    memcpy(b->name, k->name, length + 1);
    b->data = NULL;
    b->size = 0;
    s->blob_count++; /* counted first, so a failure frees the name */
    GRCORE_SnapshotWriter w = {s, b, 0};
    r = k->snapshot(context, context->registrations[i].value, &w);
  }
  if (r != GRCORE_OK) {
    snapshot_free(s);
    return r;
  }
  *out_snapshot = s;
  return GRCORE_OK;
}

/* ---- restoring ---------------------------------------------------------- */

/* The destination registration whose key is called `name`, or NULL. */
static const GRCORE_Registration * find_by_name(
    const GRCORE_Context * c, const char * name) {
  for (size_t i = 0; i < c->registration_count; i++) {
    const GRCORE_Key * k = c->registrations[i].key;
    if (hooked(k) && strcmp(k->name, name) == 0) {
      return &c->registrations[i];
    }
  }
  return NULL;
}

static void * environment(const GRCORE_RestoreEnv * env, const char * name) {
  return env != NULL && env->lookup != NULL ? env->lookup(env->user, name)
                                            : NULL;
}

static GRCORE_Result run_restore(GRCORE_Context * c, const GRCORE_Snapshot * s,
    size_t index, const GRCORE_RestoreEnv * env, GRCORE_RestoreMode mode) {
  const Blob * b = &s->blobs[index];
  const GRCORE_Registration * reg = find_by_name(c, b->name);
  GRCORE_SnapshotReader reader = {b->data, b->size, 0, false};
  return reg->key->restore(c, reg->value, &reader, environment(env, b->name),
      mode);
}

/* Undoes the APPLY of the first `applied` blobs, newest first. */
static void abandon(GRCORE_Context * c, const GRCORE_Snapshot * s,
    size_t applied, const GRCORE_RestoreEnv * env) {
  while (applied > 0) {
    const Blob * b = &s->blobs[--applied];
    const GRCORE_Registration * reg = find_by_name(c, b->name);
    (void)reg->key->settle(
        c, reg->value, environment(env, b->name), GRCORE_SETTLE_ABANDON);
  }
}

GRCORE_Result grcore_context_restore(GRCORE_Context * context,
    const GRCORE_Snapshot * snapshot, const GRCORE_RestoreEnv * env) {
  if (context == NULL || snapshot == NULL ||
      !grcore_context_owned_by_caller(context) || context->tearing_down ||
      context->config != GRCORE_CONFIG_PARKED_OUTSIDE ||
      context->fuel_scope_count != 0 || context->nested != 0 ||
      (snapshot->paused && (env == NULL || env->entry == NULL))) {
    return GRCORE_ERR_INVALID;
  }
  /* The key set must be the snapshot's, exactly: a blob with no key on this
   * side has nowhere to go, and a key with hooks and no blob would be left as
   * a fresh one in a context that is otherwise not. */
  for (size_t i = 0; i < snapshot->blob_count; i++) {
    if (find_by_name(context, snapshot->blobs[i].name) == NULL) {
      return GRCORE_ERR_INVALID;
    }
  }
  size_t destination_hooked = 0;
  for (size_t i = 0; i < context->registration_count; i++) {
    const GRCORE_Key * k = context->registrations[i].key;
    if (!hooked(k)) {
      continue;
    }
    if (k->restore == NULL || k->settle == NULL || k->name == NULL ||
        k->cardinality != GRCORE_CARDINALITY_ONE) {
      return GRCORE_ERR_INVALID;
    }
    destination_hooked++;
  }
  if (destination_hooked != snapshot->blob_count) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Result r = GRCORE_OK;
  for (size_t i = 0; i < snapshot->blob_count && r == GRCORE_OK; i++) {
    r = run_restore(context, snapshot, i, env, GRCORE_RESTORE_CHECK);
  }
  if (r != GRCORE_OK) {
    return r;
  }
  size_t applied = 0;
  while (applied < snapshot->blob_count && r == GRCORE_OK) {
    r = run_restore(context, snapshot, applied, env, GRCORE_RESTORE_APPLY);
    if (r == GRCORE_OK) {
      applied++;
    }
  }
  if (r != GRCORE_OK) {
    abandon(context, snapshot, applied, env);
    return r;
  }
  /* The destination's registration order, which is what a key that depends on
   * another being registered first relies on. */
  for (size_t i = 0; i < context->registration_count && r == GRCORE_OK; i++) {
    const GRCORE_Registration * reg = &context->registrations[i];
    if (hooked(reg->key)) {
      r = reg->key->settle(context, reg->value,
          environment(env, reg->key->name), GRCORE_SETTLE_PREPARE);
    }
  }
  if (r != GRCORE_OK) {
    abandon(context, snapshot, applied, env);
    return r;
  }
  for (size_t i = 0; i < context->registration_count; i++) {
    const GRCORE_Registration * reg = &context->registrations[i];
    if (hooked(reg->key)) {
      (void)reg->key->settle(context, reg->value,
          environment(env, reg->key->name), GRCORE_SETTLE_COMMIT);
    }
  }
  if (snapshot->paused) {
    context->entry = env->entry;
    context->entry_state = env->entry_state;
    context->pause_location.file = env->pause_file;
    context->pause_location.line = snapshot->line;
    context->last_verdict = GRCORE_VERDICT_PAUSE;
    context->config = GRCORE_CONFIG_PAUSED;
  }
  grcore_context_refresh_derived(context);
  return GRCORE_OK;
}
