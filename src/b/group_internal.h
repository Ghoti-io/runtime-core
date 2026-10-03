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
 * Group, private part: the layout contexts read.
 */

#ifndef GHOTI_IO_GRCORE_B_GROUP_INTERNAL_H
#define GHOTI_IO_GRCORE_B_GROUP_INTERNAL_H

#include <ghoti.io/runtime-core/macros.h>

#include "account_internal.h"

#include <ghoti.io/runtime-core/b/group.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GRCORE_Group {
  const GRCORE_Allocator * allocator; ///< Base, never NULL.
  const GRCORE_PageProvider * pages;  ///< Base, never NULL.
  GRCORE_Meter meter;
  GRCORE_Counting counting;
  size_t contexts; ///< Touched only through `__atomic` builtins.
};

/** @brief Counts a context in. */
void grcore_group_enter(GRCORE_Group * group);
/** @brief Counts a context out. */
void grcore_group_leave(GRCORE_Group * group);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_GROUP_INTERNAL_H */
