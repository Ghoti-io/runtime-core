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
 * A host profiles a guest without knowing the engine.
 *
 * The toy engine is the one frame_walk.c defines in miniature: a descriptor
 * that says where a poll identity is in the source. The guest runs two loops
 * under one caller, "hot" (function 3, line 300 + step) and "cold" (function
 * 4). The host attaches a profiler and asks for a sample in two ways:
 *
 *  1. By hand: the guest's host call posts a request before a poll, which is
 *     how a test makes the sampling deterministic. The hot loop is sampled on
 *     every iteration and the cold one on every fourth.
 *  2. With the timer: `grcore_profiler_timer_start` runs a thread that posts
 *     the request every millisecond. The thread touches no guest state; the
 *     samples are taken by the poll that sees the request. The guest spins in
 *     one function until the timer has taken enough samples (or a deadline),
 *     so the example never asserts on a handful.
 *
 * The report is "self" (the innermost frame was here) and "inclusive" (the
 * location was on the stack). A sample is taken at the *next poll*, so time
 * is charged to the poll that ends it: the profile is biased to safepoints.
 *
 * Build and run with `make examples`.
 */

/* clock_gettime is POSIX, and -std=c17 hides it. */
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/runtime-core/runtime-core.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "profile_toy: check failed at line %d: %s\n", __LINE__,  \
          #condition);                                                         \
      return 1;                                                                \
    }                                                                          \
  } while (0)

/* A poll identity (function, step) is line function * 100 + step of toy.src. */
static GRCORE_Location toy_locate(
    const GRCORE_Context * context, uint64_t function, uint64_t offset) {
  (void)context;
  GRCORE_Location where = {"toy.src", (int)(function * 100 + offset)};
  return where;
}

static const GRCORE_EngineDescriptor toy_engine = {"toy", NULL, toy_locate,
    NULL, {NULL, NULL, NULL}, {0, 0, 0}, NULL, NULL};

#define HOT_STEPS 40u
#define COLD_STEPS 40u

typedef struct {
  GRCORE_EngineId engine;
  GRCORE_Profiler * profiler;
  int use_timer;
} Toy;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* One call: push a frame, poll `steps` times at (function, step), pop. The
 * caller's frame records where it called from, so the walk can say. */
static GRCORE_Step toy_entry(GRCORE_Context * context, void * state) {
  Toy * toy = state;
  GRCORE_Stack * stack = grcore_context_stack(context);
  GRCORE_FrameRef caller, callee;
  if (grcore_stack_push(stack, toy->engine, 1, &caller) != GRCORE_OK) {
    return GRCORE_STEP_UNWOUND;
  }
  grcore_stack_set_identity(stack, caller, (GRCORE_PollIdentity){1, 1});
  if (!toy->use_timer) {
    for (uint64_t function = 3; function <= 4; function++) {
      if (grcore_stack_push(stack, toy->engine, 1, &callee) != GRCORE_OK) {
        return GRCORE_STEP_UNWOUND;
      }
      for (uint64_t step = 0; step < HOT_STEPS; step++) {
        /* The hot function asks for a sample every step, the cold one every
         * fourth: a 4 to 1 split in what the profile can see. */
        if (function == 3 || step % 4 == 0) {
          if (grcore_profiler_request(toy->profiler) != GRCORE_OK) {
            return GRCORE_STEP_UNWOUND;
          }
        }
        if (grcore_stack_poll(context, function, 10 + function) !=
            GRCORE_VERDICT_CONTINUE) {
          return GRCORE_STEP_UNWOUND;
        }
      }
      grcore_stack_pop(stack);
    }
  } else {
    /* Spin at one poll site until the timer has posted enough requests, or a
     * deadline passes. The timer thread does the asking. */
    if (grcore_stack_push(stack, toy->engine, 1, &callee) != GRCORE_OK) {
      return GRCORE_STEP_UNWOUND;
    }
    double deadline = now_ms() + 2000.0;
    uint64_t polls = 0;
    while (now_ms() < deadline) {
      if (grcore_stack_poll(context, 5, 7) != GRCORE_VERDICT_CONTINUE) {
        return GRCORE_STEP_UNWOUND;
      }
      if ((++polls & 0xFFFu) == 0) {
        GRCORE_ProfileTotals totals = {0, 0, 0, 0};
        /* The report is refused inside a poll but this is the guest's own
         * code, between polls, where the owner may read it. */
        if (grcore_profiler_report(toy->profiler, NULL, 0, NULL, &totals) ==
                GRCORE_OK &&
            totals.samples >= 20) {
          break;
        }
      }
    }
    grcore_stack_pop(stack);
  }
  grcore_stack_pop(stack);
  return GRCORE_STEP_FINISHED;
}

