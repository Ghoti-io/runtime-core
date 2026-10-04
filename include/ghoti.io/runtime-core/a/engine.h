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
 * @file engine.h
 * @stability free
 *
 * Engine descriptors: how an engine tells A what its frames mean (AD-18).
 *
 * An *engine* is any library that pushes frames through A. It registers one
 * static descriptor per context, and every frame it pushes afterwards names
 * that descriptor in its header. A consumer that holds a frame (the debugger,
 * the collector, the differential, the recorder) never needs to know which
 * engine pushed it: it asks the descriptor, through the abstract frame in
 * frame.h.
 *
 * The descriptor is deliberately small. Only what the stories so far read is
 * in it; C zero-fills fields added after it, so a descriptor written before a
 * field existed keeps working, and a NULL callback always means "the engine
 * has nothing to say", never an error. Root enumeration and unwinding are in
 * it now (the `roots` and `unwind` hooks). Deoptimization is not a hook: its
 * metadata is a table in codemeta.h, and the descriptor has no field for it.
 *
 * A is labelled `free` (AD-14): a consumer requires the exact version it was
 * built against.
 */

#ifndef GHOTI_IO_GRCORE_A_ENGINE_H
#define GHOTI_IO_GRCORE_A_ENGINE_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/b/roots.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A registered engine, per context. Zero is never a valid id. */
typedef uint32_t GRCORE_EngineId;

/** @brief What a frame slot holds, as its engine declares it. */
typedef enum {
  GRCORE_SLOT_RAW = 0, ///< Plain bits: an integer, a float, a code address.
  GRCORE_SLOT_VALUE    ///< An engine value, which the engine's inspector and
                       ///< conservative decoder know how to read.
} GRCORE_SlotKind;

/**
 * @brief How to read an address out of a word, without knowing the engine
 *   (AD-18).
 *
 * `address = ((word & mask) >> shift) + base`. A decoder with a zero mask
 * decodes nothing. C code may hold an encoding the decoder cannot read only
 * inside a handle.
 */
typedef struct GRCORE_ConservativeDecoder {
  uint64_t mask;  ///< Bits of the word that carry the address.
  unsigned shift; ///< How far to shift them down; below 64.
  uint64_t base;  ///< Added after the shift.
} GRCORE_ConservativeDecoder;

/**
 * @brief Decodes `word`.
 *
 * @param decoder The decoder; NULL decodes nothing.
 * @param word The word.
 * @param out_address Receives the address. Written only when this returns
 *   true.
 * @return False for a NULL decoder or output, a zero mask or a shift of 64 or
 *   more; true otherwise.
 */
GRCORE_API bool grcore_decoder_decode(const GRCORE_ConservativeDecoder * decoder,
    uint64_t word, uint64_t * out_address);

/** @brief What a scope is. */
typedef enum {
  GRCORE_SCOPE_LOCAL = 0, ///< A function's own variables.
  GRCORE_SCOPE_CLOSURE,   ///< Variables captured from an enclosing function.
  GRCORE_SCOPE_GLOBAL     ///< Module or global variables.
} GRCORE_ScopeKind;

/** @brief One scope of a frame, as the engine reports it. */
typedef struct GRCORE_ScopeInfo {
  GRCORE_ScopeKind kind;  ///< What it is.
  const char * name;      ///< For display. May be NULL; must outlive the read.
  size_t variable_count;  ///< How many variables it holds.
} GRCORE_ScopeInfo;

/** @brief One variable of a scope, as the engine reports it. */
typedef struct GRCORE_Variable {
  const char * name;     ///< For display. May be NULL; must outlive the read.
  GRCORE_SlotKind kind;  ///< What the value is.
  uint64_t value;        ///< The value's bits.
} GRCORE_Variable;

/** @brief A frame as every consumer reads it. Defined in frame.h. */
typedef struct GRCORE_AbstractFrame GRCORE_AbstractFrame;

/**
 * @brief The scope interface (AD-18): how variables are read without knowing
 *   how the engine keeps them.
 *
 * Each language keeps its own closure representation; this is the only part
 * of it A sees. The callbacks read the frame's slots through stack.h (the
 * abstract frame gives them the context and the frame). An interface whose
 * callbacks are NULL has no scopes.
 */
typedef struct GRCORE_ScopeInterface {
  /** How many scopes the frame has. */
  size_t (*scope_count)(const GRCORE_AbstractFrame * frame);
  /** Describes scope `index`, below the count. Returns ::GRCORE_OK, or
   *  ::GRCORE_ERR_INVALID for an index it does not have. */
  GRCORE_Result (*scope)(const GRCORE_AbstractFrame * frame, size_t index,
      GRCORE_ScopeInfo * out_scope);
  /** Reads variable `index` of scope `scope`. Returns ::GRCORE_OK, or
   *  ::GRCORE_ERR_INVALID. */
  GRCORE_Result (*variable)(const GRCORE_AbstractFrame * frame, size_t scope,
      size_t index, GRCORE_Variable * out_variable);
} GRCORE_ScopeInterface;

/**
 * @brief An engine descriptor. Define it `static` (or `const`) and register it
 *   by address; the object must outlive every context that holds it.
 */
