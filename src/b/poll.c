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
 * The poll (AD-4, AD-5, AD-21).
 *
 * Votes are kept per registration index (the built-in kinds first), never in
 * the order handlers happened to run, and the winning key list and the unwind
 * result are read off that table afterwards in index order. That is what makes
 * the outcome independent of handler order, which the phase-shuffle mode
 * checks.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "context_internal.h"

#include <stdint.h>

#define CORE GRCORE_REQUEST_CORE_COUNT

struct GRCORE_PollCall {
  GRCORE_Context * context;
  GRCORE_Phase phase;
  GRCORE_Verdict verdict; ///< What handlers of later phases are told.
  bool reclaim;           ///< ACT handlers are asked to reclaim memory.
  size_t current;         ///< The vote slot of the handler being run.
  bool allow_pause;       ///< This poll may pause to the host.
};

GRCORE_Phase grcore_pollcall_phase(const GRCORE_PollCall * call) {
  return call == NULL ? GRCORE_PHASE_NONE : call->phase;
}

bool grcore_pollcall_pending(
    const GRCORE_PollCall * call, GRCORE_RequestKind kind) {
  return call != NULL && grcore_context_snapshot_pending(call->context, kind);
}

GRCORE_Verdict grcore_pollcall_verdict(const GRCORE_PollCall * call) {
  return call == NULL ? GRCORE_VERDICT_CONTINUE : call->verdict;
}

bool grcore_pollcall_pause_allowed(const GRCORE_PollCall * call) {
  return call != NULL && call->allow_pause;
}

bool grcore_pollcall_reclaim_requested(const GRCORE_PollCall * call) {
  return call != NULL && call->phase == GRCORE_PHASE_ACT && call->reclaim;
}

static bool voting(const GRCORE_PollCall * call) {
  return call != NULL &&
      (call->phase == GRCORE_PHASE_DECIDE || call->phase == GRCORE_PHASE_YIELD);
}

GRCORE_Result grcore_pollcall_vote(
    GRCORE_PollCall * call, GRCORE_Verdict verdict) {
  if (!voting(call) || (unsigned)verdict > GRCORE_VERDICT_UNWIND) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Vote * vote = &call->context->votes[call->current];
  if (verdict > vote->verdict) {
    vote->verdict = verdict;
  }
  return GRCORE_OK;
}

GRCORE_Result grcore_pollcall_set_unwind_result(
    GRCORE_PollCall * call, GRCORE_Result result) {
  if (!voting(call) ||
      (result != GRCORE_ERR_LIMIT && result != GRCORE_ERR_GUEST)) {
    return GRCORE_ERR_INVALID;
  }
  call->context->votes[call->current].result = result;
  return GRCORE_OK;
}

/* The built-in keys. Each turns one core request into a vote. */

static void decide_terminate(
    GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
  (void)context;
  (void)value;
  if (grcore_pollcall_pending(call, GRCORE_REQUEST_TERMINATE)) {
    grcore_pollcall_vote(call, GRCORE_VERDICT_UNWIND);
    grcore_pollcall_set_unwind_result(call, GRCORE_ERR_LIMIT);
  }
}

static void decide_time(
    GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
  (void)context;
  (void)value;
  if (grcore_pollcall_pending(call, GRCORE_REQUEST_TIME)) {
    grcore_pollcall_vote(call, GRCORE_VERDICT_PAUSE);
  }
}

static void decide_interrupt(
    GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
  (void)context;
  (void)value;
  if (grcore_pollcall_pending(call, GRCORE_REQUEST_INTERRUPT)) {
    grcore_pollcall_vote(call, GRCORE_VERDICT_PAUSE);
  }
}

static void decide_fuel(
    GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
  (void)value;
  if (!grcore_pollcall_pending(call, GRCORE_REQUEST_FUEL)) {
    return;
  }
  /* The ceiling beats a scope: when both are exhausted the host can raise the
   * ceiling, and the next poll then finds the scope still exhausted. Only a
   * scope that alone ran out, with the unwind policy, unwinds. */
  const GRCORE_FuelScope * scope = grcore_fuel_scope_innermost(context);
  if (grcore_fuel_ceiling_exhausted(context) || scope == NULL ||
      scope->policy != GRCORE_SCOPE_POLICY_UNWIND) {
    grcore_pollcall_vote(call, GRCORE_VERDICT_PAUSE);
    return;
  }
  grcore_pollcall_vote(call, GRCORE_VERDICT_UNWIND);
  grcore_pollcall_set_unwind_result(call, GRCORE_ERR_LIMIT);
  context->fuel_vote_scoped = true;
}

/* Memory defers its verdict by one poll: the first poll over budget lets ACT
 * handlers reclaim (a collector runs before the verdict, CAP-2), and only a
 * poll that finds the context still over after that votes. */
static void decide_memory(
    GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
  (void)value;
  if (context->reclaim_tried &&
      grcore_pollcall_pending(call, GRCORE_REQUEST_MEMORY)) {
    grcore_pollcall_vote(call, GRCORE_VERDICT_PAUSE);
  }
}

