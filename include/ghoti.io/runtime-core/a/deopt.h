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
 * @file deopt.h
 * @stability free
 *
 * Reading and writing a native frame by its metadata (AD-17).
 *
 * A site of a ::GRCORE_CodeMeta (`a/codemeta.h`) says where each slot of the
 * interpreter frame it stands for can be read. These two functions are the
 * reader and the writer of that description, over a frame base the caller
 * found:
 *
 * - ::grcore_deopt_read fills an array of interpreter slots from a native
 *   frame: what a deoptimizer does when a guard fails, and what a poll does
 *   to make the interpreter's own frame current before a pause or a
 *   collection looks at it;
 * - ::grcore_deopt_write_back stores an array of interpreter slots into the
 *   native frame's reference slots, which is how a poll lets a collector that
 *   updated a reference in the interpreter's frame (a moving collector) be
 *   honoured by the compiled code that continues.
 *
 * A location that is a frame slot is a signed byte offset below the frame
 * base: the word is at `frame_base + offset`, with `offset` negative and a
 * multiple of 8 (the validator of `a/codemeta.h` enforces this for a table;
 * these functions trust a table that passed it and read only the words the
 * locations name).
 *
 * Those two are pure functions of their arguments. Neither allocates, polls or
 * keeps anything. They copy a raw word verbatim, which is all a
 * ::GRCORE_REPR_BITS location asks for.
 *
 * **Representations (AD-27).** A location may also carry a representation
 * (`a/codemeta.h`): `I32`, `I64`, `F32` or `F64`, a raw value the engine
 * converts. The rest of this header is that half, and it follows AD-27's rules:
 *
 * - ::grcore_deopt_read_tagged shows each slot as it is in the native frame,
 *   with its representation, before anything is converted (the abstract frame
 *   and a debugger inspect a frame this way, and no inspection triggers a
 *   conversion);
 * - ::grcore_deopt_bind is the binding check: code that has a converting
 *   location is refused unless the engine descriptor supplies `convert` and
 *   `reverse`. A missing conversion is an error and never a default;
 * - ::grcore_deopt_reserve takes, when compiled code is entered, the cells a
 *   rebuild converts into, and registers them as a root source, so a value
 *   being converted is a root the collector sees;
 * - ::grcore_deopt_rebuild converts in two phases from that reservation: it
 *   fills the converted slots with null, converts every raw value into its
 *   cell, and only then writes the slots. After the arguments are accepted
 *   it cannot fail, allocates nothing and does not collect, so no frame is
 *   ever seen half raw and half converted by a collector;
 * - ::grcore_deopt_write_back_checked installs each value through the engine's
 *   reverse conversion, which is also its type test. A value that does not
 *   fit writes nothing and reports that the poll's site is to be exited.
 */

#ifndef GHOTI_IO_GRCORE_A_DEOPT_H
#define GHOTI_IO_GRCORE_A_DEOPT_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/core.h>

#include <ghoti.io/runtime-core/a/codemeta.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/roots.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reads a site's interpreter frame out of a native frame.
 *
 * `slots[i]` becomes the word the site's `i`-th frame-state location names:
 * for `FRAME_SLOT` the word at `frame_base + offset`, for `CONSTANT` the
 * immediate, for `DEAD` zero.
 *
 * @param site The site; its `frame_state` is read.
 * @param frame_base The frame base (`rbp` on the x86-64 baseline, `x29` on the
 *   arm64 one).
 * @param slots Receives `slot_count` words. Written only on success.
 * @param slot_count Must equal `site->frame_state_count`.
 * @return `GRCORE_OK`, or `GRCORE_ERR_INVALID` for a NULL pointer or a count
 *   that differs from the site's (nothing is written).
 */
GRCORE_API GRCORE_Result grcore_deopt_read(const GRCORE_CodeSite * site,
    const void * frame_base, uint64_t * slots, size_t slot_count);

/**
 * @brief Writes interpreter slots back into a native frame's reference slots.
 *
 * For every frame-state location that is a `FRAME_SLOT` with slot kind
 * ::GRCORE_SLOT_VALUE, stores `slots[i]` into the word at `frame_base +
 * offset`. Every other location (raw words, constants, dead slots), and every
 * word no location names, is left untouched.
 *
 * @param site The site; its `frame_state` is read.
 * @param frame_base The frame base.
 * @param slots The interpreter slots, `slot_count` of them.
 * @param slot_count Must equal `site->frame_state_count`.
 * @return `GRCORE_OK`, or `GRCORE_ERR_INVALID` for a NULL pointer or a count
 *   that differs from the site's (nothing is written).
 */
GRCORE_API GRCORE_Result grcore_deopt_write_back(const GRCORE_CodeSite * site,
    void * frame_base, const uint64_t * slots, size_t slot_count);

/**
 * @brief A raw word as it is in a native frame, with what it stands for.
 */
typedef struct GRCORE_DeoptSlot {
  uint64_t word;                        ///< As ::grcore_deopt_read gives it.
  GRCORE_Representation representation; ///< ::GRCORE_REPR_BITS for a `DEAD`.
} GRCORE_DeoptSlot;