typedef struct GRCORE_EngineDescriptor {
  /** The engine's name, for diagnostics. Required: registration refuses a
   *  NULL or empty one. */
  const char * name;
  /** The kind of slot `index` of `frame`. NULL means every slot is
   *  ::GRCORE_SLOT_RAW. */
  GRCORE_SlotKind (*slot_kind)(
      const GRCORE_AbstractFrame * frame, size_t index);
  /** Names a poll identity as a source location (AD-18). Called only when a
   *  location is needed: on a pause or unwind, and by the frame walk. NULL
   *  means `{NULL, 0}`. The strings must outlive the context's use of them. */
  GRCORE_Location (*locate)(
      const GRCORE_Context * context, uint64_t function, uint64_t offset);
  /** Writes a human-readable form of a slot into `buffer` as `snprintf`
   *  does: it returns the length the full text needs, and writes at most
   *  `size` bytes including the terminator. NULL means hexadecimal. */
  size_t (*inspect)(const GRCORE_Context * context, GRCORE_SlotKind kind,
      uint64_t value, char * buffer, size_t size);
  /** The scope interface; all-NULL for none. */
  GRCORE_ScopeInterface scopes;
  /** The conservative decoder for this engine's value encoding; a zero mask
   *  for none. */
  GRCORE_ConservativeDecoder decoder;
  /** Reports the roots of `frame` that are not plain VALUE slots (an open
   *  upvalue, a pointer into the frame, a value held in a side table), to
   *  `visitor` as precise slots. A's root source has already reported every
   *  VALUE slot of the frame itself, so a hook reports each root once, not
   *  twice. NULL means the VALUE slots are all there is. The hook must test
   *  `visitor->slot` and `visitor->range` for NULL before calling them, and
   *  must not call the unwinder, the activation functions or the budget-scope
   *  functions. It may read the frame's slots through stack.h but must not
   *  push, pop or poll, and
   *  the frame's `location` is not filled in (a collector calls this on every
   *  frame at every collection). */
  void (*roots)(GRCORE_Context * context, const GRCORE_AbstractFrame * frame,
      const GRCORE_RootVisitor * visitor);
  /** Called by the unwinder for each frame it pops, innermost first, while
   *  the frame is still on the stack: the engine releases what the frame
   *  holds (closes its open upvalues, drops a handler). It never runs guest
   *  code, and must not push, pop or poll, nor call the unwinder, the activation
   *  functions or the budget-scope functions. NULL means nothing to release. The
   *  frame's `location` is not filled in. */
  void (*unwind)(GRCORE_Context * context, const GRCORE_AbstractFrame * frame);
} GRCORE_EngineDescriptor;

/**
 * @brief Registers an engine with a context.
 *
 * The first registration creates the context's guest stack (stack.h), which
 * is per-context state registered under a key of A's, so the context needs to
 * know nothing about A. Registration is refused while the context is running,
 * which is why the stack exists before the first push.
 *
 * @param context The context. The caller must own it, and it must not be
 *   running or being destroyed.
 * @param descriptor The descriptor, by address. Must have a name, and must not
 *   already be registered with this context.
 * @param out_id Receives the engine's id, from 1. Written only on success.
 * @return ::GRCORE_OK; ::GRCORE_ERR_INVALID for a NULL argument, an unnamed
 *   or already registered descriptor, a non-owner or a wrong state;
 *   ::GRCORE_ERR_LIMIT if the context's memory budget refuses the allocation;
 *   or ::GRCORE_ERR_OOM for any other allocation failure. A refusal leaves the engine table unchanged.
 */
GRCORE_API GRCORE_Result grcore_engine_register(GRCORE_Context * context,
    const GRCORE_EngineDescriptor * descriptor, GRCORE_EngineId * out_id);

/**
 * @brief The descriptor registered under `id`.
 *
 * Like ::grcore_engine_count, this reads the context's engine table and is for
 * the owner, or the holder of an at-poll or paused context; not for a
 * concurrent thread.
 *
 * @param context The context.
 * @param id An id returned by ::grcore_engine_register.
 * @return The descriptor; NULL for NULL, zero or an unknown id.
 */
GRCORE_API const GRCORE_EngineDescriptor * grcore_engine_descriptor(
    const GRCORE_Context * context, GRCORE_EngineId id);

/**
 * @brief How many engines the context has registered.
 *
 * For the owner, or the holder of an at-poll or paused context; not for a
 * concurrent thread.
 *
 * @param context The context.
 * @return The count; zero for NULL.
 */
GRCORE_API size_t grcore_engine_count(const GRCORE_Context * context);

/**
 * @brief Renders a slot through its engine's inspector.
 *
 * Without an inspector the text is the value in hexadecimal, `0x` and
 * lowercase digits.
 *
 * @param context The context.
 * @param id The engine whose inspector to use.
 * @param kind The slot's kind.
 * @param value The slot's bits.
 * @param buffer Receives the text, terminated, truncated to `size`. May be
 *   NULL when `size` is zero.
 * @param size The buffer's size.
 * @param out_length Receives the length the full text needs, not counting the
 *   terminator. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL, an unknown engine or
 *   a NULL buffer with a size.
 */
GRCORE_API GRCORE_Result grcore_engine_inspect(const GRCORE_Context * context,
    GRCORE_EngineId id, GRCORE_SlotKind kind, uint64_t value, char * buffer,
    size_t size, size_t * out_length);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_ENGINE_H */
