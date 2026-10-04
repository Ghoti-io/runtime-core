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
#ifndef GHOTI_IO_GRCORE_B_COND_CLOCK_INTERNAL_H
#define GHOTI_IO_GRCORE_B_COND_CLOCK_INTERNAL_H

/*
 * The clock a timed condition wait in this library is measured on, and the two
 * operations that have to agree about it: making the condition variable and
 * computing the absolute deadline handed to `pthread_cond_timedwait`.
 *
 * `pthread_cond_timedwait` takes an absolute time on the clock the condition
 * was created with, so the clock chosen at creation and the clock the deadline
 * is read from are one decision, and this is the one place it is made.
 *
 * POSIX: CLOCK_MONOTONIC, set with `pthread_condattr_setclock`. A timeout must
 * not move when the wall clock does.
 *
 * Windows (winpthreads): CLOCK_REALTIME. winpthreads accepts only
 * CLOCK_REALTIME in `pthread_condattr_setclock` (CLOCK_MONOTONIC answers
 * EINVAL) and always reads a timed wait's deadline against the system clock,
 * so a monotonic condition cannot be had. The deadline is therefore computed
 * on CLOCK_REALTIME, the one clock the wait will compare it with, and a timeout
 * on Windows is shortened or lengthened by a step of the system clock while it
 * is pending. Nothing else in the library is affected: the budget and the
 * fuel are not clocks.
 *
 * -std=c17 hides pthread_condattr_setclock and CLOCK_MONOTONIC; a file that
 * includes this defines _POSIX_C_SOURCE 200809L first.
 */

#include <ghoti.io/runtime-core/macros.h>

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#define GRCORE_COND_CLOCK CLOCK_REALTIME
#else
#define GRCORE_COND_CLOCK CLOCK_MONOTONIC
#endif

/** Initialise `cond` so that a timed wait on it reads GRCORE_COND_CLOCK.
 *  Returns 0, or the error number of the call that refused. */
static inline int grcore_cond_init(pthread_cond_t * cond) {
  pthread_condattr_t attr;
  int rc = pthread_condattr_init(&attr);
  if (rc != 0) {
    return rc;
  }
#ifndef _WIN32
  rc = pthread_condattr_setclock(&attr, GRCORE_COND_CLOCK);
#endif
  if (rc == 0) {
    rc = pthread_cond_init(cond, &attr);
  }
  pthread_condattr_destroy(&attr);
  return rc;
}

/** Set `*deadline` to `ns` nanoseconds from now on GRCORE_COND_CLOCK, clamped
 *  far enough ahead to be forever and short of overflowing `time_t`. */
static inline void grcore_cond_deadline(struct timespec * deadline, uint64_t ns) {
  clock_gettime(GRCORE_COND_CLOCK, deadline);
  uint64_t secs = ns / UINT64_C(1000000000);
  uint64_t nanos = ns % UINT64_C(1000000000) + (uint64_t)deadline->tv_nsec;
  if (nanos >= UINT64_C(1000000000)) {
    secs++;
    nanos -= UINT64_C(1000000000);
  }
  if (secs > (uint64_t)INT32_MAX * 8u) {
    secs = (uint64_t)INT32_MAX * 8u;
  }
  deadline->tv_sec += (time_t)secs;
  deadline->tv_nsec = (long)nanos;
}

#endif
