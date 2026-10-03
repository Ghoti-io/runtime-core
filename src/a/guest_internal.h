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
 * @file
 *
 * A's per-context state, private part: the stack's layout and the frame
 * header, which no consumer sees (AD-8: the representation stays behind the
 * stack interface so a native segment can replace it).
 */

#ifndef GHOTI_IO_GRCORE_SRC_A_GUEST_INTERNAL_H
#define GHOTI_IO_GRCORE_SRC_A_GUEST_INTERNAL_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/b/key.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bytes reserved at the start of the buffer, so offset zero is none. */
#define GRCORE_STACK_BASE 8u

/** @brief The size of a frame header. */
#define GRCORE_FRAME_HEADER 40u

/** @brief The first buffer's size, in bytes. */
#define GRCORE_STACK_INITIAL_BYTES 512u

/** @brief The largest slot count a frame header can describe. */
#define GRCORE_FRAME_MAX_SLOTS ((UINT32_MAX - GRCORE_FRAME_HEADER) / 8u)

/**
 * @brief A frame header, as it is laid out in the buffer. It is only ever
 *   copied in and out with `memcpy`: the buffer has no declared type.
 */
typedef struct GRCORE_FrameHeader {
  uint64_t prev;        ///< The caller's offset; zero for the outermost.
  uint64_t function;    ///< The poll identity's function.
  uint64_t offset;      ///< The poll identity's offset.
  uint32_t size;        ///< The frame's size, header included.
  uint32_t engine;      ///< The engine id; names the descriptor (AD-18).
  uint32_t slot_count;  ///< 64-bit slots after the header.
  uint32_t tag;         ///< Derived from the frame's own offset.
} GRCORE_FrameHeader;

/** @brief A's state for one context: the guest stack and the engine table. */
struct GRCORE_Stack {
  GRCORE_Context * context;
  unsigned char * buffer;      ///< NULL until the first push or reserve.
  size_t capacity;
  size_t used;                 ///< One past the top frame; the base if none.
  size_t top;                  ///< The innermost frame's offset; zero if none.
  size_t frame_count;
  uint64_t move_count;
  bool always_move;
  const GRCORE_EngineDescriptor ** engines;
  size_t engine_count;
  size_t engine_capacity;
};

/** @brief The key the state is registered under (cardinality one). */
extern const GRCORE_Key grcore_guest_key;

/**
 * @brief The result for an allocation `context`'s allocator refused: a budget
 *   refusal (the refusal counter rose since `refusals_before`) is
 *   ::GRCORE_ERR_LIMIT, anything else ::GRCORE_ERR_OOM.
 */
GRCORE_Result grcore_guest_alloc_failure(
    const GRCORE_Context * context, uint64_t refusals_before);

/**
 * @brief Reads and validates the header of `frame`.
 *
 * @return True if `frame` names a live frame, with the header in `out`.
 */
bool grcore_stack_read_frame(const GRCORE_Stack * stack, GRCORE_FrameRef frame,
    GRCORE_FrameHeader * out);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_SRC_A_GUEST_INTERNAL_H */
