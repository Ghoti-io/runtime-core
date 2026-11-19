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
 * @file seeds.h
 * @stability stable
 *
 * The group's seed sequence (AD-25): one master seed per group, from which
 * every context gets a seed of its own and every engine derives its generators.
 * Nothing in the stack is seeded from a clock, and there is no entropy callback.
 *
 * The master seed is operating-system entropy unless the host supplies one
 * (before the first context is made), so a failing run can be reproduced. The
 * contexts of a group take successive draws of the sequence as they are
 * created, by splitmix64, so the same master seed and the same creation order
 * give the same context seeds on every target. A context's own draws
 * (::grcore_context_seed_draw) are a pure function of its seed and an index:
 * draw 0 is the one an engine uses for its hash seed, and an engine that wants
 * more takes the next indices.
 */

#ifndef GHOTI_IO_GRCORE_B_SEEDS_H
#define GHOTI_IO_GRCORE_B_SEEDS_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Supplies the group's master seed.
 *
 * @param group The group.
 * @param master The seed.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for NULL or a group that already
 *   has a context (the seed of a context is fixed when it is made). Nothing
 *   changes on a refusal.
 */
GRCORE_API GRCORE_Result grcore_group_seed(GRCORE_Group * group, uint64_t master);

/**
 * @brief Draws the group's master seed from operating-system entropy
 *   (`getrandom`, then the urandom device; `rand_s` on Windows).
 *
 * A group does this itself when it is created, so this is for a host that
 * wants a fresh one after supplying another.
 *
 * @param group The group.
 * @return ::GRCORE_OK; ::GRCORE_ERR_IO when the system has no entropy to give;
 *   ::GRCORE_ERR_INVALID for NULL or a group that already has a context.
 *   Nothing changes on a refusal.
 */
GRCORE_API GRCORE_Result grcore_group_seed_random(GRCORE_Group * group);

/**
 * @brief The group's master seed.
 *
 * @param group The group.
 * @return The seed; zero for NULL.
 */
GRCORE_API uint64_t grcore_group_master_seed(const GRCORE_Group * group);

/**
 * @brief The seed of a context: the next draw of its group's sequence at the
 *   moment it was made.
 *
 * @param context The context.
 * @return The seed; zero for NULL.
 */
GRCORE_API uint64_t grcore_context_seed(const GRCORE_Context * context);

/**
 * @brief Draw `index` of a context's seed: `splitmix64` of the seed advanced
 *   `index` times. A pure function of the seed and the index, so a snapshot
 *   that keeps the seed keeps every draw.
 *
 * @param context The context.
 * @param index Which draw; 0 is the engine's hash seed.
 * @return The draw; zero for NULL.
 */
GRCORE_API uint64_t grcore_context_seed_draw(
    const GRCORE_Context * context, uint64_t index);

/**
 * @brief The splitmix64 output for a state: the standard construction
 *   (advance by the golden gamma, then mix), exposed so an engine can derive
 *   its own generators from a draw the same way on every target.
 *
 * @param state The state.
 * @return The mixed value.
 */
GRCORE_API uint64_t grcore_splitmix64(uint64_t state);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_SEEDS_H */
