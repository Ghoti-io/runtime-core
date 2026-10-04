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
 * @file run.h
 * @stability stable
 *
 * `run` and `resume`: the host's way to execute guest code (AD-13, AD-20).
 *
 * The core knows no engine. A host hands `run` an *entry function*, which an
 * engine supplies, and the entry function polls. A pause cannot keep C
 * frames, so an entry that sees a pause verdict saves its position in its
 * own state and returns ::GRCORE_STEP_PAUSED; `resume` calls the same entry
 * again. Because no C frame is left above `run`, a paused context can be
 * released and acquired by another thread.
 *
 * `run` and `resume` return ::GRCORE_OK with an outcome of finished or paused.
 * An unwind returns ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_GUEST, and the reason is
 * read from the context. A pause's keys and location are valid until the next
 * `resume`.
 */

#ifndef GHOTI_IO_GRCORE_B_RUN_H
#define GHOTI_IO_GRCORE_B_RUN_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What an entry function reports when it returns. */
typedef enum {
  GRCORE_STEP_FINISHED = 0, ///< The guest ran to the end.
  GRCORE_STEP_PAUSED,       ///< A poll said pause; the position is saved.
  GRCORE_STEP_UNWOUND       ///< A poll said unwind.
} GRCORE_Step;

/**
 * @brief An engine's entry function.
 *
 * Called by `run`, and again by `resume` with the same `state`. It must
 * return ::GRCORE_STEP_PAUSED only after a poll returned
 * ::GRCORE_VERDICT_PAUSE, and ::GRCORE_STEP_UNWOUND only after one returned
 * ::GRCORE_VERDICT_UNWIND; anything else is a bug that `run` reports as
 * ::GRCORE_ERR_INTERNAL.
 */
typedef GRCORE_Step (*GRCORE_EntryFn)(GRCORE_Context * context, void * state);

/** @brief How a `run` or `resume` that returned ::GRCORE_OK ended. */
typedef enum {
  GRCORE_OUTCOME_FINISHED = 0, ///< The entry finished.
  GRCORE_OUTCOME_PAUSED        ///< The context is paused.
} GRCORE_Outcome;

/**
 * @brief Runs `entry`.
 *
 * @param context The context. The caller must own it, and it must be parked
 *   outside `run`.
 * @param entry The entry function.
 * @param state Passed to it, and kept for `resume`.
 * @param out_outcome Receives the outcome. Written only when the result is
 *   ::GRCORE_OK.
 * @return ::GRCORE_OK; ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_GUEST for an unwind
 *   (the context is parked outside `run`); ::GRCORE_ERR_INVALID for a wrong
 *   state, a non-owner or a NULL argument (nothing changes); or
 *   ::GRCORE_ERR_INTERNAL when the entry misreported (the context is left
 *   parked outside `run`).
 */
GRCORE_API GRCORE_Result grcore_run(GRCORE_Context * context,
    GRCORE_EntryFn entry, void * state, GRCORE_Outcome * out_outcome);

/**
 * @brief Resumes a paused context.
 *
 * Clears the time and interrupt requests first, since the host has now
 * acknowledged them. Anything still true, such as exhausted fuel, pauses
 * again at the next poll.
 *
 * @param context The context. The caller must own it, and it must be paused.
 * @param out_outcome Receives the outcome. Written only when the result is
 *   ::GRCORE_OK.
 * @return As ::grcore_run.
 */
GRCORE_API GRCORE_Result grcore_resume(
    GRCORE_Context * context, GRCORE_Outcome * out_outcome);

/**
 * @brief Blocks the running context until a request is pending or the timeout
 *   passes.
 *
 * The context is parked while it waits, and running again after. A request
 * posted through a port wakes it. The request is not consumed.
 *
 * @param context The context. The caller must own it, and it must be running.
 * @param timeout_ns How long to wait, in nanoseconds; ::GRCORE_UNLIMITED
 *   waits without a time limit. The timeout is measured on a monotonic clock,
 *   except on Windows, where it is measured on the system clock and a step of
 *   that clock while the wait is pending shortens or lengthens it.
 * @param out_woken Receives true if a request is pending, false on timeout.
 *   Written only on success.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID (nothing changes) or
 *   ::GRCORE_ERR_OOM (likewise).
 */
GRCORE_API GRCORE_Result grcore_context_wait(
    GRCORE_Context * context, uint64_t timeout_ns, bool * out_woken);

/**
 * @brief How many keys issued the verdict of the last non-continue poll.
 *
 * For a pause these are the keys that caused it, in canonical order: built-in
 * keys by kind, then registered keys in registration order, whatever order
 * their handlers ran in. For an unwind they are the unwind voters. Valid until
 * the next `resume`.
 *
 * @param context The context.
 * @return The count; zero for NULL or when the last poll continued.
 */
GRCORE_API size_t grcore_context_pause_key_count(
    const GRCORE_Context * context);

/**
 * @brief One of those keys.
 *
 * @param context The context.
 * @param index From zero to the count minus one.
 * @return The key; NULL when out of range.
 */
GRCORE_API const GRCORE_Key * grcore_context_pause_key(
    const GRCORE_Context * context, size_t index);

/**
 * @brief Where the last pause (or unwind) was polled.
 *
 * @param context The context.
 * @return The location; `{NULL, 0}` if there is none.
 */
GRCORE_API GRCORE_Location grcore_context_pause_location(
    const GRCORE_Context * context);

/**
 * @brief Why the last unwind happened.
 *
 * @param context The context.
 * @return ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_GUEST after an unwind;
 *   ::GRCORE_OK if the last `run` did not unwind.
 */
GRCORE_API GRCORE_Result grcore_context_unwind_result(
    const GRCORE_Context * context);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_RUN_H */
