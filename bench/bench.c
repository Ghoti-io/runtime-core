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
 * cases of the runtime: creating and destroying a context, a counting
 * malloc/free pair, a keyed slot lookup, the poll's fast path, its slow path
 * through four handlers, a post to a port, a push and pop of a guest frame,
 * the engine-aware poll's fast path, a walk of sixteen frames at a pause, an
 * activation record entered and left, a fuel scope opened, charged and closed,
 * the enumeration of the roots of sixteen frames, a snapshot taken and
 * restored, a profiler sample at depth one and at depth thirty-two, and the
 * validation of two code-metadata tables.
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
 * Numeric budgets are not asserted here. The first measurement of every case,
 * with the machine, compiler and flags it was taken on, is recorded in
 * documentation/design.md, "Benchmarks" (AD-26).
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
    GRCORE_KEY_INIT("k0", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL, NULL, NULL, NULL, NULL),
    GRCORE_KEY_INIT("k1", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL, NULL, NULL, NULL, NULL),
    GRCORE_KEY_INIT("k2", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL, NULL, NULL, NULL, NULL),
    GRCORE_KEY_INIT("k3", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL, NULL, NULL, NULL, NULL),
    GRCORE_KEY_INIT("k4", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL, NULL, NULL, NULL, NULL),
    GRCORE_KEY_INIT("k5", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL, NULL, NULL, NULL, NULL),
    GRCORE_KEY_INIT("k6", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL, NULL, NULL, NULL, NULL),
    GRCORE_KEY_INIT("k7", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, NULL, NULL, NULL, NULL, NULL),
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

/* The poll's fast path: nothing pending, so a call is one load and one
 * branch. The loop runs inside `run`, because a poll needs a running
 * context. The figure includes the call, which a JIT's inline check does
 * not pay. */
typedef struct {
  uint64_t iterations;
  uint64_t sink;
} PollLoop;

static GRCORE_Step poll_loop_entry(GRCORE_Context * context, void * state) {
  PollLoop * loop = state;
  for (uint64_t i = 0; i < loop->iterations; i++) {
    loop->sink += (uint64_t)GRCORE_POLL(context);
  }
  return GRCORE_STEP_FINISHED;
}

static uint64_t run_poll_loop(GRCORE_Context * context, uint64_t iterations) {
  PollLoop loop = {iterations, 0};
  GRCORE_Outcome outcome;
  if (grcore_run(context, poll_loop_entry, &loop, &outcome) != GRCORE_OK ||
      outcome != GRCORE_OUTCOME_FINISHED) {
    setup_failed("run");
  }
  return loop.sink;
}

static uint64_t poll_fast_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK) {
    setup_failed("group create");
  }
  if (grcore_context_create(group, NULL, &context) != GRCORE_OK) {
    setup_failed("context create");
  }
  uint64_t sink = run_poll_loop(context, iterations);
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

static void nothing(GRCORE_Context * c, void * value, GRCORE_PollCall * call) {
  (void)c;
  (void)value;
  (void)call;
}

static const GRCORE_Key slow_keys[4] = {
    GRCORE_KEY_INIT("b-decide", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_DECIDE, NULL, nothing, NULL, NULL, NULL),
    GRCORE_KEY_INIT("b-act", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_ACT, NULL, nothing, NULL, NULL, NULL),
    GRCORE_KEY_INIT("b-observe", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_OBSERVE, NULL, nothing, NULL, NULL, NULL),
    GRCORE_KEY_INIT("b-yield", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_YIELD, NULL, nothing, NULL, NULL, NULL),
};

/* The slow path with one handler in each phase and a request pending that
 * nothing clears, so every poll runs all four phases and the five built-in
 * deciders. This is the price of a poll that has something to say. */
