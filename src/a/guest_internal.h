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

#include <ghoti.io/runtime-core/a/activation.h>
#include <ghoti.io/runtime-core/a/compiled.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/registry.h>
#include <ghoti.io/runtime-core/a/frame.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/b/roots.h>

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

/** @brief The registry, entry slots and retired list of one context
 *   (registry.c). */
typedef struct GRCORE_CodeRegistry GRCORE_CodeRegistry;

/** @brief One activation record, as the stack keeps it. */
typedef struct GRCORE_ActivationRecord {
  uint64_t id;            ///< From the stack's serial; never reused.
  GRCORE_ActivationKind kind;
  GRCORE_EngineId engine; ///< Zero for none.
  bool nested;
  size_t base_frames;     ///< The frame count when it was entered.
  uintptr_t lo;           ///< The C segment, or both zero.
  uintptr_t hi;
  uintptr_t frame_base;   ///< Innermost compiled frame's base; zero for none.
  uintptr_t return_address; ///< Where that frame is stopped (AD-28).
  bool rebuilt;           ///< A JIT record whose compiled frames were rebuilt
                          ///< into the guest frames above `base_frames`, which
                          ///< stay for the interpreter to finish.
} GRCORE_ActivationRecord;

/** @brief One budget scope A opened, as the stack keeps it. */
typedef struct GRCORE_ScopeRecord {
  uint64_t id;               ///< B's fuel scope id.
  size_t frame_base;         ///< The frame count when it was opened.
  size_t activation_base;    ///< The activation count when it was opened.
} GRCORE_ScopeRecord;

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
  /* Activation records and budget scopes, outermost first (AD-17, AD-21). */
  GRCORE_ActivationRecord * activations;
  size_t activation_count;
  size_t activation_capacity;
  uint64_t activation_serial;
  GRCORE_ScopeRecord * scopes;
  size_t scope_count;
  size_t scope_capacity;
  /* Compiled code (AD-28): the registry, entry slots and retired list, made on
   * first use, and how many JIT activation records are open, which is what
   * decides when retired code may be released. */
  GRCORE_CodeRegistry * registry;
  size_t jit_live;
};

/* ---- Reading a descriptor ------------------------------------------------
 *
 * A descriptor states how large it is (a/engine.h). A member after `decoder`
 * is read only through these, which treat a member the descriptor's `size` does
 * not cover as NULL. Registration has already refused a descriptor too small
 * to hold the members before it, so those are read directly. */

static inline void (*grcore_engine_roots(const GRCORE_EngineDescriptor * d))(
    GRCORE_Context *, const GRCORE_AbstractFrame *, const GRCORE_RootVisitor *) {
  return GRCORE_ENGINE_DESCRIPTOR_HAS(d, roots) ? d->roots : NULL;
}
static inline void (*grcore_engine_unwind(const GRCORE_EngineDescriptor * d))(
    GRCORE_Context *, const GRCORE_AbstractFrame *) {
  return GRCORE_ENGINE_DESCRIPTOR_HAS(d, unwind) ? d->unwind : NULL;
}

/** @brief The stack's snapshot hooks (stack.c): the frames, with VALUE slots
 *   written as zero, and the engine table by name. */
GRCORE_Result grcore_guest_snapshot(
    GRCORE_Context * context, void * value, GRCORE_SnapshotWriter * writer);
GRCORE_Result grcore_guest_restore(GRCORE_Context * context, void * value,
    GRCORE_SnapshotReader * reader, void * env, GRCORE_RestoreMode mode);
GRCORE_Result grcore_guest_settle(GRCORE_Context * context, void * value,
    void * env, GRCORE_SettleMode mode);

/** @brief The key the state is registered under (cardinality one). */
extern const GRCORE_Key grcore_guest_key;

/**
 * @brief The result for an allocation `context`'s allocator refused: a budget
 *   refusal (the refusal counter rose since `refusals_before`) is
 *   ::GRCORE_ERR_LIMIT, anything else ::GRCORE_ERR_OOM.
 */
GRCORE_Result grcore_guest_alloc_failure(
    const GRCORE_Context * context, uint64_t refusals_before);

/** @brief A's root source over the guest stack; the value is the stack. */
extern const GRCORE_RootSource grcore_guest_root_source;

/** @brief Whether the calling thread owns the stack's context. */
bool grcore_stack_owned(const GRCORE_Stack * stack);

/**
 * @brief Makes room for one more element of an array that grows by copy,
 *   through the context's counting allocator.
 *
 * @return ::GRCORE_OK, with the array in `out_array` (the same pointer if it
 *   had room); ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_OOM, with the array and
 *   capacity unchanged.
 */
GRCORE_Result grcore_guest_array_reserve(GRCORE_Context * context, void * array,
    size_t * capacity, size_t count, size_t element_size, void ** out_array);

/**
 * @brief Builds the abstract frame of `ref` for an engine hook. The location
 *   is not filled in.
 *
 * @return True if `ref` is a live frame, with `out` and `header` filled.
 */
bool grcore_stack_hook_frame(const GRCORE_Stack * stack, GRCORE_FrameRef ref,
    size_t depth, GRCORE_AbstractFrame * out, GRCORE_FrameHeader * header);

/** @brief Pops frames, running each engine's `unwind` hook first, until
 *   `target` remain. */
GRCORE_Result grcore_unwind_frames(
    GRCORE_Stack * stack, size_t target, size_t * popped);

/** @brief Releases the retired code once no JIT record is open (registry.c).
 *   Called when the last one leaves. */
void grcore_registry_release_retired(GRCORE_Stack * stack);

/** @brief Releases everything the registry holds, retired or not, and frees
 *   it. For the context's destruction only. */
void grcore_registry_destroy(GRCORE_Stack * stack);

/** @brief Finds the registered code containing `address`, retired or not
 *   (registry.c). */
bool grcore_registry_find(const GRCORE_Stack * stack, uintptr_t address,
    GRCORE_CodeRange * out);

/**
 * @brief Where, in the guest stack, the guest frame of a compiled frame is.
 *
 * Every guest call pushes the callee's guest frame, compiled or not (AD-28), so
 * the frames of a compiled run stand for consecutive guest frames: the run's
 * outermost frame is the function the interpreter entered (its guest frame is
 * the last one the record's `base_frames` counts), and each frame inward is
 * the next guest frame above it. Guest frames above the innermost compiled
 * frame's are not paired (a callee pushed and not yet entered).
 *
 * @param out_index Receives the index from the outermost frame, zero based.
 * @return False when the record had no guest frame at its entry, so nothing
 *   is paired.
 */
bool grcore_compiled_guest_index(
    const GRCORE_CompiledFrame * frame, size_t * out_index);

/** @brief Moves the walk-start cell compiled code stored (a/layout.h) into the
 *   innermost activation record, and clears it (activation.c).
 *
 *  @return False if the cell is set and the innermost record is not a JIT one,
 *    which compiled code cannot have done: the cell is left alone and the
 *    walk reports it. True otherwise, including for an empty cell. */
bool grcore_activation_absorb_cell(GRCORE_Stack * stack);

/** @brief Leaves the top activation, whatever the stack's state: gives back
 *   its native depth and nesting. Returns false if there is none. */
bool grcore_activation_drop_top(GRCORE_Stack * stack);

/** @brief Closes the top budget scope record and its fuel scope, and any fuel
 *   scope opened above it directly through B. Returns false if there is
 *   none. */
bool grcore_budget_scope_drop_top(GRCORE_Stack * stack);

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
