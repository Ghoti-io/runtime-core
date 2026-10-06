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
 * @file budget.h
 * @stability stable
 *
 * Enforcing the budgets: fuel, memory and depth (AD-21).
 *
 * The budgets are set as options when a context is created. The context keeps
 * the current ones here, where a host can change them while the context is
 * paused. Fuel and memory are enforced through the poll: running out of
 * either raises a derived request that the next poll turns into a pause, so
 * the host can raise the budget and resume. Depth is enforced where it is
 * entered.
 *
 * Fuel has two levels. The *ceiling* (the fuel budget above) is inclusive:
 * every charge counts against it, whatever scope is open, and it is the
 * absolute limit of the request. A *fuel scope* is an exclusive budget under
 * it: a charge goes to the innermost open scope only, so a parent's clock
 * stops while a child runs, and a runaway child can be stopped at its own
 * boundary without spending its parent's allowance. When the ceiling and a
 * scope are both exhausted the verdict is a pause, so the host can raise the
 * ceiling. When only a scope is, the verdict is an unwind, or a pause if the
 * scope's policy says so. An unwind for a scope is *scoped*: its only voter is
 * the `fuel` key, and the engine carries it out by unwinding to the scope's
 * boundary and closing the scope, which clears it so that `run` carries on.
 */

#ifndef GHOTI_IO_GRCORE_B_BUDGET_H
#define GHOTI_IO_GRCORE_B_BUDGET_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What a fuel scope does when its own budget runs out (AD-21). */
typedef enum {
  GRCORE_SCOPE_POLICY_UNWIND = 0, ///< Unwind to the scope's boundary.
  GRCORE_SCOPE_POLICY_PAUSE       ///< Pause, so the host can raise the budget.
} GRCORE_ScopePolicy;

/** @brief The two depths that are budgeted. */
typedef enum {
  GRCORE_DEPTH_GUEST = 0, ///< Guest call depth.
  GRCORE_DEPTH_NATIVE     ///< Native stack depth.
} GRCORE_DepthKind;

/**
 * @brief Sets the native-stack limit word from the stack pointer `sp` and the
 *   native-stack byte budget (AD-28).
 *
 * The limit is `sp` less the budget in bytes: the lowest address compiled code
 * may let the native stack reach. Each callable compiled function compares its
 * `rsp`, less its own frame, with this word in its prologue (`a/layout.h`
 * states where it is), and if it would pass it deoptimizes the compiled chain
 * at the call site and the interpreter continues. With no budget, or a budget
 * larger than `sp`, the word is zero and nothing is ever refused.
 *
 * ::grcore_run and ::grcore_resume call this with their own stack pointer (a
 * resume may be on another thread, with another stack), and so does the entry
 * of a nested re-entry that finds the word unset. An engine that enters
 * compiled code on a path the core does not see calls it too.
 *
 * @param context The context. The caller must own it.
 * @param sp An address on the calling thread's native stack, at or just below
 *   the base the budget is measured from.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID for NULL or a non-owner.
 */
GRCORE_API GRCORE_Result grcore_context_native_limit_set(
    GRCORE_Context * context, uintptr_t sp);

/**
 * @brief ::grcore_context_native_limit_set with the calling function's own
 *   stack pointer.
 */
GRCORE_API GRCORE_Result grcore_context_native_limit_here(GRCORE_Context * context);

/**
 * @brief The native-stack limit word: the lowest address compiled code may let
 *   the native stack reach; zero for none. Zero for NULL.
 */
GRCORE_API uintptr_t grcore_context_native_limit(const GRCORE_Context * context);

/**
 * @brief The native stack budget in bytes, as the context was made with.
 *
 * @return The budget; ::GRCORE_UNLIMITED if unset or `context` is NULL.
 */
GRCORE_API uint64_t grcore_context_native_stack_bytes(const GRCORE_Context * context);