static uint64_t poll_slow_run(uint64_t iterations) {
  static int values[4];
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_Port * port;
  GRCORE_RequestKind kind;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK) {
    setup_failed("group create");
  }
  if (grcore_context_create(group, NULL, &context) != GRCORE_OK) {
    setup_failed("context create");
  }
  for (int k = 0; k < 4; k++) {
    if (grcore_context_register(context, &slow_keys[k], &values[k]) !=
        GRCORE_OK) {
      setup_failed("register");
    }
  }
  if (grcore_context_request_kind(context, &slow_keys[0], &kind) != GRCORE_OK ||
      grcore_context_port(context, &port) != GRCORE_OK ||
      grcore_port_post(port, kind) != GRCORE_OK) {
    setup_failed("request");
  }
  uint64_t sink = run_poll_loop(context, iterations);
  grcore_port_release(port);
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

/* A post from another thread's side: the port mutex, one atomic OR and the
 * condition-variable signal. Measured on one thread, so it is the uncontended
 * cost. */
static uint64_t port_post_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_Port * port;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK) {
    setup_failed("group create");
  }
  if (grcore_context_create(group, NULL, &context) != GRCORE_OK) {
    setup_failed("context create");
  }
  if (grcore_context_port(context, &port) != GRCORE_OK) {
    setup_failed("port");
  }
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    if (grcore_port_post(port, GRCORE_REQUEST_TIME) != GRCORE_OK) {
      setup_failed("post");
    }
    sink += (uint64_t)grcore_context_request_pending(context, GRCORE_REQUEST_TIME);
  }
  grcore_port_release(port);
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

/* A static engine for the frame cases. It names nothing and reads nothing, so
 * what is measured is the stack and the walk, not a descriptor. */
static GRCORE_Location bench_locate(
    const GRCORE_Context * context, uint64_t function, uint64_t offset) {
  (void)context;
  (void)function;
  (void)offset;
  GRCORE_Location where = {"bench", 1};
  return where;
}

static GRCORE_SlotKind bench_all_values(
    const GRCORE_AbstractFrame * frame, size_t index) {
  (void)frame;
  (void)index;
  return GRCORE_SLOT_VALUE;
}

static const GRCORE_EngineDescriptor bench_value_engine = GRCORE_ENGINE_DESCRIPTOR_INIT("bench-value",
    bench_all_values, NULL, NULL, {NULL, NULL, NULL}, {0, 0, 0}, NULL, NULL);

static const GRCORE_EngineDescriptor bench_engine = GRCORE_ENGINE_DESCRIPTOR_INIT("bench", NULL,
    bench_locate, NULL, {NULL, NULL, NULL}, {0, 0, 0}, NULL, NULL);

/* A push and the matching pop of a four-slot frame on a warm stack: the price
 * of a guest call's bookkeeping, depth budget included. */
static uint64_t stack_push_pop_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_EngineId engine;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK) {
    setup_failed("group create");
  }
  if (grcore_context_create(group, NULL, &context) != GRCORE_OK) {
    setup_failed("context create");
  }
  if (grcore_engine_register(context, &bench_engine, &engine) != GRCORE_OK) {
    setup_failed("engine register");
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    GRCORE_FrameRef frame;
    if (grcore_stack_push(stack, engine, 4, &frame) != GRCORE_OK) {
      setup_failed("push");
    }
    sink += frame.offset;
    if (grcore_stack_pop(stack) != GRCORE_OK) {
      setup_failed("pop");
    }
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

/* The poll that records an identity: nothing pending, one frame, so it is
 * grcore_poll's fast path plus a keyed lookup and two stores. */
static GRCORE_Step stack_poll_entry(GRCORE_Context * context, void * state) {
  PollLoop * loop = state;
  GRCORE_FrameRef frame;
  if (grcore_stack_push(grcore_context_stack(context), 1, 1, &frame) !=
      GRCORE_OK) {
    setup_failed("push");
  }
  for (uint64_t i = 0; i < loop->iterations; i++) {
    loop->sink += (uint64_t)grcore_stack_poll(context, 1, i);
  }
  return GRCORE_STEP_FINISHED;
}

static uint64_t stack_poll_fast_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_EngineId engine;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK) {
    setup_failed("group create");
  }
  if (grcore_context_create(group, NULL, &context) != GRCORE_OK) {
    setup_failed("context create");
  }
  if (grcore_engine_register(context, &bench_engine, &engine) != GRCORE_OK ||
      engine != 1) {
    setup_failed("engine register");
  }
  PollLoop loop = {iterations, 0};
  GRCORE_Outcome outcome;
  if (grcore_run(context, stack_poll_entry, &loop, &outcome) != GRCORE_OK ||
      outcome != GRCORE_OUTCOME_FINISHED) {
    setup_failed("run");
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return loop.sink;
}

