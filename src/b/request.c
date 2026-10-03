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
 * Requests and ports (AD-4, AD-19).
 *
 * The context's request word is atomic and is the only thing the poll's fast
 * path reads. A port is the one way another thread writes to it: under the
 * port's mutex, so that a post and the context's destruction cannot overlap,
 * and so that a context waiting on the condition variable cannot miss a wake.
 */

/* pthread_condattr_setclock and CLOCK_MONOTONIC are POSIX, and -std=c17 hides
 * them. A timeout must not move when the wall clock does. */
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/runtime-core/macros.h>

#include "context_internal.h"
#include "group_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define WORD_BIT(kind) (UINT64_C(1) << (kind))
#define OVERFLOW_SIGNAL WORD_BIT(63)

struct GRCORE_Port {
  GRCORE_Group * group;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  GRCORE_Context * context; ///< NULL once the context is destroyed. Guarded.
  size_t refs;              ///< `__atomic` builtins only.
  uint64_t * overflow;      ///< Bit i is kind 63 + i. Guarded.
  size_t overflow_words;
  size_t overflow_set;      ///< How many bits are set. Guarded.
};

/* Whether `kind` is one a post or a clear may name, and so one that has to be
 * defined if it is a service kind. */
static bool kind_defined(
    const GRCORE_Context * context, GRCORE_RequestKind kind) {
  return kind < GRCORE_REQUEST_FIRST_KEYED ||
      kind - GRCORE_REQUEST_FIRST_KEYED <
      __atomic_load_n(&context->kind_count, __ATOMIC_ACQUIRE);
}

GRCORE_Result grcore_context_request_kind(GRCORE_Context * context,
    const GRCORE_Key * key, GRCORE_RequestKind * out_kind) {
  if (context == NULL || key == NULL || out_kind == NULL ||
      !grcore_context_owned_by_caller(context) || context->tearing_down ||
      context->config == GRCORE_CONFIG_RUNNING) {
    return GRCORE_ERR_INVALID;
  }
  uint32_t count = context->kind_count;
  if (count >= UINT32_MAX - GRCORE_REQUEST_FIRST_KEYED) {
    return GRCORE_ERR_LIMIT;
  }
  if (count == context->kind_capacity) {
    const GRCORE_Allocator * a = context->group->allocator;
    size_t capacity = count == 0 ? 8 : (size_t)count * 2;
    const GRCORE_Key ** grown =
        a->realloc_fn(a->ctx, context->kind_keys, capacity * sizeof *grown);
    if (grown == NULL) {
      return GRCORE_ERR_OOM;
    }
    context->kind_keys = grown;
    context->kind_capacity = capacity;
  }
  context->kind_keys[count] = key;
  /* Release: a port that sees the new count may post the new kind. */
  __atomic_store_n(&context->kind_count, count + 1, __ATOMIC_RELEASE);
  *out_kind = GRCORE_REQUEST_FIRST_KEYED + count;
  return GRCORE_OK;
}

const GRCORE_Key * grcore_context_request_kind_key(
    const GRCORE_Context * context, GRCORE_RequestKind kind) {
  if (context == NULL || kind < GRCORE_REQUEST_FIRST_KEYED ||
      kind - GRCORE_REQUEST_FIRST_KEYED >= context->kind_count) {
    return NULL;
  }
  return context->kind_keys[kind - GRCORE_REQUEST_FIRST_KEYED];
}

static GRCORE_Result port_create(GRCORE_Context * context, GRCORE_Port ** out) {
  GRCORE_Group * group = context->group;
  const GRCORE_Allocator * a = &group->counting.allocator;
  GRCORE_Port * p = a->calloc_fn(a->ctx, 1, sizeof *p);
  if (p == NULL) {
    return GRCORE_ERR_OOM;
  }
  pthread_condattr_t attr;
  if (pthread_mutex_init(&p->mutex, NULL) != 0) {
    a->free_fn(a->ctx, p);
    return GRCORE_ERR_OOM;
  }
  if (pthread_condattr_init(&attr) != 0) {
    pthread_mutex_destroy(&p->mutex);
    a->free_fn(a->ctx, p);
    return GRCORE_ERR_OOM;
  }
  int bad = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  if (bad == 0) {
    bad = pthread_cond_init(&p->cond, &attr);
  }
  pthread_condattr_destroy(&attr);
  if (bad != 0) {
    pthread_mutex_destroy(&p->mutex);
    a->free_fn(a->ctx, p);
    return GRCORE_ERR_OOM;
  }
  p->group = group;
  p->context = context;
  p->refs = 1; /* the context's */
  grcore_group_port_enter(group);
  *out = p;
  return GRCORE_OK;
}

