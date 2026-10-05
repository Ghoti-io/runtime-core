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
 * @file roots.h
 * @stability stable
 *
 * Root sources: how a collector finds a context's roots without knowing who
 * holds them (AD-11, AD-18).
 *
 * A library that holds values the collector must see (A's guest stack is the
 * first) registers a *root source* with the context. The collector pulls the
 * context's sources from B and calls them through these types alone, so it
 * includes nothing from A (AD-2). A source reports two kinds of root, both as
 * plain data:
 *
 *  - *precise* roots: the address of a 64-bit slot holding a value. The
 *    visitor may write through it, so a moving collector can update a slot in
 *    place (AD-12);
 *  - *conservative ranges*: a span of memory the collector scans word by
 *    word, with the mask, shift and base that turn a word into an address.
 *
 * An address handed to the visitor is valid only during the callback, and
 * only while the visitor does not push, pop or poll: the stack that holds a
 * slot may move when it grows (AD-17).
 *
 * Threads: a context's sources are the owner's, so every function here is
 * called by the owning thread.
 */

#ifndef GHOTI_IO_GRCORE_B_ROOTS_H
#define GHOTI_IO_GRCORE_B_ROOTS_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A span of memory to scan conservatively, and how to read a word of
 *   it: `address = ((word & mask) >> shift) + base`.
 */
typedef struct GRCORE_ConservativeRange {
  uint64_t lo;       ///< The first byte of the span.
  uint64_t hi;       ///< One past the last byte.
  uint64_t mask;     ///< Bits of a word that carry an address.
  unsigned shift;    ///< How far to shift them down; below 64.
  uint64_t base;     ///< Added after the shift.
} GRCORE_ConservativeRange;

/** @brief What a root source reports its roots to. */
typedef struct GRCORE_RootVisitor {
  void * user; ///< Handed back to both callbacks.
  /** A precise root: `slot` is the address of a 64-bit value slot, which the
   *  callback may rewrite. May be NULL, and the source then skips them. */
  void (*slot)(void * user, uint64_t * slot);
  /** A conservative range. May be NULL, and the source then skips them. */
  void (*range)(void * user, const GRCORE_ConservativeRange * range);
} GRCORE_RootVisitor;

/**
 * @brief A root source: a name and one function that reports the roots.
 *   Define it `static` (or `const`) with ::GRCORE_ROOT_SOURCE_INIT and
 *   register it by address.
 *
 * **The struct grows at the end, and `size` says how far.** The rule is
 * ::GRCORE_Key's (b/key.h has the argument and the rejected alternatives): the
 * first member, `size`, is the `sizeof(GRCORE_RootSource)` of the header the
 * source was compiled against, ::GRCORE_ROOT_SOURCE_INIT writes it, and a
 * member added after `enumerate` is read only where `size` covers it, an absent
 * one being NULL. No such member exists yet, so
 * ::GRCORE_ROOT_SOURCE_MIN_SIZE is the whole struct. ::grcore_context_add_root_source
 * refuses with ::GRCORE_ERR_INVALID a source that ::grcore_root_source_valid
 * does not accept; a larger `size` is accepted and the unknown tail ignored.
 */
typedef struct GRCORE_RootSource {
  /**
   * `sizeof(GRCORE_RootSource)` as the source's definer compiled it. Set by
   * ::GRCORE_ROOT_SOURCE_INIT. Never write it by hand.
   */
  size_t size;
  const char * name; ///< For diagnostics. May be NULL.
  /**
   * Reports every root the source holds to `visitor`, precise ones before
   * conservative ones, innermost first where the source has an order. Must not
   * add or remove a root source, or push, pop or poll.
   */
  void (*enumerate)(
      GRCORE_Context * context, void * value, const GRCORE_RootVisitor * visitor);
} GRCORE_RootSource;

/** @brief The smallest `size` a root source may state: the layout through
 *   `enumerate`, which is every member there is. */
#define GRCORE_ROOT_SOURCE_MIN_SIZE                                         \
  (offsetof(GRCORE_RootSource, enumerate) +                                 \
      sizeof(((GRCORE_RootSource *)0)->enumerate))

