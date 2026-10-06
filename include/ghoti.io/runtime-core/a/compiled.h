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
 * @file compiled.h
 * @stability free
 *
 * The precise walk of compiled frames (AD-17, AD-28).
 *
 * Compiled code calling compiled code leaves frames on the native stack that
 * no guest stack records. The walk finds them without unwinding natively, from
 * three facts.
 *
 * **The frame layout contract.** Every backend's internal calling convention
 * honours one layout: a compiled frame's *base* word holds the caller's frame
 * base, and the word after it holds the return address. That is `rbp` on
 * x86-64 and `x29` with the saved link on arm64. The frame lies below its
 * base. Nothing else about a frame is assumed.
 *
 * **Where it starts.** An activation record may carry the frame base and the
 * return address of the innermost compiled frame under it
 * (::grcore_activation_set_compiled). The return address is the one of the
 * call that left that frame: an address inside registered code
 * (`a/registry.h`) at a site's offset. The walk starts at the innermost record
 * that carries one.
 *
 * **How it proceeds.** A frame's code is found from its return address, and its
 * site, with the stack map and the identity, from `grcore_codemeta_find` at the
 * address's offset in that code. The caller's base and return address are the
 * two words at the frame's base. If that return address is in registered code
 * the caller is a compiled frame and the walk continues; if it is in none, it is
 * the entry from the interpreter and the run ends. The walk then goes to the
 * next record outward that carries compiled state, which is a compiled run
 * under a nested activation, and so on.
 *
 * **A broken chain is never skipped.** The walk checks that each caller base is
 * word-aligned, above its callee's, and, where the record has a C segment,
 * inside it; that the innermost frame's return address is in registered code at
 * a site; and that a run is above the runs inside it. A chain that fails one
 * ends the walk with ::GRCORE_CWALK_BROKEN and a reason, and the walk stays
 * broken. A consumer that cannot return an error (root enumeration) must stop,
 * since a skipped frame is a missed root.
 *
 * The walk reads native memory at the frame bases, so it is valid only while
 * those frames are on the native stack: for the owner thread at a poll, or in
 * a native called from compiled code. It reads the registry at the moment of
 * the call and holds no reference.
 */

#ifndef GHOTI_IO_GRCORE_A_COMPILED_H
#define GHOTI_IO_GRCORE_A_COMPILED_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/a/codemeta.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What one step of the walk did. */
typedef enum GRCORE_CompiledWalkStatus {
  GRCORE_CWALK_FRAME = 0, ///< A frame was written.
  GRCORE_CWALK_END,       ///< There are no more compiled frames.
  GRCORE_CWALK_BROKEN     ///< The chain is broken; see the walk's reason.
} GRCORE_CompiledWalkStatus;

/** @brief One compiled frame. */
typedef struct GRCORE_CompiledFrame {
  uintptr_t frame_base;          ///< The frame's base word.
  uintptr_t return_address;      ///< Where the frame is stopped, in its code.
  GRCORE_EngineId engine;        ///< The engine whose frame it is.
  const GRCORE_CodeMeta * meta;  ///< The code's table.
  const GRCORE_CodeSite * site;  ///< The site at that address: stack map, deopt.
  GRCORE_PollIdentity identity;  ///< The site's identity (AD-18).
  size_t depth;                  ///< Zero for the innermost compiled frame.
  size_t record;                 ///< The activation record it was found from.
  size_t base_frames;            ///< That record's guest frame count: the guest
                                 ///< frames this frame is above, in the stack.
} GRCORE_CompiledFrame;

/** @brief A walk in progress, innermost frame first. Treat as opaque. */
typedef struct GRCORE_CompiledWalk {
  const GRCORE_Context * context;
  size_t next_record;        ///< Records below this index are not yet looked at.
  size_t run_record;         ///< The record the current run came from.
  uintptr_t base;            ///< The next frame's base; zero between runs.
  uintptr_t return_address;  ///< The next frame's return address.
  uintptr_t last_base;       ///< The last frame yielded; zero before any.
  size_t depth;
  bool failing;              ///< The chain after the last frame is bad.
  bool broken;               ///< Reported; sticky.
  const char * reason;       ///< Static; set with `failing` and `broken`.
} GRCORE_CompiledWalk;

/**
 * @brief Begins a walk at the innermost record that carries compiled state.
 *
 * @param context The context. A context with no engine, or no record with
 *   compiled state, walks as empty.
 * @param out_walk Receives the walk. Written only on success.
 * @return ::GRCORE_OK, or ::GRCORE_ERR_INVALID for NULL.
 */
GRCORE_API GRCORE_Result grcore_compiled_walk_begin(
    const GRCORE_Context * context, GRCORE_CompiledWalk * out_walk);

/**
 * @brief Moves to the next compiled frame, outward.
 *
 * @param walk The walk.
 * @param out_frame Receives the frame on ::GRCORE_CWALK_FRAME only.
 * @return ::GRCORE_CWALK_FRAME, ::GRCORE_CWALK_END, or ::GRCORE_CWALK_BROKEN
 *   (and again on every later call).
 */
GRCORE_API GRCORE_CompiledWalkStatus grcore_compiled_walk_next(
    GRCORE_CompiledWalk * walk, GRCORE_CompiledFrame * out_frame);

/**
 * @brief Why a walk broke.
 *
 * @return A static string; NULL if the walk is not broken.
 */
GRCORE_API const char * grcore_compiled_walk_reason(const GRCORE_CompiledWalk * walk);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_A_COMPILED_H */
