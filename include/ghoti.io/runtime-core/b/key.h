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

#include <ghoti.io/runtime-core/core.h>

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
 * @brief Where a snapshot hook writes its bytes. Opaque; valid only for the
 *   duration of the hook. Its calls are in snapshot.h.
 */
typedef struct GRCORE_SnapshotWriter GRCORE_SnapshotWriter;

/**
 * @brief Where a restore hook reads its bytes. Opaque; valid only for the
 *   duration of the hook. Its calls are in snapshot.h.
 */
typedef struct GRCORE_SnapshotReader GRCORE_SnapshotReader;

/** @brief What a `restore` hook is being asked to do. */
typedef enum {
  /** Validate only: say whether the blob fits this destination, change
   *  nothing. Run for every key before any key is applied. */
  GRCORE_RESTORE_CHECK = 0,
  /** Build the state. Whatever it builds is undone by an ABANDON settle. */
  GRCORE_RESTORE_APPLY
} GRCORE_RestoreMode;

/** @brief What a `settle` hook is being asked to do. */
typedef enum {
  /** Every key has applied: verify that COMMIT cannot fail, allocate whatever
   *  it needs, change nothing visible. May fail. */
  GRCORE_SETTLE_PREPARE = 0,
  /** Make the restored state final. Cannot fail. */
  GRCORE_SETTLE_COMMIT,
  /** Undo this key's APPLY, leaving the destination as it was fresh. Called
   *  only when the restore ended before any COMMIT, and only for a key whose
   *  APPLY succeeded. Cannot fail. */
  GRCORE_SETTLE_ABANDON
} GRCORE_SettleMode;

/**
 * @brief Writes a registration's state into a snapshot (AD-19).
 *
 * Called by `grcore_context_snapshot` with the context paused or parked
 * outside `run`. It must not change the context. The bytes it writes must
 * hold no host address: a heap reference is an index, a host function a name.
 *
 * @param context The context.
 * @param value The value this key was registered with.
 * @param writer Where the bytes go.
 * @return ::GRCORE_OK, or the refusal: ::GRCORE_ERR_INVALID for state that
 *   cannot be captured (the snapshot is then not made), ::GRCORE_ERR_OOM or
 *   ::GRCORE_ERR_LIMIT from the writer.
 */
typedef GRCORE_Result (*GRCORE_SnapshotHook)(
    GRCORE_Context * context, void * value, GRCORE_SnapshotWriter * writer);

/**
 * @brief Rebuilds a registration's state from a snapshot.
 *
 * Called twice by `grcore_context_restore` for each blob: once in
 * ::GRCORE_RESTORE_CHECK, which must change nothing, and then in
 * ::GRCORE_RESTORE_APPLY, in snapshot order. The reader starts at the blob's
 * first byte each time.
 *
 * @param context The destination context.
 * @param value The value this key is registered with in the destination.
 * @param reader The blob.
 * @param env What the host's environment lookup returned for this key's name.
 * @param mode CHECK or APPLY.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID when the destination does not
 *   match the blob (CHECK only, ideally); ::GRCORE_ERR_LIMIT or
 *   ::GRCORE_ERR_OOM when APPLY ran out of room, having released whatever it
 *   had built.
 */
typedef GRCORE_Result (*GRCORE_RestoreHook)(GRCORE_Context * context,
    void * value, GRCORE_SnapshotReader * reader, void * env,
    GRCORE_RestoreMode mode);

/**
 * @brief Finishes a restore, in the destination's registration order.
 *
 * Runs once every key has applied, so that state that refers across keys (the
 * heap writes root slots, which live in other keys' frames) can be completed.
 *
 * @param context The destination context.
 * @param value The value this key is registered with.
 * @param env What the host's environment lookup returned for this key's name.
 * @param mode PREPARE, COMMIT or ABANDON.
 * @return ::GRCORE_OK, or the refusal (PREPARE only).
 */
typedef GRCORE_Result (*GRCORE_SettleHook)(GRCORE_Context * context,
    void * value, void * env, GRCORE_SettleMode mode);

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
   * A static key written before this field existed keeps working: C
   * zero-fills the omitted initialiser.
   */
  GRCORE_PollHandler poll;
  /**
   * Writes the registration's state into a snapshot (snapshot.h). May be
   * NULL, and the key is then not part of any snapshot: the host registers it
   * again on the destination. A key with `snapshot` must also have `restore`
   * and `settle`, or taking a snapshot of a context that holds it is refused.
   * It must have cardinality one.
   *
   * The three snapshot fields are last, so a static key written before them
   * keeps working: C zero-fills the omitted initialisers.
   */
  GRCORE_SnapshotHook snapshot;
  /** Rebuilds the state on a destination context. See ::GRCORE_RestoreHook. */
  GRCORE_RestoreHook restore;
  /** Completes a restore across keys. See ::GRCORE_SettleHook. */
  GRCORE_SettleHook settle;
} GRCORE_Key;

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_KEY_H */
