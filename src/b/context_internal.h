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
 * Context, private part: the layout, the lifecycle configurations and the one
 * checked transition that `run` and the poll will use (AD-20).
 */

#ifndef GHOTI_IO_GRCORE_B_CONTEXT_INTERNAL_H
#define GHOTI_IO_GRCORE_B_CONTEXT_INTERNAL_H

#include <ghoti.io/runtime-core/macros.h>

#include "account_internal.h"

#include <ghoti.io/runtime-core/b/budget.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/b/request.h>
#include <ghoti.io/runtime-core/b/roots.h>
#include <ghoti.io/runtime-core/b/run.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A lifecycle configuration: the public state, with "parked" split
 *   into its two kinds, because only one of them can migrate.
 */
typedef enum {
  GRCORE_CONFIG_PARKED_OUTSIDE = 0, ///< Not inside `run`.
  GRCORE_CONFIG_PARKED_INSIDE,      ///< Inside `run`, in a host call.
  GRCORE_CONFIG_RUNNING,            ///< Executing guest code.
  GRCORE_CONFIG_AT_POLL,            ///< At a poll, a handler working.
  GRCORE_CONFIG_PAUSED,             ///< `run` returned with a pause.
  GRCORE_CONFIG_COUNT
} GRCORE_ContextConfig;

/** @brief One registration. */
typedef struct GRCORE_Registration {
  const GRCORE_Key * key;
  void * value;
} GRCORE_Registration;

/** @brief One handler's vote in a poll, kept by registration index. */
typedef struct GRCORE_Vote {
  GRCORE_Verdict verdict;  ///< The handler's strongest vote.
  GRCORE_Result result;    ///< What `run` returns if it votes to unwind.
} GRCORE_Vote;

/** @brief One fuel scope (AD-21): an exclusive budget under the ceiling. */
typedef struct GRCORE_FuelScope {
  uint64_t id;     ///< From a per-context serial; never reused.
  uint64_t budget; ///< What the scope may use itself; may be UNLIMITED.
  uint64_t used;   ///< Charged while this scope was the innermost.
  GRCORE_ScopePolicy policy;
} GRCORE_FuelScope;

/** @brief One root source with the value it was added with. */
typedef struct GRCORE_RootEntry {
  const GRCORE_RootSource * source;
  void * value;
} GRCORE_RootEntry;

struct GRCORE_Context {
  GRCORE_Group * group;
  GRCORE_Options * options; ///< The context's own copy.
  GRCORE_Meter meter;
  GRCORE_Counting counting;
  uintptr_t owner; ///< 0, or a thread id. `__atomic` builtins only.
  GRCORE_ContextConfig config; ///< Guarded by the ownership protocol.
  GRCORE_Registration * registrations;
  size_t registration_count;
  size_t registration_capacity;
  bool tearing_down; ///< Set by destroy; the owner's thread only.

  /* Requests (AD-4). The word and the kind count are the only fields another
   * thread reads, and only a port does, under its mutex. */
  uint64_t request_word; ///< Bit k is kind k below 63; bit 63: overflow.
  uint32_t kind_count;   ///< Service kinds defined. `__atomic` builtins.
  const GRCORE_Key ** kind_keys; ///< Owner only; `kind_count` entries.
  size_t kind_capacity;
  GRCORE_Port * port;    ///< The context's own reference. Owner only.

  /* Budgets (AD-21). Owner only. */
  uint64_t fuel_used;
  uint64_t fuel_limit;
  uint64_t depth[2];
  bool reclaim_tried;    ///< The collector has had its chance at this overage.

  /* Fuel scopes (AD-21), outermost first. Owner only; allocated through the
   * counting allocator and freed at destroy. */
  GRCORE_FuelScope * fuel_scopes;
  size_t fuel_scope_count;
  size_t fuel_scope_capacity;
  uint64_t fuel_scope_serial;
  /* Set by the last slow poll: the id of the scope whose exhaustion alone
   * made it unwind, or zero. Closing that scope clears the unwind. */
  uint64_t scoped_unwind;
  bool fuel_vote_scoped; ///< DECIDE's fuel vote was a scoped unwind.

