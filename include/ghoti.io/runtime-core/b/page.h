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
 * @file page.h
 * @stability stable
 *
 * The page provider: how `runtime-heap` and `runtime-jit` obtain pages.
 *
 * Nothing above the core maps memory on its own. A context hands out a
 * counting provider, so every page a heap or a code cache holds is charged to
 * that context's meter (AD-13, AD-20).
 */

#ifndef GHOTI_IO_GRCORE_B_PAGE_H
#define GHOTI_IO_GRCORE_B_PAGE_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The protection a mapping can be given.
 *
 * Never both writable and executable (AD-13): a JIT maps read-write, fills
 * the pages, then flips them to read-execute once.
 */
typedef enum GRCORE_PageAccess {
  GRCORE_PAGE_READ_WRITE,   ///< Readable and writable, not executable.
  GRCORE_PAGE_READ_EXECUTE  ///< Readable and executable, not writable.
} GRCORE_PageAccess;

/**
 * @brief A source of whole pages.
 *
 * `map` returns zero-filled, readable and writable memory, or NULL on
 * failure. `page_size` must be non-zero. `size` must be a non-zero multiple
 * of `page_size`; anything else returns NULL. `unmap` takes the same `size`
 * the mapping was made with. A mapping is never both writable and executable
 * (AD-13).
 *
 * `protect` changes the protection of a range of a mapping the provider made
 * (`ptr` and `size` page-aligned) and returns false on failure. It is a
 * trailing member, so a provider written before it was added has it zero
 * filled, which means "unsupported": `grcore_page_protect` refuses with
 * `ERR_INVALID`. Added for `runtime-jit`, which cannot make code executable
 * without it; the interface is otherwise unchanged and nothing was released.
 */
typedef struct GRCORE_PageProvider {
  void * ctx;       ///< Passed to each call.
  size_t page_size; ///< The granule, a power of two.
  void * (*map)(void * ctx, size_t size);              ///< Maps pages.
  void (*unmap)(void * ctx, void * ptr, size_t size);  ///< Releases them.
  /// Changes protection; NULL means unsupported.
  bool (*protect)(
      void * ctx, void * ptr, size_t size, GRCORE_PageAccess access);
} GRCORE_PageProvider;

/**
 * @brief The process-wide default provider: anonymous `mmap`, or
 *   `VirtualAlloc` on Windows.
 *
 * @return A static provider. Never NULL, never freed.
 */
GRCORE_API const GRCORE_PageProvider * grcore_page_provider_default(void);

/**
 * @brief Changes the protection of pages a provider mapped.
 *
 * @param provider The provider that made the mapping.
 * @param ptr First byte; a multiple of the provider's `page_size`.
 * @param size Byte count; a non-zero multiple of the provider's `page_size`.
 * @param access The new protection.
 * @return `GRCORE_OK`; `GRCORE_ERR_INVALID` for NULL, a NULL `protect`, an
 *   unaligned `ptr` or `size`, or an unknown access; `GRCORE_ERR_IO` when the
 *   provider reports failure (the protection is then unspecified).
 */
GRCORE_API GRCORE_Result grcore_page_protect(const GRCORE_PageProvider * provider,
    void * ptr, size_t size, GRCORE_PageAccess access);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_PAGE_H */
