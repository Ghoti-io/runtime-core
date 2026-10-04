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
 * @file layout.h
 * @stability free
 *
 * The JIT layout descriptor: what context-independent emitted code may know
 * about a context (AD-19, AD-22).
 *
 * Compiled code is shared between contexts and reaches state through a
 * context register, so the only offsets it may bake in are the ones this
 * descriptor states. There is exactly one: the request word that the poll's
 * fast path loads. There is deliberately no keyed-slot offset (AD-19): keyed
 * state is found by key, never by position.
 *
 * The descriptor is computed in this library from the real layout of the
 * context, so a code generator reads it at compile time and stores the offset
 * it used. Code compiled against one build is valid for exactly that build,
 * which is what `free` means here.
 */

#ifndef GHOTI_IO_GRCORE_A_LAYOUT_H
#define GHOTI_IO_GRCORE_A_LAYOUT_H

#include <ghoti.io/runtime-core/macros.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The offsets and rules emitted code may use.
 *
 * The poll's fast path is one relaxed load of `request_bytes` bytes at
 * `request_word_offset` from the context pointer, and a branch: a non-zero
 * word means "take the slow path".
 */
typedef struct GRCORE_JitLayout {
  uint32_t request_word_offset; ///< Byte offset of the request word in a context.
  uint32_t request_word_bytes;  ///< Its width, 8.
  bool request_nonzero_means_slow; ///< True: zero is "nothing pending".
} GRCORE_JitLayout;

/**
 * @brief The layout descriptor of this build of the library.
 *
 * @return A static struct, the same on every call. Never NULL, never freed.
 */
GRCORE_API const GRCORE_JitLayout * grcore_jit_layout(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_LAYOUT_H */
