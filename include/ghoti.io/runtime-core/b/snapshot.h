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
 * @file snapshot.h
 * @stability stable
 *
 * Context snapshots: a frozen, shareable image of a paused or idle context
 * (AD-12, AD-19, AD-20; the spine's Reserved Seams).
 *
 * A snapshot is *data*, not memory. It is a list of named byte blobs, one per
 * registered key that has snapshot hooks (b/key.h), written by the keys
 * themselves; the core does not know what is in them. It holds no host
 * pointer: every reference a key writes is an index or a name. It is
 * immutable once made, reference-counted with an atomic count, and may be
 * restored any number of times, into contexts on any thread, concurrently,
 * and may outlive the context it came from. It is an in-memory object: there
 * is no byte format and no file (recorded as not done in design.md).
 *
 * **When one may be taken (AD-20).** Only by the owning thread, and only when
 * the context is paused or parked outside `run` with no budget scope open and
 * no nested activation. A key may refuse for state of its own (the guest
 * stack refuses with an activation record open; the heap with a weak cell, a
 * C root, a handle, a pin or a conservative range). Every refusal is
 * ::GRCORE_ERR_INVALID and changes nothing.
 *
 * **Restoring.** The host creates a destination context as it would for a
 * fresh run, registers the same keys, then calls ::grcore_context_restore.
 * The keys' `restore` hooks run in two passes in snapshot order (a CHECK that
 * changes nothing, then an APPLY), then every `settle` hook runs in the
 * destination's registration order in three modes (PREPARE, COMMIT, and ABANDON
 * for a restore that failed before COMMIT).
 * Any failure before COMMIT undoes every APPLY and leaves the destination
 * exactly as it was: a fresh, runnable context. A snapshot of a paused context
 * leaves the destination paused, resumable with ::grcore_resume.
 *
 * What the core itself captures: only whether the context was paused and where
 * (the line). What the host supplies again on the destination, as for a
 * fresh run: the group, the options and budgets (fuel used starts at zero;
 * the limits are the destination's), the page provider and allocator, and
 * every key that has no hooks.
 *
 * Threads: taking and restoring are the owner's, like every context call. A
 * snapshot's retain, release and readers are safe from any thread.
 */

#ifndef GHOTI_IO_GRCORE_B_SNAPSHOT_H
#define GHOTI_IO_GRCORE_B_SNAPSHOT_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/b/run.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An immutable, reference-counted image of a context. Opaque. */
typedef struct GRCORE_Snapshot GRCORE_Snapshot;

/** @brief The most bytes one snapshot holds in all its blobs together: 2^40 where
 *   `size_t` is 64 bits, half the address space where it is 32. */
#if SIZE_MAX > UINT32_MAX
#define GRCORE_SNAPSHOT_MAX_BYTES (SIZE_MAX >> 24)
#else
#define GRCORE_SNAPSHOT_MAX_BYTES (SIZE_MAX >> 1)
#endif

/**
 * @brief What the host supplies to a restore. Define it with
 *   ::GRCORE_RESTORE_ENV_INIT.
 *
 * All fields are optional except `entry` for a snapshot of a paused context.
 *
 * **The struct grows at the end, and `size` says how far.** The rule is
 * ::GRCORE_Key's (b/key.h has the argument and the rejected alternatives), and
 * it matters here because the struct has grown once already (`entry`,
 * `entry_state` and `pause_file` came after `lookup`) and a host fills it at
 * run time, where C zero-fills an omitted member only in an initialiser. The
 * first member, `size`, is the `sizeof(GRCORE_RestoreEnv)` of the header the
 * host compiled against, ::GRCORE_RESTORE_ENV_INIT writes it, and a member
 * after `lookup` is read only where `size` covers it
 * (::GRCORE_RESTORE_ENV_HAS), an absent one being NULL. A struct filled by
 * assignment starts from ::GRCORE_RESTORE_ENV_INIT too. ::grcore_context_restore
 * refuses with ::GRCORE_ERR_INVALID an environment that
 * ::grcore_restore_env_valid does not accept (NULL stays the host having none);
 * a larger `size` is accepted and the unknown tail ignored.
 */
typedef struct GRCORE_RestoreEnv {
  /**
   * `sizeof(GRCORE_RestoreEnv)` as the host compiled it. Set by
   * ::GRCORE_RESTORE_ENV_INIT. Never write it by hand.
   */
  size_t size;
  void * user; ///< Handed to `lookup`.
  /**
   * The environment of the key called `key_name`, handed to its `restore` and
   * `settle` hooks as `env`. May be NULL, and every hook then gets NULL. Must
   * not allocate in the context.
   */
  void * (*lookup)(void * user, const char * key_name);
  /** The entry function a paused context resumes with (the engine's). Absent
   *  (and so NULL) in an environment whose `size` ends before it. */
  GRCORE_EntryFn entry;
  void * entry_state; ///< Handed to `entry`. Absent as `entry` is.
  /** The file of the pause location, borrowed; the snapshot has only the line.
   *  Absent as `entry` is. */
  const char * pause_file;
} GRCORE_RestoreEnv;

/** @brief The smallest `size` an environment may state: the layout through
 *   `lookup`, which is the layout before `entry` existed. */
#define GRCORE_RESTORE_ENV_MIN_SIZE                                         \
  (offsetof(GRCORE_RestoreEnv, lookup) +                                    \
      sizeof(((GRCORE_RestoreEnv *)0)->lookup))

/** @brief The initialiser of a restore environment:
 *   `GRCORE_RESTORE_ENV_INIT(user, lookup, entry, entry_state, pause_file)`.
 *   Begins with `sizeof(GRCORE_RestoreEnv)`; `GRCORE_RESTORE_ENV_INIT(NULL,
 *   NULL, NULL, NULL, NULL)` is the starting point of one filled by assignment. */
#define GRCORE_RESTORE_ENV_INIT(...) { sizeof(GRCORE_RestoreEnv), __VA_ARGS__ }

/** @brief Whether an environment's `size` covers a member, so that reading it
 *   is defined. */
#define GRCORE_RESTORE_ENV_HAS(env, member)                                 \
  ((env)->size >= offsetof(GRCORE_RestoreEnv, member) + sizeof((env)->member))

/**
 * @brief Whether a restore environment is acceptable (see
 *   ::GRCORE_RestoreEnv).
 *
 * @param env The environment. NULL is not valid here, though
 *   ::grcore_context_restore takes NULL as "none".
 * @return true when `size` is at least ::GRCORE_RESTORE_ENV_MIN_SIZE and a
 *   multiple of the struct's alignment.
 */
GRCORE_API bool grcore_restore_env_valid(const GRCORE_RestoreEnv * env);

/**
 * @brief Takes a snapshot of a context.
 *
 * @param context The context. The caller must own it, and it must be paused
 *   or parked outside `run`, with no budget scope or nested activation.
 * @param allocator Allocates the snapshot, which keeps a copy of it and frees
 *   with it, so it need not outlive the call; NULL means the default. It is
 *   not the context's: a snapshot may outlive the context.
 * @param out_snapshot Receives the snapshot with a count of one. Written only
 *   on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for a NULL argument, a non-owner,
 *   a context in any other state, a key that refuses (see the keys' hooks), a
 *   key whose hooks are incomplete or whose name is NULL or duplicated; and
 *   ::GRCORE_ERR_LIMIT when the blobs exceed ::GRCORE_SNAPSHOT_MAX_BYTES;
 *   ::GRCORE_ERR_OOM for an allocation failure. A refusal changes nothing and
 *   leaves nothing allocated.
 */
GRCORE_API GRCORE_Result grcore_context_snapshot(GRCORE_Context * context,
    const GRCORE_Allocator * allocator, GRCORE_Snapshot ** out_snapshot);

/**
 * @brief Restores a snapshot into a fresh context.
 *
 * @param context The destination: owned by the caller, parked outside `run`
 *   with the same keys registered as the source had hooks for, and otherwise
 *   fresh (every key's own checks say what fresh means to it).
 * @param snapshot The snapshot.
 * @param env The host's environment, or NULL.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for a NULL argument, an invalid
 *   environment (`size`), a non-owner,
 *   a destination in the wrong state, a key set that differs from the
 *   snapshot's (a blob with no key, or a key with hooks and no blob), a paused
 *   snapshot with no `entry`, or a destination a key's CHECK refuses;
 *   ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_OOM when the destination's budget or
 *   allocator ran out in an APPLY. On any failure the destination is as it
 *   was before the call.
 */
GRCORE_API GRCORE_Result grcore_context_restore(GRCORE_Context * context,
    const GRCORE_Snapshot * snapshot, const GRCORE_RestoreEnv * env);

/**
 * @brief Adds a reference. Safe from any thread.
 *
 * @return `snapshot`, or NULL for NULL (a no-op).
 */
GRCORE_API GRCORE_Snapshot * grcore_snapshot_retain(GRCORE_Snapshot * snapshot);

/**
 * @brief Drops a reference; at zero, frees the snapshot. Safe from any thread.
 *   NULL is a no-op.
 */
GRCORE_API void grcore_snapshot_release(GRCORE_Snapshot * snapshot);

/** @brief The current count (a reading; another thread may change it). */
GRCORE_API size_t grcore_snapshot_refcount(const GRCORE_Snapshot * snapshot);

/** @brief The bytes held in all the blobs; zero for NULL. */
GRCORE_API size_t grcore_snapshot_size(const GRCORE_Snapshot * snapshot);

/** @brief Whether the source context was paused; false for NULL. */
GRCORE_API bool grcore_snapshot_was_paused(const GRCORE_Snapshot * snapshot);

/** @brief How many blobs (keys with hooks) the snapshot holds. */
GRCORE_API size_t grcore_snapshot_blob_count(const GRCORE_Snapshot * snapshot);

/**
 * @brief The name of blob `index` (its key's name), in snapshot order.
 *
 * @return The name, valid as long as the snapshot; NULL when out of range.
 */
GRCORE_API const char * grcore_snapshot_blob_name(
    const GRCORE_Snapshot * snapshot, size_t index);

/**
 * @brief The bytes of the blob called `name`, for a test or a tool that wants
 *   to prove what a snapshot holds (for example that it holds no address).
 *
 * @param out_data Receives a pointer valid as long as the snapshot.
 * @param out_size Receives the length.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for no such blob.
 */
GRCORE_API GRCORE_Result grcore_snapshot_blob(const GRCORE_Snapshot * snapshot,
    const char * name, const void ** out_data, size_t * out_size);

/* ---- for a key's hooks ------------------------------------------------- */

/**
 * @brief Appends bytes to the blob being written.
 *
 * @return ::GRCORE_OK; ::GRCORE_ERR_LIMIT past ::GRCORE_SNAPSHOT_MAX_BYTES;
 *   ::GRCORE_ERR_OOM; ::GRCORE_ERR_INVALID for NULL (with `size` non-zero).
 *   After a failure the hook should return it unchanged.
 */
GRCORE_API GRCORE_Result grcore_snapshot_writer_write(
    GRCORE_SnapshotWriter * writer, const void * data, size_t size);

/** @brief Appends one 64-bit value (native byte order: the snapshot is not a file). */
GRCORE_API GRCORE_Result grcore_snapshot_writer_u64(
    GRCORE_SnapshotWriter * writer, uint64_t value);

/** @brief Appends a length-prefixed string; NULL is written as the empty one. */
GRCORE_API GRCORE_Result grcore_snapshot_writer_string(
    GRCORE_SnapshotWriter * writer, const char * text);

/** @brief The bytes written to this blob so far. */
GRCORE_API size_t grcore_snapshot_writer_size(const GRCORE_SnapshotWriter * writer);

/**
 * @brief The snapshot's allocator, for a hook's scratch memory that outlives
 *   the call or is handed to a library that takes an allocator.
 */
GRCORE_API const GRCORE_Allocator * grcore_snapshot_writer_allocator(
    const GRCORE_SnapshotWriter * writer);

/**
 * @brief Reads the next `size` bytes. Reading past the end fails and stays
 *   failed.
 *
 * @return ::GRCORE_OK; ::GRCORE_ERR_CORRUPT past the end of the blob.
 */
GRCORE_API GRCORE_Result grcore_snapshot_reader_read(
    GRCORE_SnapshotReader * reader, void * out, size_t size);

/**
 * @brief Borrows the next `size` bytes in place, without copying.
 *
 * @param out_data Receives a pointer valid as long as the snapshot.
 * @return As ::grcore_snapshot_reader_read.
 */
GRCORE_API GRCORE_Result grcore_snapshot_reader_view(
    GRCORE_SnapshotReader * reader, size_t size, const void ** out_data);

/** @brief Reads one 64-bit value written by ::grcore_snapshot_writer_u64. */
GRCORE_API GRCORE_Result grcore_snapshot_reader_u64(
    GRCORE_SnapshotReader * reader, uint64_t * out_value);

/**
 * @brief Reads a string written by ::grcore_snapshot_writer_string.
 *
 * @param out_text Receives a NUL-terminated string valid as long as the
 *   snapshot.
 * @param out_length Receives the length; may be NULL.
 * @return As ::grcore_snapshot_reader_read.
 */
GRCORE_API GRCORE_Result grcore_snapshot_reader_string(
    GRCORE_SnapshotReader * reader, const char ** out_text, size_t * out_length);

/** @brief The bytes not yet read. */
GRCORE_API size_t grcore_snapshot_reader_remaining(
    const GRCORE_SnapshotReader * reader);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_SNAPSHOT_H */
