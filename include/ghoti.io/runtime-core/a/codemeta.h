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
 * @file codemeta.h
 * @stability free
 *
 * The code-metadata format: stack maps and deoptimization records for
 * machine code (AD-14, AD-17).
 *
 * A code generator emits one *site* for every place where a collection can
 * run (a poll, an allocation slow path, a call out of guest code, a frame
 * push, a nested-activation entry) and for every guard that can fail. A site
 * names its place in the emitted code by an offset, and says where, relative
 * to the frame base, every live reference is, and where each slot of the
 * interpreter frame it stands for can be read. The collector reads the first
 * to find precise roots in a compiled frame; pause-time rebuild and a failed
 * guard read the second. Both are written against this header so that the
 * reader exists before any one writer does.
 *
 * Nothing here knows an engine, a service or a register. The *frame base* is
 * whatever the code generator calls it: `rbp` for the x86-64 baseline, `x29` for
 * the arm64 one. The
 * frame lies below the base, and a slot is named by the signed byte offset of
 * its lowest byte from the base (so a slot of either baseline is `-8`, `-16`,
 * ...).
 *
 * A table is immutable once built and owned by whoever built it: the
 * functions here only read it. It is also untrusted input to the readers, so
 * ::grcore_codemeta_validate checks everything a reader would otherwise
 * assume, and never reads outside the table it was given.
 *
 * Labelled `free` (AD-14): a consumer requires the exact version it was built
 * against, and ::GRCORE_CODEMETA_FORMAT_VERSION is checked by the validator.
 */

#ifndef GHOTI_IO_GRCORE_A_CODEMETA_H
#define GHOTI_IO_GRCORE_A_CODEMETA_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/core.h>

#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/stack.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The version of this format. A table of any other version is corrupt. */
#define GRCORE_CODEMETA_FORMAT_VERSION UINT32_C(1)

/**
 * @brief What a site is. A site is exactly one kind.
 *
 * The first five are AD-17's GC points. A call out of guest code is a
 * ::GRCORE_SITE_GC_POINT_CALL; a plain native helper call that cannot reach a
 * GC point is not a site at all.
 */
typedef enum GRCORE_CodeSiteKind {
  GRCORE_SITE_GC_POINT_POLL,         ///< The slow call of a poll.
  GRCORE_SITE_GC_POINT_ALLOC_SLOW,   ///< An allocation slow path.
  GRCORE_SITE_GC_POINT_CALL,         ///< A call out of guest code.
  GRCORE_SITE_GC_POINT_FRAME_PUSH,   ///< A frame push.
  GRCORE_SITE_GC_POINT_NESTED_ENTRY, ///< A nested-activation entry.
  GRCORE_SITE_GUARD,                 ///< A guard's deoptimizing exit.
  GRCORE_SITE_KIND_COUNT             ///< Not a kind; closes the enum.
} GRCORE_CodeSiteKind;

/** @brief Where a value is. */
typedef enum GRCORE_CodeLocationKind {
  GRCORE_LOC_FRAME_SLOT, ///< In the frame: `value` is a signed byte offset.
  GRCORE_LOC_CONSTANT,   ///< Known: `value` is the 64-bit immediate.
  GRCORE_LOC_DEAD,       ///< Not needed; a rebuild may put anything there.
  GRCORE_LOC_KIND_COUNT  ///< Not a kind; closes the enum.
} GRCORE_CodeLocationKind;

/**
 * @brief Where one word is, and what it is.
 *
 * `slot_kind` tells a consumer whether the word is a reference
 * (::GRCORE_SLOT_VALUE) or plain bits (::GRCORE_SLOT_RAW). For a `CONSTANT`
 * the immediate is `(uint64_t)value`.
 */
typedef struct GRCORE_CodeLocation {
  GRCORE_CodeLocationKind kind; ///< Which of the three.
  GRCORE_SlotKind slot_kind;    ///< Reference or raw bits.
  int64_t value;                ///< Offset or immediate; zero for `DEAD`.
} GRCORE_CodeLocation;

