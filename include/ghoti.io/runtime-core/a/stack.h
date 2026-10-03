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
 * @file stack.h
 * @stability free
 *
 * The guest stack: where an engine keeps its frames (AD-8, AD-17).
 *
 * A call from guest code to guest code pushes a frame here instead of
 * recursing in C, so a paused context holds its whole position as data and
 * can move to another thread. The stack belongs to the context and is
 * registered under a key of A's by the first ::grcore_engine_register, so it
 * migrates with the context and B never includes A.
 *
 * Three rules keep the representation open (AD-8: a native stack segment
 * stays possible behind this interface):
 *
 *  - Frames are named by a ::GRCORE_FrameRef, an *offset*, never an address.
 *    The stack grows by allocating a bigger buffer, copying and freeing the
 *    old one, so an address does not survive a push.
 *  - ::grcore_stack_slots hands out an address anyway, because an
 *    interpreter's inner loop needs one. It is valid only until the next
 *    push, pop, reserve or poll (AD-17: every one of those is a GC point), and
 *    must be reloaded after each.
 *  - Every frame header names its engine's descriptor, so a frame can be
 *    read without knowing who pushed it.
 *
 * A push enters the guest-depth budget and a pop leaves it, so the depth limit
 * counts frames (AD-16, AD-21). The stack's memory comes from the context's
 * counting allocator, so it is charged to the context.
 *
 * Threads: a stack is its context's, and every function that changes it or
 * reads a slot must be called by the context's owner. The header readers
 * (::grcore_stack_slot_count, ::grcore_stack_identity, ::grcore_stack_engine,
 * ::grcore_stack_caller, ::grcore_stack_frame_valid) do not check, and are for
 * the owner or the holder of an at-poll or paused context, never a concurrent
 * thread. Reading from outside
 * `run` goes through the frame walk in frame.h, which also checks the state.
 */

#ifndef GHOTI_IO_GRCORE_A_STACK_H
#define GHOTI_IO_GRCORE_A_STACK_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A context's guest stack. Opaque. */
typedef struct GRCORE_Stack GRCORE_Stack;

/**
 * @brief A frame, by its offset in the stack. Zero is no frame; the first
 *   frame is at offset 8.
 *
 * A reference stays valid across growth. It stops being valid when its frame
 * is popped; a later push may reuse the offset, and then the reference names
 * the new frame, as a freed pointer would.
 */
typedef struct GRCORE_FrameRef {
  size_t offset; ///< Zero for none.
} GRCORE_FrameRef;

/**
 * @brief A poll site: a function and a position in it, the same on every tier
 *   (AD-18).
 *
 * What the numbers mean is the engine's: usually a function index and a
 * bytecode offset.
 */
typedef struct GRCORE_PollIdentity {
  uint64_t function; ///< Which function.
  uint64_t offset;   ///< Where in it.
} GRCORE_PollIdentity;

/**
 * @brief The context's guest stack.
 *
 * @param context The context.
 * @return The stack; NULL for NULL, or before an engine is registered.
 */
GRCORE_API GRCORE_Stack * grcore_context_stack(const GRCORE_Context * context);

/**
 * @brief Pushes a frame of `slot_count` zeroed 64-bit slots, caller below.
 *
 * Enters one level of guest depth first, and leaves it again if the stack
 * cannot grow. The buffer may move; every address previously taken from
 * ::grcore_stack_slots is dead.
 *
 * @param stack The stack. The caller must own its context.
 * @param engine The engine pushing the frame, as registered.
 * @param slot_count How many slots.
 * @param out_frame Receives the frame. Written only on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_LIMIT if the guest-depth budget is
 *   reached, or the context's memory budget refuses the growth;
 *   ::GRCORE_ERR_OOM for any other failure to allocate;
 *   ::GRCORE_ERR_INVALID for a NULL argument, an unregistered engine, id zero,
 *   a non-owner or a slot count too large for a frame. A failure leaves the
 *   stack and the depth unchanged.
 */
