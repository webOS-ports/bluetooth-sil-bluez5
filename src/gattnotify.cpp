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

#include "gattnotify.h"

#include <utility>

GattNotifyQueue::GattNotifyQueue(Call call)
	: mState(std::make_shared<State>())
{
	mState->call = std::move(call);
}

GattNotifyQueue::~GattNotifyQueue()
{
	// The state stays with the call that is out, which answers everyone; the call function, which may use the
	// owner, is not used again.
	mState->gone = true;
	mState->call = nullptr;
}

void GattNotifyQueue::request(bool enable, Result callback)
{
	State &state = *mState;
	if (!state.busy)
	{
		send(mState, enable, std::vector<Result>{ std::move(callback) });
		return;
	}
	if (!state.queued)
	{
		if (enable == state.busyEnable)
		{
			state.busyCallers.push_back(std::move(callback));
			return;
		}
		state.queued = true;
		state.queuedEnable = enable;
		state.queuedCallers.push_back(std::move(callback));
		return;
	}
	if (enable == state.queuedEnable)
	{
		state.queuedCallers.push_back(std::move(callback));
		return;
	}
	// Back to the state of the call that is out: the waiting call is not needed any more.
	for (auto &waiting : state.queuedCallers)
		state.supersededCallers.push_back(std::move(waiting));
	state.queuedCallers.clear();
	state.queued = false;
	state.busyCallers.push_back(std::move(callback));
}

void GattNotifyQueue::send(const std::shared_ptr<State> &state, bool enable, std::vector<Result> callers)
{
	state->busy = true;
	state->busyEnable = enable;
	state->busyCallers = std::move(callers);
	// The reply keeps the state, not the queue: it may come after the queue has gone.
	state->call(enable, [state](bool success) { finished(state, success); });
}

void GattNotifyQueue::finished(const std::shared_ptr<State> &state, bool success)
{
	if (!state->busy)	// answered twice: ignored
		return;
	// Everything is settled before anyone is answered: a caller may ask again, or delete the queue.
	const std::vector<Result> answered = std::move(state->busyCallers);
	std::vector<Result> failed = std::move(state->supersededCallers);
	state->busyCallers.clear();
	state->supersededCallers.clear();
	state->busy = false;

	if (state->queued)
	{
		std::vector<Result> next = std::move(state->queuedCallers);
		state->queuedCallers.clear();
		state->queued = false;
		if (state->gone)
		{
			for (auto &waiting : next)
				failed.push_back(std::move(waiting));
		}
		else
			send(state, state->queuedEnable, std::move(next));
	}

	for (const auto &callback : answered)
		callback(success);
	for (const auto &callback : failed)
		callback(false);
}
