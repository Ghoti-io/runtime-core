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
 * @file activation.h
 * @stability free
 *
 * Activation records: how control crossed between host, interpreter, JIT code
 * and C (AD-17).
 *
 * Every crossing records a record on the context's stack, entered and left in
 * strict LIFO order. The records let the collector walk a mixed stack: guest
 * frames are read precisely, and only the C segments that records name become
 * conservative ranges (AD-11, AD-18). They also let the unwinder know where to
 * stop, and they carry two budgets:
 *
 *  - A JIT, NATIVE or REENTRY record enters the *native depth* budget when it
 *    is entered and gives it back when it is left (AD-21). A HOST or
 *    INTERPRETER record does not: an interpreter makes no C call to enter a
 *    guest frame (AD-8).
 *  - A *nested* record (a REENTRY is always one) cannot pause to the host
 *    (AD-5): while any is open, a pause verdict becomes a limit unwind, and
 *    after it is left the outer poll pauses as usual.
 *
 * A record stays open across a pause, so a context paused inside one resumes
 * inside it, even on another thread. A record holds no address into the guest
 * stack, only counts and offsets, so growth cannot invalidate one.
 *
 * Threads: entering, leaving and unwinding are the context owner's; the
 * readers do not check and are for the owner or the holder of an at-poll or
 * paused context.
 */

#ifndef GHOTI_IO_GRCORE_A_ACTIVATION_H
#define GHOTI_IO_GRCORE_A_ACTIVATION_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What crossed into the context. */
typedef enum {
  GRCORE_ACTIVATION_HOST = 0, ///< The host called in.
  GRCORE_ACTIVATION_INTERPRETER, ///< An engine's interpreter loop.
  GRCORE_ACTIVATION_JIT,      ///< JIT code, on the native stack.
  GRCORE_ACTIVATION_NATIVE,   ///< A C function the guest called.
  GRCORE_ACTIVATION_REENTRY   ///< C code calling back into guest code.
} GRCORE_ActivationKind;

/**
 * @brief A record, named so that a stale or forged name is refused. Zero is
 *   none.
 */
typedef struct GRCORE_ActivationRef {
  uint64_t id; ///< Never reused within a stack; zero for none.
} GRCORE_ActivationRef;

/**
 * @brief A span of the native stack a record owns, to be scanned
 *   conservatively. `lo` is the lowest address, `hi` one past the highest.
 */
typedef struct GRCORE_CSegment {
  uintptr_t lo; ///< The first byte.
  uintptr_t hi; ///< One past the last byte; above `lo`.
} GRCORE_CSegment;

/** @brief What a record says. */
typedef struct GRCORE_ActivationInfo {
  GRCORE_ActivationKind kind; ///< What crossed.
  GRCORE_EngineId engine;     ///< The engine involved; zero for none.
  bool nested;                ///< Whether it blocks a pause to the host.
  size_t base_frame_count;    ///< Guest frames on the stack when it began.
  uintptr_t segment_lo;       ///< The C segment, or zero.
  uintptr_t segment_hi;       ///< One past it, or zero.
  uintptr_t frame_base;       ///< Innermost compiled frame's base, or zero.
  uintptr_t return_address;   ///< Where that frame is stopped, or zero.
} GRCORE_ActivationInfo;

/**
 * @brief Records a crossing, innermost until it is left.
 *
 * @param stack The stack. The caller must own its context.
 * @param kind What crossed.
 * @param engine The engine involved, as registered, or zero for none (the
 *   collector then scans a segment with the identity decoder).
 * @param nested Whether the activation cannot pause to the host. A REENTRY is
 *   always nested, whatever is passed.
 * @param segment The C segment the record owns, or NULL for none. Only a
 *   record with one contributes a conservative range.
 * @param out_ref Receives the record. Written only on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_LIMIT if the record enters the native
 *   depth budget and it is full, or the context's memory budget refuses the
 *   array's growth; ::GRCORE_ERR_OOM for any other allocation failure;
 *   ::GRCORE_ERR_INVALID for NULL, an unknown kind or engine, an empty
 *   segment or a non-owner. A refusal adds no record and leaves the native
 *   depth and the nesting as they were.
 */