#define WALK_FRAMES 16u

/* Sixteen frames, paused on fuel, then repeated walks of all of them reading
 * every frame's location: the cost a debugger or a collector pays per pause.
 * An iteration is one frame read, so the figure is per frame. */
static GRCORE_Step walk_entry(GRCORE_Context * context, void * state) {
  (void)state;
  GRCORE_Stack * stack = grcore_context_stack(context);
  for (unsigned i = 0; i < WALK_FRAMES; i++) {
    GRCORE_FrameRef frame;
    if (grcore_stack_push(stack, 1, 2, &frame) != GRCORE_OK) {
      setup_failed("push");
    }
  }
  grcore_context_charge_fuel(context, 1);
  return grcore_stack_poll(context, 1, 1) == GRCORE_VERDICT_PAUSE
      ? GRCORE_STEP_PAUSED
      : GRCORE_STEP_FINISHED;
}

static uint64_t frame_walk_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_Options * options;
  GRCORE_EngineId engine;
  if (grcore_options_create(NULL, &options) != GRCORE_OK ||
      grcore_options_set_fuel(options, 0) != GRCORE_OK ||
      grcore_group_create(NULL, NULL, &group) != GRCORE_OK ||
      grcore_context_create(group, options, &context) != GRCORE_OK) {
    setup_failed("setup");
  }
  grcore_options_destroy(options);
  if (grcore_engine_register(context, &bench_engine, &engine) != GRCORE_OK) {
    setup_failed("engine register");
  }
  GRCORE_Outcome outcome;
  if (grcore_run(context, walk_entry, NULL, &outcome) != GRCORE_OK ||
      outcome != GRCORE_OUTCOME_PAUSED) {
    setup_failed("run");
  }
  uint64_t sink = 0;
  uint64_t frames = 0;
  while (frames < iterations) {
    GRCORE_FrameWalk walk;
    GRCORE_AbstractFrame frame;
    if (grcore_frame_walk_begin(context, &walk) != GRCORE_OK) {
      setup_failed("walk begin");
    }
    while (grcore_frame_walk_next(&walk, &frame)) {
      sink += (uint64_t)frame.location.line + frame.depth;
      frames++;
    }
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

/* An activation record entered and left: the price of recording one crossing
 * (a JIT or native call), native depth budget included. */
static uint64_t activation_pair_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_EngineId engine;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK ||
      grcore_context_create(group, NULL, &context) != GRCORE_OK ||
      grcore_engine_register(context, &bench_engine, &engine) != GRCORE_OK) {
    setup_failed("setup");
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  GRCORE_CSegment segment = {0x1000, 0x2000};
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    GRCORE_ActivationRef ref;
    if (grcore_activation_enter(stack, GRCORE_ACTIVATION_NATIVE, engine, 0,
            &segment, &ref) != GRCORE_OK ||
        grcore_activation_leave(stack, ref) != GRCORE_OK) {
      setup_failed("activation");
    }
    sink += ref.id;
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

/* A budget scope opened, charged once and closed: the price of a template
 * call's boundary on the fuel path. */
static uint64_t fuel_scope_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_EngineId engine;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK ||
      grcore_context_create(group, NULL, &context) != GRCORE_OK ||
      grcore_engine_register(context, &bench_engine, &engine) != GRCORE_OK) {
    setup_failed("setup");
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    GRCORE_BudgetScope scope;
    if (grcore_budget_scope_open(
            stack, 1000, GRCORE_SCOPE_POLICY_UNWIND, &scope) != GRCORE_OK) {
      setup_failed("scope open");
    }
    sink += grcore_context_charge_fuel(context, 20);
    if (grcore_budget_scope_close(stack, scope) != GRCORE_OK) {
      setup_failed("scope close");
    }
    sink += scope.id;
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

static void count_slot(void * user, uint64_t * slot) {
  *(uint64_t *)user += *slot;
}

/* The roots of sixteen frames of two VALUE slots each, enumerated through B's
 * types alone, as a collector would. An iteration is one frame, so the figure
 * is per frame. */
static uint64_t roots_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_EngineId engine;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK ||
      grcore_context_create(group, NULL, &context) != GRCORE_OK ||
      grcore_engine_register(context, &bench_value_engine, &engine) !=
          GRCORE_OK) {
    setup_failed("setup");
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  for (unsigned i = 0; i < WALK_FRAMES; i++) {
    GRCORE_FrameRef frame;
    if (grcore_stack_push(stack, engine, 2, &frame) != GRCORE_OK) {
      setup_failed("push");
    }
  }
  uint64_t sink = 0;
  GRCORE_RootVisitor visitor = {&sink, count_slot, NULL};
  for (uint64_t frames = 0; frames < iterations; frames += WALK_FRAMES) {
    if (grcore_context_enumerate_roots(context, &visitor) != GRCORE_OK) {
      setup_failed("enumerate");
    }
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

/* A key with snapshot hooks whose state is a block of words, so that a take
 * and a restore each move a known number of bytes through B's writer and
 * reader. The state is plain memory; nothing here is an engine. */
#define SNAP_WORDS 1024u
static uint64_t snap_block[SNAP_WORDS];

static GRCORE_Result snap_write(
    GRCORE_Context * context, void * value, GRCORE_SnapshotWriter * writer) {
  (void)context;
  return grcore_snapshot_writer_write(writer, value, SNAP_WORDS * 8u);
}

static GRCORE_Result snap_read(GRCORE_Context * context, void * value,
    GRCORE_SnapshotReader * reader, void * env, GRCORE_RestoreMode mode) {
  (void)context;
  (void)env;
  if (mode == GRCORE_RESTORE_CHECK) {
    return GRCORE_OK;
  }
  return grcore_snapshot_reader_read(reader, value, SNAP_WORDS * 8u);
}

static GRCORE_Result snap_settle(
    GRCORE_Context * context, void * value, void * env, GRCORE_SettleMode mode) {
  (void)context;
  (void)value;
  (void)env;
  (void)mode;
  return GRCORE_OK;
}

static const GRCORE_Key snap_key = GRCORE_KEY_INIT("bench-snapshot", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_NONE, NULL, NULL, snap_write, snap_read, snap_settle);

static GRCORE_Context * snap_context(GRCORE_Group ** out_group) {
  GRCORE_Context * context;
  if (grcore_group_create(NULL, NULL, out_group) != GRCORE_OK ||
      grcore_context_create(*out_group, NULL, &context) != GRCORE_OK ||
      grcore_context_register(context, &snap_key, snap_block) != GRCORE_OK) {
    setup_failed("setup");
  }
  return context;
}

/* One take of a context holding one 8 KiB blob, and the release of it. */
static uint64_t snapshot_take_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context = snap_context(&group);
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    GRCORE_Snapshot * snapshot;
    snap_block[0] = i;
    if (grcore_context_snapshot(context, NULL, &snapshot) != GRCORE_OK) {
      setup_failed("snapshot");
    }
    sink += grcore_snapshot_size(snapshot);
    grcore_snapshot_release(snapshot);
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

/* One restore of that snapshot into a context (the same one each time, which
 * the keys allow: the toy state is plain memory). */
static uint64_t snapshot_restore_run(uint64_t iterations) {
  GRCORE_Group * group;
  GRCORE_Context * context = snap_context(&group);
  GRCORE_Snapshot * snapshot;
  if (grcore_context_snapshot(context, NULL, &snapshot) != GRCORE_OK) {
    setup_failed("snapshot");
  }
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    if (grcore_context_restore(context, snapshot, NULL) != GRCORE_OK) {
      setup_failed("restore");
    }
    sink += snap_block[i % SNAP_WORDS];
  }
  grcore_snapshot_release(snapshot);
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return sink;
}

/* One profiler sample at a given stack depth: a request posted through the
 * port, then the poll that takes it (a frame walk counted into the table, the
 * request cleared). The figure is per sample, post included. Locations come
 * from `bench_locate`, so every frame is one location: the cost is the walk
 * and the lookups, not the table growing. */
typedef struct {
  uint64_t iterations;
  uint64_t sink;
  unsigned depth;
  GRCORE_Profiler * profiler;
} SampleLoop;

static GRCORE_Step profile_sample_entry(GRCORE_Context * context, void * state) {
  SampleLoop * loop = state;
  GRCORE_Stack * stack = grcore_context_stack(context);
  for (unsigned i = 0; i < loop->depth; i++) {
    GRCORE_FrameRef frame;
    if (grcore_stack_push(stack, 1, 1, &frame) != GRCORE_OK) {
      setup_failed("push");
    }
  }
  for (uint64_t i = 0; i < loop->iterations; i++) {
    if (grcore_profiler_request(loop->profiler) != GRCORE_OK) {
      setup_failed("request");
    }
    loop->sink += (uint64_t)grcore_stack_poll(context, 1, i);
  }
  return GRCORE_STEP_FINISHED;
}

static uint64_t profile_sample_run(uint64_t iterations, unsigned depth) {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRCORE_EngineId engine;
  GRCORE_Profiler * profiler;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK ||
      grcore_context_create(group, NULL, &context) != GRCORE_OK ||
      grcore_engine_register(context, &bench_engine, &engine) != GRCORE_OK ||
      engine != 1 ||
      grcore_profiler_attach(context, 0, &profiler) != GRCORE_OK) {
    setup_failed("setup");
  }
  SampleLoop loop = {iterations, 0, depth, profiler};
  GRCORE_Outcome outcome;
  if (grcore_run(context, profile_sample_entry, &loop, &outcome) != GRCORE_OK ||
      outcome != GRCORE_OUTCOME_FINISHED) {
    setup_failed("run");
  }
  GRCORE_ProfileTotals totals;
  if (grcore_profiler_report(profiler, NULL, 0, NULL, &totals) != GRCORE_OK ||
      totals.samples != iterations) {
    setup_failed("every request must have taken one sample");
  }
  grcore_context_destroy(context);
  grcore_group_destroy(group);
  return loop.sink + totals.samples;
}

