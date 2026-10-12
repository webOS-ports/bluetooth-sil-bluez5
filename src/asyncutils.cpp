// Copyright (c) 2014-2018 LG Electronics, Inc.
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

#include "asyncutils.h"

void glibAsyncMethodWrapper(GObject *sourceObject, GAsyncResult *result, gpointer user_data)
{
	GlibAsyncFunctionWrapper *wrapper = static_cast<GlibAsyncFunctionWrapper*>(user_data);
	wrapper->call(sourceObject, result);
	delete wrapper;
}

gboolean glibSourceMethodWrapper(gpointer user_data)
{
	GlibSourceFunctionWrapper *wrapper = static_cast<GlibSourceFunctionWrapper*>(user_data);
	gboolean result = wrapper->call();
	// Kept while the callback asks to run again: it was freed after the first dispatch, so a repeating source ran
	// on freed memory.
	if (!result)
		delete wrapper;
	return result;
}

namespace {

gboolean ownedSourceDispatch(gpointer user_data)
{
	return static_cast<GlibSourceFunctionWrapper*>(user_data)->call();
}

void ownedSourceDestroy(gpointer user_data)
{
	delete static_cast<GlibSourceFunctionWrapper*>(user_data);
}

} // namespace

guint glibTimeoutAdd(guint interval, GlibSourceFunction func)
{
	return g_timeout_add_full(G_PRIORITY_DEFAULT, interval, ownedSourceDispatch,
	                          new GlibSourceFunctionWrapper(func), ownedSourceDestroy);
}

ResultJoin::ResultJoin(size_t count, std::function<void(bool allSucceeded)> done)
	: mState(std::make_shared<State>(State{count, true, std::move(done)}))
{
	if (count == 0)
	{
		auto finish = std::move(mState->done);
		mState->done = nullptr;
		finish(true);
	}
}

std::function<void(bool success)> ResultJoin::part() const
{
	return [state = mState](bool success) {
		if (state->pending == 0)
			return;
		state->allSucceeded = state->allSucceeded && success;
		if (--state->pending > 0)
			return;
		// Moved out first: done may drop the last reference to the parts, and so to the state.
		auto finish = std::move(state->done);
		state->done = nullptr;
		finish(state->allSucceeded);
	};
}
