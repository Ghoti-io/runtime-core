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
 * `run`, `resume` and `wait` (AD-13, AD-20, AD-21).
 *
 * `run` owns the lifecycle edges at its two ends and nothing in between: the
 * entry function polls, and the poll moves the context through AT_POLL. When
 * the entry returns, `run` checks what it reports against what the poll
 * decided, because an entry that pauses without a pause verdict has left the
 * context in a state no later call can make sense of.
 */

#include <ghoti.io/runtime-core/macros.h>

#include "context_internal.h"

#include <stdint.h>

/* The start of a run or a resume: what the last one reported is stale. */
static void begin_run(GRCORE_Context * c) {
  c->verdict_key_count = 0;
  c->pause_location.file = NULL;
  c->pause_location.line = 0;
  c->unwind_result = GRCORE_OK;
  c->last_verdict = GRCORE_VERDICT_CONTINUE;
  c->scoped_unwind = 0;
}

/* The end of a run that is not a pause: the context is parked outside `run`
 * and terminate, which stays pending until `run` returns (AD-21), is done. */
static void end_run(GRCORE_Context * c) {
  c->config = GRCORE_CONFIG_PARKED_OUTSIDE;
  c->entry = NULL;
  c->entry_state = NULL;
  grcore_context_clear_terminate(c);
}

static GRCORE_Result entry_lied(GRCORE_Context * c) {
  end_run(c);
  return GRCORE_ERR_INTERNAL;
}

static GRCORE_Result settle(
    GRCORE_Context * c, GRCORE_Step step, GRCORE_Outcome * out_outcome) {
  switch (step) {
    case GRCORE_STEP_FINISHED:
      /* Finishing after an unwind verdict would swallow terminate. */
      if (c->config != GRCORE_CONFIG_RUNNING ||
          c->last_verdict == GRCORE_VERDICT_UNWIND) {
        return entry_lied(c);
      }
      end_run(c);
      *out_outcome = GRCORE_OUTCOME_FINISHED;
      return GRCORE_OK;
    case GRCORE_STEP_PAUSED:
      if (c->config != GRCORE_CONFIG_AT_POLL ||
          c->last_verdict != GRCORE_VERDICT_PAUSE ||
          grcore_context_transition(c, GRCORE_CONFIG_PAUSED) != GRCORE_OK) {
        return entry_lied(c);
      }
      *out_outcome = GRCORE_OUTCOME_PAUSED;
      return GRCORE_OK;
    case GRCORE_STEP_UNWOUND:
      if (c->config != GRCORE_CONFIG_RUNNING ||
          c->last_verdict != GRCORE_VERDICT_UNWIND) {
        return entry_lied(c);
      }
      {
        GRCORE_Result reason = c->unwind_result;
        end_run(c);
        return reason;
      }
  }
  return entry_lied(c);
}

GRCORE_Result grcore_run(GRCORE_Context * context, GRCORE_EntryFn entry,
    void * state, GRCORE_Outcome * out_outcome) {
  if (context == NULL || entry == NULL || out_outcome == NULL ||
      !grcore_context_owned_by_caller(context) || context->tearing_down ||
      context->config != GRCORE_CONFIG_PARKED_OUTSIDE) {
    return GRCORE_ERR_INVALID;
  }
  context->entry = entry;
  context->entry_state = state;
  begin_run(context);
  grcore_context_transition(context, GRCORE_CONFIG_RUNNING);
  return settle(context, entry(context, state), out_outcome);
}

GRCORE_Result grcore_resume(
    GRCORE_Context * context, GRCORE_Outcome * out_outcome) {
  if (context == NULL || out_outcome == NULL ||
      !grcore_context_owned_by_caller(context) || context->tearing_down ||
      context->config != GRCORE_CONFIG_PAUSED || context->entry == NULL) {
    return GRCORE_ERR_INVALID;
  }
  /* The host has now seen the time and the interrupt. Anything still true,
   * such as exhausted fuel, is a level and is decided again at the next
   * poll. */
  grcore_context_clear_edge_requests(context);
  begin_run(context);
  grcore_context_transition(context, GRCORE_CONFIG_RUNNING);
  return settle(context, context->entry(context, context->entry_state),
      out_outcome);
}

GRCORE_Result grcore_context_wait(
    GRCORE_Context * context, uint64_t timeout_ns, bool * out_woken) {
  if (context == NULL || out_woken == NULL ||
      !grcore_context_owned_by_caller(context) || context->tearing_down ||
      context->config != GRCORE_CONFIG_RUNNING) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Port * port;
  GRCORE_Result r = grcore_context_port_ensure(context, &port);
  if (r != GRCORE_OK) {
    return r;
  }
  /* A derived bit left stale by a freed block or a raised budget would end
   * the wait at once. */
  grcore_context_refresh_derived(context);
  if (__atomic_load_n(&context->request_word, __ATOMIC_ACQUIRE) != 0) {
    *out_woken = true;
    return GRCORE_OK;
  }
  grcore_context_park(context);
  bool woken = grcore_port_wait(port, context, timeout_ns);
  grcore_context_unpark(context);
  *out_woken = woken;
  return GRCORE_OK;
}

size_t grcore_context_pause_key_count(const GRCORE_Context * context) {
  return context == NULL ? 0 : context->verdict_key_count;
}

const GRCORE_Key * grcore_context_pause_key(
    const GRCORE_Context * context, size_t index) {
  if (context == NULL || index >= context->verdict_key_count) {
    return NULL;
  }
  return context->verdict_keys[index];
}

GRCORE_Location grcore_context_pause_location(const GRCORE_Context * context) {
  GRCORE_Location none = {NULL, 0};
  return context == NULL ? none : context->pause_location;
}

GRCORE_Result grcore_context_unwind_result(const GRCORE_Context * context) {
  return context == NULL ? GRCORE_OK : context->unwind_result;
}
