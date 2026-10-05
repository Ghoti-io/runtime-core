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

#include <stdbool.h>
#include <stddef.h>

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
 * @brief A key. Define it `static` (or `const`) with ::GRCORE_KEY_INIT and
 *   register it by address.
 *
 * The object must outlive every context that holds a registration under it.
 *
 * **The struct grows at the end, and `size` says how far.** Every key is
 * written by a library other than this one, as a static initialiser, so the
 * layout is part of the contract. The first member, `size`, is the
 * `sizeof(GRCORE_Key)` of the header the key was compiled against, and
 * ::GRCORE_KEY_INIT writes it, so a definition cannot forget it. The core
 * reads a member after `destroy` only when `size` reaches the end of that
 * member, and takes an absent one as NULL. A key built before a field existed
 * therefore stays valid, and is distinguishable from one with garbage in the
 * new field, which a trailing marker could not be (code built before the
 * marker never wrote it).
 *
 * Registration (::grcore_context_register, ::grcore_context_request_kind)
 * refuses with ::GRCORE_ERR_INVALID a key whose `size` is below
 * ::GRCORE_KEY_MIN_SIZE (name, cardinality, phase and destroy are always
 * read, so they must exist) or is not a multiple of the struct's alignment.
 * A `size` larger than this header's struct is accepted: the key is from a
 * newer header, and members this core does not know are ignored, so each
 * future member must be one whose absence is a defined, safe degradation
 * (a hook not run), never a requirement.
 *
 * Rejected alternatives (design.md has the argument): a trailing size or
 * version number (code built before it was added never wrote it, so it is
 * indistinguishable from garbage); a version number (it has to be bumped by
 * hand, and says nothing about which fields exist); a vtable pointer (an
 * indirection and a second static object per key, and the address of the
 * key, its identity, would no longer be the object that holds the data);
 * designated initialisers only (nothing makes a caller write `.size`, C++20
 * requires declaration order and forbids mixing, and an omitted size is only
 * caught at run time).
 */
typedef struct GRCORE_Key {
  /**
   * `sizeof(GRCORE_Key)` as the key's definer compiled it. Set by
   * ::GRCORE_KEY_INIT. Never write it by hand.
   */
  size_t size;
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
   * is never polled whatever this holds. Absent (and so NULL) in a key whose
   * `size` ends before it.
   */
  GRCORE_PollHandler poll;
  /**
   * Writes the registration's state into a snapshot (snapshot.h). May be
   * NULL, and the key is then not part of any snapshot: the host registers it
   * again on the destination. A key with `snapshot` must also have `restore`
   * and `settle`, or taking a snapshot of a context that holds it is refused.
   * It must have cardinality one. Absent in a key whose `size` ends before it.
   */
  GRCORE_SnapshotHook snapshot;
  /** Rebuilds the state on a destination context. See ::GRCORE_RestoreHook. */
  GRCORE_RestoreHook restore;
  /** Completes a restore across keys. See ::GRCORE_SettleHook. */
  GRCORE_SettleHook settle;
} GRCORE_Key;

/**
 * @brief The smallest `size` a key may state: the layout through `destroy`.
 *
 * It is the layout of the first generation, before any trailing hook existed.
 */
#define GRCORE_KEY_MIN_SIZE \
  (offsetof(GRCORE_Key, destroy) + sizeof(((GRCORE_Key *)0)->destroy))

/**
 * @brief The initialiser of a key: `GRCORE_KEY_INIT(name, cardinality, phase,
 *   destroy, poll, snapshot, restore, settle)`.
 *
 * Expands to a braced initialiser that begins with `sizeof(GRCORE_Key)`, so
 * it is a constant expression in a static initialiser in C17 and C++20, and
 * the members given are the ones after `size`, positionally (a C file may use
 * designators instead). Give every member, NULL where unused: an omitted one
 * is zero, but a `-Wextra` C++ compile warns about it, which is what makes
 * the next field added to this struct visible at each definition.
 * @code
 * static const GRCORE_Key my_key = GRCORE_KEY_INIT("my", GRCORE_CARDINALITY_ONE,
 *     GRCORE_PHASE_NONE, my_destroy, NULL, NULL, NULL, NULL);
 * @endcode
 * A definition written before `size` existed starts with a string where this
 * has a `size_t`, and does not compile; that is the intended break.
 */
#define GRCORE_KEY_INIT(...) { sizeof(GRCORE_Key), __VA_ARGS__ }

/**
 * @brief Whether a key's `size` covers a member, so that reading it is
 *   defined.
 *
 * @code
 * GRCORE_KEY_HAS(key, poll) && key->poll != NULL
 * @endcode
 */
#define GRCORE_KEY_HAS(key, member)                                         \
  ((key)->size >= offsetof(GRCORE_Key, member) + sizeof((key)->member))

/**
 * @brief Whether a key is acceptable to register (see ::GRCORE_Key).
 *
 * @param key The key. NULL is not valid.
 * @return true when `size` is at least ::GRCORE_KEY_MIN_SIZE and a multiple
 *   of the struct's alignment.
 */
GRCORE_API bool grcore_key_valid(const GRCORE_Key * key);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_KEY_H */
