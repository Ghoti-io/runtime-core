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
 * @file profile.h
 * @stability stable
 *
 * A sampling profiler: where a guest's time goes, as source locations.
 *
 * The profiler is one keyed registration of phase OBSERVE (AD-5, AD-19) and
 * one request kind that belongs to that key. Anything that posts the kind
 * (the optional timer thread below, or a host call) makes the *next poll*
 * take one sample: a walk of the guest frames, innermost first, through the
 * abstract frame, counted by source location. The innermost frame's location
 * gets a *self* hit; every distinct location in the walk gets an *inclusive*
 * hit, so a recursive function counts once per sample. The profiler knows no
 * engine: it reads a frame only through the engine descriptor's `locate`, so
 * the same profiler serves every engine, and, because a poll identity is the
 * same on every tier (AD-18), an interpreter profile and a JIT profile of one
 * program agree.
 *
 * What a sample is allowed to do. Its handler runs inside the poll with the
 * context at-poll, so the frame walk is legal there, and it reads and changes
 * nothing the guest can see: it votes nothing, charges no fuel, and allocates
 * nothing (the location table is a fixed size, allocated once at attach
 * through the context's counting allocator; a location that does not fit is
 * counted in `dropped`). It commutes with every other DECIDE and OBSERVE
 * handler that does not look at the profiler's own request kind, which the
 * phase-shuffle test mode checks. The one change it makes is to clear that kind,
 * as every service that owns a kind does, and `grcore_pollcall_pending` is
 * live: an OBSERVE handler that asks about the profiler's kind sees it cleared
 * if it runs after the profiler (registration order, unshuffled), so it must be
 * registered before the profiler or not ask. The tests' oracle is registered
 * first for this reason.
 *
 * **Safepoint bias.** A sample is taken at the next poll after the request,
 * not at the instant of the request. Time that passes between polls is
 * therefore charged to the poll that ends it: in a loop, to the loop's
 * back-edge. This is the price of reading a guest stack only where it is
 * consistent (AD-4: another thread acts on a context only by posting a
 * request), and it is the same bias on every tier.
 *
 * **Threads.** Everything is the context owner's except ::grcore_profiler_request
 * and the kind: a post is safe from any thread while the profiler is alive
 * (the profiler is freed with its context, so the profiler *pointer* is invalid
 * after the context is destroyed; a thread that may outlive the context holds a
 * retained ::GRCORE_Port and posts ::grcore_profiler_kind through it, and only
 * that post is refused cleanly after the context is destroyed). The timer is the one place the core starts a thread
 * of its own. It holds only a retained port and the kind, touches no guest
 * state, and is stopped and joined by the key's destructor before the
 * profiler is freed, so destroying a context whose timer is running is safe.
 * The thread starts with every signal blocked, so a signal sent to the process
 * is never run on it; and a process that forks while a timer runs leaves the
 * child with a profiler that believes in a thread it does not have, so the
 * timer is stopped before a fork (::grcore_profiler_timer_stop) and started
 * again after it, in whichever process keeps profiling.
 *
 * **Snapshots.** The key has no snapshot hook, so a profiler is not part of
 * any snapshot: a context with one still snapshots, and the host attaches
 * again on the restored context.
 *
 * A context with no profiler, or one with no sample pending, pays nothing:
 * nothing here is on the poll's fast path.
 */

#ifndef GHOTI_IO_GRCORE_B_PROFILE_H
#define GHOTI_IO_GRCORE_B_PROFILE_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/b/request.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A context's profiler. Opaque; owned by the context. */
typedef struct GRCORE_Profiler GRCORE_Profiler;

/** @brief The table capacity ::grcore_profiler_attach uses for zero. */
#define GRCORE_PROFILER_DEFAULT_CAPACITY ((size_t)512)

/** @brief The largest table capacity: more is ::GRCORE_ERR_LIMIT. */
#define GRCORE_PROFILER_MAX_CAPACITY ((size_t)1 << 20)

/** @brief The longest timer interval, in microseconds (one day). */
#define GRCORE_PROFILER_MAX_INTERVAL_US (UINT64_C(86400) * UINT64_C(1000000))

/** @brief One source location's counts. */
typedef struct GRCORE_ProfileEntry {
  const char * file;     ///< The pointer the engine's `locate` gave first. It
                         ///< is the engine's string and must outlive the
                         ///< profiler's use of it (the ::GRCORE_Location
                         ///< contract): the profiler compares stored pointers by
                         ///< text on later samples and in a report. A host
                         ///< resets or discards the profiler before freeing a
                         ///< program whose names it holds; a copy of a report
                         ///< entry's name is the host's to make.
  int line;              ///< The line.
  uint64_t self;         ///< Samples whose innermost frame was here.
  uint64_t inclusive;    ///< Samples with a frame here, each counted once.
} GRCORE_ProfileEntry;

/** @brief What a profile adds up to. */
typedef struct GRCORE_ProfileTotals {
  uint64_t samples;      ///< Samples taken, including those with no frame.
  uint64_t no_frame;     ///< Samples taken at a poll with an empty guest
                         ///< stack (a subset of `samples`; no entry added).
  uint64_t dropped;      ///< Frame locations that did not fit the table and
                         ///< were counted nowhere else.
  size_t locations;      ///< Distinct locations in the table.
} GRCORE_ProfileTotals;