GRCORE_API GRCORE_Result grcore_stack_push(GRCORE_Stack * stack,
    GRCORE_EngineId engine, size_t slot_count, GRCORE_FrameRef * out_frame);

/**
 * @brief Pops the top frame and leaves one level of guest depth.
 *
 * @param stack The stack. The caller must own its context.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID when there is no frame, for
 *   NULL or a non-owner.
 */
GRCORE_API GRCORE_Result grcore_stack_pop(GRCORE_Stack * stack);

/**
 * @brief The top (innermost) frame.
 *
 * @param stack The stack.
 * @return The frame; offset zero when the stack is empty or NULL.
 */
GRCORE_API GRCORE_FrameRef grcore_stack_top(const GRCORE_Stack * stack);

/**
 * @brief The frame that called `frame`.
 *
 * @param stack The stack.
 * @param frame A frame.
 * @return Its caller; offset zero for the outermost frame, an invalid
 *   reference or NULL.
 */
GRCORE_API GRCORE_FrameRef grcore_stack_caller(
    const GRCORE_Stack * stack, GRCORE_FrameRef frame);

/**
 * @brief Whether `frame` names a live frame of the stack.
 *
 * A reference outside the stack, not at a frame boundary or carrying a
 * forged tag is not valid. This is a check against forgery by accident, not a
 * defence against a hostile caller: a reference that was once a frame and has
 * not been popped since is the only kind that passes.
 *
 * @param stack The stack.
 * @param frame A reference.
 * @return True for a live frame.
 */
GRCORE_API bool grcore_stack_frame_valid(
    const GRCORE_Stack * stack, GRCORE_FrameRef frame);

/**
 * @brief How many frames the stack holds.
 *
 * @param stack The stack.
 * @return The count; zero for NULL.
 */
GRCORE_API size_t grcore_stack_frame_count(const GRCORE_Stack * stack);

/**
 * @brief Bytes the frames occupy, headers included.
 *
 * @param stack The stack.
 * @return The size; zero for NULL or an empty stack.
 */
GRCORE_API size_t grcore_stack_bytes_used(const GRCORE_Stack * stack);

/**
 * @brief Bytes the stack can hold before it must grow.
 *
 * @param stack The stack.
 * @return The size of the current buffer; zero before the first push or
 *   reserve, and for NULL.
 */
GRCORE_API size_t grcore_stack_bytes_capacity(const GRCORE_Stack * stack);

/**
 * @brief How many times the buffer has moved: grown, or replaced in
 *   always-move mode.
 *
 * @param stack The stack.
 * @return The count; zero for NULL.
 */
GRCORE_API uint64_t grcore_stack_move_count(const GRCORE_Stack * stack);

/**
 * @brief Makes room for `bytes` more of frames, so that pushes that fit do
 *   not move the buffer.
 *
 * This is itself a move when it grows, so it kills slot addresses like a push.
 *
 * @param stack The stack. The caller must own its context.
 * @param bytes How many bytes of frames (headers included) to make room for.
 * @return ::GRCORE_OK; ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_OOM as
 *   ::grcore_stack_push; ::GRCORE_ERR_INVALID for NULL or a non-owner.
 */
GRCORE_API GRCORE_Result grcore_stack_reserve(GRCORE_Stack * stack, size_t bytes);

/**
 * @brief Test mode: moves the buffer on every push, whatever its room.
 *
 * With it on, a stale slot address is never accidentally still valid, so a
 * hold across a push shows up the first time a test runs.
 *
 * @param stack The stack. The caller must own its context.
 * @param enabled Whether to move.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID.
 */
GRCORE_API GRCORE_Result grcore_stack_set_always_move(
    GRCORE_Stack * stack, bool enabled);

/**
 * @brief How many slots `frame` has.
 *
 * @param stack The stack.
 * @param frame The frame.
 * @param out_count Receives the count. Written only on success.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID (NULL or an invalid frame).
 */
GRCORE_API GRCORE_Result grcore_stack_slot_count(const GRCORE_Stack * stack,
    GRCORE_FrameRef frame, size_t * out_count);

