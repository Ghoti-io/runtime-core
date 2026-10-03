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
 * @file context.h
 * @stability stable
 *
 * The execution context: creation, lifecycle, ownership and keyed state.
 *
 * A context has exactly one owning thread at a time (AD-6): its creator, until
 * it calls ::grcore_context_release. It moves to another thread only when it
 * is parked outside `run` or paused (AD-20). There is deliberately no lock
 * around a context. The owner is recorded in an atomic that is released and
 * acquired by the library's own calls, so a hand-off that uses only
 * ::grcore_context_release and ::grcore_context_acquire is free of data races
 * and a thread sanitizer can check it.
 *
 * Every function that takes a context must be called by its owner unless
 * stated otherwise; the ones that can tell, refuse with
 * ::GRCORE_ERR_INVALID.
 */

#ifndef GHOTI_IO_GRCORE_B_CONTEXT_H
#define GHOTI_IO_GRCORE_B_CONTEXT_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/b/group.h>
#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/b/options.h>
#include <ghoti.io/runtime-core/b/page.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The four lifecycle states (AD-20).
 *
 * ::GRCORE_CONTEXT_PARKED covers both "not inside `run`" and "inside a host
 * call that is bracketed by park and unpark"; only the first can migrate.
 */
typedef enum {
  GRCORE_CONTEXT_PARKED = 0, ///< Not executing guest code.
  GRCORE_CONTEXT_RUNNING,    ///< Executing inside `run`.
  GRCORE_CONTEXT_AT_POLL,    ///< Stopped at a poll while a handler works.
  GRCORE_CONTEXT_PAUSED      ///< `run` has returned with a pause.
} GRCORE_ContextState;

/**
 * @brief Creates a context in `group`, parked and owned by the caller.
 *
 * The options are copied; every budget not set is ::GRCORE_UNLIMITED. On
 * failure nothing is allocated and the group's count is unchanged.
 *
 * @param group The group the context belongs to for its whole life.
 * @param options The options, or NULL for the defaults.
 * @param out_context Receives the context. Written only on success.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID for a NULL `group` or output, or
 *   ::GRCORE_ERR_OOM.
 */
GRCORE_API GRCORE_Result grcore_context_create(GRCORE_Group * group,
    const GRCORE_Options * options, GRCORE_Context ** out_context);

/**
 * @brief Destroys a context: runs the key destructors in reverse
 *   registration order, then frees it.
 *
 * @param context The context. The caller must own it, and it must be parked
 *   outside `run` or paused (a paused context the host abandons can be
 *   destroyed). Running and at-poll contexts are refused. While destructors
 *   run, the context refuses register, acquire, release, transition and
 *   destroy, and a registration is removed before its destructor is called.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID (the context is unchanged).
 */
GRCORE_API GRCORE_Result grcore_context_destroy(GRCORE_Context * context);

/**
 * @brief The lifecycle state. Meaningful to the owner only.
 *
 * @param context The context.
 * @return The state; ::GRCORE_CONTEXT_PARKED for NULL.
 */
GRCORE_API GRCORE_ContextState grcore_context_state(
    const GRCORE_Context * context);

/**
 * @brief Brackets a host call made from inside `run`: RUNNING to PARKED.
 *
 * @param context The context, RUNNING.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID (state unchanged).
 */
GRCORE_API GRCORE_Result grcore_context_park(GRCORE_Context * context);

/**
 * @brief Ends the bracket begun by ::grcore_context_park.
 *
 * @param context The context, parked by ::grcore_context_park.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID (state unchanged).
 */
GRCORE_API GRCORE_Result grcore_context_unpark(GRCORE_Context * context);

/**
 * @brief Takes ownership of an unowned context.
 *
 * @param context A released context.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID if it is owned (by anyone,
 *   the caller included) or NULL.
 */
GRCORE_API GRCORE_Result grcore_context_acquire(GRCORE_Context * context);

/**
 * @brief Gives up ownership so another thread can acquire the context.
 *
 * Allowed only when the context is parked outside `run`, or paused.
 *
 * @param context The context. The caller must own it.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID (ownership is unchanged).
 */
GRCORE_API GRCORE_Result grcore_context_release(GRCORE_Context * context);

/**
 * @brief Whether the calling thread owns the context.
 *
 * Safe to call from any thread.
 *
 * @param context The context.
 * @return True for the owner; false otherwise, and for NULL.
 */
GRCORE_API bool grcore_context_is_owner(const GRCORE_Context * context);

/**
 * @brief Whether guest state may be read now: at-poll or paused, and by the
 *   owner (AD-20).
 *
 * Safe to call from any thread.
 *
 * @param context The context.
 * @return True only for the owner of an at-poll or paused context.
 */
GRCORE_API bool grcore_context_guest_state_readable(
    const GRCORE_Context * context);