static int print_report(GRCORE_Profiler * profiler, GRCORE_ProfileEntry * out,
    size_t room, size_t * count, GRCORE_ProfileTotals * totals) {
  CHECK(grcore_profiler_report(profiler, out, room, count, totals) == GRCORE_OK);
  printf("%llu samples, %llu with no frame, %llu dropped\n",
      (unsigned long long)totals->samples, (unsigned long long)totals->no_frame,
      (unsigned long long)totals->dropped);
  for (size_t i = 0; i < *count; i++) {
    printf("  %s:%d  self %llu  inclusive %llu\n", out[i].file, out[i].line,
        (unsigned long long)out[i].self, (unsigned long long)out[i].inclusive);
  }
  return 0;
}

int main(void) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  CHECK(grcore_group_create(NULL, NULL, &group) == GRCORE_OK);
  CHECK(grcore_context_create(group, NULL, &context) == GRCORE_OK);
  Toy toy = {0, NULL, 0};
  CHECK(grcore_engine_register(context, &toy_engine, &toy.engine) == GRCORE_OK);
  CHECK(grcore_profiler_attach(context, 0, &toy.profiler) == GRCORE_OK);

  /* 1. Samples by hand. */
  GRCORE_Outcome outcome;
  CHECK(grcore_run(context, toy_entry, &toy, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_FINISHED);
  GRCORE_ProfileEntry entries[8];
  size_t count = 0;
  GRCORE_ProfileTotals totals;
  CHECK(print_report(toy.profiler, entries, 8, &count, &totals) == 0);
  CHECK(totals.samples == HOT_STEPS + COLD_STEPS / 4);
  CHECK(totals.no_frame == 0 && totals.dropped == 0);
  /* Hottest first: the hot function's line, then the cold one, then the
   * caller, which is on the stack for every sample and innermost for none. */
  CHECK(count == 3);
  CHECK(entries[0].line == 313 && entries[0].self == HOT_STEPS);
  CHECK(entries[1].line == 414 && entries[1].self == COLD_STEPS / 4);
  CHECK(entries[2].line == 101 && entries[2].self == 0);
  CHECK(entries[2].inclusive == totals.samples);
  CHECK(strcmp(entries[0].file, "toy.src") == 0);

  /* 2. Samples by timer. */
  CHECK(grcore_profiler_reset(toy.profiler) == GRCORE_OK);
  toy.use_timer = 1;
  CHECK(grcore_profiler_timer_start(toy.profiler, 1000) == GRCORE_OK);
  CHECK(grcore_run(context, toy_entry, &toy, &outcome) == GRCORE_OK);
  CHECK(grcore_profiler_timer_stop(toy.profiler) == GRCORE_OK);
  CHECK(print_report(toy.profiler, entries, 8, &count, &totals) == 0);
  CHECK(totals.samples >= 20);
  CHECK(count == 2 && entries[0].line == 507 && entries[0].self == totals.samples);

  /* The timer is joined by the context's destruction too: nothing is left to
   * stop. */
  CHECK(grcore_profiler_timer_start(toy.profiler, 1000) == GRCORE_OK);
  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  printf("ok\n");
  return 0;
}
