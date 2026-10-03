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
 * @file group.h
 * @stability stable
 *
 * Context groups (AD-7).
 *
 * Only the host creates groups, and every context belongs to exactly one from
 * the moment it exists. A group holds the allocator and page provider its
 * contexts draw on, and a meter for what is charged to the group rather than
 * to any one context. Ports, shared regions and group policy attach here in
 * later stories.
 *
 * Contexts of one group may be created and destroyed on different threads at
 * once. Destroying the group is the host's job, after every context is gone,
 * and needs quiescence: no create or destroy of a context of that group may be
 * running on any thread, or the count it checks can change under it.
 */

#ifndef GHOTI_IO_GRCORE_B_GROUP_H
#define GHOTI_IO_GRCORE_B_GROUP_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/b/page.h>
#include <ghoti.io/runtime-core/core.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A context group. Opaque. */
typedef struct GRCORE_Group GRCORE_Group;

/**
 * @brief Creates a group.
 *
 * @param allocator The base allocator for the group and its contexts, or NULL
 *   for the default. It must outlive the group and every context.
 * @param pages The base page provider, or NULL for the default. Same
 *   lifetime. A custom one needs a non-zero `page_size` and non-NULL `map` and
 *   `unmap`.
 * @param out_group Receives the group. Written only on success.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID for a NULL output or an unusable
 *   provider, or ::GRCORE_ERR_OOM.
 */
GRCORE_API GRCORE_Result grcore_group_create(const GRCORE_Allocator * allocator,
    const GRCORE_PageProvider * pages, GRCORE_Group ** out_group);

/**
 * @brief Destroys a group.
 *
 * @param group The group.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or a group that still
 *   has contexts; the group is then unchanged.
 */
GRCORE_API GRCORE_Result grcore_group_destroy(GRCORE_Group * group);

/**
 * @brief How many contexts the group has.
 *
 * @param group The group.
 * @return The count; zero for NULL.
 */
GRCORE_API size_t grcore_group_context_count(const GRCORE_Group * group);

/**
 * @brief The group's counting allocator: what group-level code allocates
 *   through, charged to the group.
 *
 * @param group The group.
 * @return An allocator owned by the group; NULL for NULL.
 */
GRCORE_API const GRCORE_Allocator * grcore_group_allocator(
    const GRCORE_Group * group);

/**
 * @brief The group's counting page provider, charged to the group.
 *
 * @param group The group.
 * @return A provider owned by the group; NULL for NULL.
 */
GRCORE_API const GRCORE_PageProvider * grcore_group_page_provider(
    const GRCORE_Group * group);

/**
 * @brief Bytes currently charged to the group.
 *
 * @param group The group.
 * @return The byte count; zero for NULL.
 */
GRCORE_API uint64_t grcore_group_memory_in_use(const GRCORE_Group * group);

/**
 * @brief The most bytes ever charged to the group at once.
 *
 * @param group The group.
 * @return The high-water mark; zero for NULL.
 */
GRCORE_API uint64_t grcore_group_memory_peak(const GRCORE_Group * group);

/**
 * @brief Live blocks charged to the group (allocations and page mappings).
 *
 * @param group The group.
 * @return The count; zero for NULL.
 */
GRCORE_API uint64_t grcore_group_memory_blocks(const GRCORE_Group * group);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_GROUP_H */
