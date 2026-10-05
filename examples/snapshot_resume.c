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
 * Freeze a paused context, and finish it twice somewhere else.
 *
 * The "guest" is the counting loop of `pause_resume.c`. Its position lives in
 * a keyed state, and the key has snapshot hooks, so the position travels with
 * the snapshot: `snapshot` writes it, `restore` reads it back, and `settle`
 * has nothing to add (a state with references into a heap would use it to
 * write them once every key had its frames).
 *
 * The host pauses the loop, takes a snapshot, and restores it into two fresh
 * contexts. One resumes on this thread and one on another; both finish with
 * the answer an uninterrupted run gives, and the paused original can still
 * finish too. The snapshot is data, so it could be restored a third time after
 * the original was gone.
 *
 * Build and run with `make examples`.
 */

#include <ghoti.io/runtime-core/runtime-core.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define LOOP_ITERATIONS 1000u

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "snapshot_resume: check failed at line %d: %s\n",        \
          __LINE__, #condition);                                               \
      return 1;                                                                \
    }                                                                          \
  } while (0)

typedef struct {
  uint64_t next;
  uint64_t sum;
} Loop;

static GRCORE_Step loop_entry(GRCORE_Context * context, void * state) {
  Loop * loop = state;
  while (loop->next < LOOP_ITERATIONS) {
    grcore_context_charge_fuel(context, 1);
    GRCORE_Verdict verdict = GRCORE_POLL(context);
    if (verdict == GRCORE_VERDICT_PAUSE) {
      return GRCORE_STEP_PAUSED;
    }
    if (verdict == GRCORE_VERDICT_UNWIND) {
      return GRCORE_STEP_UNWOUND;
    }
    loop->sum += loop->next;
    loop->next++;
  }
  return GRCORE_STEP_FINISHED;
}

/* The key's hooks. Nothing in the bytes is an address: two integers. */
static GRCORE_Result loop_snapshot(
    GRCORE_Context * context, void * value, GRCORE_SnapshotWriter * writer) {
  (void)context;
  Loop * loop = value;
  GRCORE_Result r = grcore_snapshot_writer_u64(writer, loop->next);
  return r != GRCORE_OK ? r : grcore_snapshot_writer_u64(writer, loop->sum);
}

static GRCORE_Result loop_restore(GRCORE_Context * context, void * value,
    GRCORE_SnapshotReader * reader, void * env, GRCORE_RestoreMode mode) {
  (void)context;
  (void)env;
  Loop * loop = value;
  uint64_t next, sum;
  GRCORE_Result r = grcore_snapshot_reader_u64(reader, &next);
  if (r == GRCORE_OK) {
    r = grcore_snapshot_reader_u64(reader, &sum);
  }
  if (r == GRCORE_OK && mode == GRCORE_RESTORE_APPLY) {
    loop->next = next; /* CHECK changes nothing; APPLY builds */
    loop->sum = sum;
  }
  return r;
}

static GRCORE_Result loop_settle(GRCORE_Context * context, void * value,
    void * env, GRCORE_SettleMode mode) {
  (void)context;
  (void)env;
  if (mode == GRCORE_SETTLE_ABANDON) {
    Loop * loop = value;
    loop->next = loop->sum = 0; /* back to a fresh one */
  }
  return GRCORE_OK;
}

static const GRCORE_Key loop_key = GRCORE_KEY_INIT("example loop", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_NONE, NULL, NULL, loop_snapshot, loop_restore, loop_settle);

typedef struct {
  GRCORE_Group * group;
  GRCORE_Context * context;
  Loop loop;
} Side;

/* A fresh context with the loop's key registered, and restored from the
 * snapshot, paused and ready to resume. */
static int restore_into(Side * side, const GRCORE_Snapshot * snapshot) {
  CHECK(grcore_group_create(NULL, NULL, &side->group) == GRCORE_OK);
  CHECK(grcore_context_create(side->group, NULL, &side->context) == GRCORE_OK);
  CHECK(grcore_context_register(side->context, &loop_key, &side->loop) ==
      GRCORE_OK);
  GRCORE_RestoreEnv env = GRCORE_RESTORE_ENV_INIT(NULL, NULL, NULL, NULL, NULL);
  env.entry = loop_entry; /* what a paused context resumes with */
  env.entry_state = &side->loop;
  CHECK(grcore_context_restore(side->context, snapshot, &env) == GRCORE_OK);
  CHECK(grcore_context_state(side->context) == GRCORE_CONTEXT_PAUSED);
  return 0;
}

static void * finish_on_another_thread(void * arg) {
  Side * side = arg;
  GRCORE_Outcome outcome;
  if (grcore_context_acquire(side->context) != GRCORE_OK ||
      grcore_resume(side->context, &outcome) != GRCORE_OK ||
      outcome != GRCORE_OUTCOME_FINISHED) {
    side->loop.sum = 0;
  }
  grcore_context_release(side->context); /* back to whoever joins */
  return NULL;
}

int main(void) {
  const uint64_t expected = (uint64_t)LOOP_ITERATIONS * (LOOP_ITERATIONS - 1) / 2;

  /* The original, paused part way. */
  GRCORE_Group * group;
  GRCORE_Context * context;
  Loop loop = {0, 0};
  GRCORE_Options * options;
  GRCORE_Outcome outcome;
  CHECK(grcore_group_create(NULL, NULL, &group) == GRCORE_OK);
  CHECK(grcore_options_create(NULL, &options) == GRCORE_OK);
  grcore_options_set_fuel(options, 300);
  CHECK(grcore_context_create(group, options, &context) == GRCORE_OK);
  grcore_options_destroy(options);
  CHECK(grcore_context_register(context, &loop_key, &loop) == GRCORE_OK);
  CHECK(grcore_run(context, loop_entry, &loop, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_PAUSED);
  CHECK(loop.next > 0 && loop.next < LOOP_ITERATIONS);

  /* Freeze it. */
  GRCORE_Snapshot * snapshot;
  CHECK(grcore_context_snapshot(context, NULL, &snapshot) == GRCORE_OK);
  printf("snapshot of a context paused at %llu of %u: %zu bytes\n",
      (unsigned long long)loop.next, LOOP_ITERATIONS,
      grcore_snapshot_size(snapshot));

  /* Two restores: one finished here, one on another thread. */
  Side here = {0}, there = {0};
  if (restore_into(&here, snapshot) != 0 || restore_into(&there, snapshot) != 0) {
    return 1;
  }
  CHECK(grcore_resume(here.context, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_FINISHED);
  CHECK(here.loop.sum == expected);

  pthread_t thread;
  CHECK(grcore_context_release(there.context) == GRCORE_OK);
  CHECK(pthread_create(&thread, NULL, finish_on_another_thread, &there) == 0);
  pthread_join(thread, NULL);
  CHECK(there.loop.sum == expected);

  /* The original is untouched by all of it, and finishes the same way. */
  CHECK(grcore_context_set_fuel(context, GRCORE_UNLIMITED) == GRCORE_OK);
  CHECK(grcore_resume(context, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_FINISHED);
  CHECK(loop.sum == expected);
  printf("original, restored here and restored on another thread all give %llu\n",
      (unsigned long long)expected);

  grcore_snapshot_release(snapshot);
  CHECK(grcore_context_destroy(here.context) == GRCORE_OK);
  CHECK(grcore_group_destroy(here.group) == GRCORE_OK);
  CHECK(grcore_context_acquire(there.context) == GRCORE_OK);
  CHECK(grcore_context_destroy(there.context) == GRCORE_OK);
  CHECK(grcore_group_destroy(there.group) == GRCORE_OK);
  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  return 0;
}