/** @brief The initialiser of a root source:
 *   `GRCORE_ROOT_SOURCE_INIT(name, enumerate)`. Begins with
 *   `sizeof(GRCORE_RootSource)`. */
#define GRCORE_ROOT_SOURCE_INIT(...) { sizeof(GRCORE_RootSource), __VA_ARGS__ }

/** @brief Whether a root source's `size` covers a member, so that reading it
 *   is defined. */
#define GRCORE_ROOT_SOURCE_HAS(source, member)                              \
  ((source)->size >= offsetof(GRCORE_RootSource, member) +                  \
      sizeof((source)->member))

/**
 * @brief Whether a root source is acceptable (see ::GRCORE_RootSource).
 *
 * @param source The source. NULL is not valid.
 * @return true when `size` is at least ::GRCORE_ROOT_SOURCE_MIN_SIZE and a
 *   multiple of the struct's alignment.
 */
GRCORE_API bool grcore_root_source_valid(const GRCORE_RootSource * source);

/**
 * @brief Adds a root source to a context, after the ones already there.
 *
 * The table is allocated through the context's counting allocator and freed
 * when the context is destroyed. The source must outlive the context.
 *
 * @param context The context. The caller must own it, and it must not be
 *   tearing down.
 * @param source The source, by address. Must be valid (see
 *   ::grcore_root_source_valid) and have an `enumerate` function.
 * @param value Handed to `enumerate`.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for a NULL argument, a source
 *   that is invalid (`size`) or has no `enumerate`, a source and value already added, or a non-owner;
 *   ::GRCORE_ERR_LIMIT if the memory budget refuses the table's growth;
 *   ::GRCORE_ERR_OOM for any other allocation failure. A refusal leaves the
 *   table unchanged.
 */
GRCORE_API GRCORE_Result grcore_context_add_root_source(GRCORE_Context * context,
    const GRCORE_RootSource * source, void * value);

/**
 * @brief Removes a root source added with the same source and value.
 *
 * @param context The context. The caller must own it.
 * @param source The source.
 * @param value The value it was added with.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for a NULL argument, a
 *   non-owner or a source that is not there (nothing changes).
 */
GRCORE_API GRCORE_Result grcore_context_remove_root_source(
    GRCORE_Context * context, const GRCORE_RootSource * source, void * value);

/**
 * @brief How many root sources the context holds.
 *
 * @param context The context.
 * @return The count; zero for NULL.
 */
GRCORE_API size_t grcore_context_root_source_count(const GRCORE_Context * context);

/**
 * @brief Reads root source `index`, in the order they were added.
 *
 * For a consumer that needs to say *which* source a root came from (a
 * retention query names the source and the slot's ordinal in its
 * enumeration), which ::grcore_context_enumerate_roots cannot tell it: that
 * call hands every root to one visitor. The consumer calls the source's own
 * `enumerate` with a visitor of its own, which is exactly what the
 * enumeration does for each entry.
 *
 * Added for the retention query (runtime-heap, CAP-13), which the original
 * root-source surface could not serve; it is part of this header's `stable`
 * surface. The index is a position in the table, so removing a source moves
 * every source after it down by one.
 *
 * @param context The context. The caller must own it.
 * @param index From zero to the count minus one.
 * @param out_source Receives the source, or may be NULL. Written only on
 *   success.
 * @param out_value Receives the value it was added with, or may be NULL.
 *   Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL, a non-owner or an
 *   index out of range (nothing is written).
 */
GRCORE_API GRCORE_Result grcore_context_root_source(
    const GRCORE_Context * context, size_t index,
    const GRCORE_RootSource ** out_source, void ** out_value);

/**
 * @brief Asks every root source for its roots, in the order they were added.
 *
 * Allowed in any state, since the collector runs at-poll and a host may
 * enumerate a paused context.
 *
 * @param context The context. The caller must own it.
 * @param visitor Receives the roots. Must not be NULL.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or a non-owner.
 */
GRCORE_API GRCORE_Result grcore_context_enumerate_roots(
    GRCORE_Context * context, const GRCORE_RootVisitor * visitor);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_ROOTS_H */
