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
 * @file poll.h
 * @stability stable
 *
 * The poll and its four fixed phases (AD-5).
 *
 * An engine polls at function entry, at loop back-edges and at throw points
 * (AD-4); a native whose work the guest controls polls through
 * ::grcore_runtime_poll. The fast path, with no request pending, is one
 * atomic load and one branch and runs nothing. Otherwise the slow path runs
 * the phases in order, which `core` owns and a service can only register into:
 *
 *  1. DECIDE: handlers vote a verdict. They are side-effect-free, the votes
 *     are levels (re-decided at every poll), and the strongest vote wins:
 *     continue, then pause, then unwind. Votes are kept per registration, so
 *     the winning set of keys and the unwind result do not depend on the order
 *     the handlers ran in. The built-in keys decide first, then the keyed
 *     ones.
 *  2. ACT: handlers carry the verdict out, in registration order. A collector
 *     runs here, so it runs before a memory verdict is reached.
 *  3. OBSERVE: read-only handlers.
 *  4. YIELD: only when the verdict is continue, in registration order. A
 *     handler here may still vote, and so raise the verdict.
 *
 * DECIDE and OBSERVE handlers must commute. The phase-shuffle mode, which
 * runs them in a seeded random order, is how a test checks that they do
 * (::grcore_context_set_phase_shuffle). They are given no API that allocates
 * or reaches a collection point.
 *
 * The poll must be called by the context's owner while it is running.
 */

#ifndef GHOTI_IO_GRCORE_B_POLL_H
#define GHOTI_IO_GRCORE_B_POLL_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/b/request.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What a poll decided, weakest to strongest. */
typedef enum {
  GRCORE_VERDICT_CONTINUE = 0, ///< Carry on.
  GRCORE_VERDICT_PAUSE,        ///< Return from `run` to the host.
  GRCORE_VERDICT_UNWIND        ///< Stop the guest; `run` returns an error.
} GRCORE_Verdict;

/** @brief Where a poll was: a source file and line, or any name an engine
 *   chooses. The strings must outlive the context's use of the location. */
typedef struct GRCORE_Location {
  const char * file; ///< A string with static storage, usually `__FILE__`.
  int line;          ///< A line number.
} GRCORE_Location;

/** @brief The location of the expression that names it. */
#ifdef __cplusplus
#define GRCORE_HERE (GRCORE_Location{__FILE__, __LINE__})
#else
#define GRCORE_HERE ((GRCORE_Location){__FILE__, __LINE__})
#endif

/** @brief A poll at the place it is written. */
#define GRCORE_POLL(context) grcore_poll((context), GRCORE_HERE)

/**
 * @brief Polls.
 *
 * With no request pending this returns ::GRCORE_VERDICT_CONTINUE after one
 * load: the context must not be NULL, and nothing else is checked, so a poll
 * made outside `run` with nothing pending is a harmless continue. Otherwise it
 * runs the phases. On a pause the context stays at-poll and the caller must
 * unwind its C frames and return ::GRCORE_STEP_PAUSED from the entry
 * function; `run` completes the transition. On an unwind the caller does the
 * same with ::GRCORE_STEP_UNWOUND.
 *
 * On the slow path, a poll made by a thread that does not own the context, or
 * while the context is not running, returns ::GRCORE_VERDICT_UNWIND and
 * changes no lifecycle state. The owner can then read ::GRCORE_ERR_INVALID
 * from ::grcore_context_unwind_result; a non-owner writes nothing.
 *
 * @param context The context.
 * @param location Where this poll is, kept as the pause location.
 * @return The verdict.
 */
GRCORE_API GRCORE_Verdict grcore_poll(
    GRCORE_Context * context, GRCORE_Location location);