static uint64_t profile_sample_1_run(uint64_t iterations) {
  return profile_sample_run(iterations, 1);
}

static uint64_t profile_sample_32_run(uint64_t iterations) {
  return profile_sample_run(iterations, 32);
}

/* Validating a code-metadata table of many sites: 100 sites of a thousand
 * live references and a thousand derived pointers each (a base that must be
 * found among the live slots), and then 20,000 sites of 20,000 different
 * functions (each of which must agree with the first earlier site of its
 * function, of which there is none). A validator that searches the live
 * slots for each derived pointer, or the earlier sites for each site, is
 * quadratic in both. An iteration is one validation of both tables. */
#define META_WIDE_SITES 100u
#define META_WIDE_SLOTS 1000u
#define META_MANY_SITES 20000u

static uint64_t codemeta_validate_run(uint64_t iterations) {
  static GRCORE_CodeSite wide_sites[META_WIDE_SITES];
  static GRCORE_CodeLocation wide_live[META_WIDE_SLOTS];
  static GRCORE_DerivedPointer wide_derived[META_WIDE_SLOTS];
  static GRCORE_CodeSite many_sites[META_MANY_SITES];
  for (uint32_t i = 0; i < META_WIDE_SLOTS; i++) {
    wide_live[i].kind = GRCORE_LOC_FRAME_SLOT;
    wide_live[i].slot_kind = GRCORE_SLOT_VALUE;
    wide_live[i].value = -8 * (int64_t)(i + 1);
    wide_derived[i].slot = -8 * (int64_t)(META_WIDE_SLOTS + i + 1);
    wide_derived[i].base_slot = -8 * (int64_t)(META_WIDE_SLOTS - i);
    wide_derived[i].delta = 16;
  }
  for (uint32_t i = 0; i < META_WIDE_SITES; i++) {
    wide_sites[i].code_offset = 10 + i * 10;
    wide_sites[i].kind = GRCORE_SITE_GC_POINT_CALL;
    wide_sites[i].identity.function = 1;
    wide_sites[i].identity.offset = i;
    wide_sites[i].live = wide_live;
    wide_sites[i].live_count = META_WIDE_SLOTS;
    wide_sites[i].derived = wide_derived;
    wide_sites[i].derived_count = META_WIDE_SLOTS;
  }
  for (uint32_t i = 0; i < META_MANY_SITES; i++) {
    many_sites[i].code_offset = 4 + i * 4;
    many_sites[i].kind = GRCORE_SITE_GUARD;
    many_sites[i].identity.function = 1000 + i;
  }
  GRCORE_CodeMeta wide = {GRCORE_CODEMETA_FORMAT_VERSION,
      16u * META_WIDE_SLOTS, 10u * META_WIDE_SITES + 20u, META_WIDE_SITES, wide_sites};
  GRCORE_CodeMeta many = {GRCORE_CODEMETA_FORMAT_VERSION, 64u,
      4u * META_MANY_SITES + 8u, META_MANY_SITES, many_sites};
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iterations; i++) {
    const char * why = NULL;
    if (grcore_codemeta_validate(&wide, wide.code_bytes, &why) != GRCORE_OK ||
        grcore_codemeta_validate(&many, many.code_bytes, &why) != GRCORE_OK) {
      fprintf(stderr, "bench: the code-metadata tables did not validate: %s\n",
          why != NULL ? why : "(no reason)");
      abort();
    }
    sink += META_WIDE_SITES + META_MANY_SITES;
  }
  return sink;
}

