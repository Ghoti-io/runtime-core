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
 * @file namespace.h
 * @stability stable
 *
 * Maps every public name of this library into its version namespace.
 *
 * Kept in one file rather than beside each declaration: a type rename has to
 * be in effect before any struct tag that uses the name, and an internal
 * header may define such a tag without including the public header that
 * declares the typedef.
 *
 * `make check-symbols` fails if an exported symbol is missing from this list.
 *
 * See CONVENTIONS.md section 4.
 */

#ifndef GHOTI_IO_GRCORE_NAMESPACE_H
#define GHOTI_IO_GRCORE_NAMESPACE_H

#include <ghoti.io/runtime-core/libver.h>

/// @cond HIDDEN_SYMBOLS

/* Public types, and the private ones an internal header may name through a
 * struct tag. GCU_* names are cutil's; cutil has already renamed them. */
#define GRCORE_Allocator GHOTIIO_RUNTIME_CORE(GRCORE_Allocator)
#define GRCORE_Cardinality GHOTIIO_RUNTIME_CORE(GRCORE_Cardinality)
#define GRCORE_Context GHOTIIO_RUNTIME_CORE(GRCORE_Context)
#define GRCORE_ContextConfig GHOTIIO_RUNTIME_CORE(GRCORE_ContextConfig)
#define GRCORE_ContextState GHOTIIO_RUNTIME_CORE(GRCORE_ContextState)
#define GRCORE_Counting GHOTIIO_RUNTIME_CORE(GRCORE_Counting)
#define GRCORE_Group GHOTIIO_RUNTIME_CORE(GRCORE_Group)
#define GRCORE_Key GHOTIIO_RUNTIME_CORE(GRCORE_Key)
#define GRCORE_Meter GHOTIIO_RUNTIME_CORE(GRCORE_Meter)
#define GRCORE_Options GHOTIIO_RUNTIME_CORE(GRCORE_Options)
#define GRCORE_PageProvider GHOTIIO_RUNTIME_CORE(GRCORE_PageProvider)
#define GRCORE_Phase GHOTIIO_RUNTIME_CORE(GRCORE_Phase)
#define GRCORE_Registration GHOTIIO_RUNTIME_CORE(GRCORE_Registration)
#define GRCORE_Result GHOTIIO_RUNTIME_CORE(GRCORE_Result)

