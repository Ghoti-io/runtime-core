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
 */

#ifndef GHOTI_IO_GRCORE_B_BUDGET_H
#define GHOTI_IO_GRCORE_B_BUDGET_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The two depths that are budgeted. */
typedef enum {
  GRCORE_DEPTH_GUEST = 0, ///< Guest call depth.
  GRCORE_DEPTH_NATIVE     ///< Native stack depth.
} GRCORE_DepthKind;

/**
 * @brief Charges fuel. Owner.
 *
 * The count saturates at ::GRCORE_UNLIMITED. Fuel is exhausted when more has
 * been used than the budget; the context then raises the fuel request and the
 * next poll pauses it.
 *
 * @param context The context.
 * @param amount The instruction cost to charge.
 * @return True if fuel is now exhausted; false for NULL.
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