/**
 * @brief Attaches a profiler to a context.
 *
 * Registers the profiler's key (OBSERVE, cardinality one), defines its request
 * kind, takes a reference to the context's port and allocates the location
 * table, all through the context's counting allocator. The profiler lives as
 * long as the context; there is no detach.
 *
 * @param context The context. The caller must own it, and it must not be
 *   running or being destroyed.
 * @param capacity How many distinct locations the table holds; zero means
 *   ::GRCORE_PROFILER_DEFAULT_CAPACITY.
 * @param out_profiler Receives the profiler. Written only on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for NULL, a non-owner, a context
 *   in a state that refuses registration, or one that already has a profiler;
 *   ::GRCORE_ERR_LIMIT for a capacity over ::GRCORE_PROFILER_MAX_CAPACITY;
 *   ::GRCORE_ERR_OOM. On failure everything allocated is released (a request
 *   kind, once defined, cannot be, and stays defined).
 */
GRCORE_API GRCORE_Result grcore_profiler_attach(GRCORE_Context * context,
    size_t capacity, GRCORE_Profiler ** out_profiler);

/**
 * @brief The context's profiler.
 *
 * @param context The context.
 * @return The profiler; NULL for NULL or a context with none.
 */
GRCORE_API GRCORE_Profiler * grcore_profiler_of(const GRCORE_Context * context);

/**
 * @brief The request kind that makes the next poll take a sample.
 *
 * @param profiler The profiler.
 * @return The kind; ::GRCORE_REQUEST_TERMINATE (never a sampling kind) for
 *   NULL.
 */
GRCORE_API GRCORE_RequestKind grcore_profiler_kind(
    const GRCORE_Profiler * profiler);

/**
 * @brief Asks for a sample at the next poll. Any thread, while the profiler
 *   (and so its context) is alive; the pointer is invalid after the context is
 *   destroyed. Use a retained port for a post that may outlive it.
 *
 * Posting while a sample is already pending is not an error and still takes
 * one sample.
 *
 * @param profiler The profiler.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL, or when the port
 *   refuses the post.
 */
GRCORE_API GRCORE_Result grcore_profiler_request(const GRCORE_Profiler * profiler);

/**
 * @brief Starts the timer thread: it posts the kind every `interval_us`.
 *
 * The sample rate is the host's. A tick while a sample is pending merges into
 * it, so a guest that polls rarely is sampled at its poll rate, not faster.
 *
 * @param profiler The profiler. Its context's owner calls this.
 * @param interval_us The period in microseconds, from 1 to
 *   ::GRCORE_PROFILER_MAX_INTERVAL_US.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for NULL, a non-owner, a zero
 *   interval or a timer already running (nothing changes);
 *   ::GRCORE_ERR_LIMIT for an interval over the maximum; ::GRCORE_ERR_OOM
 *   when the timer or its thread cannot be created.
 */
GRCORE_API GRCORE_Result grcore_profiler_timer_start(
    GRCORE_Profiler * profiler, uint64_t interval_us);

/**
 * @brief Stops the timer and joins its thread. A no-op when none runs.
 *
 * @param profiler The profiler. Its context's owner calls this.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or a non-owner.
 */
GRCORE_API GRCORE_Result grcore_profiler_timer_stop(GRCORE_Profiler * profiler);

/**
 * @brief Whether the timer is running.
 *
 * @param profiler The profiler.
 * @return True while a timer thread exists; false for NULL.
 */
GRCORE_API bool grcore_profiler_timer_running(const GRCORE_Profiler * profiler);

/**
 * @brief Copies the profile out, hottest first.
 *
 * Entries are ordered by `self` descending, then `inclusive` descending, then
 * file (by content) and line ascending, so a report is deterministic. If the
 * array is smaller than the table, the first `capacity` entries of that order
 * are copied (the cost is the table size times `capacity`; it allocates
 * nothing). Callable by the owner only, and not from inside a poll.
 *
 * @param profiler The profiler.
 * @param entries The array to fill; may be NULL when `capacity` is zero.
 * @param capacity How many entries `entries` holds.
 * @param out_count Receives how many were copied; may be NULL. Written only
 *   on success.
 * @param out_totals Receives the totals; may be NULL. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL, a non-owner, a
 *   context at a poll, or NULL `entries` with a non-zero capacity.
 */
GRCORE_API GRCORE_Result grcore_profiler_report(const GRCORE_Profiler * profiler,
    GRCORE_ProfileEntry * entries, size_t capacity, size_t * out_count,
    GRCORE_ProfileTotals * out_totals);

/**
 * @brief Forgets every sample: empties the table (so its locations are free
 *   again) and zeroes the totals. Allocates nothing.
 *
 * @param profiler The profiler.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL, a non-owner or a
 *   context at a poll.
 */
GRCORE_API GRCORE_Result grcore_profiler_reset(GRCORE_Profiler * profiler);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_PROFILE_H */
