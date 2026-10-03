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
 * The benchmark harness (AD-26).
 *
 * Every library ships one from its first commit, so that "performant" is a
 * claim with a way to check it. Besides the calibration case it holds the
 * first cases of the runtime: creating and destroying a context, a counting
 * malloc/free pair, and a keyed slot lookup. Polls and frame walks arrive in
 * later stories, and each brings its case here.
 *
 * The calibration case is a fixed amount of integer work that touches no
 * library code, run the same way every real case will be, so a figure from a
 * real case can be read against the machine it was taken on: a poll that costs 3 ns means one thing next to a calibration
 * of 1 ns per step and another next to 6. A budget recorded without the
 * calibration beside it cannot be compared across hosts or compilers.
 *
 * Usage:
 *   bench           run every case: several repeats, report min and median
 *   bench --smoke   run every case once with a tiny workload (what `make
 *                   test` does); proves the harness builds, links and runs
 *
 * Numeric budgets are not recorded here. AD-26 records them once a first
 * measurement of a real case exists.
 */

/* clock_gettime(CLOCK_MONOTONIC) is POSIX, and -std=c17 hides it. A benchmark
 * wants a clock that cannot step backwards, so it asks for it by name. */
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/runtime-core/runtime-core.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  const char * name;
  /** Performs @p iterations units of work; returns a value derived from all
   *  of it so the compiler cannot discard the loop. */
  uint64_t (*run)(uint64_t iterations);
  uint64_t iterations;       /* per repeat, full run */
  uint64_t smoke_iterations; /* per repeat, --smoke */
} Case;

/* A case that cannot set itself up must not report a short, fast run as a
 * measurement. */
static _Noreturn void setup_failed(const char * what) {
  fprintf(stderr, "bench: setup failed: %s\n", what);
  abort();
}

static double now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* xorshift64: one dependent chain of shifts and xors per step. The chain is
 * serial on purpose, so the figure is latency-bound and does not move with
 * how wide the host's execution units are. */
static uint64_t calibration_run(uint64_t iterations) {
  uint64_t x = 0x9E3779B97F4A7C15ull;
  for (uint64_t i = 0; i < iterations; i++) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
  }
  return x;
}

/* Create and destroy a context in a group: the cost of a context that does
 * nothing, which is the floor under every request a host serves. */
static uint64_t context_create_destroy_run(uint64_t iterations) {
  GRCORE_Group * group;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK) {
    setup_failed("group create");
  }
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    GRCORE_Context * context;
    if (grcore_context_create(group, NULL, &context) != GRCORE_OK) {
      setup_failed("context create");
    }
    sink += grcore_group_context_count(group);
    if (grcore_context_destroy(context) != GRCORE_OK) {
      setup_failed("context destroy");
    }
  }
  grcore_group_destroy(group);
  return sink;
}

/* A counting malloc and free pair: the price of the exact meter over a plain
 * allocation. Sizes vary so the allocator cannot settle on one free list. */
static uint64_t counting_malloc_free_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK) {
    setup_failed("group create");
  }
  if (grcore_context_create(group, NULL, &context) != GRCORE_OK) {
    setup_failed("context create");
  }
  const GRCORE_Allocator * a = grcore_context_allocator(context);
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    void * p = a->malloc_fn(a->ctx, 16u + (size_t)(i & 63u) * 8u);
    if (p == NULL) {
      setup_failed("malloc");
    }
    sink += grcore_context_memory_blocks(context);
    a->free_fn(a->ctx, p);
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

static const GRCORE_Key bench_keys[8] = {
    {"k0", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL},
    {"k1", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL},
    {"k2", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL},
    {"k3", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL},
    {"k4", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL},
    {"k5", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL},
    {"k6", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL},
    {"k7", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL},
};

/* A keyed slot lookup with eight registrations, asking for the last: the
 * worst case of the linear table that a service pays on a cold path. */
static uint64_t keyed_lookup_run(uint64_t iterations) {
  static int values[8];
  GRCORE_Group * group;
  GRCORE_Context * context;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK) {
    setup_failed("group create");
  }
  if (grcore_context_create(group, NULL, &context) != GRCORE_OK) {
    setup_failed("context create");
  }
  for (int k = 0; k < 8; k++) {
    if (grcore_context_register(context, &bench_keys[k], &values[k]) !=
        GRCORE_OK) {
      setup_failed("register");
    }
  }
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    sink += (uint64_t)(uintptr_t)grcore_context_slot(context, &bench_keys[7]);
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

static const Case cases[] = {
    {"calibration", calibration_run, 200u * 1000u * 1000u, 1000u * 1000u},
    {"ctx-create", context_create_destroy_run, 1000u * 1000u, 1000u},
    {"count-malloc", counting_malloc_free_run, 20u * 1000u * 1000u, 10000u},
    {"keyed-lookup", keyed_lookup_run, 100u * 1000u * 1000u, 100000u},
};

#define REPEATS 7

static int compare_double(const void * a, const void * b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(int argc, char ** argv) {
  int smoke = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--smoke") == 0) {
      smoke = 1;
    } else {
      fprintf(stderr, "bench: unknown argument '%s'\n", argv[i]);
      return 2;
    }
  }

  /* Naming the library's version proves the harness linked the library it
   * claims to measure, and ties every figure to the build that produced it. */
  printf("runtime-core %s, %s run\n", grcore_version_string(),
      smoke ? "smoke" : "full");

  int repeats = smoke ? 1 : REPEATS;
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    uint64_t n = smoke ? cases[c].smoke_iterations : cases[c].iterations;
    double ns[REPEATS];
    uint64_t sink = 0;
    for (int r = 0; r < repeats; r++) {
      double start = now_ns();
      sink ^= cases[c].run(n);
      ns[r] = now_ns() - start;
    }
    qsort(ns, (size_t)repeats, sizeof(ns[0]), compare_double);
    double best = ns[0] / (double)n;
    double median = ns[repeats / 2] / (double)n;
    if (!(best > 0.0)) {
      fprintf(stderr, "bench: %s measured no time; the clock is unusable\n",
          cases[c].name);
      return 1;
    }
    printf("%-12s best %8.3f ns/op  median %8.3f ns/op  (%llu ops x %d, "
           "check %016llx)\n",
        cases[c].name, best, median, (unsigned long long)n, repeats,
        (unsigned long long)sink);
  }
  return 0;
}
