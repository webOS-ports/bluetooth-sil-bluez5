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

// The StartNotify and StopNotify calls of one remote characteristic, one at a time, kept apart from the D-Bus
// proxy so that they can be tested.
//
// BlueZ keeps one notify session per D-Bus client, and the SIL is one client for all its callers. While the
// session's StartNotify waits for the device to acknowledge its CCCD write, a second StartNotify from the same client
// fails with org.bluez.Error.InProgress, and a StopNotify removes the session under the pending call. So a second
// caller asking for the same state joins the call that is out and gets its result, and a caller asking for the
// other state waits for it and is then sent. The characteristic ends in the state the last caller asked for.

#ifndef GATTNOTIFY_H
#define GATTNOTIFY_H

#include <functional>
#include <memory>
#include <vector>

class GattNotifyQueue
{
public:
	typedef std::function<void(bool success)> Result;
	// Makes one call, StartNotify for enable, else StopNotify, and calls done once with its result.
	typedef std::function<void(bool enable, Result done)> Call;

	explicit GattNotifyQueue(Call call);
	// Nothing is sent afterwards. A call that is out still answers its callers when its reply comes; the callers
	// waiting for a call that will not be made are answered with a failure then.
	~GattNotifyQueue();
	GattNotifyQueue(const GattNotifyQueue &) = delete;
	GattNotifyQueue &operator=(const GattNotifyQueue &) = delete;

	// Asks for notifications on (enable) or off. callback is called once, when the call it joined or waits for has
	// been answered, with that call's result:
	// - with no call out, the call is made;
	// - with a call out for the same state and nothing waiting, it joins that call;
	// - with a call out for the other state, it waits, together with the callers after it that ask for the same
	//   state, and that call is made when the one out has been answered;
	// - when a caller asks for the state of the call out while others wait for the other state, it joins that call
	//   and the waiting call is not made: those waiting are answered with a failure when the call out is answered,
	//   as the characteristic does not end in their state.
	void request(bool enable, Result callback);

private:
	struct State
	{
		Call call;
		bool gone = false;
		bool busy = false;	// a call is out
		bool busyEnable = false;
		std::vector<Result> busyCallers;
		bool queued = false;	// a call to make after it
		bool queuedEnable = false;
		std::vector<Result> queuedCallers;
		std::vector<Result> supersededCallers;	// waited for a state given up meanwhile
	};

	static void send(const std::shared_ptr<State> &state, bool enable, std::vector<Result> callers);
	static void finished(const std::shared_ptr<State> &state, bool success);

	std::shared_ptr<State> mState;
};

#endif
