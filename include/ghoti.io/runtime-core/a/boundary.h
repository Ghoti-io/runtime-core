/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
/**
 * @file boundary.h
 * @stability free
 * Resumable, engine-neutral cross-engine activations and exception identity.
 */
#ifndef GHOTI_IO_GRCORE_A_BOUNDARY_H
#define GHOTI_IO_GRCORE_A_BOUNDARY_H

#include <ghoti.io/runtime-core/macros.h>
#include <ghoti.io/runtime-core/a/activation.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Names a live boundary activation. */
typedef struct GRCORE_BoundaryRef { uint64_t id; } GRCORE_BoundaryRef;

/** @brief Opens a re-entry activation with an instance token resolved at call time.
 *
 * A resumable boundary may pause only when every currently nested activation
 * is itself such a boundary. The engine must save its continuation and return
 * through every C frame before reporting GRCORE_STEP_PAUSED. `instance_token`
 * is opaque engine-owned identity, never an address or heap pointer.
 */
GRCORE_API GRCORE_Result grcore_boundary_enter(GRCORE_Stack * stack,
    GRCORE_EngineId caller, GRCORE_EngineId callee, uintptr_t instance_token,
    bool resumable, GRCORE_BoundaryRef * out_ref);

/** @brief Leaves the innermost boundary activation. */
GRCORE_API GRCORE_Result grcore_boundary_leave(GRCORE_Stack * stack,
    GRCORE_BoundaryRef ref);

/** @brief Resolves the innermost active instance token for an engine. */
GRCORE_API GRCORE_Result grcore_boundary_active_instance(
    const GRCORE_Stack * stack, GRCORE_EngineId engine,
    uintptr_t * out_instance_token);

/** @brief Stores the originating engine and opaque identity of a foreign
 * exception on a live boundary, preserving it while frames suspend/unwind.
 * The engine remains responsible for rooting the identity token's referent.
 */
GRCORE_API GRCORE_Result grcore_boundary_set_foreign_exception(
    GRCORE_Stack * stack, GRCORE_BoundaryRef ref, GRCORE_EngineId engine,
    uint64_t identity);

/** @brief Reads the foreign exception identity stored on a boundary. */
GRCORE_API GRCORE_Result grcore_boundary_get_foreign_exception(
    const GRCORE_Stack * stack, GRCORE_BoundaryRef ref,
    GRCORE_EngineId * out_engine, uint64_t * out_identity);

#ifdef __cplusplus
}
#endif
#endif