/**
 * @brief The poll for natives (AD-21): charges `work`, then polls allowing
 *   only continue and unwind.
 *
 * A native whose work the guest controls calls this at a bounded interval of
 * work. A pause verdict is refused: it becomes an unwind with
 * ::GRCORE_ERR_LIMIT, and the keys that asked for the pause stay readable as
 * the reason. Terminate unwinds from here too.
 *
 * @param context The context.
 * @param work The fuel to charge for the work done since the last call.
 * @param location Where this poll is.
 * @return ::GRCORE_OK to continue; otherwise the unwind result, which the
 *   native returns to its caller (::GRCORE_ERR_LIMIT for a budget).
 */
GRCORE_API GRCORE_Result grcore_runtime_poll(
    GRCORE_Context * context, uint64_t work, GRCORE_Location location);

/**
 * @brief The phase being run.
 *
 * @param call The poll in progress.
 * @return The phase; ::GRCORE_PHASE_NONE for NULL.
 */
GRCORE_API GRCORE_Phase grcore_pollcall_phase(const GRCORE_PollCall * call);

/**
 * @brief Whether a request is pending, as this poll sees it.
 *
 * @param call The poll in progress.
 * @param kind The kind.
 * @return True if pending.
 */
GRCORE_API bool grcore_pollcall_pending(
    const GRCORE_PollCall * call, GRCORE_RequestKind kind);

/**
 * @brief The verdict so far: continue during DECIDE, the DECIDE result during
 *   ACT and OBSERVE, and continue during YIELD.
 *
 * @param call The poll in progress.
 * @return The verdict.
 */
GRCORE_API GRCORE_Verdict grcore_pollcall_verdict(const GRCORE_PollCall * call);

/**
 * @brief Whether the context is over its memory budget and has not yet been
 *   given the chance to reclaim, so an ACT handler that can collect should.
 *
 * @param call The poll in progress.
 * @return True during the ACT phase of the first poll over budget.
 */
GRCORE_API bool grcore_pollcall_reclaim_requested(const GRCORE_PollCall * call);

/**
 * @brief Votes a verdict. Valid in DECIDE and YIELD only.
 *
 * A handler's vote is its strongest one. The vote counts for the registration
 * being run, whatever order handlers run in.
 *
 * @param call The poll in progress.
 * @param verdict The vote.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID in another phase or for a
 *   value outside the enum.
 */
GRCORE_API GRCORE_Result grcore_pollcall_vote(
    GRCORE_PollCall * call, GRCORE_Verdict verdict);

/**
 * @brief Sets what `run` returns if this handler's unwind vote wins.
 *
 * Valid in DECIDE and YIELD. The default is ::GRCORE_ERR_LIMIT. When several
 * handlers vote to unwind, the lowest registration index wins, built-in keys
 * first.
 *
 * @param call The poll in progress.
 * @param result ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_GUEST.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID.
 */
GRCORE_API GRCORE_Result grcore_pollcall_set_unwind_result(
    GRCORE_PollCall * call, GRCORE_Result result);

/**
 * @brief The built-in key for a core request kind.
 *
 * These are the keys that appear in a pause's key list for terminate, time,
 * interrupt, fuel and memory.
 *
 * @param kind One of ::GRCORE_REQUEST_TERMINATE to ::GRCORE_REQUEST_MEMORY.
 * @return A static key; NULL for any other kind.
 */
GRCORE_API const GRCORE_Key * grcore_core_key(GRCORE_RequestKind kind);

/**
 * @brief Turns the phase-shuffle test mode on or off.
 *
 * With it on, the DECIDE handlers (after the built-ins) and the OBSERVE
 * handlers run in an order drawn from `seed`, which differs from poll to poll.
 * Handlers that commute give the same verdict and key list for every seed.
 *
 * @param context The context. The caller must own it.
 * @param enabled Whether to shuffle.
 * @param seed The seed.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID.
 */
GRCORE_API GRCORE_Result grcore_context_set_phase_shuffle(
    GRCORE_Context * context, bool enabled, uint64_t seed);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_POLL_H */
