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
 * @file registry.h
 * @stability free
 *
 * The registry of compiled code, the entry slots and the retired list of one
 * context (AD-28).
 *
 * **The registry** maps an address range to the code that occupies it, so the
 * frame walk (`a/compiled.h`) can find the code, and through it the stack map,
 * of a frame from a return address alone. Ranges never overlap. Lookup is a
 * binary search.
 *
 * **An entry slot** is one word that compiled code loads to call a function:
 * the address of the function's compiled entry, or zero for none. The word
 * lives in memory that does not move until the context is destroyed, so
 * compiled code may embed its address.
 *
 * **Code lifetime.** The registry and every entry slot that names code hold a
 * counted reference (`a/code.h`). Clearing a slot, replacing its code or
 * unregistering a range does not release the reference at once: a compiled
 * frame may still return into that code. The reference goes to the *retired
 * list*, and is released when the context has no open
 * ::GRCORE_ACTIVATION_JIT record, which is when no compiled frame can be on
 * its native stack. With none open it is released at once. Retired code is
 * still found by ::grcore_code_lookup (it says so), because a frame that
 * returns into it must still be walked. A new range may not overlap retired
 * code either: the memory is still held.
 *
 * **Threads.** The registry, the slots and the list are the context owner's,
 * and a collector reads them at a poll on that thread. The code handles'
 * counts are atomic (`a/code.h`), so another thread may hold and drop a
 * reference of its own; the thread that drops the last runs the release.
 *
 * A code handle's release callback may run from ::grcore_activation_leave (and
 * the unwinder), ::grcore_code_unregister, ::grcore_entry_slot_set and
 * ::grcore_entry_slot_clear, and at the context's destruction. It must not call
 * back into this header (register, unregister, set or clear) or into the
 * context: the registry is being edited when it runs.
 *
 * Entry slots live until the context is destroyed; there is no call that frees
 * one. Everything is released when the context is destroyed, whatever is open. The
 * registry needs an engine to be registered in the context (it lives with A's
 * guest state), and refuses with ::GRCORE_ERR_INVALID until then.
 */

#ifndef GHOTI_IO_GRCORE_A_REGISTRY_H
#define GHOTI_IO_GRCORE_A_REGISTRY_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/code.h>
#include <ghoti.io/runtime-core/a/codemeta.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What a lookup found: one registered range. */
typedef struct GRCORE_CodeRange {
  uintptr_t start;               ///< The first byte of the code.
  uintptr_t end;                 ///< One past the last byte.
  GRCORE_EngineId engine;        ///< The engine whose frames the code runs.
  GRCORE_Code * code;            ///< The registry's handle; not a new reference.
  const GRCORE_CodeMeta * meta;  ///< The stack maps, as registered.
  bool retired;                  ///< Unregistered, but a frame may return in.
} GRCORE_CodeRange;

/**
 * @brief Registers code at an address range.
 *
 * The registry takes a counted reference to `code` and keeps `meta`, which
 * must stay valid for as long as the code does (it is normally part of the
 * code's payload). The table is validated against `size`
 * (::grcore_codemeta_validate).
 *
 * @param context The context. The caller must own it, and it must have an
 *   engine.
 * @param engine The engine whose frames the code runs, as registered.
 * @param code The handle that owns the code.
 * @param start The address of the first byte.
 * @param size The code's size in bytes; non-zero, and `start + size` must not
 *   wrap.
 * @param meta The code's table; `meta->code_bytes` must equal `size`.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for a NULL argument, a non-owner,
 *   an unknown engine, an empty or wrapping range, a context with no engine,
 *   or a range that overlaps one already registered (retired or not);
 *   ::GRCORE_ERR_CORRUPT when the table does not validate;
 *   ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_OOM when the memory is refused. A
 *   refusal changes nothing.
 */
GRCORE_API GRCORE_Result grcore_code_register(GRCORE_Context * context,
    GRCORE_EngineId engine, GRCORE_Code * code, uintptr_t start, size_t size,
    const GRCORE_CodeMeta * meta);

/**
 * @brief Unregisters the range that starts at `start`.
 *
 * The registry's reference is retired: released at once if the context has no
 * open JIT record, otherwise when the last one is left. Until then the range
 * is still found, as retired.
 *
 * @param context The context. The caller must own it.
 * @param start The `start` the range was registered with.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for a NULL context, a
 *   non-owner, or no live range starting there (a retired one is not).
 */
GRCORE_API GRCORE_Result grcore_code_unregister(
    GRCORE_Context * context, uintptr_t start);

/**
 * @brief Finds the range that contains `address`, retired ones included.
 *
 * The range is `[start, end)`: `start` and `end - 1` hit, `end` does not.
 *
 * @param context The context.
 * @param address Any address.
 * @param out_range Receives the range; may be NULL. Written only on a hit.
 * @return True on a hit.
 */
