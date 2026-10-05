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
 * The sampling profiler: a keyed OBSERVE service, one request kind and an
 * optional timer thread (b/profile.h).
 *
 * The handler allocates nothing: the whole table is one block taken at
 * attach. A sample is a frame walk counted into a fixed hash table keyed by
 * the file's text and the line, never by pointer (an engine may hand out two
 * pointers to equal strings).
 */

/* pthread_condattr_setclock and CLOCK_MONOTONIC are POSIX, and -std=c17 hides
 * them (see cond_clock_internal.h for the clock a timeout is measured on). */
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/frame.h>
#include <ghoti.io/runtime-core/b/profile.h>

#include "cond_clock_internal.h"
#include "context_internal.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef struct Entry {
  const char * file; ///< The pointer first seen; it outlives the context's use.
  int line;
  uint64_t self;
  uint64_t inclusive;
  uint64_t seen; ///< The last sample that counted this entry inclusively.
} Entry;

typedef struct Timer {
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool stop;               ///< Guarded by `mutex`.
  uint64_t interval_us;
  GRCORE_Port * port;      ///< A retained reference; the thread's only link.
  GRCORE_RequestKind kind;
} Timer;

struct GRCORE_Profiler {
  GRCORE_Context * context;
  GRCORE_Port * port;      ///< Retained for the profiler's life.
  GRCORE_RequestKind kind;
  size_t capacity;         ///< Entries.
  size_t buckets;          ///< A power of two, at least twice the capacity.
  size_t used;             ///< Entries in use.
  Entry * entries;
  uint32_t * table;        ///< Entry index plus one, or zero for empty.
  uint64_t samples;
  uint64_t no_frame;
  uint64_t dropped;
  Timer * timer;           ///< Owner's; NULL when no timer runs.
};

static void profiler_destroy(GRCORE_Context * context, void * value);
static void profiler_poll(
    GRCORE_Context * context, void * value, GRCORE_PollCall * call);

/* No snapshot hook: a profiler is not part of any snapshot. */
static const GRCORE_Key profiler_key = GRCORE_KEY_INIT("runtime-core.profiler",
    GRCORE_CARDINALITY_ONE, GRCORE_PHASE_OBSERVE, profiler_destroy,
    profiler_poll, NULL, NULL, NULL);

/* ---- The table ---------------------------------------------------------- */

static uint64_t hash_location(const char * file, int line) {
  uint64_t h = UINT64_C(0xCBF29CE484222325); /* FNV-1a */
  if (file != NULL) {
    for (const unsigned char * p = (const unsigned char *)file; *p != 0; p++) {
      h = (h ^ *p) * UINT64_C(0x100000001B3);
    }
  }
  h = (h ^ (uint64_t)(uint32_t)line) * UINT64_C(0x100000001B3);
  h ^= h >> 32;
  return h;
}

static bool same_location(const Entry * e, const char * file, int line) {
  if (e->line != line) {
    return false;
  }
  if (e->file == file) {
    return true;
  }
  return e->file != NULL && file != NULL && strcmp(e->file, file) == 0;
}

/* The entry for a location, adding it if there is room; NULL when full. */
static Entry * find_entry(GRCORE_Profiler * p, const char * file, int line) {
  size_t mask = p->buckets - 1;
  size_t i = (size_t)hash_location(file, line) & mask;
  for (;;) {
    uint32_t slot = p->table[i];
    if (slot == 0) {
      break;
    }
    Entry * e = &p->entries[slot - 1];
    if (same_location(e, file, line)) {
      return e;
    }
    i = (i + 1) & mask;
  }
  if (p->used == p->capacity) {
    return NULL;
  }
  Entry * e = &p->entries[p->used];
  e->file = file;
  e->line = line;
  e->self = 0;
  e->inclusive = 0;
  e->seen = 0;
  p->used++;
  p->table[i] = (uint32_t)p->used;
  return e;
}

/* ---- The handler -------------------------------------------------------- */

/* One sample. Reads frames, writes only this profiler's own counters, and
 * allocates nothing (AD-5: an OBSERVE handler is read-only and commutes). */
