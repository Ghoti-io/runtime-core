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
 * @file namespace.h
 * @stability stable
 *
 * Maps every public name of this library into its version namespace.
 *
 * Kept in one file rather than beside each declaration: a type rename has to
 * be in effect before any struct tag that uses the name, and an internal
 * header may define such a tag without including the public header that
 * declares the typedef.
 *
 * `make check-symbols` fails if an exported symbol is missing from this list.
 *
 * See CONVENTIONS.md section 4.
 */

#ifndef GHOTI_IO_GRCORE_NAMESPACE_H
#define GHOTI_IO_GRCORE_NAMESPACE_H

#include <ghoti.io/runtime-core/libver.h>

/// @cond HIDDEN_SYMBOLS

/* Public types. GCU_* names are cutil's; cutil has already renamed them. */
#define GRCORE_Allocator GHOTIIO_RUNTIME_CORE(GRCORE_Allocator)
#define GRCORE_Result GHOTIIO_RUNTIME_CORE(GRCORE_Result)

#define grcore_allocator_default GHOTIIO_RUNTIME_CORE(grcore_allocator_default)
#define grcore_result_string GHOTIIO_RUNTIME_CORE(grcore_result_string)
#define grcore_version_number GHOTIIO_RUNTIME_CORE(grcore_version_number)
#define grcore_version_string GHOTIIO_RUNTIME_CORE(grcore_version_string)

/// @endcond

#endif /* GHOTI_IO_GRCORE_NAMESPACE_H */