/**
 * @brief A derived pointer (AD-12): an interior pointer a moving collector
 *   must rewrite from its base object.
 *
 * Its value is the word in `base_slot` plus `delta`. Both slots are frame
 * slots; the base is one of the site's live references.
 */
typedef struct GRCORE_DerivedPointer {
  int64_t slot;      ///< Frame-slot offset holding the derived pointer.
  int64_t base_slot; ///< Frame-slot offset of its base reference.
  int64_t delta;     ///< Byte delta from the base to the derived pointer.
} GRCORE_DerivedPointer;

/**
 * @brief One site.
 *
 * `code_offset` is the offset from the start of the code of the return
 * address for a call or a poll's slow call, and of the failing branch's exit
 * stub for a guard. It is unique and increasing within a table. `identity` is
 * the poll identity of AD-18 (what a deoptimization returns to). `live` is
 * the stack map: every live reference, each a ::GRCORE_LOC_FRAME_SLOT.
 * `frame_state` is one location per slot of the interpreter frame, in the
 * interpreter's slot order.
 */
typedef struct GRCORE_CodeSite {
  uint32_t code_offset;                     ///< See above.
  GRCORE_CodeSiteKind kind;                 ///< Which kind of site.
  GRCORE_PollIdentity identity;             ///< (function, offset), AD-18.
  const GRCORE_CodeLocation * live;         ///< The stack map.
  size_t live_count;                        ///< Entries of `live`.
  const GRCORE_DerivedPointer * derived;    ///< Derived pointers.
  size_t derived_count;                     ///< Entries of `derived`.
  const GRCORE_CodeLocation * frame_state;  ///< The deopt frame state.
  size_t frame_state_count;                 ///< Interpreter slot count.
} GRCORE_CodeSite;

/**
 * @brief A table of sites for one piece of code.
 *
 * Immutable once built; owned by whoever built it (the arrays are theirs).
 * `frame_bytes` is the size of the frame below the frame base, which bounds
 * every slot offset; `code_bytes` the size of the code the offsets refer to.
 */
typedef struct GRCORE_CodeMeta {
  uint32_t version;              ///< ::GRCORE_CODEMETA_FORMAT_VERSION.
  uint32_t frame_bytes;          ///< Frame size below the base, bytes.
  uint32_t code_bytes;           ///< Code size, bytes.
  size_t site_count;             ///< Entries of `sites`.
  const GRCORE_CodeSite * sites; ///< Sorted by strictly increasing offset.
} GRCORE_CodeMeta;

/**
 * @brief Checks a table the way a reader would rely on it.
 *
 * Refuses with `GRCORE_ERR_CORRUPT`, naming the first offence in
 * `*out_reason`, for: a wrong version; a table whose `code_bytes` is not the
 * argument; offsets that are not strictly increasing or that are not inside
 * the code; a site kind or location kind that is not one of the enum's; a
 * slot offset that is not 8-byte aligned or not inside `frame_bytes`; a stack
 * map entry that is not a frame slot; a derived pointer whose base is not a
 * live reference of the same site; a function identity whose interpreter-slot
 * count differs between two of its sites; and a NULL array with a non-zero
 * count. Reads only the table it is given. The cost is the sites times the
 * number of distinct function identities among them.
 *
 * @param meta The table.
 * @param code_bytes The size of the code it describes.
 * @param out_reason Receives a static string on `GRCORE_ERR_CORRUPT`; may be
 *   NULL.
 * @return `GRCORE_OK`, `GRCORE_ERR_INVALID` for a NULL table, or
 *   `GRCORE_ERR_CORRUPT`.
 */
GRCORE_API GRCORE_Result grcore_codemeta_validate(
    const GRCORE_CodeMeta * meta, size_t code_bytes, const char ** out_reason);

/**
 * @brief Finds the site at exactly `code_offset`.
 *
 * Binary search over a validated table.
 *
 * @return The site, or NULL when there is none at that offset (or the table
 *   is NULL).
 */
GRCORE_API const GRCORE_CodeSite * grcore_codemeta_find(
    const GRCORE_CodeMeta * meta, uint32_t code_offset);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_CODEMETA_H */
