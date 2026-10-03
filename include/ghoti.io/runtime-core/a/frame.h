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
 * @file frame.h
 * @stability free
 *
 * The abstract frame and the frame walk: the one way every consumer reads a
 * frame (AD-18).
 *
 * The debugger, the collector, the frame-level differential and the recorder
 * all read this, and none of them knows which engine pushed a frame. An
 * abstract frame carries the frame's engine and descriptor, its poll
 * identity, the source location the descriptor gives that identity, and its
 * depth; the slots and scopes are read through accessors that ask the
 * descriptor.
 *
 * Reading is two-tier. The engine that owns the running context uses the
 * stack accessors in stack.h, in any state. Every other consumer uses the
 * walk, which is refused unless the context is at-poll or paused and the
 * caller holds it (AD-20). The accessors here repeat that check, so an
 * abstract frame kept past a resume cannot be read through.
 */

#ifndef GHOTI_IO_GRCORE_A_FRAME_H
#define GHOTI_IO_GRCORE_A_FRAME_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A frame as every consumer reads it. A value: copy it freely; it is
 *   readable only while the context is.
 */
struct GRCORE_AbstractFrame {
  const GRCORE_Context * context;          ///< The context it belongs to.
  GRCORE_EngineId engine;                  ///< The engine that pushed it.
  const GRCORE_EngineDescriptor * descriptor; ///< That engine's descriptor.
  GRCORE_PollIdentity identity;            ///< Where the frame is.
  GRCORE_Location location;                ///< The descriptor's name for it.
  size_t slot_count;                       ///< How many slots.
  size_t depth;                            ///< Zero for the innermost frame.
  GRCORE_FrameRef frame;                   ///< The frame, for stack.h.
};

/** @brief A walk in progress, innermost frame first. */
typedef struct GRCORE_FrameWalk {
  const GRCORE_Context * context; ///< The context being walked.
  GRCORE_FrameRef next;           ///< The next frame; offset zero at the end.
  size_t depth;                   ///< The depth the next frame gets.
} GRCORE_FrameWalk;

/**
 * @brief Begins a walk at the innermost frame.
 *
 * @param context The context, at-poll or paused and held by the caller.
 * @param out_walk Receives the walk. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL, a running or
 *   parked context, or a thread that does not hold it. A context with no
 *   engine, or no frames, walks as empty.
 */
GRCORE_API GRCORE_Result grcore_frame_walk_begin(
    const GRCORE_Context * context, GRCORE_FrameWalk * out_walk);

/**
 * @brief Reads the next frame, moving outward.
 *
 * @param walk The walk.
 * @param out_frame Receives the frame. Written only when this returns true.
 * @return True with a frame; false at the outermost frame's end, or when the
 *   context is no longer readable.
 */
GRCORE_API bool grcore_frame_walk_next(
    GRCORE_FrameWalk * walk, GRCORE_AbstractFrame * out_frame);

/**
 * @brief Reads a slot: its kind, as the engine declares it, and its bits.
 *
 * @param frame The frame.
 * @param index From zero to the slot count minus one.
 * @param out_kind Receives the kind, or may be NULL. Written only on success.
 * @param out_value Receives the value, or may be NULL. Written only on
 *   success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL, an index out of
 *   range or an unreadable context.
 */
GRCORE_API GRCORE_Result grcore_frame_slot(const GRCORE_AbstractFrame * frame,
    size_t index, GRCORE_SlotKind * out_kind, uint64_t * out_value);

/**
 * @brief Renders a slot as its engine's inspector does.
 *
 * @param frame The frame.
 * @param index The slot.
 * @param buffer Receives the text; see ::grcore_engine_inspect.
 * @param size The buffer's size.
 * @param out_length Receives the length the full text needs. Written only on
 *   success.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID.
 */
GRCORE_API GRCORE_Result grcore_frame_inspect(const GRCORE_AbstractFrame * frame,
    size_t index, char * buffer, size_t size, size_t * out_length);

/**
 * @brief How many scopes the frame has.
 *
 * @param frame The frame.
 * @return The count; zero for NULL, an engine with no scope interface, or an
 *   unreadable context.
 */
GRCORE_API size_t grcore_frame_scope_count(const GRCORE_AbstractFrame * frame);

/**
 * @brief Describes one scope.
 *
 * @param frame The frame.
 * @param index From zero to the scope count minus one.
 * @param out_scope Receives the scope. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL, an index out of
 *   range or an unreadable context.
 */
GRCORE_API GRCORE_Result grcore_frame_scope(const GRCORE_AbstractFrame * frame,
    size_t index, GRCORE_ScopeInfo * out_scope);

/**
 * @brief Reads one variable of one scope.
 *
 * @param frame The frame.
 * @param scope The scope index.
 * @param index The variable index, below the scope's variable count.
 * @param out_variable Receives the variable. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL, either index out of
 *   range or an unreadable context.
 */
GRCORE_API GRCORE_Result grcore_frame_variable(
    const GRCORE_AbstractFrame * frame, size_t scope, size_t index,
    GRCORE_Variable * out_variable);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_FRAME_H */
