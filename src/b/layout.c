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
 * The JIT layout descriptor. It lives in B's source because the offset is the
 * real `offsetof` of a field of B's private context struct.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "context_internal.h"

#include <ghoti.io/runtime-core/a/layout.h>

#include <stddef.h>

static const GRCORE_JitLayout layout = {
    (uint32_t)offsetof(GRCORE_Context, request_word),
    (uint32_t)sizeof(((GRCORE_Context *)0)->request_word),
    true,
    (uint32_t)offsetof(GRCORE_Context, walk_cell),
    (uint32_t)offsetof(GRCORE_Context, native_limit),
};

const GRCORE_JitLayout * grcore_jit_layout(void) {
  return &layout;
}
