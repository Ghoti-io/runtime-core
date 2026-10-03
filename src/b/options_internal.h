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
 * Options, private part: the deep copy a context keeps.
 */

#ifndef GHOTI_IO_GRCORE_B_OPTIONS_INTERNAL_H
#define GHOTI_IO_GRCORE_B_OPTIONS_INTERNAL_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/options.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Deep-copies options, keyed bytes included.
 *
 * @param source The options, or NULL for a default object.
 * @param allocator The new object's allocator; NULL for the default.
 * @param out_options Receives the copy. Written only on success.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID or ::GRCORE_ERR_OOM.
 */
GRCORE_Result grcore_options_clone(const GRCORE_Options * source,
    const GRCORE_Allocator * allocator, GRCORE_Options ** out_options);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_OPTIONS_INTERNAL_H */
