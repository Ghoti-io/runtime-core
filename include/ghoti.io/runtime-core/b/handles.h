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
 * @file handles.h
 * @stability stable
 *
 * The cross-heap handle table (CAP-14).
 *
 * A context holds a reference to a value that lives in *another* context's
 * heap only through this table, never through a pointer. The group owns one
 * table (::grcore_group_handles). An entry records one 64-bit *value word*
 * (opaque here: whatever the owner's heap uses to name a value), the context
 * that *owns* it (the context whose heap the value lives in) and the context
 * that *holds* it (the one that was given the token). The holder keeps a
 * ::GRCORE_HandleToken, which is a name for the entry and nothing else: it
 * cannot be turned into the value except by the owner, on the owner's thread
 * (::grcore_handles_resolve).
 *
 * **Roots.** A value held through the table is a root of its owner. The first
 * time a context owns an entry, the table registers one root source with that
 * context (b/roots.h), and the collector finds the value word the way it finds
 * any other precise root: through the visitor, which may rewrite the word in
 * place, so a moving collector updates it and a later resolve returns the new
 * word. The table has no knowledge of what a value word means, and holds no
 * trace hook of its own: the root source is the hook. A value is therefore
 * rooted exactly as long as its entry exists.
 *
 * **Tokens.** A token carries the table's tag, the entry's index and a
 * 64-bit generation drawn from a counter that only goes up, so a generation is
 * never issued twice by one table. A released entry's token is *stale* for
 * ever, even after its index is reused. A token is refused from any context
 * but the holder (and the owner), so a guessed or copied token is no use to a
 * third party. The tag is a per-table, per-process-run number that refuses a
 * token from another table or another run; it is not a secret and the design
 * does not rely on it being one.
 *
 * **Lifetime.** The table sees a context come and go through
 * ::grcore_context_destroy, which calls into it:
 *
 *  - when a context that *owns* entries is destroyed, each of them becomes a
 *    *tombstone*: the value word is dropped and nothing is rooted, but the
 *    entry stays so that the holder's token answers
 *    ::GRCORE_HANDLE_TOMBSTONE (the target is gone) rather than looking as if
 *    it had never existed. The holder releases it, or dies, to free the entry;
 *  - when a context that *holds* entries is destroyed, they are released.
 *
 * After every context of a group is gone the table is empty and the group
 * memory it charged is back to zero.
 *
 * **Threads.** The table is shared by the group's contexts, which may be on
 * different threads, and is guarded by one mutex. Every call takes it for a
 * bounded, allocation-free stretch (a call that pins may grow the entry array)
 * and holds no other lock while it does. A call that names a context requires
 * the caller to own that context (b/context.h), because a context's value
 * words and its hold on the table are the owning thread's to touch; the one
 * call that names a holder from outside is ::grcore_handles_pin, which takes
 * the holder as a ::GRCORE_HandleHolder, a checked name rather than a pointer,
 * so a holder that dies while an owner pins for it is refused instead of
 * dereferenced. A root visitor must not call the table: a root source is
 * enumerated with the mutex held.
 *
 * **Limits.** Live entries are counted per holder and in total, and a pin that
 * would pass either is refused with ::GRCORE_ERR_LIMIT. A tombstone still
 * counts against its holder until it is released. The limits are the
 * table's only budget: its calls are bounded and charge no fuel; memory for
 * entries is charged to the group's meter (::grcore_group_memory_in_use).
 *
 * **What this table is not.** It does not run operations on a token: a
 * holder's request to read or call through a token is an ordinary request to
 * the owner (b/request.h), which the owner answers at its next poll by
 * resolving the token. It does not trace for cycles that span heaps; the
 * enumeration a later collector needs is the root source, per owner. A
 * context snapshot does not capture its entries (a snapshot holds no host
 * state), so restoring into a context that owns live entries is refused.
 */

#ifndef GHOTI_IO_GRCORE_B_HANDLES_H
#define GHOTI_IO_GRCORE_B_HANDLES_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A group's handle table. Opaque; owned by the group. */
typedef struct GRCORE_HandleTable GRCORE_HandleTable;

/**
 * @brief A name for one entry. Plain data: copy it freely.
 *
 * All zero is never a token ::grcore_handles_pin issues, so a zeroed token
 * (::GRCORE_HANDLE_TOKEN_INIT) is a safe "none" and is always stale.
 */
typedef struct GRCORE_HandleToken {
  uint64_t generation; ///< From a counter that never repeats; never zero.
  uint32_t index;      ///< The entry's position; reused after a release.
  uint32_t tag;        ///< The table's tag; never zero.
} GRCORE_HandleToken;

/** @brief The initialiser of a "none" token. */
#define GRCORE_HANDLE_TOKEN_INIT { 0, 0, 0 }

/**
 * @brief A checked name for a context that may hold tokens: what an owner
 *   names when it pins for a holder it does not own.
 *
 * Made by the holder's own thread with ::grcore_handles_enroll and handed to
 * the owner by whatever channel the host has. It refers to the context by a
 * number the table never reuses, so it cannot be mistaken for a later context
 * that was given the same address.
 */
typedef struct GRCORE_HandleHolder {
  uint64_t serial; ///< From a counter that never repeats; never zero.
  uint32_t slot;   ///< The holder's position in the table.
  uint32_t tag;    ///< The table's tag; never zero.
} GRCORE_HandleHolder;

/** @brief The initialiser of a "none" holder. */
#define GRCORE_HANDLE_HOLDER_INIT { 0, 0, 0 }

/** @brief What a token is, to the context that asks. */
typedef enum {
  /** The entry exists and its owner is alive. */
  GRCORE_HANDLE_LIVE = 0,
  /** The entry exists and its owner is gone: no value, nothing rooted. */
  GRCORE_HANDLE_TOMBSTONE,
  /** No such entry: released, never issued, forged, or from another table.
   *  A released token is stale for ever. */
  GRCORE_HANDLE_STALE,
  /** The entry exists, and the asking context is neither its holder nor its
   *  owner. Nothing about the entry is reported. */
  GRCORE_HANDLE_FOREIGN,
  /** The call itself is wrong: a NULL argument, a context the calling thread
   *  does not own, a context of another group, or one tearing down. */
  GRCORE_HANDLE_BAD_CALL
} GRCORE_HandleStatus;

/** @brief The limit meaning "no limit". */
#define GRCORE_HANDLES_UNLIMITED SIZE_MAX

/** @brief The default most live entries one holder may have. */
#define GRCORE_HANDLES_DEFAULT_PER_HOLDER ((size_t)65536)

/** @brief The default most live entries a table may have. */
#define GRCORE_HANDLES_DEFAULT_TOTAL ((size_t)1048576)

/** @brief The most entries a table can ever hold, whatever its limits say:
 *   an entry's index is 32 bits. */
#define GRCORE_HANDLES_MAX_ENTRIES ((size_t)0x7fffffff)

/**
 * @brief The group's handle table.
 *
 * @param group The group.
 * @return The table, which lives as long as the group; NULL for NULL.
 */
GRCORE_API GRCORE_HandleTable * grcore_group_handles(GRCORE_Group * group);

/**
 * @brief Makes a context a possible holder, and returns its checked name.
 *
 * Idempotent: a context already enrolled gets the same name back. The name
 * stops being valid when the context is destroyed.
 *
 * @param table The table.
 * @param holder The context. The caller must own it; it must belong to the
 *   table's group and not be tearing down.
 * @param out_holder Receives the name. Written only on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for a NULL argument or a context
 *   that is not the caller's, is of another group or is tearing down;
 *   ::GRCORE_ERR_OOM.
 */
GRCORE_API GRCORE_Result grcore_handles_enroll(GRCORE_HandleTable * table,
    GRCORE_Context * holder, GRCORE_HandleHolder * out_holder);

/**
 * @brief Whether the holder named is a context that is still alive.
 *
 * @param table The table.
 * @param holder The name from ::grcore_handles_enroll.
 * @return true if it names a live context of the table's group; false for a
 *   NULL table, a malformed name and a context that has been destroyed.
 */
GRCORE_API bool grcore_handles_holder_alive(
    GRCORE_HandleTable * table, GRCORE_HandleHolder holder);

/**
 * @brief Roots a value word on behalf of a holder, and returns its token.
 *
 * The value word becomes a root of `owner` (see the file comment) for as long
 * as the entry exists. It is any 64-bit word; the table never looks at it. The
 * owner is enrolled on the way if it was not.
 *
 * @param table The table.
 * @param owner The context whose heap the value lives in. The caller must own
 *   it; it must belong to the table's group and not be tearing down.
 * @param holder The context to hand the token to; may be `owner` itself.
 * @param value The value word.
 * @param out_token Receives the token. Written only on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for a NULL argument, a context
 *   that is not the caller's, of another group or tearing down, a malformed
 *   holder name, or a holder that is gone (::grcore_handles_holder_alive tells
 *   the last apart); ::GRCORE_ERR_LIMIT if the holder is at its limit, the
 *   table is at its total, or the table has no generation left to issue;
 *   ::GRCORE_ERR_OOM. A refusal changes nothing a later call can see.
 */
GRCORE_API GRCORE_Result grcore_handles_pin(GRCORE_HandleTable * table,
    GRCORE_Context * owner, GRCORE_HandleHolder holder, uint64_t value,
    GRCORE_HandleToken * out_token);

/**
 * @brief Reads the value word of a token the caller owns.
 *
 * Only the owner can do this, on the thread that owns it, which is what makes
 * a stored word safe to use: it is the word as a moving collector last left
 * it, and the owner's heap is not running concurrently.
 *
 * @param table The table.
 * @param token The token.
 * @param owner The context the token's value lives in. The caller must own it.
 * @param out_value Receives the word, or may be NULL. Written only when the
 *   status is ::GRCORE_HANDLE_LIVE.
 * @return ::GRCORE_HANDLE_LIVE; ::GRCORE_HANDLE_STALE for a token that is not
 *   an entry; ::GRCORE_HANDLE_FOREIGN if `owner` does not own the entry (a
 *   holder, or a tombstone's holder, is never the owner and gets this);
 *   ::GRCORE_HANDLE_BAD_CALL.
 */
GRCORE_API GRCORE_HandleStatus grcore_handles_resolve(GRCORE_HandleTable * table,
    GRCORE_HandleToken token, GRCORE_Context * owner, uint64_t * out_value);

/**
 * @brief What a token is, to the context asking: the check a holder makes
 *   before it trusts one, on every call it makes with it.
 *
 * @param table The table.
 * @param token The token.
 * @param asker The asking context. The caller must own it. It must be the
 *   entry's holder or owner to learn more than that the token exists.
 * @param out_owner Receives the owner's id (a number the table never reuses),
 *   or may be NULL. Written only for ::GRCORE_HANDLE_LIVE and
 *   ::GRCORE_HANDLE_TOMBSTONE.
 * @param out_holder Receives the holder's id likewise, or may be NULL.
 * @return The status; ::GRCORE_HANDLE_LIVE or ::GRCORE_HANDLE_TOMBSTONE when
 *   the asker is the holder or the owner, ::GRCORE_HANDLE_FOREIGN when the
 *   entry exists and the asker is neither, ::GRCORE_HANDLE_STALE when it does
 *   not, and ::GRCORE_HANDLE_BAD_CALL. (A context that is not a party to an
 *   entry can tell a live token from a stale one. That is one bit, and the
 *   token is unusable to it.)
 */
GRCORE_API GRCORE_HandleStatus grcore_handles_inspect(
    GRCORE_HandleTable * table, GRCORE_HandleToken token, GRCORE_Context * asker,
    uint64_t * out_owner, uint64_t * out_holder);

/**
 * @brief Releases an entry. The asker is the holder (it is done with the
 *   token) or the owner (it revokes it).
 *
 * The value stops being rooted by this entry, the word is cleared, and every
 * copy of the token is stale from now on.
 *
 * @param table The table.
 * @param token The token.
 * @param asker The releasing context. The caller must own it.
 * @return What the entry was: ::GRCORE_HANDLE_LIVE or
 *   ::GRCORE_HANDLE_TOMBSTONE mean it has been released. Any other status
 *   (::GRCORE_HANDLE_STALE, ::GRCORE_HANDLE_FOREIGN, ::GRCORE_HANDLE_BAD_CALL)
 *   means nothing changed.
 */
GRCORE_API GRCORE_HandleStatus grcore_handles_release(
    GRCORE_HandleTable * table, GRCORE_HandleToken token, GRCORE_Context * asker);

/**
 * @brief Sets the limits (see the file comment). They bind new pins only; an
 *   entry that exists is never evicted by lowering them.
 *
 * @param table The table.
 * @param per_holder The most live entries one holder may have, from 1; or
 *   ::GRCORE_HANDLES_UNLIMITED.
 * @param total The most the table may have, from 1, and at most
 *   ::GRCORE_HANDLES_MAX_ENTRIES; or ::GRCORE_HANDLES_UNLIMITED, which means
 *   that maximum.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL or a zero or too large
 *   limit (nothing changes). Zero is refused so that it can never be read as
 *   either "unlimited" or "the default".
 */
GRCORE_API GRCORE_Result grcore_handles_set_limits(
    GRCORE_HandleTable * table, size_t per_holder, size_t total);

/** @brief The per-holder limit. Zero for NULL. */
GRCORE_API size_t grcore_handles_limit_per_holder(GRCORE_HandleTable * table);

/** @brief The total limit. Zero for NULL. */
GRCORE_API size_t grcore_handles_limit_total(GRCORE_HandleTable * table);

/** @brief How many entries exist, live and tombstone. Zero for NULL. */
GRCORE_API size_t grcore_handles_count(GRCORE_HandleTable * table);

/** @brief How many of them are tombstones. Zero for NULL. */
GRCORE_API size_t grcore_handles_tombstone_count(GRCORE_HandleTable * table);

/**
 * @brief How many entries a context holds, live and tombstone.
 *
 * @param table The table.
 * @param context The context. The caller must own it.
 * @return The count; zero for NULL, for a context the caller does not own and
 *   for one that never took part.
 */
GRCORE_API size_t grcore_handles_held_count(
    GRCORE_HandleTable * table, GRCORE_Context * context);

/**
 * @brief How many live entries a context owns, which is how many values of its
 *   heap the table roots.
 *
 * @param table The table.
 * @param context The context. The caller must own it.
 * @return The count; zero for NULL, for a context the caller does not own and
 *   for one that never took part.
 */
GRCORE_API size_t grcore_handles_owned_count(
    GRCORE_HandleTable * table, GRCORE_Context * context);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_HANDLES_H */