/**
 * @brief Registers `value` under `key`.
 *
 * The context holds the pointer and calls `key->destroy` on it at teardown. A
 * cardinality-one key refuses a second registration; a cardinality-many key
 * keeps them all, in order. Allowed when the context is parked (either
 * kind: outside `run`, or inside a host call), at-poll or paused; refused
 * while it is running or being destroyed.
 *
 * @param context The context. The caller must own it.
 * @param key The key, by address.
 * @param value The value; NULL is refused, so that a NULL lookup always means
 *   absent.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID (the table is unchanged), or
 *   ::GRCORE_ERR_OOM (likewise).
 */
GRCORE_API GRCORE_Result grcore_context_register(
    GRCORE_Context * context, const GRCORE_Key * key, void * value);

/**
 * @brief The first value registered under `key`.
 *
 * For a cardinality-many key the rest are reached through
 * ::grcore_context_registration.
 *
 * Owner only; not checked, to keep the lookup cheap.
 *
 * @param context The context.
 * @param key The key.
 * @return The value, or NULL if none is registered.
 */
GRCORE_API void * grcore_context_slot(
    const GRCORE_Context * context, const GRCORE_Key * key);

/**
 * @brief How many registrations the context holds, across all keys.
 *
 * @param context The context.
 * @return The count; zero for NULL.
 */
GRCORE_API size_t grcore_context_registration_count(
    const GRCORE_Context * context);

/**
 * @brief Reads registration `index`, in registration order.
 *
 * @param context The context.
 * @param index From zero to the count minus one.
 * @param out_key Receives the key, or may be NULL. Written only on success.
 * @param out_value Receives the value, or may be NULL. Written only on
 *   success.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID for NULL or an index out of
 *   range.
 */
GRCORE_API GRCORE_Result grcore_context_registration(
    const GRCORE_Context * context, size_t index, const GRCORE_Key ** out_key,
    void ** out_value);

/**
 * @brief The group the context belongs to.
 *
 * @param context The context.
 * @return The group; NULL for NULL.
 */
GRCORE_API GRCORE_Group * grcore_context_group(const GRCORE_Context * context);

/**
 * @brief The context's counting allocator: what services and engines must
 *   allocate through, so the context's meter is exact.
 *
 * @param context The context.
 * @return An allocator owned by the context; NULL for NULL.
 */
GRCORE_API const GRCORE_Allocator * grcore_context_allocator(
    const GRCORE_Context * context);

/**
 * @brief The context's counting page provider.
 *
 * @param context The context.
 * @return A provider owned by the context; NULL for NULL.
 */
GRCORE_API const GRCORE_PageProvider * grcore_context_page_provider(
    const GRCORE_Context * context);

/**
 * @brief The context's private copy of its options.
 *
 * @param context The context.
 * @return Read-only options owned by the context; NULL for NULL.
 */
GRCORE_API const GRCORE_Options * grcore_context_options(
    const GRCORE_Context * context);

/**
 * @brief The fuel budget the context was created with.
 *
 * @param context The context.
 * @return The budget; ::GRCORE_UNLIMITED if unset or `context` is NULL.
 */
GRCORE_API uint64_t grcore_context_fuel(const GRCORE_Context * context);

/**
 * @brief The memory budget the context was created with, in bytes.
 *
 * @param context The context.
 * @return The budget; ::GRCORE_UNLIMITED if unset or `context` is NULL.
 */
GRCORE_API uint64_t grcore_context_memory_bytes(const GRCORE_Context * context);

/**
 * @brief The guest-depth budget the context was created with.
 *
 * @param context The context.
 * @return The budget; ::GRCORE_UNLIMITED if unset or `context` is NULL.
 */
GRCORE_API uint64_t grcore_context_guest_depth(const GRCORE_Context * context);

/**
 * @brief The native-depth budget the context was created with.
 *
 * @param context The context.
 * @return The budget; ::GRCORE_UNLIMITED if unset or `context` is NULL.
 */
GRCORE_API uint64_t grcore_context_native_depth(const GRCORE_Context * context);

/**
 * @brief Bytes currently charged to the context.
 *
 * @param context The context.
 * @return The byte count; zero for NULL.
 */
GRCORE_API uint64_t grcore_context_memory_in_use(const GRCORE_Context * context);

/**
 * @brief The most bytes ever charged to the context at once.
 *
 * @param context The context.
 * @return The high-water mark; zero for NULL.
 */
GRCORE_API uint64_t grcore_context_memory_peak(const GRCORE_Context * context);

/**
 * @brief Live blocks charged to the context (allocations and page mappings).
 *
 * @param context The context.
 * @return The count; zero for NULL.
 */
GRCORE_API uint64_t grcore_context_memory_blocks(const GRCORE_Context * context);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_CONTEXT_H */
