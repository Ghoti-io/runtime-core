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
 * A host reads a paused guest's frames without knowing the engine.
 *
 * There is no real engine here. The host defines a descriptor (what a slot
 * is, where a poll identity is in the source, how a value prints, which
 * variables a frame has), registers it, and runs a guest that pushes three
 * nested frames on the context's guest stack and polls in the innermost one
 * under a small fuel budget. When the poll pauses, the host walks the frames
 * with the one reader every consumer shares (the abstract frame): the same
 * calls would read a frame of any other engine. It prints each frame's
 * function, location, slots and scope variables, checks what it printed,
 * raises the budget and resumes to completion.
 *
 * Why the guest keeps its position on the guest stack, and not in C frames:
 * a pause returns to the host, and the host can then read, and even move,
 * the context.
 *
 * Build and run with `make examples`.
 */

#include <ghoti.io/runtime-core/runtime-core.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WORK_STEPS 20u
#define FUEL 8u

/* Checked in every build: an example that asserts nothing under NDEBUG is
 * not an example of anything. */
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "frame_walk: check failed at line %d: %s\n", __LINE__,   \
          #condition);                                                         \
      return 1;                                                                \
    }                                                                          \
  } while (0)

/* ---- The engine's side: a descriptor --------------------------------- */

/* Each toy frame has two slots: slot 0 is an argument (plain bits) and slot 1
 * is the running total (an engine value). */
static GRCORE_SlotKind toy_slot_kind(
    const GRCORE_AbstractFrame * frame, size_t index) {
  (void)frame;
  return index == 0 ? GRCORE_SLOT_RAW : GRCORE_SLOT_VALUE;
}

/* A poll identity is (function, step); the toy "source" has one line per step
 * of each function. */
static GRCORE_Location toy_locate(
    const GRCORE_Context * context, uint64_t function, uint64_t offset) {
  (void)context;
  GRCORE_Location where = {"toy.src", (int)(function * 100 + offset)};
  return where;
}

static size_t toy_inspect(const GRCORE_Context * context, GRCORE_SlotKind kind,
    uint64_t value, char * buffer, size_t size) {
  (void)context;
  int n = snprintf(buffer, size, kind == GRCORE_SLOT_VALUE ? "total=%llu" : "%llu",
      (unsigned long long)value);
  return n < 0 ? 0 : (size_t)n;
}

/* One scope, "locals", whose two variables are the frame's two slots. */
static size_t toy_scope_count(const GRCORE_AbstractFrame * frame) {
  (void)frame;
  return 1;
}

static GRCORE_Result toy_scope(const GRCORE_AbstractFrame * frame, size_t index,
    GRCORE_ScopeInfo * out_scope) {
  (void)frame;
  if (index != 0) {
    return GRCORE_ERR_INVALID;
  }
  out_scope->kind = GRCORE_SCOPE_LOCAL;
  out_scope->name = "locals";
  out_scope->variable_count = 2;
  return GRCORE_OK;
}

static GRCORE_Result toy_variable(const GRCORE_AbstractFrame * frame,
    size_t scope, size_t index, GRCORE_Variable * out_variable) {
  static const char * const names[2] = {"arg", "total"};
  uint64_t value;
  if (scope != 0 || index >= 2 ||
      grcore_stack_slot_get(grcore_context_stack(frame->context), frame->frame,
          index, &value) != GRCORE_OK) {
    return GRCORE_ERR_INVALID;
  }
  out_variable->name = names[index];
  out_variable->kind = toy_slot_kind(frame, index);
  out_variable->value = value;
  return GRCORE_OK;
}

static const GRCORE_EngineDescriptor toy_engine = GRCORE_ENGINE_DESCRIPTOR_INIT("toy", toy_slot_kind,
    toy_locate, toy_inspect, {toy_scope_count, toy_scope, toy_variable},
    {0, 0, 0}, NULL, NULL);

/* ---- The guest -------------------------------------------------------- */

typedef struct {
  GRCORE_EngineId engine;
  int pushed;
  unsigned step;
  uint64_t total;
  GRCORE_Result failure; /* why a push stopped the guest, if one did */
} Toy;

static GRCORE_Step toy_entry(GRCORE_Context * context, void * state) {
  Toy * toy = state;
  GRCORE_Stack * stack = grcore_context_stack(context);
  if (!toy->pushed) {
    /* Three nested calls: function 1 calls 2 calls 3. Each outer frame records
     * where it called from, so a walk can say where it is. */
    for (uint64_t function = 1; function <= 3; function++) {
      GRCORE_FrameRef frame;
      toy->failure = grcore_stack_push(stack, toy->engine, 2, &frame);
      if (toy->failure != GRCORE_OK) {
        return GRCORE_STEP_FINISHED; /* the host reads `failure` */
      }
      grcore_stack_slot_set(stack, frame, 0, function * 10);
      if (function < 3) {
        GRCORE_PollIdentity call_site = {function, function};
        grcore_stack_set_identity(stack, frame, call_site);
      }
    }
    toy->pushed = 1;
  }
  while (toy->step < WORK_STEPS) {
    grcore_context_charge_fuel(context, 1);
    /* The poll names this site: function 3, this step. A frame's address would
     * not survive a push, but (function, offset) is the same on every tier. */
    GRCORE_Verdict verdict = grcore_stack_poll(context, 3, toy->step);
    if (verdict == GRCORE_VERDICT_PAUSE) {
      return GRCORE_STEP_PAUSED; /* the position is on the stack and in `toy` */
    }
    if (verdict == GRCORE_VERDICT_UNWIND) {
      return GRCORE_STEP_UNWOUND;
    }
    toy->total += toy->step;
    toy->step++;
    /* Reload the top frame each time: nothing raw is kept across a poll. */
    grcore_stack_slot_set(stack, grcore_stack_top(stack), 1, toy->total);
  }
  while (grcore_stack_frame_count(stack) > 0) {
    grcore_stack_pop(stack);
  }
  return GRCORE_STEP_FINISHED;
}

