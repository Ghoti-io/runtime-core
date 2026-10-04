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
 * @file code.h
 * @stability free
 *
 * Reference-counted compiled code (AD-13).
 *
 * A code generator hands the core a payload (its mapping, its metadata, its
 * entry point) and a function that releases it; the core owns the count. An
 * engine's cache holds one reference and every entry into the code retains
 * one for the call's duration, so code discarded during a run is freed only
 * after the last call that uses it has returned.
 *
 * The count is atomic, which is a documented exception to the single-thread
 * rule of CONVENTIONS section 13: under AD-22 compiled code is shared between
 * contexts, which run on different threads, so a retain and a release may race
 * on one handle. Retain is relaxed and the release that reaches zero is
 * acquire-release, as for `GRCORE_Port`, so the thread that runs `release`
 * sees every write the other holders made to the payload.
 *
 * The library does not know what a payload is. The handle's allocator is a
 * copy taken at creation, so the allocator argument need not outlive it.
 */

#ifndef GHOTI_IO_GRCORE_A_CODE_H
#define GHOTI_IO_GRCORE_A_CODE_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/core.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An opaque, reference-counted piece of compiled code. */
typedef struct GRCORE_Code GRCORE_Code;

/**
 * @brief Creates a handle with a count of one.
 *
 * @param allocator Allocates the handle; NULL means the default.
 * @param payload Whatever the owner needs back at release; may be NULL.
 * @param release Called once, with `payload`, when the count reaches zero,
 *   before the handle is freed. Must not be NULL.
 * @param out Receives the handle on success only.
 * @return `GRCORE_OK`, `GRCORE_ERR_INVALID` for a NULL `release` or `out`, or
 *   `GRCORE_ERR_OOM`. Nothing is written to `out` on failure.
 */
GRCORE_API GRCORE_Result grcore_code_create(const GRCORE_Allocator * allocator,
    void * payload, void (*release)(void * payload), GRCORE_Code ** out);

/**
 * @brief Adds a reference. Safe from any thread.
 *
 * @return `code`, or NULL when `code` is NULL (a no-op).
 */
GRCORE_API GRCORE_Code * grcore_code_retain(GRCORE_Code * code);

/**
 * @brief Drops a reference; at zero, releases the payload and frees the
 *   handle. Safe from any thread. A NULL handle is a no-op.
 */
GRCORE_API void grcore_code_release(GRCORE_Code * code);

/** @brief The payload given at creation; NULL for a NULL handle. */
GRCORE_API void * grcore_code_payload(const GRCORE_Code * code);

/**
 * @brief The current count (a snapshot; another thread may change it). Zero
 *   for a NULL handle.
 */
GRCORE_API size_t grcore_code_refcount(const GRCORE_Code * code);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_CODE_H */