static const GRCORE_Key core_keys[CORE] = {
    GRCORE_KEY_INIT("terminate", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_DECIDE, NULL,
        decide_terminate, NULL, NULL, NULL),
    GRCORE_KEY_INIT("time", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_DECIDE, NULL, decide_time, NULL, NULL, NULL),
    GRCORE_KEY_INIT("interrupt", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_DECIDE, NULL,
        decide_interrupt, NULL, NULL, NULL),
    GRCORE_KEY_INIT("fuel", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_DECIDE, NULL, decide_fuel, NULL, NULL, NULL),
    GRCORE_KEY_INIT("memory", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_DECIDE, NULL,
        decide_memory, NULL, NULL, NULL),
};

const GRCORE_Key * grcore_core_key(GRCORE_RequestKind kind) {
  return kind < CORE ? &core_keys[kind] : NULL;
}

GRCORE_Result grcore_context_set_phase_shuffle(
    GRCORE_Context * context, bool enabled, uint64_t seed) {
  if (context == NULL || !grcore_context_owned_by_caller(context) ||
      context->tearing_down) {
    return GRCORE_ERR_INVALID;
  }
  context->shuffle = enabled;
  context->shuffle_state = seed;
  return GRCORE_OK;
}

/* splitmix64: a small generator with no bad seed, which is all a test mode
 * needs. */
static uint64_t next_random(GRCORE_Context * c) {
  uint64_t z = (c->shuffle_state += UINT64_C(0x9E3779B97F4A7C15));
  z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
  return z ^ (z >> 31);
}

/* Runs the handlers of `phase` over registrations [0, count), in registration
 * order or, when `shuffled` and the test mode is on, in a seeded random one.
 * Handlers are looked up afresh each time: one may register at-poll, which
 * can move the table. A handler registered during the phase is not run until
 * the next poll. */
static void run_phase(GRCORE_Context * c, GRCORE_PollCall * call,
    GRCORE_Phase phase, bool shuffled) {
  size_t count = c->registration_count;
  size_t n = 0;
  call->phase = phase;
  for (size_t i = 0; i < count; i++) {
    const GRCORE_Key * key = c->registrations[i].key;
    if (key->phase == phase && grcore_key_poll(key) != NULL) {
      c->order[n++] = i;
    }
  }
  if (shuffled && c->shuffle) {
    for (size_t i = n; i > 1; i--) {
      size_t j = (size_t)(next_random(c) % i);
      size_t t = c->order[i - 1];
      c->order[i - 1] = c->order[j];
      c->order[j] = t;
    }
  }
  for (size_t k = 0; k < n; k++) {
    size_t i = c->order[k];
    GRCORE_Registration reg = c->registrations[i];
    call->current = CORE + i;
    grcore_key_poll(reg.key)(c, reg.value, call);
  }
}

/* The strongest vote, and the keys that cast it. Returns the verdict and
 * leaves the key list and the unwind result set. */
static GRCORE_Verdict tally(GRCORE_Context * c, size_t registrations) {
  GRCORE_Verdict strongest = GRCORE_VERDICT_CONTINUE;
  for (size_t i = 0; i < CORE + registrations; i++) {
    if (c->votes[i].verdict > strongest) {
      strongest = c->votes[i].verdict;
    }
  }
  size_t n = 0;
  bool have_result = false;
  c->unwind_result = GRCORE_ERR_LIMIT;
  if (strongest != GRCORE_VERDICT_CONTINUE) {
    for (size_t i = 0; i < CORE + registrations; i++) {
      if (c->votes[i].verdict != strongest) {
        continue;
      }
      c->verdict_keys[n++] =
          i < CORE ? &core_keys[i] : c->registrations[i - CORE].key;
      if (strongest == GRCORE_VERDICT_UNWIND && !have_result) {
        c->unwind_result = c->votes[i].result;
        have_result = true;
      }
    }
  }
  c->verdict_key_count = n;
  return strongest;
}

static bool over_memory_budget(const GRCORE_Context * c) {
  uint64_t limit = grcore_counting_limit(&c->counting);
  return limit != GRCORE_UNLIMITED && grcore_meter_in_use(&c->meter) > limit;
}