/**
 * @brief Reads a slot.
 *
 * @param stack The stack. The caller must own its context.
 * @param frame The frame.
 * @param index From zero to the slot count minus one.
 * @param out_value Receives the value. Written only on success.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID (a forged frame, an index out
 *   of range, NULL or a non-owner).
 */
GRCORE_API GRCORE_Result grcore_stack_slot_get(const GRCORE_Stack * stack,
    GRCORE_FrameRef frame, size_t index, uint64_t * out_value);

/**
 * @brief Writes a slot.
 *
 * @param stack The stack. The caller must own its context.
 * @param frame The frame.
 * @param index From zero to the slot count minus one.
 * @param value The value.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID; the slot is untouched on a
 *   refusal.
 */
GRCORE_API GRCORE_Result grcore_stack_slot_set(GRCORE_Stack * stack,
    GRCORE_FrameRef frame, size_t index, uint64_t value);

/**
 * @brief The address of a frame's slots, for an interpreter's inner loop.
 *
 * Valid only until the next push, pop, reserve or poll (AD-17). Reload it
 * after each; do not keep it in a local across a call that can do any of
 * them.
 *
 * @param stack The stack. The caller must own its context.
 * @param frame The frame.
 * @return The first slot; NULL for a frame that is invalid, has no slots, or
 *   a non-owner.
 */
GRCORE_API uint64_t * grcore_stack_slots(GRCORE_Stack * stack, GRCORE_FrameRef frame);

/**
 * @brief The engine that pushed `frame`.
 *
 * @param stack The stack.
 * @param frame The frame.
 * @param out_engine Receives the id. Written only on success.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID.
 */
GRCORE_API GRCORE_Result grcore_stack_engine(const GRCORE_Stack * stack,
    GRCORE_FrameRef frame, GRCORE_EngineId * out_engine);

/**
 * @brief Records a poll identity in `frame`, as a call site.
 *
 * An engine sets this on the caller's frame before it pushes the callee, so
 * that a walk can say where each outer frame is. The innermost frame's
 * identity is recorded by ::grcore_stack_poll.
 *
 * @param stack The stack. The caller must own its context.
 * @param frame The frame.
 * @param identity The identity.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID (nothing changes).
 */
GRCORE_API GRCORE_Result grcore_stack_set_identity(
    GRCORE_Stack * stack, GRCORE_FrameRef frame, GRCORE_PollIdentity identity);

/**
 * @brief The identity last recorded in `frame`; zeroes until one is.
 *
 * @param stack The stack.
 * @param frame The frame.
 * @param out_identity Receives the identity. Written only on success.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID.
 */
GRCORE_API GRCORE_Result grcore_stack_identity(const GRCORE_Stack * stack,
    GRCORE_FrameRef frame, GRCORE_PollIdentity * out_identity);

/**
 * @brief The poll an engine makes at a poll site (AD-4, AD-18).
 *
 * Records (`function`, `offset`) in the top frame, then polls. When the poll
 * has something to say, the location it reports for a pause or an unwind is
 * what the top frame's engine descriptor says that identity is; the
 * descriptor is not consulted on the fast path. With no frames (the stack is
 * empty, or no engine is registered) it polls exactly as ::grcore_poll does,
 * with the location `{NULL, 0}` and no identity.
 *
 * A poll is a GC point, so slot addresses must be reloaded after it (AD-17).
 *
 * @param context The running context, called by its owner.
 * @param function The function's identity.
 * @param offset The position in it.
 * @return The verdict, as ::grcore_poll.
 */
GRCORE_API GRCORE_Verdict grcore_stack_poll(
    GRCORE_Context * context, uint64_t function, uint64_t offset);

/**
 * @brief The identity of the innermost frame, for a consumer outside the
 *   engine: readable states only (AD-20).
 *
 * @param context The context.
 * @param out_identity Receives the identity. Written only on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID when the context is not at-poll or
 *   paused on the calling thread, or has no frame.
 */
GRCORE_API GRCORE_Result grcore_context_poll_identity(
    const GRCORE_Context * context, GRCORE_PollIdentity * out_identity);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_STACK_H */
