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
 * @brief A source of whole pages. Define it with ::GRCORE_PAGE_PROVIDER_INIT.
 *
 * `map` returns zero-filled, readable and writable memory, or NULL on
 * failure. `page_size` must be non-zero. `size` must be a non-zero multiple
 * of `page_size`; anything else returns NULL. `unmap` takes the same `size`
 * the mapping was made with. A mapping is never both writable and executable
 * (AD-13).
 *
 * `protect` changes the protection of a range of a mapping the provider made
 * (`ptr` and `size` page-aligned) and returns false on failure. It is a
 * trailing member, and a provider whose `size` ends before it has none, which
 * means "unsupported": `grcore_page_protect` refuses with `ERR_INVALID`. Added
 * for `runtime-jit`, which cannot make code executable without it; the
 * interface is otherwise unchanged and nothing was released.
 *
 * **The struct grows at the end, and `size` says how far.** The rule is
 * ::GRCORE_Key's (key.h has the argument and the rejected alternatives). A
 * provider is filled in by the host, by an initialiser or by assigning members
 * to a zeroed struct, and C zero-fills an omitted member only in an
 * initialiser, which is exactly what code compiled before a member existed
 * cannot be relied on to have done. So the first member, `size`, is the
 * `sizeof(GRCORE_PageProvider)` of the header the provider was compiled
 * against, ::GRCORE_PAGE_PROVIDER_INIT writes it, and a member after `unmap`
 * is read only where `size` covers it (::GRCORE_PAGE_PROVIDER_HAS), an absent
 * one being NULL. A struct filled by assignment starts from
 * ::GRCORE_PAGE_PROVIDER_INIT too, never from `{}`: a `size` of zero is
 * refused, not read.
 *
 * Everything that takes a provider (::grcore_group_create,
 * ::grcore_page_protect) refuses with ::GRCORE_ERR_INVALID one that
 * ::grcore_page_provider_valid does not accept. A `size` larger than this
 * header's is accepted and the members this library does not know are
 * ignored, so each future member must be one whose absence is a defined
 * degradation, as `protect`'s is.
 */
typedef struct GRCORE_PageProvider {
  /**
   * `sizeof(GRCORE_PageProvider)` as the provider's definer compiled it. Set
   * by ::GRCORE_PAGE_PROVIDER_INIT. Never write it by hand.
   */
  size_t size;
  void * ctx;       ///< Passed to each call.
  size_t page_size; ///< The granule, a power of two.
  void * (*map)(void * ctx, size_t size);              ///< Maps pages.
  void (*unmap)(void * ctx, void * ptr, size_t size);  ///< Releases them.
  /// Changes protection; NULL means unsupported. Absent (and so NULL) in a
  /// provider whose `size` ends before it.
  bool (*protect)(
      void * ctx, void * ptr, size_t size, GRCORE_PageAccess access);
} GRCORE_PageProvider;

/**
 * @brief The smallest `size` a provider may state: the layout through
 *   `unmap`, which is the layout before `protect` existed.
 */
#define GRCORE_PAGE_PROVIDER_MIN_SIZE                                       \
  (offsetof(GRCORE_PageProvider, unmap) +                                   \
      sizeof(((GRCORE_PageProvider *)0)->unmap))

/**
 * @brief The initialiser of a page provider:
 *   `GRCORE_PAGE_PROVIDER_INIT(ctx, page_size, map, unmap, protect)`.
 *
 * Expands to a braced initialiser that begins with
 * `sizeof(GRCORE_PageProvider)`, a constant expression in C17 and C++20. Give
 * every member, NULL where unused; `GRCORE_PAGE_PROVIDER_INIT(NULL, 0, NULL,
 * NULL, NULL)` is the starting point of a provider filled in by assignment.
 * @code
 * static const GRCORE_PageProvider mine = GRCORE_PAGE_PROVIDER_INIT(
 *     &state, 4096, my_map, my_unmap, NULL);
 * @endcode
 */
#define GRCORE_PAGE_PROVIDER_INIT(...) \
  { sizeof(GRCORE_PageProvider), __VA_ARGS__ }

/**
 * @brief Whether a provider's `size` covers a member, so that reading it is
 *   defined.
 *
 * @code
 * GRCORE_PAGE_PROVIDER_HAS(p, protect) && p->protect != NULL
 * @endcode
 */
#define GRCORE_PAGE_PROVIDER_HAS(provider, member)                          \
  ((provider)->size >= offsetof(GRCORE_PageProvider, member) +              \
      sizeof((provider)->member))

/**
 * @brief Whether a provider is acceptable (see ::GRCORE_PageProvider).
 *
 * @param provider The provider. NULL is not valid.
 * @return true when `size` is at least ::GRCORE_PAGE_PROVIDER_MIN_SIZE and a
 *   multiple of the struct's alignment.
 */
GRCORE_API bool grcore_page_provider_valid(const GRCORE_PageProvider * provider);

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
 * @return `GRCORE_OK`; `GRCORE_ERR_INVALID` for NULL, an invalid provider, one with no
 *   `protect`, an unaligned `ptr` or `size`, or an unknown access; `GRCORE_ERR_IO` when the
 *   provider reports failure (the protection is then unspecified).
 */
GRCORE_API GRCORE_Result grcore_page_protect(const GRCORE_PageProvider * provider,
    void * ptr, size_t size, GRCORE_PageAccess access);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_PAGE_H */
