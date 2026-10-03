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
 * @file unwind.h
 * @stability free
 *
 * The unwinder: taking the stack apart when a run ends early (AD-5, AD-18).
 *
 * It pops guest frames innermost first, calling each frame's engine `unwind`
 * hook while the frame is still on the stack, and it never runs guest code (a
 * limit unwind cannot be caught and runs no guest `finally`). Deeper
 * activation records are left, which gives back their native depth and
 * nesting, and deeper budget scopes are closed. The guest depth comes back
 * with each pop. It never crosses a host frame: it only edits data, and the
 * C frames above `run` unwind by returning.
 *
 * Unwinding closes fuel scopes, and closing the scope that caused a scoped
 * unwind ends that unwind: `run` then holds no unwind verdict, and a later
 * ::GRCORE_STEP_UNWOUND from the entry reports ::GRCORE_ERR_INTERNAL. An engine
 * that wants the run to end with the limit error must not close that scope
 * (nor call ::grcore_unwind_all, which closes every scope). Engine hooks must
 * not call anything in this header.
 *
 * Threads: the context's owner, in any state.
 */

#ifndef GHOTI_IO_GRCORE_A_UNWIND_H
#define GHOTI_IO_GRCORE_A_UNWIND_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/activation.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/core.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Unwinds to an activation, which stays.
 *
 * Pops every frame above the activation's base, running each hook; leaves
 * every deeper activation; closes every budget scope opened inside it (the
 * fuel scopes in B with them). The target and everything outside it, scopes
 * included, are as they were.
 *
 * @param stack The stack. The caller must own its context.
 * @param ref An open activation.
 * @param out_popped Receives the number of frames popped; may be NULL.
 *   Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID (nothing changes) for NULL, a
 *   non-owner, or a stale or forged reference.
 */
GRCORE_API GRCORE_Result grcore_unwind_to_activation(
    GRCORE_Stack * stack, GRCORE_ActivationRef ref, size_t * out_popped);

/**
 * @brief Unwinds everything: after this the stack is empty, no activation or
 *   scope is open, and the guest and native depths are zero.
 *
 * It is the end of a run, whether it ended in an unwind or finished with
 * frames left on the stack.
 *
 * @param stack The stack. The caller must own its context.
 * @param out_popped Receives the number of frames popped; may be NULL.
 *   Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or a non-owner.
 */
GRCORE_API GRCORE_Result grcore_unwind_all(
    GRCORE_Stack * stack, size_t * out_popped);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_UNWIND_H */