static const Case cases[] = {
    {"calibration", calibration_run, 200u * 1000u * 1000u, 1000u * 1000u},
    {"ctx-create", context_create_destroy_run, 1000u * 1000u, 1000u},
    {"count-malloc", counting_malloc_free_run, 20u * 1000u * 1000u, 10000u},
    {"keyed-lookup", keyed_lookup_run, 100u * 1000u * 1000u, 100000u},
    {"poll-fast", poll_fast_run, 200u * 1000u * 1000u, 100000u},
    {"poll-slow-4", poll_slow_run, 5u * 1000u * 1000u, 10000u},
    {"port-post", port_post_run, 10u * 1000u * 1000u, 10000u},
    {"stack-pushpop", stack_push_pop_run, 50u * 1000u * 1000u, 100000u},
    {"stack-poll", stack_poll_fast_run, 200u * 1000u * 1000u, 100000u},
    {"frame-walk", frame_walk_run, 20u * 1000u * 1000u, 10000u},
    {"activation", activation_pair_run, 50u * 1000u * 1000u, 100000u},
    {"fuel-scope", fuel_scope_run, 50u * 1000u * 1000u, 100000u},
    {"roots-16", roots_run, 50u * 1000u * 1000u, 10000u},
    {"snapshot-take", snapshot_take_run, 1u * 1000u * 1000u, 1000u},
    {"snapshot-restore", snapshot_restore_run, 2u * 1000u * 1000u, 1000u},
    {"profile-sample-1", profile_sample_1_run, 5u * 1000u * 1000u, 10000u},
    {"profile-sample-32", profile_sample_32_run, 2u * 1000u * 1000u, 10000u},
    {"codemeta-validate", codemeta_validate_run, 20u, 1u},
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
    printf("%-16s best %8.3f ns/op  median %8.3f ns/op  (%llu ops x %d, "
           "check %016llx)\n",
        cases[c].name, best, median, (unsigned long long)n, repeats,
        (unsigned long long)sink);
  }
  return 0;
}