static void take_sample(GRCORE_Profiler * p) {
  p->samples++;
  GRCORE_FrameWalk walk;
  GRCORE_AbstractFrame frame;
  if (grcore_frame_walk_begin(p->context, &walk) != GRCORE_OK ||
      !grcore_frame_walk_next(&walk, &frame)) {
    p->no_frame++;
    return;
  }
  bool innermost = true;
  do {
    Entry * e = find_entry(p, frame.location.file, frame.location.line);
    if (e == NULL) {
      p->dropped++;
    } else {
      if (e->seen != p->samples) {
        e->seen = p->samples; /* a recursion counts once per sample */
        e->inclusive++;
      }
      if (innermost) {
        e->self++;
      }
    }
    innermost = false;
  } while (grcore_frame_walk_next(&walk, &frame));
}

static void profiler_poll(
    GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
  GRCORE_Profiler * p = value;
  if (!grcore_pollcall_pending(call, p->kind)) {
    return;
  }
  /* The request is this service's own: it is cleared here, as the debugger
   * and the JIT clear theirs. A failure to clear cannot happen for a service
   * kind, and would only take one more sample. */
  (void)grcore_context_clear_request(context, p->kind);
  take_sample(p);
}

/* ---- The timer ---------------------------------------------------------- */

static void * timer_main(void * arg) {
  Timer * t = arg;
  pthread_mutex_lock(&t->mutex);
  while (!t->stop) {
    struct timespec deadline;
    grcore_cond_deadline(&deadline, t->interval_us * UINT64_C(1000));
    int rc = 0;
    while (!t->stop && rc != ETIMEDOUT) {
      rc = pthread_cond_timedwait(&t->cond, &t->mutex, &deadline);
      if (rc != 0 && rc != ETIMEDOUT) {
        /* A hard error (EINVAL, ...) would spin here, and the destructor's
         * join would wait on a thread that never looks at `stop` again. The
         * timer ends; the profiler keeps working with posts from the host. */
        pthread_mutex_unlock(&t->mutex);
        return NULL;
      }
    }
    if (t->stop) {
      break;
    }
    /* A refused post (the context is gone) is the end of the timer's use;
     * the destructor stops it anyway, so ignore it and keep waiting. */
    pthread_mutex_unlock(&t->mutex);
    (void)grcore_port_post(t->port, t->kind);
    pthread_mutex_lock(&t->mutex);
  }
  pthread_mutex_unlock(&t->mutex);
  return NULL;
}

static void timer_destroy(GRCORE_Profiler * p) {
  Timer * t = p->timer;
  if (t == NULL) {
    return;
  }
  pthread_mutex_lock(&t->mutex);
  t->stop = true;
  pthread_cond_signal(&t->cond);
  pthread_mutex_unlock(&t->mutex);
  pthread_join(t->thread, NULL);
  pthread_cond_destroy(&t->cond);
  pthread_mutex_destroy(&t->mutex);
  grcore_port_release(t->port);
  const GRCORE_Allocator * a = grcore_context_allocator(p->context);
  a->free_fn(a->ctx, t);
  p->timer = NULL;
}

/* ---- Attach and teardown ------------------------------------------------ */

static void profiler_free(GRCORE_Profiler * p) {
  const GRCORE_Allocator * a = grcore_context_allocator(p->context);
  grcore_port_release(p->port);
  a->free_fn(a->ctx, p->entries);
  a->free_fn(a->ctx, p->table);
  a->free_fn(a->ctx, p);
}

static void profiler_destroy(GRCORE_Context * context, void * value) {
  (void)context;
  GRCORE_Profiler * p = value;
  timer_destroy(p); /* before anything the timer's post could touch is freed */
  profiler_free(p);
}