static void port_free(GRCORE_Port * p) {
  GRCORE_Group * group = p->group;
  const GRCORE_Allocator * a = &group->counting.allocator;
  pthread_cond_destroy(&p->cond);
  pthread_mutex_destroy(&p->mutex);
  a->free_fn(a->ctx, p->overflow);
  a->free_fn(a->ctx, p);
  /* Last, because the group may be destroyed as soon as the count drops. */
  grcore_group_port_leave(group);
}

GRCORE_Result grcore_context_port_ensure(
    GRCORE_Context * context, GRCORE_Port ** out_port) {
  if (context->port == NULL) {
    GRCORE_Result r = port_create(context, &context->port);
    if (r != GRCORE_OK) {
      return r;
    }
  }
  *out_port = context->port;
  return GRCORE_OK;
}

GRCORE_Result grcore_context_port(
    GRCORE_Context * context, GRCORE_Port ** out_port) {
  if (context == NULL || out_port == NULL ||
      !grcore_context_owned_by_caller(context) || context->tearing_down) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Port * p;
  GRCORE_Result r = grcore_context_port_ensure(context, &p);
  if (r != GRCORE_OK) {
    return r;
  }
  *out_port = grcore_port_retain(p);
  return GRCORE_OK;
}

void grcore_context_port_detach(GRCORE_Context * context) {
  GRCORE_Port * p = context->port;
  if (p == NULL) {
    return;
  }
  pthread_mutex_lock(&p->mutex);
  p->context = NULL;
  pthread_mutex_unlock(&p->mutex);
  context->port = NULL;
  grcore_port_release(p);
}

GRCORE_Port * grcore_port_retain(GRCORE_Port * port) {
  if (port != NULL) {
    __atomic_add_fetch(&port->refs, 1, __ATOMIC_RELAXED);
  }
  return port;
}

