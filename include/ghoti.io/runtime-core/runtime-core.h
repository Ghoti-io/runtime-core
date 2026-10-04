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
 * @file runtime-core.h
 * @stability stable
 *
 * Umbrella header for Ghoti.io Runtime-core.
 *
 * The library is two layers with one include direction: A, the frame
 * protocol (`a/`, labelled `free`), may include B, the execution context
 * (`b/`, labelled `stable`), and the top-level shared basics; B and the
 * basics never include A. This is the only header that includes both, which
 * is why it sits at the top and is exempt from the direction gate.
 *
 * B is complete for now (`b/`: groups, contexts, options, keys, the page
 * provider with memory accounting behind them, requests and ports, the poll,
 * `run` and `resume`, the budgets with their fuel scopes, and root sources).
 * A is complete for now too (`a/`: engine descriptors, the guest stack, the
 * abstract frame with its walk and scopes, activation records, the unwinder,
 * budget scopes, the code-metadata format and the JIT layout descriptor).
 * What comes next is the collector, the JIT and the
 * debugger, each in a library of its own.
 */

#ifndef GHOTI_IO_GRCORE_RUNTIME_CORE_H
#define GHOTI_IO_GRCORE_RUNTIME_CORE_H

#include <ghoti.io/runtime-core/macros.h>

#include <ghoti.io/runtime-core/allocator.h>
#include <ghoti.io/runtime-core/core.h>
#include <ghoti.io/runtime-core/libver.h>

#include <ghoti.io/runtime-core/a/activation.h>
#include <ghoti.io/runtime-core/a/budget_scope.h>
#include <ghoti.io/runtime-core/a/code.h>
#include <ghoti.io/runtime-core/a/codemeta.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/frame.h>
#include <ghoti.io/runtime-core/a/layout.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/a/unwind.h>
#include <ghoti.io/runtime-core/b/budget.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>
#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/b/options.h>
#include <ghoti.io/runtime-core/b/page.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/b/request.h>
#include <ghoti.io/runtime-core/b/roots.h>
#include <ghoti.io/runtime-core/b/run.h>

#endif /* GHOTI_IO_GRCORE_RUNTIME_CORE_H */