GRCORE_Result grcore_profiler_attach(GRCORE_Context * context, size_t capacity,
    GRCORE_Profiler ** out_profiler) {
  if (context == NULL || out_profiler == NULL ||
      !grcore_context_owned_by_caller(context) ||
      grcore_context_slot(context, &profiler_key) != NULL) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_ContextState state = grcore_context_state(context);
  if (state == GRCORE_CONTEXT_RUNNING) {
    return GRCORE_ERR_INVALID;
  }
  if (capacity == 0) {
    capacity = GRCORE_PROFILER_DEFAULT_CAPACITY;
  }
  if (capacity > GRCORE_PROFILER_MAX_CAPACITY) {
    return GRCORE_ERR_LIMIT;
  }
  const GRCORE_Allocator * a = grcore_context_allocator(context);
  GRCORE_Profiler * p = a->calloc_fn(a->ctx, 1, sizeof *p);
  if (p == NULL) {
    return GRCORE_ERR_OOM;
  }
  p->context = context;
  p->capacity = capacity;
  p->buckets = 2;
  while (p->buckets < capacity * 2) {
    p->buckets <<= 1;
  }
  p->entries = a->calloc_fn(a->ctx, capacity, sizeof *p->entries);
  p->table = a->calloc_fn(a->ctx, p->buckets, sizeof *p->table);
  GRCORE_Result result = GRCORE_OK;
  if (p->entries == NULL || p->table == NULL) {
    result = GRCORE_ERR_OOM;
  } else {
    result = grcore_context_port(context, &p->port);
  }
  if (result == GRCORE_OK) {
    result = grcore_context_request_kind(context, &profiler_key, &p->kind);
    if (result == GRCORE_OK) {
      result = grcore_context_register(context, &profiler_key, p);
    }
    if (result != GRCORE_OK) {
      grcore_port_release(p->port);
    }
  }
  if (result != GRCORE_OK) {
    a->free_fn(a->ctx, p->entries);
    a->free_fn(a->ctx, p->table);
    a->free_fn(a->ctx, p);
    return result;
  }
  *out_profiler = p;
  return GRCORE_OK;
}

GRCORE_Profiler * grcore_profiler_of(const GRCORE_Context * context) {
  return context == NULL ? NULL : grcore_context_slot(context, &profiler_key);
}

GRCORE_RequestKind grcore_profiler_kind(const GRCORE_Profiler * profiler) {
  return profiler == NULL ? GRCORE_REQUEST_TERMINATE : profiler->kind;
}

GRCORE_Result grcore_profiler_request(const GRCORE_Profiler * profiler) {
  if (profiler == NULL) {
    return GRCORE_ERR_INVALID;
  }
  return grcore_port_post(profiler->port, profiler->kind);
}

GRCORE_Result grcore_profiler_timer_start(
    GRCORE_Profiler * profiler, uint64_t interval_us) {
  if (profiler == NULL || interval_us < GRCORE_PROFILER_MIN_INTERVAL_US ||
      profiler->timer != NULL ||
      !grcore_context_owned_by_caller(profiler->context)) {
    return GRCORE_ERR_INVALID;
  }
  if (interval_us > GRCORE_PROFILER_MAX_INTERVAL_US) {
    return GRCORE_ERR_LIMIT;
  }
  const GRCORE_Allocator * a = grcore_context_allocator(profiler->context);
  Timer * t = a->calloc_fn(a->ctx, 1, sizeof *t);
  if (t == NULL) {
    return GRCORE_ERR_OOM;
  }
  if (pthread_mutex_init(&t->mutex, NULL) != 0) {
    a->free_fn(a->ctx, t);
    return GRCORE_ERR_OOM;
  }
  if (grcore_cond_init(&t->cond) != 0) {
    pthread_mutex_destroy(&t->mutex);
    a->free_fn(a->ctx, t);
    return GRCORE_ERR_OOM;
  }
  t->interval_us = interval_us;
  t->kind = profiler->kind;
  t->port = grcore_port_retain(profiler->port);
  /* The timer thread starts with every signal blocked, so a process-directed
   * signal (SIGINT, SIGTERM, a host's SIGUSR1) is delivered to a thread the
   * host chose and never runs the host's handler on this one. The new thread
   * inherits the mask in effect at pthread_create; the caller's is restored
   * straight after. */
#ifndef _WIN32
  sigset_t all, previous;
  sigfillset(&all);
  bool masked = pthread_sigmask(SIG_BLOCK, &all, &previous) == 0;
#endif
  int created = pthread_create(&t->thread, NULL, timer_main, t);
#ifndef _WIN32
  if (masked) {
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
  }
#endif
  if (created != 0) {
    grcore_port_release(t->port);
    pthread_cond_destroy(&t->cond);
    pthread_mutex_destroy(&t->mutex);
    a->free_fn(a->ctx, t);
    return GRCORE_ERR_OOM;
  }
  profiler->timer = t;
  return GRCORE_OK;
}