void grcore_port_release(GRCORE_Port * port) {
  if (port == NULL) {
    return;
  }
  if (__atomic_sub_fetch(&port->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    port_free(port);
  }
}

/* Sets or clears one bit of the overflow set. The port's mutex is held. */
static GRCORE_Result overflow_set_bit(
    GRCORE_Port * p, GRCORE_RequestKind kind) {
  size_t index = kind - GRCORE_REQUEST_WORD_KINDS;
  size_t word = index / 64;
  if (word >= p->overflow_words) {
    const GRCORE_Allocator * a = &p->group->counting.allocator;
    size_t words = word + 1;
    uint64_t * grown =
        a->realloc_fn(a->ctx, p->overflow, words * sizeof *grown);
    if (grown == NULL) {
      return GRCORE_ERR_OOM;
    }
    memset(grown + p->overflow_words, 0,
        (words - p->overflow_words) * sizeof *grown);
    p->overflow = grown;
    p->overflow_words = words;
  }
  uint64_t bit = UINT64_C(1) << (index % 64);
  if ((p->overflow[word] & bit) == 0) {
    p->overflow[word] |= bit;
    p->overflow_set++;
  }
  return GRCORE_OK;
}

static bool overflow_test(const GRCORE_Port * p, GRCORE_RequestKind kind) {
  size_t index = kind - GRCORE_REQUEST_WORD_KINDS;
  size_t word = index / 64;
  return word < p->overflow_words &&
      (p->overflow[word] & (UINT64_C(1) << (index % 64))) != 0;
}

GRCORE_Result grcore_port_post(GRCORE_Port * port, GRCORE_RequestKind kind) {
  if (port == NULL || kind == GRCORE_REQUEST_FUEL ||
      kind == GRCORE_REQUEST_MEMORY) {
    return GRCORE_ERR_INVALID;
  }
  pthread_mutex_lock(&port->mutex);
  GRCORE_Context * context = port->context;
  if (context == NULL || !kind_defined(context, kind)) {
    pthread_mutex_unlock(&port->mutex);
    return GRCORE_ERR_INVALID;
  }
  uint64_t bit;
  if (kind < GRCORE_REQUEST_WORD_KINDS) {
    bit = WORD_BIT(kind);
  } else {
    GRCORE_Result r = overflow_set_bit(port, kind);
    if (r != GRCORE_OK) {
      pthread_mutex_unlock(&port->mutex);
      return r;
    }
    bit = OVERFLOW_SIGNAL;
  }
  /* Release: what the poster wrote before posting is visible to a poll that
   * sees the bit. */
  __atomic_fetch_or(&context->request_word, bit, __ATOMIC_RELEASE);
  pthread_cond_broadcast(&port->cond);
  pthread_mutex_unlock(&port->mutex);
  return GRCORE_OK;
}

bool grcore_context_request_pending(
    const GRCORE_Context * context, GRCORE_RequestKind kind) {
  if (context == NULL || !kind_defined(context, kind)) {
    return false;
  }
  if (kind < GRCORE_REQUEST_WORD_KINDS) {
    return (__atomic_load_n(&context->request_word, __ATOMIC_ACQUIRE) &
               WORD_BIT(kind)) != 0;
  }
  GRCORE_Port * p = context->port;
  if (p == NULL) {
    return false;
  }
  pthread_mutex_lock(&p->mutex);
  bool pending = overflow_test(p, kind);
  pthread_mutex_unlock(&p->mutex);
  return pending;
}

GRCORE_Result grcore_context_clear_request(
    GRCORE_Context * context, GRCORE_RequestKind kind) {
  if (context == NULL || !grcore_context_owned_by_caller(context) ||
      kind == GRCORE_REQUEST_TERMINATE || kind == GRCORE_REQUEST_FUEL ||
      kind == GRCORE_REQUEST_MEMORY || !kind_defined(context, kind)) {
    return GRCORE_ERR_INVALID;
  }
  if (kind < GRCORE_REQUEST_WORD_KINDS) {
    __atomic_fetch_and(&context->request_word, ~WORD_BIT(kind),
        __ATOMIC_ACQ_REL);
    return GRCORE_OK;
  }
  GRCORE_Port * p = context->port;
  if (p == NULL) {
    return GRCORE_OK;
  }
  pthread_mutex_lock(&p->mutex);
  if (overflow_test(p, kind)) {
    size_t index = kind - GRCORE_REQUEST_WORD_KINDS;
    p->overflow[index / 64] &= ~(UINT64_C(1) << (index % 64));
    if (--p->overflow_set == 0) {
      __atomic_fetch_and(&context->request_word, ~OVERFLOW_SIGNAL,
          __ATOMIC_ACQ_REL);
    }
  }
  pthread_mutex_unlock(&p->mutex);
  return GRCORE_OK;
}

GRCORE_Result grcore_context_terminate(GRCORE_Context * context) {
  if (context == NULL || !grcore_context_owned_by_caller(context) ||
      context->tearing_down) {
    return GRCORE_ERR_INVALID;
  }
  __atomic_fetch_or(&context->request_word, WORD_BIT(GRCORE_REQUEST_TERMINATE),
      __ATOMIC_RELEASE);
  return GRCORE_OK;
}

void grcore_context_clear_terminate(GRCORE_Context * context) {
  __atomic_fetch_and(&context->request_word,
      ~WORD_BIT(GRCORE_REQUEST_TERMINATE), __ATOMIC_ACQ_REL);
}

void grcore_context_clear_edge_requests(GRCORE_Context * context) {
  __atomic_fetch_and(&context->request_word,
      ~(WORD_BIT(GRCORE_REQUEST_TIME) | WORD_BIT(GRCORE_REQUEST_INTERRUPT)),
      __ATOMIC_ACQ_REL);
}

bool grcore_port_wait(
    GRCORE_Port * port, const GRCORE_Context * context, uint64_t timeout_ns) {
  struct timespec deadline = {0, 0};
  if (timeout_ns != UINT64_MAX) {
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    uint64_t secs = timeout_ns / 1000000000u;
    uint64_t nanos = timeout_ns % 1000000000u + (uint64_t)deadline.tv_nsec;
    if (nanos >= 1000000000u) {
      secs++;
      nanos -= 1000000000u;
    }
    /* Far enough ahead to be forever, and short of overflowing time_t. */
    if (secs > (uint64_t)INT32_MAX * 8u) {
      secs = (uint64_t)INT32_MAX * 8u;
    }
    deadline.tv_sec += (time_t)secs;
    deadline.tv_nsec = (long)nanos;
  }
  pthread_mutex_lock(&port->mutex);
  while (__atomic_load_n(&context->request_word, __ATOMIC_ACQUIRE) == 0) {
    if (timeout_ns == UINT64_MAX) {
      pthread_cond_wait(&port->cond, &port->mutex);
    } else if (pthread_cond_timedwait(&port->cond, &port->mutex, &deadline) ==
        ETIMEDOUT) {
      break;
    }
  }
  bool woken =
      __atomic_load_n(&context->request_word, __ATOMIC_ACQUIRE) != 0;
  pthread_mutex_unlock(&port->mutex);
  return woken;
}
