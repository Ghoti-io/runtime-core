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
 * @file
 *
 * The handle table, private part: the layout the group embeds, and the hooks
 * the group and the context call.
 */

#ifndef GHOTI_IO_GRCORE_B_HANDLES_INTERNAL_H
#define GHOTI_IO_GRCORE_B_HANDLES_INTERNAL_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>
#include <ghoti.io/runtime-core/b/handles.h>

#include <ghoti.io/cutil/mutex.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief "No entry", in a list link or the free list. */
#define GRCORE_HANDLE_NONE UINT32_MAX

typedef struct GRCORE_HandleParty GRCORE_HandleParty;

/**
 * @brief One entry. Free when `generation` is zero.
 *
 * A live entry is on its holder's `held` list and, while its owner lives, on
 * its owner's `owned` list; a tombstone is on the first only. The lists link
 * by index because the array moves when it grows.
 */
typedef struct GRCORE_HandleEntry {
  uint64_t value;        ///< The owner's root slot. Zero when not rooted.
  uint64_t generation;   ///< Zero: the entry is free.
  uint64_t owner_serial; ///< The owner's id; kept by a tombstone.
  uint64_t holder_serial;
  GRCORE_HandleParty * owner;  ///< NULL once the owner is gone.
  GRCORE_HandleParty * holder; ///< NULL only for a free entry.
  uint32_t owned_prev, owned_next;
  uint32_t held_prev, held_next;
  uint32_t next_free;    ///< The free list, for a free entry.
} GRCORE_HandleEntry;

/**
 * @brief A context's part in the table. Made on first use, freed when the
 *   context is destroyed. Individually allocated, because the root source it
 *   registers holds its address.
 */
struct GRCORE_HandleParty {
  GRCORE_HandleTable * table;
  uint64_t serial;       ///< Never reused.
  uint32_t slot;         ///< Index in the table's party array.
  uint32_t owned_head;   ///< Entries whose value is in this context's heap.
  uint32_t held_head;    ///< Entries this context was given a token for.
  size_t owned_count;
  size_t held_count;
  bool root_registered;
};

struct GRCORE_HandleTable {
  GRCORE_Group * group;
  GCU_MUTEX_T mutex;
  bool mutex_ready;
  uint32_t tag;               ///< Nonzero; per table.
  uint64_t next_generation;   ///< Next to issue; starts at 1.
  uint64_t next_serial;       ///< Next party serial; starts at 1.
  GRCORE_HandleEntry * entries;
  size_t entry_used;          ///< Slots ever handed out (a high-water mark).
  size_t entry_capacity;
  uint32_t free_head;
  size_t live;                ///< Entries in use, tombstones included.
  size_t tombstones;
  GRCORE_HandleParty ** parties;
  size_t party_capacity;
  size_t party_count;
  size_t limit_per_holder;
  size_t limit_total;
};

/** @brief Sets up the table a group embeds. */
GRCORE_Result grcore_handles_table_init(
    GRCORE_HandleTable * table, GRCORE_Group * group);

/** @brief Frees the table's memory. The group has no contexts left. */
void grcore_handles_table_destroy(GRCORE_HandleTable * table);

/**
 * @brief Called by context destroy, with the context still valid: owned
 *   entries become tombstones, held entries are released, the party is freed.
 */
void grcore_handles_context_destroyed(GRCORE_Context * context);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRCORE_B_HANDLES_INTERNAL_H */