/**
 * @brief Charges fuel. Owner.
 *
 * The count saturates at ::GRCORE_UNLIMITED. The charge counts against the
 * ceiling and against the innermost open fuel scope, if any, and no other.
 * Fuel is exhausted when more has been used than the budget, of the ceiling or
 * of that scope; the context then raises the fuel request and the next poll
 * decides.
 *
 * @param context The context.
 * @param amount The instruction cost to charge.
 * @return True if fuel is now exhausted, at either level; false for NULL.
 */
GRCORE_API bool grcore_context_charge_fuel(
    GRCORE_Context * context, uint64_t amount);

/**
 * @brief Fuel charged so far.
 *
 * @param context The context.
 * @return The total; zero for NULL.
 */
GRCORE_API uint64_t grcore_context_fuel_used(const GRCORE_Context * context);

/**
 * @brief Fuel left before exhaustion.
 *
 * @param context The context.
 * @return The budget minus the use, zero once exhausted;
 *   ::GRCORE_UNLIMITED when the budget is unlimited or `context` is NULL.
 */
GRCORE_API uint64_t grcore_context_fuel_remaining(
    const GRCORE_Context * context);

/**
 * @brief The current fuel budget: the total the context may use.
 *
 * @param context The context.
 * @return The budget; ::GRCORE_UNLIMITED if unset or `context` is NULL.
 */
GRCORE_API uint64_t grcore_context_fuel_limit(const GRCORE_Context * context);

/**
 * @brief Sets the fuel budget. The use so far stays, so raising the budget by
 *   some amount buys exactly that much more.
 *
 * @param context The context. The caller must own it, and it must be parked
 *   outside `run` (not in a host call), at-poll, or paused.
 * @param limit The new total, or ::GRCORE_UNLIMITED.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID (nothing changes).
 */
GRCORE_API GRCORE_Result grcore_context_set_fuel(
    GRCORE_Context * context, uint64_t limit);

/**
 * @brief Opens a fuel scope, innermost until it is closed.
 *
 * The scope's budget is exclusive: it counts only what is charged while the
 * scope is the innermost, and not what a scope opened inside it is charged.
 * Every charge still counts against the ceiling.
 *
 * @param context The context. The caller must own it, and it must not be
 *   tearing down. It may be running.
 * @param budget What the scope may use itself, or ::GRCORE_UNLIMITED.
 * @param policy What happens when the budget runs out.
 * @param out_id Receives the scope's id, never zero and never reused. Written
 *   only on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for NULL, a non-owner, or an
 *   unknown policy; ::GRCORE_ERR_LIMIT if the context's memory budget refuses
 *   the table's growth; ::GRCORE_ERR_OOM for any other allocation failure.
 *   A refusal leaves the scopes unchanged.
 */
GRCORE_API GRCORE_Result grcore_context_fuel_scope_open(GRCORE_Context * context,
    uint64_t budget, GRCORE_ScopePolicy policy, uint64_t * out_id);

/**
 * @brief Closes the innermost fuel scope.
 *
 * If the last poll's unwind was for this scope alone, closing it clears that
 * unwind, so that `run` does not treat it as a terminal one: the engine has
 * unwound to the scope's boundary and carries on.
 *
 * @param context The context. The caller must own it.
 * @param id The innermost scope's id.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID (nothing changes) for NULL, a
 *   non-owner, an unknown or stale id, or a scope that is not the innermost.
 */
GRCORE_API GRCORE_Result grcore_context_fuel_scope_close(
    GRCORE_Context * context, uint64_t id);

/**
 * @brief Sets a scope's budget. The use so far stays, so raising it by some
 *   amount buys exactly that much more.
 *
 * @param context The context. The caller must own it, and it must be parked
 *   outside `run`, at-poll, or paused, as for ::grcore_context_set_fuel.
 * @param id Any open scope.
 * @param budget The new budget, or ::GRCORE_UNLIMITED.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID (nothing changes).
 */
GRCORE_API GRCORE_Result grcore_context_fuel_scope_set_budget(
    GRCORE_Context * context, uint64_t id, uint64_t budget);

/**
 * @brief What a scope has used itself.
 *
 * @param context The context.
 * @param id Any open scope.
 * @param out_used Receives the use. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or an unknown id.
 */
