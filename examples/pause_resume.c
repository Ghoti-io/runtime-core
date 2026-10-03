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
 * Pause, raise the budget, resume; unwind; and resume on another thread.
 *
 * The "guest" is a counting loop, which is all `runtime-core` needs to show
 * the mechanism without an engine. It adds up the integers below `n` and
 * charges one unit of fuel for each, polling as it goes. A pause cannot keep
 * C frames, so the loop keeps its position in its own state: when a poll says
 * pause, the entry function returns and `resume` calls it again, and it
 * carries on from where the state says. Real engines keep that position on
 * the guest stack.
 *
 * Three runs of the same loop:
 *   1. Under a small fuel budget it pauses. The host reads which key caused
 *      the pause and where, raises the budget, resumes, and gets the same
 *      answer as an uninterrupted run.
 *   2. A second run pauses, the host posts terminate and resumes: the run
 *      ends with GRCORE_ERR_LIMIT, and the context says why.
 *   3. A third run pauses on this thread and is resumed on another one, which
 *      is legal because a paused context has no host C frame above `run`.
 *
 * Build and run with `make examples`.
 */

#include <ghoti.io/runtime-core/runtime-core.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define LOOP_ITERATIONS 1000u
#define FUEL_SLICE 300u

/* Checked in every build: an example that asserts nothing under NDEBUG is
 * not an example of anything. */
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "pause_resume: check failed at line %d: %s\n", __LINE__, \
          #condition);                                                         \
      return 1;                                                                \
    }                                                                          \
  } while (0)

typedef struct {
  uint64_t next; /* the position, saved across a pause */
  uint64_t sum;
} Loop;

static GRCORE_Step loop_entry(GRCORE_Context * context, void * state) {
  Loop * loop = state;
  while (loop->next < LOOP_ITERATIONS) {
    grcore_context_charge_fuel(context, 1);
    GRCORE_Verdict verdict = GRCORE_POLL(context);
    if (verdict == GRCORE_VERDICT_PAUSE) {
      return GRCORE_STEP_PAUSED; /* loop->next says where to pick up */
    }
    if (verdict == GRCORE_VERDICT_UNWIND) {
      return GRCORE_STEP_UNWOUND;
    }
    loop->sum += loop->next;
    loop->next++;
  }
  return GRCORE_STEP_FINISHED;
}

static uint64_t expected_sum(void) {
  uint64_t sum = 0;
  for (uint64_t i = 0; i < LOOP_ITERATIONS; i++) {
    sum += i;
  }
  return sum;
}

typedef struct {
  GRCORE_Context * context;
  GRCORE_Result result;
  GRCORE_Outcome outcome;
} Handoff;

static void * resume_elsewhere(void * argument) {
  Handoff * handoff = argument;
  handoff->result = grcore_context_acquire(handoff->context);
  if (handoff->result != GRCORE_OK) {
    return NULL;
  }
  handoff->result = grcore_context_set_fuel(handoff->context, GRCORE_UNLIMITED);
  if (handoff->result == GRCORE_OK) {
    handoff->result = grcore_resume(handoff->context, &handoff->outcome);
  }
  grcore_context_release(handoff->context);
  return NULL;
}

static GRCORE_Result make_context(GRCORE_Group ** group,
    GRCORE_Context ** context) {
  GRCORE_Options * options;
  GRCORE_Result r = grcore_options_create(NULL, &options);
  if (r != GRCORE_OK) {
    return r;
  }
  grcore_options_set_fuel(options, FUEL_SLICE);
  r = grcore_group_create(NULL, NULL, group);
  if (r == GRCORE_OK) {
    r = grcore_context_create(*group, options, context);
    if (r != GRCORE_OK) {
      grcore_group_destroy(*group);
    }
  }
  grcore_options_destroy(options);
  return r;
}

static void teardown(GRCORE_Group * group, GRCORE_Context * context) {
  grcore_context_destroy(context);
  grcore_group_destroy(group);
}

int main(void) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_Outcome outcome;
  Loop loop;

  /* 1. Pause on fuel, raise the budget, resume. */
  CHECK(make_context(&group, &context) == GRCORE_OK);
  loop.next = loop.sum = 0;
  CHECK(grcore_run(context, loop_entry, &loop, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_PAUSED);
  CHECK(grcore_context_pause_key_count(context) == 1);
  const GRCORE_Key * key = grcore_context_pause_key(context, 0);
  GRCORE_Location where = grcore_context_pause_location(context);
  printf("paused after %llu of %u iterations: key '%s' at %s:%d\n",
      (unsigned long long)loop.next, LOOP_ITERATIONS, key->name, where.file,
      where.line);
  CHECK(key == grcore_core_key(GRCORE_REQUEST_FUEL));
  CHECK(loop.next == FUEL_SLICE);
  CHECK(grcore_context_set_fuel(context, GRCORE_UNLIMITED) == GRCORE_OK);
  CHECK(grcore_resume(context, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_FINISHED);
  CHECK(loop.sum == expected_sum());
  printf("resumed and finished: sum %llu, as an uninterrupted run gives\n",
      (unsigned long long)loop.sum);
  teardown(group, context);

  /* 2. Pause, then terminate: the run ends with a limit error. */
  CHECK(make_context(&group, &context) == GRCORE_OK);
  loop.next = loop.sum = 0;
  CHECK(grcore_run(context, loop_entry, &loop, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_PAUSED);
  CHECK(grcore_context_terminate(context) == GRCORE_OK);
  GRCORE_Result r = grcore_resume(context, &outcome);
  CHECK(r == GRCORE_ERR_LIMIT);
  CHECK(grcore_context_unwind_result(context) == GRCORE_ERR_LIMIT);
  CHECK(grcore_context_pause_key(context, 0) ==
      grcore_core_key(GRCORE_REQUEST_TERMINATE));
  printf("terminated: %s, key '%s'\n", grcore_result_string(r),
      grcore_context_pause_key(context, 0)->name);
  teardown(group, context);

  /* 3. Pause here, resume on another thread. */
  CHECK(make_context(&group, &context) == GRCORE_OK);
  loop.next = loop.sum = 0;
  CHECK(grcore_run(context, loop_entry, &loop, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_PAUSED);
  CHECK(grcore_context_release(context) == GRCORE_OK);
  Handoff handoff = {context, GRCORE_ERR_INTERNAL, GRCORE_OUTCOME_PAUSED};
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, resume_elsewhere, &handoff) == 0);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(handoff.result == GRCORE_OK);
  CHECK(handoff.outcome == GRCORE_OUTCOME_FINISHED);
  CHECK(loop.sum == expected_sum());
  CHECK(grcore_context_acquire(context) == GRCORE_OK);
  printf("resumed on another thread: sum %llu\n", (unsigned long long)loop.sum);
  teardown(group, context);
  return 0;
}