/**
 * @brief Like ::grcore_deopt_read, but each slot keeps its representation.
 *
 * Nothing is converted. For code with only ::GRCORE_REPR_BITS locations the
 * words are exactly ::grcore_deopt_read's.
 *
 * @param site The site.
 * @param frame_base The frame base.
 * @param out Receives `slot_count` slots. Written only on success.
 * @param slot_count Must equal `site->frame_state_count`.
 * @return `GRCORE_OK`, or `GRCORE_ERR_INVALID` for a NULL pointer or a wrong
 *   count (nothing is written).
 */
GRCORE_API GRCORE_Result grcore_deopt_read_tagged(const GRCORE_CodeSite * site,
    const void * frame_base, GRCORE_DeoptSlot * out, size_t slot_count);

/**
 * @brief The binding check (AD-27): may this engine run this code?
 *
 * Validates the table (`grcore_codemeta_validate_at`), then checks every
 * converting location in every site against the engine's descriptor. A
 * descriptor that is missing `convert` or `reverse`, or whose `size` ends
 * before them, does not bind code that has a converting location. Code with
 * only ::GRCORE_REPR_BITS locations binds to any descriptor.
 *
 * @param context The context the engine is registered in.
 * @param engine The engine.
 * @param meta The code's table.
 * @param out_reason Receives a static string on a refusal, naming the
 *   representation that has no conversion; may be NULL.
 * @return `GRCORE_OK`; `GRCORE_ERR_UNSUPPORTED` when a converting location has
 *   no conversion; `GRCORE_ERR_CORRUPT` when the table does not validate;
 *   `GRCORE_ERR_INVALID` for a NULL argument or an unknown engine.
 */
GRCORE_API GRCORE_Result grcore_deopt_bind(const GRCORE_Context * context,
    GRCORE_EngineId engine, const GRCORE_CodeMeta * meta,
    const char ** out_reason);

/** @brief The cells a rebuild converts into. Opaque; see
 *    ::grcore_deopt_reserve. */
typedef struct GRCORE_DeoptReservation GRCORE_DeoptReservation;

/**
 * @brief How many converting locations `site` has: the cells one rebuild of it
 *   needs. A compiler sizes the reservation for the largest such count of any
 *   site, and for a chain adds one per compiled call (AD-28).
 */
GRCORE_API size_t grcore_deopt_converting_count(const GRCORE_CodeSite * site);

/**
 * @brief Takes a reservation of `capacity` cells, when compiled code is
 *   entered, so that a later rebuild allocates nothing.
 *
 * The cells are allocated through the context's allocator, charged to it, and
 * the reservation is added to the context as a root source that reports every
 * cell a rebuild has filled, as a precise root. The caller must own the
 * context. The collector reads the reservation at a poll, on the thread that
 * owns the context, so it needs no synchronization.
 *
 * @param context The context.
 * @param capacity The cells; may be zero.
 * @param out Receives the reservation. Written only on success.
 * @return `GRCORE_OK`; `GRCORE_ERR_INVALID` for a NULL argument or a non-owner;
 *   `GRCORE_ERR_LIMIT` or `GRCORE_ERR_OOM` when the memory cannot be had (this
 *   is where a rebuild's cost can fail, and not later).
 */
GRCORE_API GRCORE_Result grcore_deopt_reserve(GRCORE_Context * context,
    size_t capacity, GRCORE_DeoptReservation ** out);

/** @brief The cells `reservation` holds. Zero for NULL. */
GRCORE_API size_t grcore_deopt_reservation_capacity(
    const GRCORE_DeoptReservation * reservation);

/**
 * @brief Adds `cells` to a reservation: what each compiled call does for its
 *   callee (AD-28).
 *
 * A chain of compiled frames is rebuilt in one piece (::grcore_compiled_rebuild
 * in `a/compiled.h`), so the cells it converts into must hold every frame's
 * values at once. Each compiled call therefore extends the reservation by the
 * callee's own maximum before the callee runs, so that the rebuild that a guard
 * or a pause in the callee may start cannot fail for want of one. The memory is
 * taken here, through the context's allocator, where a refusal can still be
 * answered: the call site exits instead of making the call.
 *
 * @param context The context. The caller must own it.
 * @param reservation The reservation.
 * @param cells How many to add; zero is accepted and does nothing.
 * @return `GRCORE_OK`; `GRCORE_ERR_INVALID` for a NULL argument, a non-owner or
 *   a count that overflows; `GRCORE_ERR_LIMIT` or `GRCORE_ERR_OOM`, with the
 *   reservation unchanged.
 */
GRCORE_API GRCORE_Result grcore_deopt_reservation_extend(GRCORE_Context * context,
    GRCORE_DeoptReservation * reservation, size_t cells);

/**
 * @brief Gives back what ::grcore_deopt_reservation_extend added, once the call
 *   it was for has returned.
 *
 * Never allocates or frees, so it cannot fail; the memory is kept for the next
 * call. Retracting more than was added leaves the capacity at zero.
 *
 * @param reservation The reservation; NULL does nothing.
 * @param cells How many to give back.
 */