GRCORE_API GRCORE_Result grcore_context_fuel_scope_used(
    const GRCORE_Context * context, uint64_t id, uint64_t * out_used);

/**
 * @brief What a scope has left before it is exhausted.
 *
 * @param context The context.
 * @param id Any open scope.
 * @param out_remaining Receives the budget minus the use, zero once
 *   exhausted, or ::GRCORE_UNLIMITED for an unlimited scope. Written only on
 *   success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or an unknown id.
 */
GRCORE_API GRCORE_Result grcore_context_fuel_scope_remaining(
    const GRCORE_Context * context, uint64_t id, uint64_t * out_remaining);

/**
 * @brief How many fuel scopes are open.
 *
 * @param context The context.
 * @return The count; zero for NULL.
 */
GRCORE_API uint64_t grcore_context_fuel_scope_depth(const GRCORE_Context * context);

/**
 * @brief The id of the innermost open fuel scope.
 *
 * @param context The context.
 * @return The id; zero if none is open or `context` is NULL.
 */
GRCORE_API uint64_t grcore_context_fuel_scope_top(const GRCORE_Context * context);

/**
 * @brief The scope the last poll's unwind was for.
 *
 * Nonzero only after a poll whose verdict was an unwind that no one but the
 * `fuel` key voted for, because the innermost scope was exhausted and the
 * ceiling was not. Closing that scope clears it.
 *
 * @param context The context.
 * @return The scope's id; zero if there is no such unwind or `context` is
 *   NULL.
 */
GRCORE_API uint64_t grcore_context_fuel_scope_unwinding(
    const GRCORE_Context * context);

/**
 * @brief The current memory budget in bytes.
 *
 * @param context The context.
 * @return The budget; ::GRCORE_UNLIMITED if unset or `context` is NULL.
 */
GRCORE_API uint64_t grcore_context_memory_limit(const GRCORE_Context * context);

/**
 * @brief Sets the memory budget. The reserve stays what the options said.
 *
 * @param context The context. The caller must own it, and it must be parked
 *   outside `run` (not in a host call), at-poll, or paused.
 * @param bytes The new budget, or ::GRCORE_UNLIMITED.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID (nothing changes).
 */
GRCORE_API GRCORE_Result grcore_context_set_memory_bytes(
    GRCORE_Context * context, uint64_t bytes);

/**
 * @brief The reserve: how far past the budget an allocation is still served.
 *
 * @param context The context.
 * @return The reserve in bytes; ::GRCORE_DEFAULT_MEMORY_RESERVE for NULL.
 */
GRCORE_API uint64_t grcore_context_memory_reserve(
    const GRCORE_Context * context);

/**
 * @brief How many allocations the context's allocator and page provider have
 *   refused for being past the budget and the reserve.
 *
 * @param context The context.
 * @return The count; zero for NULL.
 */
GRCORE_API uint64_t grcore_context_memory_refusals(
    const GRCORE_Context * context);

/**
 * @brief Enters one level of depth. The depth budgets come from the options
 *   only; there is no setter yet.
 *
 * @param context The context.
 * @param kind Which depth.
 * @return ::GRCORE_OK; ::GRCORE_ERR_LIMIT when the depth is already at its
 *   budget (the depth is unchanged); ::GRCORE_ERR_INVALID for NULL or an
 *   unknown kind.
 */
GRCORE_API GRCORE_Result grcore_context_enter_depth(
    GRCORE_Context * context, GRCORE_DepthKind kind);

/**
 * @brief Leaves one level of depth.
 *
 * @param context The context.
 * @param kind Which depth.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID when the depth is zero, for
 *   NULL, or an unknown kind.
 */
GRCORE_API GRCORE_Result grcore_context_leave_depth(
    GRCORE_Context * context, GRCORE_DepthKind kind);

/**
 * @brief The current depth.
 *
 * @param context The context.
 * @param kind Which depth.
 * @return The depth; zero for NULL or an unknown kind.
 */
GRCORE_API uint64_t grcore_context_depth(
    const GRCORE_Context * context, GRCORE_DepthKind kind);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_BUDGET_H */
