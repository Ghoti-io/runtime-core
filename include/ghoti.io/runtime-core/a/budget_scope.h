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
 * @file budget_scope.h
 * @stability free
 *
 * Budget scopes: stopping a runaway call at its own boundary (AD-21).
 *
 * An engine call boundary may open a scope with its own exclusive fuel budget
 * (in lang-tang, a template call). The counting is B's (budget.h): the scope's
 * budget counts what is charged while it is the innermost, the parent's clock
 * stops while a child runs, and every charge counts against the ceiling too.
 * This header adds what only A can: the scope remembers where on the guest
 * stack, and among the activation records, it began, so the engine can unwind
 * to that boundary and close it.
 *
 * The cycle is: open at the boundary; poll as usual; if the poll unwinds and
 * ::grcore_budget_scope_exhausted names the scope, call
 * ::grcore_budget_scope_unwind and carry on with whatever the caller does
 * about a failed call. Closing the scope clears the scoped unwind, so `run` is
 * not left holding a terminal one. A scope that was *not* the cause (terminate,
 * or the ceiling) is not cleared, and the engine unwinds the whole run.
 *
 * Threads: the context's owner.
 */

#ifndef GHOTI_IO_GRCORE_A_BUDGET_SCOPE_H
#define GHOTI_IO_GRCORE_A_BUDGET_SCOPE_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/b/budget.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An open budget scope. A value; a stale one is refused. */
typedef struct GRCORE_BudgetScope {
  uint64_t id; ///< B's fuel scope id; zero for none.
} GRCORE_BudgetScope;

/**
 * @brief Opens a budget scope at the current top of the stack.
 *
 * @param stack The stack. The caller must own its context.
 * @param fuel The scope's exclusive fuel budget, or ::GRCORE_UNLIMITED.
 * @param policy What happens when it runs out.
 * @param out_scope Receives the scope. Written only on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_OOM as
 *   ::grcore_context_fuel_scope_open; ::GRCORE_ERR_INVALID for NULL, an
 *   unknown policy or a non-owner. A refusal opens nothing.
 */
GRCORE_API GRCORE_Result grcore_budget_scope_open(GRCORE_Stack * stack,
    uint64_t fuel, GRCORE_ScopePolicy policy, GRCORE_BudgetScope * out_scope);

/**
 * @brief Closes the innermost scope, which must be at its base.
 *
 * @param stack The stack. The caller must own its context.
 * @param scope The innermost scope.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID with nothing changed for NULL,
 *   a non-owner, a stale scope, a scope that is not the innermost, a stack
 *   whose frame count is not the one the scope began with, or an activation
 *   opened since that has not been left.
 */
GRCORE_API GRCORE_Result grcore_budget_scope_close(
    GRCORE_Stack * stack, GRCORE_BudgetScope scope);

/**
 * @brief Unwinds to a scope's boundary and closes it.
 *
 * Pops the frames above the scope's base (running the engines' hooks), leaves
 * the activations opened since, closes the scopes inside it, and then closes
 * the scope itself.
 *
 * @param stack The stack. The caller must own its context.
 * @param scope An open scope.
 * @param out_popped Receives the number of frames popped; may be NULL.
 *   Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID (nothing changes) for NULL, a
 *   non-owner or a stale scope.
 */
GRCORE_API GRCORE_Result grcore_budget_scope_unwind(
    GRCORE_Stack * stack, GRCORE_BudgetScope scope, size_t * out_popped);

/**
 * @brief Whether this scope is the cause of the last poll's unwind: the poll
 *   unwound because the scope ran out and nothing else voted to.
 *
 * @param stack The stack.
 * @param scope A scope.
 * @return True if the scope is open and is the cause.
 */
GRCORE_API bool grcore_budget_scope_exhausted(
    const GRCORE_Stack * stack, GRCORE_BudgetScope scope);

/**
 * @brief How many budget scopes this stack has open.
 *
 * @param stack The stack.
 * @return The count; zero for NULL.
 */
GRCORE_API size_t grcore_budget_scope_count(const GRCORE_Stack * stack);

/**
 * @brief The innermost open scope.
 *
 * @param stack The stack.
 * @param out_scope Receives it. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or no scope.
 */
GRCORE_API GRCORE_Result grcore_budget_scope_innermost(
    const GRCORE_Stack * stack, GRCORE_BudgetScope * out_scope);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_BUDGET_SCOPE_H */
