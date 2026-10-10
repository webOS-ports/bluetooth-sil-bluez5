// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

// GLib source wrappers: a callback that asks to run again keeps its wrapper (under AddressSanitizer a freed one is
// reported), and glibTimeoutAdd frees its callback when the source ends or is removed.

#include <gtest/gtest.h>

#include <memory>

#include "asyncutils.h"

namespace {

void runUntil(const std::function<bool()> &done)
{
	gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	while (!done() && g_get_monotonic_time() < end)
		g_main_context_iteration(nullptr, TRUE);
}

} // namespace

TEST(AsyncUtils, SourceWrapperRepeatsWhileAsked)
{
	int calls = 0;
	g_timeout_add(1, glibSourceMethodWrapper, new GlibSourceFunctionWrapper([&calls]() { return ++calls < 3; }));
	runUntil([&calls]() { return calls >= 3; });
	EXPECT_EQ(calls, 3);
}

TEST(AsyncUtils, TimeoutAddFreesItsCallbackWhenDone)
{
	auto token = std::make_shared<int>(0);
	int calls = 0;
	glibTimeoutAdd(1, [token, &calls]() { return ++calls < 2; });
	EXPECT_EQ(token.use_count(), 2);
	runUntil([&calls]() { return calls >= 2; });
	g_main_context_iteration(nullptr, FALSE);
	EXPECT_EQ(calls, 2);
	EXPECT_EQ(token.use_count(), 1);
}

TEST(AsyncUtils, TimeoutAddFreesItsCallbackWhenRemoved)
{
	auto token = std::make_shared<int>(0);
	guint id = glibTimeoutAdd(60000, [token]() { return false; });
	EXPECT_EQ(token.use_count(), 2);
	g_source_remove(id);
	EXPECT_EQ(token.use_count(), 1);
}
