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
 * Memory accounting: an atomic meter, and the counting allocator and page
 * provider that charge one (AD-13, AD-20). Private to the library.
 *
 * The structs hold plain integers, touched only through the `__atomic`
 * builtins, so that a C++ test can include this header.
 */

#ifndef GHOTI_IO_GRCORE_B_ACCOUNT_INTERNAL_H
#define GHOTI_IO_GRCORE_B_ACCOUNT_INTERNAL_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/b/page.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What is charged: bytes in use, the high-water mark, live blocks. */
typedef struct GRCORE_Meter {
  uint64_t in_use; ///< Bytes of payload currently held.
  uint64_t peak;   ///< Greatest value `in_use` has had.
  uint64_t blocks; ///< Live allocations and page mappings.
} GRCORE_Meter;

/**
 * @brief A counting allocator and page provider over a base pair, charging
 *   one meter. Must not move once initialised: its interfaces point at it.
 */
typedef struct GRCORE_Counting {
  GRCORE_Allocator allocator; ///< Counting; its ctx is this struct.
  GRCORE_PageProvider pages;  ///< Counting; same.
  const GRCORE_Allocator * base_allocator;
  const GRCORE_PageProvider * base_pages;
  GRCORE_Meter * meter;
} GRCORE_Counting;

/** @brief Zeroes a meter. */
void grcore_meter_init(GRCORE_Meter * meter);
/** @brief Bytes in use (relaxed load). */
uint64_t grcore_meter_in_use(const GRCORE_Meter * meter);
/** @brief High-water mark (relaxed load). */
uint64_t grcore_meter_peak(const GRCORE_Meter * meter);
/** @brief Live blocks (relaxed load). */
uint64_t grcore_meter_blocks(const GRCORE_Meter * meter);

/**
 * @brief Wires a counting pair over `base_allocator` and `base_pages`.
 *
 * @param counting The struct to fill, at its final address.
 * @param meter The meter to charge.
 * @param base_allocator The allocator to wrap; NULL for the default.
 * @param base_pages The provider to wrap; NULL for the default.
 */
void grcore_counting_init(GRCORE_Counting * counting, GRCORE_Meter * meter,
    const GRCORE_Allocator * base_allocator,
    const GRCORE_PageProvider * base_pages);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_ACCOUNT_INTERNAL_H */