  /* Nested activations (AD-5): a pause inside one becomes a limit unwind. */
  uint64_t nested;

  /* Root sources (AD-18). Owner only. */
  GRCORE_RootEntry * roots;
  size_t root_count;
  size_t root_capacity;

  /* The poll. One block, sized for the registrations the table can hold, so a
   * poll never allocates: `votes` and `verdict_keys` have the built-in kinds
   * first, then one entry per registration, and `order` one per registration. */
  GRCORE_Vote * votes;
  const GRCORE_Key ** verdict_keys;
  size_t * order;
  size_t scratch_capacity;       ///< Registrations the block was sized for.
  size_t verdict_key_count;
  GRCORE_Location pause_location;
  GRCORE_Verdict last_verdict;   ///< What the last slow poll decided.
  GRCORE_Result unwind_result;
  bool shuffle;                  ///< The phase-shuffle test mode.
  uint64_t shuffle_state;

  /* run */
  GRCORE_EntryFn entry;
  void * entry_state;
};

/**
 * @brief An identifier of the calling thread, never zero and never reused.
 *
 * Assigned lazily from a global atomic counter on a thread's first call, so a
 * thread that has ended can never share its id with a later one.
 */
uintptr_t grcore_thread_id(void);

/** @brief Whether the calling thread owns the context (acquire load). */
bool grcore_context_owned_by_caller(const GRCORE_Context * context);

/**
 * @brief Recomputes the derived fuel and memory requests from the budgets, so
 *   each bit is set exactly when its condition holds.
 */
void grcore_context_refresh_derived(GRCORE_Context * context);

/** @brief Whether the fuel ceiling (the request budget) is exhausted. */
static inline bool grcore_fuel_ceiling_exhausted(const GRCORE_Context * c) {
  return c->fuel_used > c->fuel_limit;
}

/** @brief The innermost fuel scope; NULL if none is open. */
static inline const GRCORE_FuelScope * grcore_fuel_scope_innermost(
    const GRCORE_Context * c) {
  return c->fuel_scope_count == 0 ? NULL
                                  : &c->fuel_scopes[c->fuel_scope_count - 1];
}

/** @brief Whether the innermost fuel scope has used more than its budget. */
static inline bool grcore_fuel_scope_exhausted(const GRCORE_Context * c) {
  const GRCORE_FuelScope * s = grcore_fuel_scope_innermost(c);
  return s != NULL && s->used > s->budget;
}

/** @brief Clears the terminate request. Only the end of `run` does this. */
void grcore_context_clear_terminate(GRCORE_Context * context);

/** @brief Clears time and interrupt, which `resume` acknowledges. */
void grcore_context_clear_edge_requests(GRCORE_Context * context);

/**
 * @brief The context's port, created if it has none. No reference is taken.
 *
 * @return ::GRCORE_OK or ::GRCORE_ERR_OOM.
 */
GRCORE_Result grcore_context_port_ensure(
    GRCORE_Context * context, GRCORE_Port ** out_port);

/** @brief Cuts the port from the context and drops the context's reference. */
void grcore_context_port_detach(GRCORE_Context * context);

/**
 * @brief Blocks until the context's request word is non-zero or the timeout
 *   passes.
 *
 * @return True if the word is non-zero.
 */
bool grcore_port_wait(
    GRCORE_Port * port, const GRCORE_Context * context, uint64_t timeout_ns);

/**
 * @brief Moves a context along one of the eight legal lifecycle edges.
 *
 * The legal edges are PARKED_OUTSIDE to RUNNING (run begins), RUNNING to
 * AT_POLL, AT_POLL to RUNNING, AT_POLL to PAUSED, PAUSED to RUNNING (resume),
 * RUNNING to PARKED_INSIDE (park), PARKED_INSIDE to RUNNING (unpark) and
 * RUNNING to PARKED_OUTSIDE (run ends).
 *
 * @param context The context. The caller must own it.
 * @param to The configuration to enter.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID with the state unchanged for an
 *   illegal edge, a non-owner, or a NULL or out-of-range argument.
 */
GRCORE_Result grcore_context_transition(
    GRCORE_Context * context, GRCORE_ContextConfig to);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_CONTEXT_INTERNAL_H */