static GRCORE_Verdict poll_slow(GRCORE_Context * c, GRCORE_Location location,
    bool allow_pause, GRCORE_Result * result) {
  if (!grcore_context_owned_by_caller(c) ||
      c->config != GRCORE_CONFIG_RUNNING) {
    /* Nothing is changed, except that the owner may read why. */
    if (grcore_context_owned_by_caller(c)) {
      c->unwind_result = GRCORE_ERR_INVALID;
    }
    *result = GRCORE_ERR_INVALID;
    return GRCORE_VERDICT_UNWIND;
  }
  grcore_context_transition(c, GRCORE_CONFIG_AT_POLL);
  grcore_context_refresh_derived(c);
  /* What every handler of this poll is told is pending, whatever an earlier
   * handler clears: the outcome must not depend on handler order. */
  grcore_context_snapshot_requests(c);
  c->fuel_vote_scoped = false;
  /* A pause cannot return to the host from inside a nested activation
   * (AD-5), so there it is refused exactly as the runtime poll refuses it. */
  allow_pause = allow_pause && c->nested == 0;

  size_t registrations = c->registration_count;
  for (size_t i = 0; i < CORE + registrations; i++) {
    c->votes[i].verdict = GRCORE_VERDICT_CONTINUE;
    c->votes[i].result = GRCORE_ERR_LIMIT;
  }
  bool over = over_memory_budget(c);
  if (!over) {
    c->reclaim_tried = false;
  }
  GRCORE_PollCall call = {c, GRCORE_PHASE_DECIDE, GRCORE_VERDICT_CONTINUE,
      over && !c->reclaim_tried, 0, allow_pause};

  /* 1. DECIDE: the built-ins in kind order, then the keyed handlers. */
  for (size_t k = 0; k < CORE; k++) {
    call.current = k;
    core_keys[k].poll(c, NULL, &call);
  }
  run_phase(c, &call, GRCORE_PHASE_DECIDE, true);
  GRCORE_Verdict verdict = tally(c, registrations);
  bool promoted = false;
  if (verdict == GRCORE_VERDICT_PAUSE && !allow_pause) {
    verdict = GRCORE_VERDICT_UNWIND;
    promoted = true;
  }
  call.verdict = verdict;

  /* 2. ACT. A collector that frees memory here is why the memory verdict
   * waits for the next poll. */
  run_phase(c, &call, GRCORE_PHASE_ACT, false);
  grcore_context_refresh_derived(c);
  if (!over_memory_budget(c)) {
    c->reclaim_tried = false;
  } else if (call.reclaim) {
    c->reclaim_tried = true;
  }

  /* 3. OBSERVE. */
  run_phase(c, &call, GRCORE_PHASE_OBSERVE, true);

  /* 4. YIELD, only when nothing has stopped the guest. */
  if (verdict == GRCORE_VERDICT_CONTINUE) {
    call.verdict = GRCORE_VERDICT_CONTINUE;
    run_phase(c, &call, GRCORE_PHASE_YIELD, false);
    verdict = tally(c, registrations);
    if (verdict == GRCORE_VERDICT_PAUSE && !allow_pause) {
      verdict = GRCORE_VERDICT_UNWIND;
      promoted = true;
    }
  }

  if (verdict == GRCORE_VERDICT_UNWIND && promoted) {
    c->unwind_result = GRCORE_ERR_LIMIT;
  }
  c->last_verdict = verdict;
  /* A scoped unwind: the exhausted scope alone, through the fuel key alone,
   * and not a pause that was refused. The engine unwinds to the scope's
   * boundary and closes it, which ends the unwind (budget.c). */
  c->scoped_unwind = 0;
  if (verdict == GRCORE_VERDICT_UNWIND && !promoted && c->fuel_vote_scoped &&
      c->verdict_key_count == 1 &&
      c->verdict_keys[0] == &core_keys[GRCORE_REQUEST_FUEL]) {
    c->scoped_unwind = grcore_context_fuel_scope_top(c);
  }
  if (verdict != GRCORE_VERDICT_CONTINUE) {
    c->pause_location = location;
  }
  *result = verdict == GRCORE_VERDICT_UNWIND ? c->unwind_result : GRCORE_OK;
  if (verdict != GRCORE_VERDICT_PAUSE) {
    grcore_context_transition(c, GRCORE_CONFIG_RUNNING);
  }
  return verdict;
}

GRCORE_Verdict grcore_poll(GRCORE_Context * context, GRCORE_Location location) {
  /* The fast path: one load and one branch. */
  if (__builtin_expect(
          __atomic_load_n(&context->request_word, __ATOMIC_RELAXED) == 0, 1)) {
    return GRCORE_VERDICT_CONTINUE;
  }
  GRCORE_Result ignored;
  return poll_slow(context, location, true, &ignored);
}

GRCORE_Result grcore_runtime_poll(
    GRCORE_Context * context, uint64_t work, GRCORE_Location location) {
  if (context == NULL || !grcore_context_owned_by_caller(context)) {
    return GRCORE_ERR_INVALID;
  }
  grcore_context_charge_fuel(context, work);
  if (__atomic_load_n(&context->request_word, __ATOMIC_RELAXED) == 0) {
    return GRCORE_OK;
  }
  GRCORE_Result result;
  poll_slow(context, location, false, &result);
  return result;
}

GRCORE_Result grcore_context_nested_enter(GRCORE_Context * context) {
  if (context == NULL || !grcore_context_owned_by_caller(context) ||
      context->tearing_down || context->nested == UINT64_MAX) {
    return GRCORE_ERR_INVALID;
  }
  context->nested++;
  return GRCORE_OK;
}

GRCORE_Result grcore_context_nested_leave(GRCORE_Context * context) {
  if (context == NULL || !grcore_context_owned_by_caller(context) ||
      context->nested == 0) {
    return GRCORE_ERR_INVALID;
  }
  context->nested--;
  return GRCORE_OK;
}

uint64_t grcore_context_nested_depth(const GRCORE_Context * context) {
  return context == NULL ? 0 : context->nested;
}
