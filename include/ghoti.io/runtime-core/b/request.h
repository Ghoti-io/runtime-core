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
 * @file request.h
 * @stability stable
 *
 * Requests and ports: the only way another thread acts on a context (AD-4).
 *
 * A request is a kind. The core defines five. Terminate, time and interrupt
 * are posted; fuel and memory are *derived*, computed from the budgets at each
 * slow poll, so they are levels that can never be lost and never be posted.
 * A service defines more kinds of its own with ::grcore_context_request_kind,
 * which ties each to the key it belongs to so a request can always be
 * attributed (AD-19).
 *
 * Kinds below ::GRCORE_REQUEST_WORD_KINDS are bits of one atomic word that the
 * poll's fast path loads; a poll with nothing pending is that one load and one
 * branch. Kinds at or above it live in a set beside the port and are signalled
 * by the word's top bit, so any number of kinds is allowed and the fast path
 * does not change.
 *
 * Threads: a ::GRCORE_Port is the one object here that any thread may use. It
 * is reference-counted, owned by the context's group, and it stays valid after
 * its context is destroyed, when a post is refused. It holds a mutex and a
 * condition variable: a post sets its bit under the mutex and signals, which is
 * what wakes a context blocked in ::grcore_context_wait. The poll never takes
 * the mutex unless a kind from the overflow set is asked about. Everything
 * else in this header is the context owner's.
 */

#ifndef GHOTI_IO_GRCORE_B_REQUEST_H
#define GHOTI_IO_GRCORE_B_REQUEST_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A request kind. */
typedef uint32_t GRCORE_RequestKind;

/** @brief The kinds the core defines, and where the others begin. */
enum {
  GRCORE_REQUEST_TERMINATE = 0, ///< Stop the run for good (an unwind).
  GRCORE_REQUEST_TIME,          ///< The host's wall-clock timer expired.
  GRCORE_REQUEST_INTERRUPT,     ///< The host asks for a pause.
  GRCORE_REQUEST_FUEL,          ///< Derived: fuel is exhausted. Not postable.
  GRCORE_REQUEST_MEMORY,        ///< Derived: memory is over budget. Not
                                ///< postable.
  GRCORE_REQUEST_CORE_COUNT,    ///< How many core kinds there are (five).
  GRCORE_REQUEST_FIRST_KEYED = GRCORE_REQUEST_CORE_COUNT, ///< First service
                                                          ///< kind.
  GRCORE_REQUEST_WORD_KINDS = 63 ///< Kinds from here on are in the overflow
                                 ///< set.
};

/** @brief A reference-counted way to post requests to a context. Opaque. */
typedef struct GRCORE_Port GRCORE_Port;

/**
 * @brief Defines a new request kind for `key`.
 *
 * Each call defines a new kind, numbered from ::GRCORE_REQUEST_FIRST_KEYED in
 * order. `key` need not be registered anywhere. The number of kinds is bounded
 * by memory and by the 32-bit numbering.
 *
 * @param context The context. The caller must own it, and it must not be
 *   running or being destroyed.
 * @param key The key the kind is attributed to.
 * @param out_kind Receives the kind. Written only on success.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID (nothing changes),
 *   ::GRCORE_ERR_OOM (likewise), or ::GRCORE_ERR_LIMIT (likewise) at the
 *   ceiling of the 32-bit kind numbering.
 */
GRCORE_API GRCORE_Result grcore_context_request_kind(GRCORE_Context * context,
    const GRCORE_Key * key, GRCORE_RequestKind * out_kind);

/**
 * @brief The key a service kind was defined for.
 *
 * @param context The context.
 * @param kind A kind from ::grcore_context_request_kind.
 * @return The key; NULL for a core kind, an undefined kind or NULL.
 */
GRCORE_API const GRCORE_Key * grcore_context_request_kind_key(
    const GRCORE_Context * context, GRCORE_RequestKind kind);

/**
 * @brief The context's port, created on first use.
 *
 * @param context The context. The caller must own it.
 * @param out_port Receives the port with one reference taken for the caller,
 *   released with ::grcore_port_release. Written only on success.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID or ::GRCORE_ERR_OOM.
 */
GRCORE_API GRCORE_Result grcore_context_port(
    GRCORE_Context * context, GRCORE_Port ** out_port);

/**
 * @brief Takes another reference to a port. Any thread, if it holds one.
 *
 * @param port The port.
 * @return `port`; NULL for NULL.
 */
GRCORE_API GRCORE_Port * grcore_port_retain(GRCORE_Port * port);

/**
 * @brief Gives up a reference. The last one frees the port, which also
 *   releases it from the group's port count. NULL is ignored.
 *
 * @param port The port.
 */
GRCORE_API void grcore_port_release(GRCORE_Port * port);

/**
 * @brief Posts a request to the port's context. Any thread.
 *
 * Sets the kind's bit under the port's mutex and wakes a context blocked in
 * ::grcore_context_wait. Posting a kind that is already pending is not an
 * error. Terminate, time, interrupt and every defined service kind are
 * postable; fuel and memory are not.
 *
 * @param port The port.
 * @param kind The kind.
 * @return ::GRCORE_OK, ::GRCORE_ERR_INVALID for a NULL port, a derived or
 *   undefined kind, or a context that has been destroyed (nothing is set), or
 *   ::GRCORE_ERR_OOM when an overflow kind cannot be recorded (likewise).
 */
GRCORE_API GRCORE_Result grcore_port_post(
    GRCORE_Port * port, GRCORE_RequestKind kind);

/**
 * @brief Whether a request is pending. Owner.
 *
 * @param context The context.
 * @param kind The kind, including the derived ones as of the last poll.
 * @return True if pending; false for NULL or an undefined kind.
 */
GRCORE_API bool grcore_context_request_pending(
    const GRCORE_Context * context, GRCORE_RequestKind kind);

/**
 * @brief Clears a pending request. Owner.
 *
 * Clearing one that is not pending succeeds. The derived kinds are levels and
 * cannot be cleared, and terminate is cleared only by the end of `run`
 * (AD-21).
 *
 * @param context The context. The caller must own it.
 * @param kind The kind.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for a kind that cannot be
 *   cleared or is undefined.
 */
GRCORE_API GRCORE_Result grcore_context_clear_request(
    GRCORE_Context * context, GRCORE_RequestKind kind);

/**
 * @brief Posts terminate to the context directly. Owner.
 *
 * Other threads post ::GRCORE_REQUEST_TERMINATE through a port.
 *
 * @param context The context. The caller must own it.
 * @return ::GRCORE_OK or ::GRCORE_ERR_INVALID.
 */
GRCORE_API GRCORE_Result grcore_context_terminate(GRCORE_Context * context);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_REQUEST_H */
