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

#include <ghoti.io/runtime-core/b/context.h>

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
};

/**
 * @brief An identifier of the calling thread, never zero and never reused.
 *
 * Assigned lazily from a global atomic counter on a thread's first call, so a
 * thread that has ended can never share its id with a later one.
 */
uintptr_t grcore_thread_id(void);

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
