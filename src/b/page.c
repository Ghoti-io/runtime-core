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
 * The default page provider.
 */

/* -std=c17 hides MAP_ANONYMOUS; ask for it by name, before any include. */
#ifndef _WIN32
#define _DEFAULT_SOURCE
#endif

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/page.h>

#include <stdint.h>

#ifdef _WIN32
/* TODO(windows): VirtualAlloc/VirtualFree branch, unverified; see
 * notes/suite/WINDOWS-TODO.md. */
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static void * default_map(void * ctx, size_t size);
static void default_unmap(void * ctx, void * ptr, size_t size);
static bool default_protect(
    void * ctx, void * ptr, size_t size, GRCORE_PageAccess access);

/* Everything but the page size is a constant, so the provider is usable from
 * the first instruction. The page size is looked up on first use. */
static GRCORE_PageProvider default_provider = {
    NULL, 0, default_map, default_unmap, default_protect};

/* Idempotent: every thread stores the same value, relaxed. The constructor
 * below makes it a constant before main in the ordinary case; this covers a
 * caller that runs earlier (another constructor, a static initialiser). */
static size_t page_size_now(void) {
  size_t size = __atomic_load_n(&default_provider.page_size, __ATOMIC_RELAXED);
  if (size == 0) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    size = (size_t)info.dwPageSize;
#else
    long n = sysconf(_SC_PAGESIZE);
    size = n > 0 ? (size_t)n : 4096u;
#endif
    __atomic_store_n(&default_provider.page_size, size, __ATOMIC_RELAXED);
  }
  return size;
}

static void * default_map(void * ctx, size_t size) {
  (void)ctx;
  if (size == 0 || size % page_size_now() != 0) {
    return NULL;
  }
#ifdef _WIN32
  return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  void * p = mmap(
      NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
#endif
}

static void default_unmap(void * ctx, void * ptr, size_t size) {
  (void)ctx;
  if (ptr == NULL) {
    return;
  }
#ifdef _WIN32
  (void)size;
  VirtualFree(ptr, 0, MEM_RELEASE);
#else
  munmap(ptr, size);
#endif
}

static bool default_protect(
    void * ctx, void * ptr, size_t size, GRCORE_PageAccess access) {
  (void)ctx;
  if (ptr == NULL || size == 0) {
    return false;
  }
#ifdef _WIN32
  /* TODO(windows): VirtualProtect branch, unverified; see
   * notes/suite/WINDOWS-TODO.md. */
  DWORD want = access == GRCORE_PAGE_READ_EXECUTE ? PAGE_EXECUTE_READ
                                                  : PAGE_READWRITE;
  DWORD old;
  return VirtualProtect(ptr, size, want, &old) != 0;
#else
  int prot = access == GRCORE_PAGE_READ_EXECUTE ? (PROT_READ | PROT_EXEC)
                                                : (PROT_READ | PROT_WRITE);
  return mprotect(ptr, size, prot) == 0;
#endif
}

GRCORE_Result grcore_page_protect(const GRCORE_PageProvider * provider,
    void * ptr, size_t size, GRCORE_PageAccess access) {
  if (provider == NULL || provider->protect == NULL || ptr == NULL ||
      provider->page_size == 0 || size == 0 ||
      (access != GRCORE_PAGE_READ_WRITE &&
          access != GRCORE_PAGE_READ_EXECUTE) ||
      (uintptr_t)ptr % provider->page_size != 0 ||
      size % provider->page_size != 0) {
    return GRCORE_ERR_INVALID;
  }
  return provider->protect(provider->ctx, ptr, size, access) ? GRCORE_OK
                                                             : GRCORE_ERR_IO;
}

GRCORE_INIT_FUNCTION(page_provider_init) {
  (void)page_size_now();
}

const GRCORE_PageProvider * grcore_page_provider_default(void) {
  (void)page_size_now();
  return &default_provider;
}
