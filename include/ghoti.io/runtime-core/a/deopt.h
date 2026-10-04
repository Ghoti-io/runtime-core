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
 * Both are pure functions of their arguments. Neither allocates, polls or
 * keeps anything.
 */

#ifndef GHOTI_IO_GRCORE_A_DEOPT_H
#define GHOTI_IO_GRCORE_A_DEOPT_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/core.h>

#include <ghoti.io/runtime-core/a/codemeta.h>

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

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_DEOPT_H */