GRCORE_API void grcore_deopt_reservation_retract(
    GRCORE_DeoptReservation * reservation, size_t cells);

/**
 * @brief Removes the root source and frees the reservation.
 *
 * The caller must own the context, as for ::grcore_deopt_reserve. **Release
 * every reservation before the context is destroyed, or in a key's `destroy`
 * hook**: destruction frees the root-source table but cannot know a
 * reservation, whose memory would be leaked. While the context is being
 * destroyed (a key's destructor is called then) the root table cannot be edited
 * and is about to be freed whole, so the reservation is freed without being
 * removed from it.
 *
 * @param context The context the reservation was taken from.
 * @param reservation The reservation; NULL is accepted and does nothing.
 * @return `GRCORE_OK`; `GRCORE_ERR_INVALID` for a NULL context or a
 *   non-owner, or, outside destruction, when the root source cannot be
 *   removed. A refusal frees nothing, because a collector could still read
 *   what was not removed.
 */
GRCORE_API GRCORE_Result grcore_deopt_release(
    GRCORE_Context * context, GRCORE_DeoptReservation * reservation);

/**
 * @brief Rebuilds one site's interpreter frame, converting raw values.
 *
 * A converting `CONSTANT` location is converted too (its immediate is the raw
 * value); ::grcore_deopt_write_back_checked skips it, since a constant has no
 * frame word to write. A `DEAD` location is never converted: it reads as zero.
 *
 * Phase 0 reads every word as ::grcore_deopt_read does. Phase 1 puts null (0)
 * in every slot of a converting location. Phase 2 converts each raw value,
 * through the engine's `convert`, into a cell of `reservation`, which the
 * context's collector sees as a root. Phase 3 copies the cells into the
 * slots. Once the arguments are accepted nothing can fail: a collection
 * cannot run, and a slot that is not yet written holds null, never a raw
 * word. A short reservation is refused up front.
 *
 * @param context The context. The caller must own it.
 * @param engine The engine whose descriptor converts.
 * @param site The site.
 * @param frame_base The frame base.
 * @param reservation Must hold at least ::grcore_deopt_converting_count cells.
 * @param slots Receives `slot_count` slots, the engine's values for the
 *   converted ones. It is the guest frame's own slot array, which the
 *   collector scans (the engine's slot kinds say which are references), so a
 *   converted value is a root the moment it is written and until then the slot
 *   is null; the reservation holds the value only while it is being
 *   converted. Written only on success; while `convert` runs it holds the
 *   nulls of phase 1 and the words of the others.
 * @param slot_count Must equal `site->frame_state_count`.
 * @return `GRCORE_OK`; `GRCORE_ERR_INVALID` for a NULL argument, a wrong
 *   count or a reservation that is short; `GRCORE_ERR_UNSUPPORTED` when the
 *   engine has no `convert` and the site needs one. Nothing is written for any
 *   refusal.
 */
GRCORE_API GRCORE_Result grcore_deopt_rebuild(GRCORE_Context * context,
    GRCORE_EngineId engine, const GRCORE_CodeSite * site, const void * frame_base,
    GRCORE_DeoptReservation * reservation, uint64_t * slots, size_t slot_count);

/** @brief What a checked write-back did. */
typedef enum GRCORE_DeoptOutcome {
  GRCORE_DEOPT_WRITTEN = 0, ///< Every slot was installed.
  GRCORE_DEOPT_EXIT_AT_SITE ///< A value did not fit; nothing was written, and
                            ///< compiled code exits at the poll's own site.
} GRCORE_DeoptOutcome;

/**
 * @brief ::grcore_deopt_write_back, installing converted slots too.
 *
 * Every `FRAME_SLOT` of a converting representation is installed through the
 * engine's `reverse`, which is its type test. All of them are tested before
 * any word is written, so a value that does not fit leaves the frame exactly
 * as it was and `*out_outcome` is ::GRCORE_DEOPT_EXIT_AT_SITE. A converting
 * `CONSTANT` location is skipped (it has no frame word). Otherwise
 * `VALUE` slots and converted slots are written, and nothing else is.
 *
 * @param context The context.
 * @param engine The engine.
 * @param site The site.
 * @param frame_base The frame base.
 * @param slots The interpreter slots.
 * @param slot_count Must equal `site->frame_state_count`.
 * @param out_outcome Receives the outcome on `GRCORE_OK`.
 * @param out_misfit Receives the slot index of the first value that did not
 *   fit, and `SIZE_MAX` otherwise (also on a refusal); may be NULL.
 * @return `GRCORE_OK` (read `*out_outcome`); `GRCORE_ERR_INVALID` for a NULL
 *   argument or a wrong count; `GRCORE_ERR_UNSUPPORTED` when the engine has
 *   no `reverse` and the site needs one.
 */
GRCORE_API GRCORE_Result grcore_deopt_write_back_checked(
    GRCORE_Context * context, GRCORE_EngineId engine, const GRCORE_CodeSite * site,
    void * frame_base, const uint64_t * slots, size_t slot_count,
    GRCORE_DeoptOutcome * out_outcome, size_t * out_misfit);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_DEOPT_H */
