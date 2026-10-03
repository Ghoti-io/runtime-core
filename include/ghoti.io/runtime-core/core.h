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
 * @file core.h
 * @stability stable
 *
 * Result codes and the version this build reports.
 */

#ifndef GHOTI_IO_GRCORE_CORE_H
#define GHOTI_IO_GRCORE_CORE_H

#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of an operation.
 *
 * Zero is success.  ::GRCORE_RESULT_COUNT closes the enum so a test can check
 * the string table is complete.
 *
 * This is the suite's fixed vocabulary plus one constant, ::GRCORE_ERR_GUEST,
 * which is a recorded departure (CONVENTIONS.md section 13). It is appended
 * last, before the count, so that every other value keeps the number it has in
 * the other libraries.
 */
typedef enum {
  GRCORE_OK = 0,          ///< The operation succeeded.
  GRCORE_ERR_IO,          ///< A read, write, or seek failed.
  GRCORE_ERR_FORMAT,      ///< Well-formed bytes, but not a format handled here.
  GRCORE_ERR_UNSUPPORTED, ///< The format is known and the feature is not
                          ///< implemented.
  GRCORE_ERR_LIMIT,       ///< A stated cap was exceeded.
  GRCORE_ERR_CORRUPT,     ///< The bytes are not a valid encoding of this
                          ///< format.
  GRCORE_ERR_OOM,         ///< The allocator returned NULL.
  GRCORE_ERR_INVALID,     ///< A caller-supplied argument is wrong.
  GRCORE_ERR_INTERNAL,    ///< The library's own invariant failed.
  GRCORE_ERR_GUEST,       ///< Guest code ended in an uncaught exception or a
                          ///< trap. This is the guest's failure, not the
                          ///< runtime's; the reason is read from the context.
  GRCORE_RESULT_COUNT
} GRCORE_Result;

/**
 * @brief Static description of a result code.
 *
 * The string is never NULL, never allocated, and never contains caller data.
 *
 * @param result The result code, including values outside the enum.
 * @return A static string.
 */
GRCORE_API const char * grcore_result_string(GRCORE_Result result);

/**
 * @brief This build's version, as the string the Makefile generated.
 *
 * @return A static string, never NULL.  "0.0.0", or "0.0.0-dev" when BRANCH
 *   was overridden.
 */
GRCORE_API const char * grcore_version_string(void);

/**
 * @brief This build's version, packed as ::GRCORE_MAKE_VERSION packs it.
 *
 * @return `(major << 16) | (minor << 8) | patch`.
 */
GRCORE_API unsigned grcore_version_number(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_CORE_H */