GRCORE_Result grcore_profiler_timer_stop(GRCORE_Profiler * profiler) {
  if (profiler == NULL || !grcore_context_owned_by_caller(profiler->context)) {
    return GRCORE_ERR_INVALID;
  }
  timer_destroy(profiler);
  return GRCORE_OK;
}

bool grcore_profiler_timer_running(const GRCORE_Profiler * profiler) {
  return profiler != NULL && profiler->timer != NULL;
}

/* ---- Reading ------------------------------------------------------------ */

static bool readable(const GRCORE_Profiler * p) {
  return p != NULL && grcore_context_owned_by_caller(p->context) &&
      grcore_context_state(p->context) != GRCORE_CONTEXT_AT_POLL;
}

/* Hotter first: self, then inclusive, then file text and line. */
static bool before(const Entry * a, const Entry * b) {
  if (a->self != b->self) {
    return a->self > b->self;
  }
  if (a->inclusive != b->inclusive) {
    return a->inclusive > b->inclusive;
  }
  if (a->file != b->file) {
    if (a->file == NULL || b->file == NULL) {
      return a->file == NULL;
    }
    int c = strcmp(a->file, b->file);
    if (c != 0) {
      return c < 0;
    }
  }
  return a->line < b->line;
}

GRCORE_Result grcore_profiler_report(const GRCORE_Profiler * profiler,
    GRCORE_ProfileEntry * entries, size_t capacity, size_t * out_count,
    GRCORE_ProfileTotals * out_totals) {
  if (!readable(profiler) || (entries == NULL && capacity != 0)) {
    return GRCORE_ERR_INVALID;
  }
  /* Insertion into the caller's array, kept sorted: the best `capacity`
   * entries, with no scratch memory. */
  size_t count = 0;
  for (size_t i = 0; i < profiler->used && capacity != 0; i++) {
    const Entry * e = &profiler->entries[i];
    size_t pos = count;
    while (pos > 0) {
      Entry other = {entries[pos - 1].file, entries[pos - 1].line,
          entries[pos - 1].self, entries[pos - 1].inclusive, 0};
      if (!before(e, &other)) {
        break;
      }
      pos--;
    }
    if (pos >= capacity) {
      continue; /* worse than everything kept, and the array is full */
    }
    size_t last = count < capacity ? count : capacity - 1;
    for (size_t j = last; j > pos; j--) {
      entries[j] = entries[j - 1];
    }
    entries[pos].file = e->file;
    entries[pos].line = e->line;
    entries[pos].self = e->self;
    entries[pos].inclusive = e->inclusive;
    if (count < capacity) {
      count++;
    }
  }
  if (out_count != NULL) {
    *out_count = count;
  }
  if (out_totals != NULL) {
    out_totals->samples = profiler->samples;
    out_totals->no_frame = profiler->no_frame;
    out_totals->dropped = profiler->dropped;
    out_totals->locations = profiler->used;
  }
  return GRCORE_OK;
}

GRCORE_Result grcore_profiler_reset(GRCORE_Profiler * profiler) {
  if (!readable(profiler)) {
    return GRCORE_ERR_INVALID;
  }
  memset(profiler->table, 0, profiler->buckets * sizeof *profiler->table);
  memset(profiler->entries, 0, profiler->capacity * sizeof *profiler->entries);
  profiler->used = 0;
  profiler->samples = 0;
  profiler->no_frame = 0;
  profiler->dropped = 0;
  return GRCORE_OK;
}