GRCORE_API GRCORE_Result grcore_activation_enter(GRCORE_Stack * stack,
    GRCORE_ActivationKind kind, GRCORE_EngineId engine, bool nested,
    const GRCORE_CSegment * segment, GRCORE_ActivationRef * out_ref);

/**
 * @brief Leaves the innermost activation, giving back what it entered.
 *
 * @param stack The stack. The caller must own its context.
 * @param ref The innermost record.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID with nothing changed for NULL,
 *   a non-owner, a stale or forged reference, a record that is not the
 *   innermost, a stack whose frame count is not the one the record began
 *   with (frames pushed since were not popped), or a budget scope opened
 *   since that has not been closed.
 */
GRCORE_API GRCORE_Result grcore_activation_leave(
    GRCORE_Stack * stack, GRCORE_ActivationRef ref);

/**
 * @brief How many records are open.
 *
 * @param stack The stack.
 * @return The count; zero for NULL.
 */
GRCORE_API size_t grcore_activation_count(const GRCORE_Stack * stack);

/**
 * @brief Reads record `index`, from zero for the outermost.
 *
 * @param stack The stack.
 * @param index Below the count.
 * @param out_info Receives the record. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or an index out of
 *   range.
 */
GRCORE_API GRCORE_Result grcore_activation_at(const GRCORE_Stack * stack,
    size_t index, GRCORE_ActivationInfo * out_info);

/**
 * @brief Reads the record a reference names.
 *
 * @param stack The stack.
 * @param ref An open record.
 * @param out_info Receives the record. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or a reference that
 *   is not open.
 */
GRCORE_API GRCORE_Result grcore_activation_info(const GRCORE_Stack * stack,
    GRCORE_ActivationRef ref, GRCORE_ActivationInfo * out_info);

/**
 * @brief The innermost record.
 *
 * @param stack The stack.
 * @param out_ref Receives the reference. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or no record.
 */
GRCORE_API GRCORE_Result grcore_activation_top(
    const GRCORE_Stack * stack, GRCORE_ActivationRef * out_ref);

/**
 * @brief Records, or clears, the innermost compiled frame under a record
 *   (AD-28).
 *
 * Compiled code stores these before it calls anything that can reach a GC
 * point (a native, the poll's slow path), so that the walk in
 * `a/compiled.h` can find every compiled frame from here. `frame_base` is the
 * frame base of the innermost compiled frame and `return_address` the address
 * in its code that the call returns to, which names the site (the stack map).
 * A record with a non-zero `frame_base` starts a compiled run; zero in both
 * clears the state, which a record that pauses, or whose compiled frames have
 * been rebuilt or unwound, must do. Nothing is checked here: a base that is
 * not a frame is found by the walk, which reports a broken chain.
 *
 * Compiled code does not use this: it stores the same two words in the
 * context's walk-start cell (`a/layout.h`), which core moves into the innermost
 * record at the next activation entry and at a walk (`a/compiled.h`). This is
 * for an engine or a test that has the words in hand.
 *
 * The record, and the JIT record in particular, is also what keeps retired
 * code alive (`a/registry.h`): code retired while a JIT record is open is
 * released when the last one is left.
 *
 * @param stack The stack. The caller must own its context.
 * @param ref An open ::GRCORE_ACTIVATION_JIT record, not necessarily the
 *   innermost: only those keep retired code alive.
 * @param frame_base The frame base, or zero.
 * @param return_address The return address, or zero.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL, a non-owner, a
 *   stale or forged reference, a record that is not a JIT one, or a zero `frame_base` with a non-zero
 *   `return_address`.
 */
GRCORE_API GRCORE_Result grcore_activation_set_compiled(GRCORE_Stack * stack,
    GRCORE_ActivationRef ref, uintptr_t frame_base, uintptr_t return_address);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_ACTIVATION_H */