GRCORE_API bool grcore_code_lookup(const GRCORE_Context * context,
    uintptr_t address, GRCORE_CodeRange * out_range);

/** @brief How many ranges are registered and not retired. Zero for NULL. */
GRCORE_API size_t grcore_code_registered_count(const GRCORE_Context * context);

/**
 * @brief How many references wait on the retired list: retired ranges and
 *   retired slot code. Zero for NULL.
 */
GRCORE_API size_t grcore_code_retired_count(const GRCORE_Context * context);

/**
 * @brief The most references the retired list has held at once, since the
 *   context was made. Zero for NULL.
 *
 * Retired code is released only when no JIT activation is open, so a long
 * compiled run that keeps replacing code in its slots retains the old code
 * without a bound; no bound is imposed (AD-28), and this is the measurement of
 * it. Each retired reference holds one compiled function's code, so the figure
 * is a count of functions, not of bytes.
 */
GRCORE_API size_t grcore_code_retired_peak(const GRCORE_Context * context);

/**
 * @brief The word of an entry slot whose function cannot be compiled.
 *
 * A call site loads the slot's entry word and compares it once: above this it
 * is compiled code to call; zero it is empty, and the engine's compile-at-call
 * hook is asked; this it is *refused*, and the site exits without asking
 * (AD-28: a callee that cannot be compiled is an exit, remembered so it costs
 * one compare). It is odd, and below any address code is mapped at.
 */
#define GRCORE_ENTRY_REFUSED ((uintptr_t)1)

/**
 * @brief An entry slot. Compiled code loads `entry` to call the function.
 *
 * Only the owner thread writes it, and only through the functions below. The
 * slot's address, and so `entry`'s, does not change until the context is
 * destroyed.
 */
typedef struct GRCORE_EntrySlot {
  uintptr_t entry; ///< The compiled entry address; zero when the slot is empty;
                   ///< ::GRCORE_ENTRY_REFUSED when its function cannot be
                   ///< compiled.
  void * reserved; ///< The library's; never read or written by a consumer.
} GRCORE_EntrySlot;

/**
 * @brief Makes an empty entry slot.
 *
 * @param context The context. The caller must own it, and it must have an
 *   engine.
 * @param out_slot Receives the slot, which lives until the context is
 *   destroyed. Written only on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for a NULL argument, a non-owner
 *   or a context with no engine; ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_OOM.
 */
GRCORE_API GRCORE_Result grcore_entry_slot_create(
    GRCORE_Context * context, GRCORE_EntrySlot ** out_slot);

/**
 * @brief Points a slot at compiled code.
 *
 * The slot takes a counted reference to `code`. If it already held code, that
 * reference is retired.
 *
 * @param context The context. The caller must own it.
 * @param slot A slot of this context.
 * @param code The handle that owns the code at `entry`.
 * @param entry The entry address; above ::GRCORE_ENTRY_REFUSED.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for a NULL argument, an `entry`
 *   that is not above ::GRCORE_ENTRY_REFUSED, a non-owner or a slot that is not this context's;
 *   ::GRCORE_ERR_LIMIT or ::GRCORE_ERR_OOM, with the slot unchanged.
 */
GRCORE_API GRCORE_Result grcore_entry_slot_set(GRCORE_Context * context,
    GRCORE_EntrySlot * slot, GRCORE_Code * code, uintptr_t entry);

/**
 * @brief Empties a slot. Its reference is retired; the entry word is zero
 *   from this call on, so no later call goes through it.
 *
 * Never allocates, so it cannot fail for want of memory.
 *
 * @param context The context. The caller must own it.
 * @param slot A slot of this context.
 * @return ::GRCORE_OK (also for a slot that is already empty), or
 *   ::GRCORE_ERR_INVALID for a NULL argument, a non-owner or a slot that is
 *   not this context's.
 */
GRCORE_API GRCORE_Result grcore_entry_slot_clear(
    GRCORE_Context * context, GRCORE_EntrySlot * slot);

/**
 * @brief Marks a slot's function as one that cannot be compiled.
 *
 * Like ::grcore_entry_slot_clear (any code it held is retired, and it never
 * allocates) but the entry word becomes ::GRCORE_ENTRY_REFUSED, so a call
 * site that finds it exits at once instead of asking the engine again. A later
 * ::grcore_entry_slot_set or ::grcore_entry_slot_clear replaces the mark.
 *
 * @param context The context. The caller must own it.
 * @param slot A slot of this context.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for a NULL argument, a
 *   non-owner or a slot that is not this context's.
 */
GRCORE_API GRCORE_Result grcore_entry_slot_refuse(
    GRCORE_Context * context, GRCORE_EntrySlot * slot);

/** @brief The code a slot holds a reference to; NULL for an empty or refused
 *   slot. */
GRCORE_API GRCORE_Code * grcore_entry_slot_code(const GRCORE_EntrySlot * slot);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_REGISTRY_H */