/* Public functions, and the private ones the static archive carries. */
#define grcore_allocator_default GHOTIIO_RUNTIME_CORE(grcore_allocator_default)
#define grcore_context_acquire GHOTIIO_RUNTIME_CORE(grcore_context_acquire)
#define grcore_context_allocator GHOTIIO_RUNTIME_CORE(grcore_context_allocator)
#define grcore_context_create GHOTIIO_RUNTIME_CORE(grcore_context_create)
#define grcore_context_destroy GHOTIIO_RUNTIME_CORE(grcore_context_destroy)
#define grcore_context_fuel GHOTIIO_RUNTIME_CORE(grcore_context_fuel)
#define grcore_context_group GHOTIIO_RUNTIME_CORE(grcore_context_group)
#define grcore_context_guest_depth GHOTIIO_RUNTIME_CORE(grcore_context_guest_depth)
#define grcore_context_guest_state_readable GHOTIIO_RUNTIME_CORE(grcore_context_guest_state_readable)
#define grcore_context_is_owner GHOTIIO_RUNTIME_CORE(grcore_context_is_owner)
#define grcore_context_memory_blocks GHOTIIO_RUNTIME_CORE(grcore_context_memory_blocks)
#define grcore_context_memory_bytes GHOTIIO_RUNTIME_CORE(grcore_context_memory_bytes)
#define grcore_context_memory_in_use GHOTIIO_RUNTIME_CORE(grcore_context_memory_in_use)
#define grcore_context_memory_peak GHOTIIO_RUNTIME_CORE(grcore_context_memory_peak)
#define grcore_context_native_depth GHOTIIO_RUNTIME_CORE(grcore_context_native_depth)
#define grcore_context_options GHOTIIO_RUNTIME_CORE(grcore_context_options)
#define grcore_context_page_provider GHOTIIO_RUNTIME_CORE(grcore_context_page_provider)
#define grcore_context_park GHOTIIO_RUNTIME_CORE(grcore_context_park)
#define grcore_context_register GHOTIIO_RUNTIME_CORE(grcore_context_register)
#define grcore_context_registration GHOTIIO_RUNTIME_CORE(grcore_context_registration)
#define grcore_context_registration_count GHOTIIO_RUNTIME_CORE(grcore_context_registration_count)
#define grcore_context_release GHOTIIO_RUNTIME_CORE(grcore_context_release)
#define grcore_context_slot GHOTIIO_RUNTIME_CORE(grcore_context_slot)
#define grcore_context_state GHOTIIO_RUNTIME_CORE(grcore_context_state)
#define grcore_context_transition GHOTIIO_RUNTIME_CORE(grcore_context_transition)
#define grcore_context_unpark GHOTIIO_RUNTIME_CORE(grcore_context_unpark)
#define grcore_counting_init GHOTIIO_RUNTIME_CORE(grcore_counting_init)
#define grcore_group_allocator GHOTIIO_RUNTIME_CORE(grcore_group_allocator)
#define grcore_group_context_count GHOTIIO_RUNTIME_CORE(grcore_group_context_count)
#define grcore_group_create GHOTIIO_RUNTIME_CORE(grcore_group_create)
#define grcore_group_destroy GHOTIIO_RUNTIME_CORE(grcore_group_destroy)
#define grcore_group_enter GHOTIIO_RUNTIME_CORE(grcore_group_enter)
#define grcore_group_leave GHOTIIO_RUNTIME_CORE(grcore_group_leave)
#define grcore_group_memory_blocks GHOTIIO_RUNTIME_CORE(grcore_group_memory_blocks)
#define grcore_group_memory_in_use GHOTIIO_RUNTIME_CORE(grcore_group_memory_in_use)
#define grcore_group_memory_peak GHOTIIO_RUNTIME_CORE(grcore_group_memory_peak)
#define grcore_group_page_provider GHOTIIO_RUNTIME_CORE(grcore_group_page_provider)
#define grcore_meter_blocks GHOTIIO_RUNTIME_CORE(grcore_meter_blocks)
#define grcore_meter_in_use GHOTIIO_RUNTIME_CORE(grcore_meter_in_use)
#define grcore_meter_init GHOTIIO_RUNTIME_CORE(grcore_meter_init)
#define grcore_meter_peak GHOTIIO_RUNTIME_CORE(grcore_meter_peak)
#define grcore_options_clone GHOTIIO_RUNTIME_CORE(grcore_options_clone)
#define grcore_options_create GHOTIIO_RUNTIME_CORE(grcore_options_create)
#define grcore_options_destroy GHOTIIO_RUNTIME_CORE(grcore_options_destroy)
#define grcore_options_get_fuel GHOTIIO_RUNTIME_CORE(grcore_options_get_fuel)
#define grcore_options_get_guest_depth GHOTIIO_RUNTIME_CORE(grcore_options_get_guest_depth)
#define grcore_options_get_keyed GHOTIIO_RUNTIME_CORE(grcore_options_get_keyed)
#define grcore_options_get_memory_bytes GHOTIIO_RUNTIME_CORE(grcore_options_get_memory_bytes)
#define grcore_options_get_native_depth GHOTIIO_RUNTIME_CORE(grcore_options_get_native_depth)
#define grcore_options_set_fuel GHOTIIO_RUNTIME_CORE(grcore_options_set_fuel)
#define grcore_options_set_guest_depth GHOTIIO_RUNTIME_CORE(grcore_options_set_guest_depth)
#define grcore_options_set_keyed GHOTIIO_RUNTIME_CORE(grcore_options_set_keyed)
#define grcore_options_set_memory_bytes GHOTIIO_RUNTIME_CORE(grcore_options_set_memory_bytes)
#define grcore_options_set_native_depth GHOTIIO_RUNTIME_CORE(grcore_options_set_native_depth)
#define grcore_page_provider_default GHOTIIO_RUNTIME_CORE(grcore_page_provider_default)
#define grcore_result_string GHOTIIO_RUNTIME_CORE(grcore_result_string)
#define grcore_thread_id GHOTIIO_RUNTIME_CORE(grcore_thread_id)
#define grcore_version_number GHOTIIO_RUNTIME_CORE(grcore_version_number)
#define grcore_version_string GHOTIIO_RUNTIME_CORE(grcore_version_string)

/// @endcond

#endif /* GHOTI_IO_GRCORE_NAMESPACE_H */
