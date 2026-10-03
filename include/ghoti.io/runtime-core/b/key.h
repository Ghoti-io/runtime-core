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
 * @file key.h
 * @stability stable
 *
 * Static keys: how a service or an engine attaches to a context.
 *
 * A library defines its keys as static objects. A key's identity is its
 * address, so two libraries can never collide on a slot, and a context never
 * has to know what a key stands for (AD-1, AD-19).
 */

#ifndef GHOTI_IO_GRCORE_B_KEY_H
#define GHOTI_IO_GRCORE_B_KEY_H

#include <ghoti.io/runtime-core/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A context. Defined in context.h. */
typedef struct GRCORE_Context GRCORE_Context;

/**
 * @brief What a poll handler is given to read the poll and to vote with.
 *
 * Opaque. Valid only for the duration of the handler call. Its accessors are
 * in poll.h.
 */
typedef struct GRCORE_PollCall GRCORE_PollCall;

/**
 * @brief A poll handler: run by the poll in its key's phase (AD-5).
 *
 * @param context The context being polled, at-poll.
 * @param value The value this handler was registered with. NULL for the
 *   built-in keys.
 * @param call The poll in progress.
 */
typedef void (*GRCORE_PollHandler)(
    GRCORE_Context * context, void * value, GRCORE_PollCall * call);

/**
 * @brief The poll phase a registration belongs to.
 *
 * The phases are the poll's four fixed ones (AD-5). ::GRCORE_PHASE_NONE is
 * for a key that holds state and takes no part in the poll.
 */
typedef enum {
  GRCORE_PHASE_NONE = 0, ///< Holds state only.
  GRCORE_PHASE_DECIDE,   ///< Turns requests into verdicts.
  GRCORE_PHASE_ACT,      ///< Carries verdicts out.
  GRCORE_PHASE_OBSERVE,  ///< Watches; changes nothing.
  GRCORE_PHASE_YIELD     ///< Gives the host a chance to intervene.
} GRCORE_Phase;

/** @brief How many registrations one context accepts for one key. */
typedef enum {
  GRCORE_CARDINALITY_ONE = 0, ///< A second registration is refused.
  GRCORE_CARDINALITY_MANY     ///< Registrations accumulate, in order.
} GRCORE_Cardinality;

/**
 * @brief A key. Define it `static` (or `const`) and register it by address.
 *
 * The object must outlive every context that holds a registration under it.
 */
typedef struct GRCORE_Key {
  const char * name;             ///< For diagnostics. May be NULL.
  GRCORE_Cardinality cardinality; ///< Registrations accepted per context.
  GRCORE_Phase phase;            ///< The poll phase this key belongs to.
  /**
   * Releases one registered value. Called at teardown, in reverse
   * registration order, with the context still valid. May be NULL when the
   * value needs no release.
   */
  void (*destroy)(GRCORE_Context * context, void * value);
  /**
   * The handler the poll runs in `phase`, with the registered value. May be
   * NULL, and is then never run. A key whose phase is ::GRCORE_PHASE_NONE
   * is never polled whatever this holds.
   *
   * This field is last, so a static key written before it existed keeps
   * working: C zero-fills the omitted initialiser.
   */
  GRCORE_PollHandler poll;
} GRCORE_Key;

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_KEY_H */
