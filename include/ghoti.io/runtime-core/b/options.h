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
 * @file options.h
 * @stability stable
 *
 * Context options: an opaque object configured through setters.
 *
 * Runtime budgets are options, not a `_Limits` struct (AD-13): a field added
 * to a struct breaks every caller that built one, and a setter does not. An
 * option that has not been set is ::GRCORE_UNLIMITED. Creating a context
 * copies the options it is given; changing them afterwards does not change
 * the context.
 *
 * An options object is used from one thread at a time.
 */

#ifndef GHOTI_IO_GRCORE_B_OPTIONS_H
#define GHOTI_IO_GRCORE_B_OPTIONS_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/core.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The value of a budget that has not been set.
 *
 * It is the largest `uint64_t`, so it is distinct from any budget a caller
 * could mean to enforce.
 */
#define GRCORE_UNLIMITED UINT64_MAX

/**
 * @brief The memory reserve of a context that has not set one, in bytes.
 *
 * The reserve is how far past its memory budget a context may allocate, so
 * that the allocation that crosses the budget can be served and the poll can
 * react to it (AD-21).
 */
#define GRCORE_DEFAULT_MEMORY_RESERVE 262144u

/** @brief Context options. Opaque. */
typedef struct GRCORE_Options GRCORE_Options;

/**
 * @brief Creates options with every budget ::GRCORE_UNLIMITED.
 *
 * @param allocator The allocator for the object and everything it holds, or
 *   NULL for the default. It must outlive the object.
 * @param out_options Receives the object. Written only on success.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID for a NULL output, or
 *   ::GRCORE_ERR_OOM.
 */
GRCORE_API GRCORE_Result grcore_options_create(
    const GRCORE_Allocator * allocator, GRCORE_Options ** out_options);

/**
 * @brief Destroys options. NULL is ignored.
 *
 * @param options The object, from ::grcore_options_create.
 */
GRCORE_API void grcore_options_destroy(GRCORE_Options * options);

/**
 * @brief Sets the fuel budget, the deterministic time budget (AD-21).
 *
 * @param options The options.
 * @param fuel Instruction cost, or ::GRCORE_UNLIMITED.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID for NULL options.
 */
GRCORE_API GRCORE_Result grcore_options_set_fuel(
    GRCORE_Options * options, uint64_t fuel);

/**
 * @brief Sets the memory budget in bytes.
 *
 * @param options The options.
 * @param bytes A byte count, or ::GRCORE_UNLIMITED.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID for NULL options.
 */
GRCORE_API GRCORE_Result grcore_options_set_memory_bytes(
    GRCORE_Options * options, uint64_t bytes);

/**
 * @brief Sets the memory reserve: the bytes a context may allocate beyond its
 *   memory budget before an allocation is refused (AD-21).
 *
 * @param options The options.
 * @param bytes A byte count. Zero means no reserve: the budget is a hard cap.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID for NULL options.
 */
GRCORE_API GRCORE_Result grcore_options_set_memory_reserve(
    GRCORE_Options * options, uint64_t bytes);

/**
 * @brief Sets the guest call-depth budget.
 *
 * @param options The options.
 * @param depth A depth, or ::GRCORE_UNLIMITED.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID for NULL options.
 */
GRCORE_API GRCORE_Result grcore_options_set_guest_depth(
    GRCORE_Options * options, uint64_t depth);

/**
 * @brief Sets the native stack-depth budget.
 *
 * @param options The options.
 * @param depth A depth, or ::GRCORE_UNLIMITED.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID for NULL options.
 */
GRCORE_API GRCORE_Result grcore_options_set_native_depth(
    GRCORE_Options * options, uint64_t depth);

/**
 * @brief The fuel budget.
 *
 * @param options The options.
 * @return The budget; ::GRCORE_UNLIMITED if unset or `options` is NULL.
 */
GRCORE_API uint64_t grcore_options_get_fuel(const GRCORE_Options * options);

/**
 * @brief The memory budget in bytes.
 *
 * @param options The options.
 * @return The budget; ::GRCORE_UNLIMITED if unset or `options` is NULL.
 */
GRCORE_API uint64_t grcore_options_get_memory_bytes(
    const GRCORE_Options * options);

/**
 * @brief The memory reserve in bytes.
 *
 * @param options The options.
 * @return The reserve; ::GRCORE_DEFAULT_MEMORY_RESERVE if unset or `options`
 *   is NULL.
 */
GRCORE_API uint64_t grcore_options_get_memory_reserve(
    const GRCORE_Options * options);

/**
 * @brief The guest call-depth budget.
 *
 * @param options The options.
 * @return The budget; ::GRCORE_UNLIMITED if unset or `options` is NULL.
 */
GRCORE_API uint64_t grcore_options_get_guest_depth(
    const GRCORE_Options * options);

/**
 * @brief The native stack-depth budget.
 *
 * @param options The options.
 * @return The budget; ::GRCORE_UNLIMITED if unset or `options` is NULL.
 */
GRCORE_API uint64_t grcore_options_get_native_depth(
    const GRCORE_Options * options);

/**
 * @brief Stores a copy of `size` bytes under `key`, for a service to read
 *   back when it attaches.
 *
 * The core never interprets the bytes (AD-1). Setting a key again replaces
 * its bytes. A `size` of zero (with any `bytes`) removes the entry. On
 * failure the options are unchanged.
 *
 * @param options The options.
 * @param key The key, identified by address.
 * @param bytes The bytes to copy. May be NULL only when `size` is zero.
 * @param size The byte count.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID for NULL `options`/`key` or NULL
 *   `bytes` with a non-zero `size`, or ::GRCORE_ERR_OOM.
 */
GRCORE_API GRCORE_Result grcore_options_set_keyed(GRCORE_Options * options,
    const GRCORE_Key * key, const void * bytes, size_t size);

/**
 * @brief Reads the bytes stored under `key`.
 *
 * An absent key is not an error: it reads as NULL and zero.
 *
 * @param options The options.
 * @param key The key.
 * @param out_bytes Receives a pointer into the options, valid until they are
 *   next changed or destroyed. Written only on success.
 * @param out_size Receives the byte count. Written only on success.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID for a NULL argument.
 */
GRCORE_API GRCORE_Result grcore_options_get_keyed(const GRCORE_Options * options,
    const GRCORE_Key * key, const void ** out_bytes, size_t * out_size);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_OPTIONS_H */
