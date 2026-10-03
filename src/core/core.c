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
 * Result strings.
 *
 * The string table is indexed by the enum. A new result that is not added
 * here falls through to "Unknown error", and the unit test that walks
 * GRCORE_RESULT_COUNT rejects that.
 */

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/core.h>

const char * grcore_result_string(GRCORE_Result result) {
  switch (result) {
    case GRCORE_OK:
      return "No error";
    case GRCORE_ERR_IO:
      return "Input/output error";
    case GRCORE_ERR_FORMAT:
      return "Format not recognised";
    case GRCORE_ERR_UNSUPPORTED:
      return "Unsupported feature";
    case GRCORE_ERR_LIMIT:
      return "Limit exceeded";
    case GRCORE_ERR_CORRUPT:
      return "Corrupt input";
    case GRCORE_ERR_OOM:
      return "Out of memory";
    case GRCORE_ERR_INVALID:
      return "Invalid argument";
    case GRCORE_ERR_INTERNAL:
      return "Internal error";
    case GRCORE_ERR_GUEST:
      return "Guest code failed";
    case GRCORE_RESULT_COUNT:
      break;
  }
  return "Unknown error";
}