/* ---- The host's side -------------------------------------------------- */

int main(void) {
  GRCORE_Options * options;
  GRCORE_Group * group;
  GRCORE_Context * context;
  CHECK(grcore_options_create(NULL, &options) == GRCORE_OK);
  CHECK(grcore_options_set_fuel(options, FUEL) == GRCORE_OK);
  CHECK(grcore_group_create(NULL, NULL, &group) == GRCORE_OK);
  CHECK(grcore_context_create(group, options, &context) == GRCORE_OK);
  grcore_options_destroy(options);

  Toy toy = {0, 0, 0, 0, GRCORE_OK};
  CHECK(grcore_engine_register(context, &toy_engine, &toy.engine) == GRCORE_OK);

  GRCORE_Outcome outcome;
  CHECK(grcore_run(context, toy_entry, &toy, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_PAUSED);
  CHECK(toy.failure == GRCORE_OK);

  GRCORE_Location where = grcore_context_pause_location(context);
  printf("paused at %s:%d\n", where.file, where.line);
  CHECK(strcmp(where.file, "toy.src") == 0);
  CHECK(where.line == 300 + (int)FUEL); /* the ninth charge, at step 8 */

  /* Walk the frames, innermost first. Nothing here is specific to the toy
   * engine: a debugger or a collector would write exactly this. */
  GRCORE_FrameWalk walk;
  GRCORE_AbstractFrame frame;
  CHECK(grcore_frame_walk_begin(context, &walk) == GRCORE_OK);
  size_t frames = 0;
  while (grcore_frame_walk_next(&walk, &frame)) {
    printf("frame %zu: engine %s, function %llu at %s:%d\n", frame.depth,
        frame.descriptor->name, (unsigned long long)frame.identity.function,
        frame.location.file, frame.location.line);
    for (size_t i = 0; i < frame.slot_count; i++) {
      GRCORE_SlotKind kind;
      uint64_t value;
      char text[32];
      size_t length;
      CHECK(grcore_frame_slot(&frame, i, &kind, &value) == GRCORE_OK);
      CHECK(grcore_frame_inspect(&frame, i, text, sizeof text, &length) ==
          GRCORE_OK);
      printf("  slot %zu (%s): %s\n", i, kind == GRCORE_SLOT_RAW ? "raw" : "value",
          text);
    }
    for (size_t s = 0; s < grcore_frame_scope_count(&frame); s++) {
      GRCORE_ScopeInfo scope;
      CHECK(grcore_frame_scope(&frame, s, &scope) == GRCORE_OK);
      for (size_t v = 0; v < scope.variable_count; v++) {
        GRCORE_Variable variable;
        CHECK(grcore_frame_variable(&frame, s, v, &variable) == GRCORE_OK);
        printf("  scope %s: %s = %llu\n", scope.name, variable.name,
            (unsigned long long)variable.value);
      }
    }
    /* What the toy engine declared is what every consumer reads. */
    CHECK(frame.depth == frames);
    CHECK(frame.descriptor == &toy_engine);
    CHECK(frame.slot_count == 2);
    CHECK(frame.identity.function == (frames == 0 ? 3 : 3 - frames));
    CHECK(frame.location.line ==
        (int)(frame.identity.function * 100 + frame.identity.offset));
    uint64_t arg;
    CHECK(grcore_frame_slot(&frame, 0, NULL, &arg) == GRCORE_OK);
    CHECK(arg == (3 - frames) * 10);
    frames++;
  }
  CHECK(frames == 3);

  /* The innermost frame carries the running total the guest had reached. */
  CHECK(grcore_frame_walk_begin(context, &walk) == GRCORE_OK);
  CHECK(grcore_frame_walk_next(&walk, &frame));
  uint64_t total;
  CHECK(grcore_frame_slot(&frame, 1, NULL, &total) == GRCORE_OK);
  CHECK(total == 28); /* 0 + 1 + ... + 7 */

  /* Raise the budget and run to the end. The answer is the one an
   * uninterrupted run gives. */
  CHECK(grcore_context_set_fuel(context, GRCORE_UNLIMITED) == GRCORE_OK);
  CHECK(grcore_resume(context, &outcome) == GRCORE_OK);
  CHECK(outcome == GRCORE_OUTCOME_FINISHED);
  CHECK(toy.failure == GRCORE_OK);
  CHECK(toy.total == 190); /* 0 + 1 + ... + 19 */
  CHECK(grcore_stack_frame_count(grcore_context_stack(context)) == 0);
  printf("finished: total %llu\n", (unsigned long long)toy.total);

  CHECK(grcore_context_destroy(context) == GRCORE_OK);
  CHECK(grcore_group_destroy(group) == GRCORE_OK);
  return 0;
}
