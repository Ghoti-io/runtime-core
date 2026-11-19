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
 * The group's seed sequence. See b/seeds.h for the contract.
 */

/* Before any include: -std=c17 hides getrandom and open unless a feature macro
 * is set, and a feature macro after the first include is ignored. */
#if defined(_WIN32)
#define _CRT_RAND_S
#elif !defined(__APPLE__)
#define _GNU_SOURCE
#endif

#include <ghoti.io/runtime-core/macros.h>

#include "context_internal.h"
#include "group_internal.h"

#include <ghoti.io/runtime-core/b/seeds.h>

#include <errno.h>
#include <stdlib.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

#define GOLDEN_GAMMA UINT64_C(0x9E3779B97F4A7C15)

uint64_t grcore_splitmix64(uint64_t state) {
  uint64_t z = state + GOLDEN_GAMMA;
  z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
  return z ^ (z >> 31);
}

bool grcore_entropy(void * bytes, size_t length) {
#if defined(_WIN32)
  unsigned char * out = bytes;
  for (size_t i = 0; i < length;) {
    unsigned int word;
    if (rand_s(&word) != 0) {
      return false;
    }
    for (size_t b = 0; b < sizeof(word) && i < length; ++b, ++i) {
      out[i] = (unsigned char)(word >> (8u * b));
    }
  }
  return true;
#else
  unsigned char * out = bytes;
  size_t done = 0;
#if defined(__linux__)
  while (done < length) {
    ssize_t n = getrandom(out + done, length - done, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    done += (size_t)n;
  }
  if (done == length) {
    return true;
  }
#endif
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    return false;
  }
  while (done < length) {
    ssize_t n = read(fd, out + done, length - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      close(fd);
      return false;
    }
    done += (size_t)n;
  }
  close(fd);
  return true;
#endif
}

uint64_t grcore_group_next_seed(GRCORE_Group * group) {
  uint64_t k = __atomic_fetch_add(&group->seed_counter, 1, __ATOMIC_RELAXED);
  /* Draw k is the (k+1)th output of the generator started at the master. */
  return grcore_splitmix64(group->master_seed + k * GOLDEN_GAMMA);
}

GRCORE_Result grcore_group_seed(GRCORE_Group * group, uint64_t master) {
  if (group == NULL || __atomic_load_n(&group->contexts, __ATOMIC_ACQUIRE) != 0 ||
      __atomic_load_n(&group->seed_counter, __ATOMIC_ACQUIRE) != 0) {
    return GRCORE_ERR_INVALID;
  }
  group->master_seed = master;
  return GRCORE_OK;
}

GRCORE_Result grcore_group_seed_random(GRCORE_Group * group) {
  if (group == NULL || __atomic_load_n(&group->contexts, __ATOMIC_ACQUIRE) != 0 ||
      __atomic_load_n(&group->seed_counter, __ATOMIC_ACQUIRE) != 0) {
    return GRCORE_ERR_INVALID;
  }
  uint64_t master;
  if (!grcore_entropy(&master, sizeof master)) {
    return GRCORE_ERR_IO;
  }
  group->master_seed = master;
  return GRCORE_OK;
}

uint64_t grcore_group_master_seed(const GRCORE_Group * group) {
  return group == NULL ? 0 : group->master_seed;
}

uint64_t grcore_context_seed(const GRCORE_Context * context) {
  return context == NULL ? 0 : context->seed;
}

uint64_t grcore_context_seed_draw(const GRCORE_Context * context, uint64_t index) {
  return context == NULL ? 0 : grcore_splitmix64(context->seed + index * GOLDEN_GAMMA);
}
