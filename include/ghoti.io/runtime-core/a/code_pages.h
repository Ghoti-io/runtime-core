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
 */

/**
 * @file code_pages.h
 * @stability free
 *
 * A reference-counted, budgeted executable-page provider for module-shared
 * compiled code. Its mappings are charged to an independent module-owned
 * group's page meter and limited by a separate code-byte cap.
 */

#ifndef GHOTI_IO_GRCORE_A_CODE_PAGES_H
#define GHOTI_IO_GRCORE_A_CODE_PAGES_H

#include <ghoti.io/runtime-core/macros.h>
#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/b/page.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GRCORE_CodePages GRCORE_CodePages;

/**
 * @brief Creates a budgeted provider and independent module-owned group.
 *
 * `byte_cap` counts whole mapped pages and is independent of the group's
 * overall memory meter. A zero cap is valid and refuses every mapping. Each
 * successful mapping holds an object reference until its matching unmap, so
 * releasing the module's owner reference cannot invalidate a live provider.
 *
 * @param allocator Allocator retained by the owned group; NULL uses the
 *   default allocator. It must remain valid until the last mapping unmaps.
 * @param byte_cap Maximum mapped executable bytes for this provider.
 * @param out_pages Receives the provider, written only on success.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID, ::GRCORE_ERR_LIMIT or
 *   ::GRCORE_ERR_OOM.
 */
GRCORE_API GRCORE_Result grcore_code_pages_create(
    const GRCORE_Allocator * allocator, uint64_t byte_cap,
    GRCORE_CodePages ** out_pages);

/** @brief Adds a reference, or returns NULL for NULL. */
GRCORE_API GRCORE_CodePages * grcore_code_pages_retain(
    GRCORE_CodePages * pages);

/** @brief Drops a reference; its owned group is destroyed at the last one. */
GRCORE_API void grcore_code_pages_release(
    GRCORE_CodePages * pages);

/** @brief The budgeted provider passed to a JIT compiler. */
GRCORE_API const GRCORE_PageProvider *
grcore_code_pages_provider(
    const GRCORE_CodePages * pages);

/** @brief The counted allocator of the module-owned code group. */
GRCORE_API const GRCORE_Allocator * grcore_code_pages_allocator(
    const GRCORE_CodePages * pages);

/** @brief Current mapped code bytes. */
GRCORE_API uint64_t grcore_code_pages_in_use(
    const GRCORE_CodePages * pages);

/** @brief Peak mapped code bytes. */
GRCORE_API uint64_t grcore_code_pages_peak(
    const GRCORE_CodePages * pages);

/** @brief The immutable byte cap. */
GRCORE_API uint64_t grcore_code_pages_cap(
    const GRCORE_CodePages * pages);

/** @brief Bytes currently charged to the module-owned code group. */
GRCORE_API uint64_t grcore_code_pages_accounted_bytes(
    const GRCORE_CodePages * pages);

#ifdef __cplusplus
}
#endif

#endif
