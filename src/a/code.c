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
 * Reference-counted compiled code.
 */

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/code.h>

struct GRCORE_Code {
  GRCORE_Allocator allocator; ///< A copy taken at creation.
  void * payload;
  void (*release)(void * payload);
  size_t refs; ///< `__atomic` builtins only.
};

GRCORE_Result grcore_code_create(const GRCORE_Allocator * allocator,
    void * payload, void (*release)(void * payload), GRCORE_Code ** out) {
  if (release == NULL || out == NULL) {
    return GRCORE_ERR_INVALID;
  }
  const GRCORE_Allocator * a =
      allocator != NULL ? allocator : grcore_allocator_default();
  GRCORE_Code * c = a->malloc_fn(a->ctx, sizeof *c);
  if (c == NULL) {
    return GRCORE_ERR_OOM;
  }
  c->allocator = *a;
  c->payload = payload;
  c->release = release;
  c->refs = 1;
  *out = c;
  return GRCORE_OK;
}

GRCORE_Code * grcore_code_retain(GRCORE_Code * code) {
  if (code != NULL) {
    __atomic_add_fetch(&code->refs, 1, __ATOMIC_RELAXED);
  }
  return code;
}

void grcore_code_release(GRCORE_Code * code) {
  if (code == NULL) {
    return;
  }
  if (__atomic_sub_fetch(&code->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    code->release(code->payload);
    GRCORE_Allocator a = code->allocator;
    a.free_fn(a.ctx, code);
  }
}

void * grcore_code_payload(const GRCORE_Code * code) {
  return code != NULL ? code->payload : NULL;
}

size_t grcore_code_refcount(const GRCORE_Code * code) {
  return code != NULL ? __atomic_load_n(&code->refs, __ATOMIC_ACQUIRE) : 0;
}
