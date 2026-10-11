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

// StartNotify and StopNotify of one characteristic, one at a time. A second caller while StartNotify waited for the
// device sent a second StartNotify, which BlueZ fails with InProgress for the same client; a StopNotify meanwhile
// removed the session under the pending call.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gattnotify.h"

namespace {

// The calls made, held until a test answers them.
struct FakeCalls
{
	struct Call
	{
		bool enable;
		GattNotifyQueue::Result done;
	};
	std::vector<Call> out;
	int made = 0;
	int maxOut = 0;

	GattNotifyQueue::Call function()
	{
		return [this](bool enable, GattNotifyQueue::Result done) {
			++made;
			out.push_back(Call{ enable, std::move(done) });
			maxOut = std::max(maxOut, static_cast<int>(out.size()));
		};
	}

	// Answers the oldest call out.
	void answer(bool success)
	{
		ASSERT_FALSE(out.empty());
		auto done = std::move(out.front().done);
		out.erase(out.begin());
		done(success);
	}
};

// One caller's answers.
struct Answers
{
	std::vector<bool> results;

	GattNotifyQueue::Result callback()
	{
		return [this](bool success) { results.push_back(success); };
	}
};

} // namespace

TEST(GattNotifyQueue, OneCallerIsSentAndAnswered)
{
	FakeCalls calls;
	GattNotifyQueue queue(calls.function());
	Answers a;
	queue.request(true, a.callback());
	ASSERT_EQ(calls.out.size(), 1u);
	EXPECT_TRUE(calls.out[0].enable);
	EXPECT_TRUE(a.results.empty());	// not before the reply
	calls.answer(true);
	EXPECT_EQ(a.results, std::vector<bool>({ true }));
}

// The second enable joins the StartNotify that is out: one call, both answered with its result.
TEST(GattNotifyQueue, TwoEnablesDuringOnePendingCall)
{
	FakeCalls calls;
	GattNotifyQueue queue(calls.function());
	Answers first, second;
	queue.request(true, first.callback());
	queue.request(true, second.callback());
	EXPECT_EQ(calls.made, 1);
	calls.answer(true);
	EXPECT_EQ(first.results, std::vector<bool>({ true }));
	EXPECT_EQ(second.results, std::vector<bool>({ true }));
	EXPECT_EQ(calls.made, 1);
	EXPECT_TRUE(calls.out.empty());
}

// The device refused the CCCD write: everyone who joined the call is told.
TEST(GattNotifyQueue, ErrorReplyReachesEveryoneWhoJoined)
{
	FakeCalls calls;
	GattNotifyQueue queue(calls.function());
	Answers first, second;
	queue.request(true, first.callback());
	queue.request(true, second.callback());
	calls.answer(false);
	EXPECT_EQ(first.results, std::vector<bool>({ false }));
	EXPECT_EQ(second.results, std::vector<bool>({ false }));
	EXPECT_EQ(calls.made, 1);

	// Nothing left stuck: the next request is sent.
	Answers third;
	queue.request(true, third.callback());
	ASSERT_EQ(calls.out.size(), 1u);
	calls.answer(true);
	EXPECT_EQ(third.results, std::vector<bool>({ true }));
}

// The last subscriber went before the device answered: StopNotify follows the StartNotify, not alongside it.
TEST(GattNotifyQueue, EnableThenDisableBeforeTheReply)
{
	FakeCalls calls;
	GattNotifyQueue queue(calls.function());
	Answers enable, disable;
	queue.request(true, enable.callback());
	queue.request(false, disable.callback());
	EXPECT_EQ(calls.made, 1);	// the StopNotify waits
	calls.answer(true);
	EXPECT_EQ(enable.results, std::vector<bool>({ true }));
	EXPECT_TRUE(disable.results.empty());
	ASSERT_EQ(calls.out.size(), 1u);
	EXPECT_FALSE(calls.out[0].enable);
	calls.answer(true);
	EXPECT_EQ(disable.results, std::vector<bool>({ true }));
	EXPECT_EQ(calls.made, 2);
	EXPECT_EQ(calls.maxOut, 1);
}

TEST(GattNotifyQueue, DisableThenEnable)
{
	FakeCalls calls;
	GattNotifyQueue queue(calls.function());
	Answers disable, enable, enableToo;
	queue.request(false, disable.callback());
	queue.request(true, enable.callback());
	queue.request(true, enableToo.callback());	// waits with the other enable
	EXPECT_EQ(calls.made, 1);
	calls.answer(true);
	EXPECT_EQ(disable.results, std::vector<bool>({ true }));
	ASSERT_EQ(calls.out.size(), 1u);
	EXPECT_TRUE(calls.out[0].enable);
	calls.answer(true);
	EXPECT_EQ(enable.results, std::vector<bool>({ true }));
	EXPECT_EQ(enableToo.results, std::vector<bool>({ true }));
	EXPECT_EQ(calls.made, 2);
	EXPECT_EQ(calls.maxOut, 1);
}

// A failed StartNotify does not keep the StopNotify waiting for it from being sent.
TEST(GattNotifyQueue, WaitingCallIsSentAfterAFailure)
{
	FakeCalls calls;
	GattNotifyQueue queue(calls.function());
	Answers enable, disable;
	queue.request(true, enable.callback());
	queue.request(false, disable.callback());
	calls.answer(false);
	EXPECT_EQ(enable.results, std::vector<bool>({ false }));
	ASSERT_EQ(calls.out.size(), 1u);
	EXPECT_FALSE(calls.out[0].enable);
	calls.answer(false);
	EXPECT_EQ(disable.results, std::vector<bool>({ false }));
}

// Enable, disable, enable while the first is out: the characteristic ends enabled with the one StartNotify, and the
// disable, whose state was given up before its call was made, is told it did not happen.
TEST(GattNotifyQueue, LastCallerWinsBackToThePendingState)
{
	FakeCalls calls;
	GattNotifyQueue queue(calls.function());
	Answers first, disable, last;
	queue.request(true, first.callback());
	queue.request(false, disable.callback());
	queue.request(true, last.callback());
	calls.answer(true);
	EXPECT_EQ(first.results, std::vector<bool>({ true }));
	EXPECT_EQ(last.results, std::vector<bool>({ true }));
	EXPECT_EQ(disable.results, std::vector<bool>({ false }));
	EXPECT_EQ(calls.made, 1);
	EXPECT_TRUE(calls.out.empty());
}

// And the other way round: disable out, enable asked, then disable again.
TEST(GattNotifyQueue, LastCallerWinsDisable)
{
	FakeCalls calls;
	GattNotifyQueue queue(calls.function());
	Answers first, enable, last;
	queue.request(false, first.callback());
	queue.request(true, enable.callback());
	queue.request(false, last.callback());
	calls.answer(true);
	EXPECT_EQ(first.results, std::vector<bool>({ true }));
	EXPECT_EQ(last.results, std::vector<bool>({ true }));
	EXPECT_EQ(enable.results, std::vector<bool>({ false }));
	EXPECT_EQ(calls.made, 1);
	EXPECT_TRUE(calls.out.empty());
}

// The characteristic (with its device or profile) went while StartNotify was out: the reply still answers the
// caller, once, and the StopNotify that waited is not sent but failed.
TEST(GattNotifyQueue, ReplyAfterDestruction)
{
	FakeCalls calls;
	Answers enable, disable;
	{
		GattNotifyQueue queue(calls.function());
		queue.request(true, enable.callback());
		queue.request(false, disable.callback());
	}
	ASSERT_EQ(calls.out.size(), 1u);
	calls.answer(true);
	EXPECT_EQ(enable.results, std::vector<bool>({ true }));
	EXPECT_EQ(disable.results, std::vector<bool>({ false }));
	EXPECT_EQ(calls.made, 1);
	EXPECT_TRUE(calls.out.empty());
}

TEST(GattNotifyQueue, ErrorReplyAfterDestruction)
{
	FakeCalls calls;
	Answers enable, joined;
	{
		GattNotifyQueue queue(calls.function());
		queue.request(true, enable.callback());
		queue.request(true, joined.callback());
	}
	calls.answer(false);
	EXPECT_EQ(enable.results, std::vector<bool>({ false }));
	EXPECT_EQ(joined.results, std::vector<bool>({ false }));
	EXPECT_EQ(calls.made, 1);
}

// A caller that asks again from its callback, or deletes the queue there, finds it settled.
TEST(GattNotifyQueue, CallbackMayAskAgainOrDeleteTheQueue)
{
	FakeCalls calls;
	auto queue = std::make_unique<GattNotifyQueue>(calls.function());
	Answers again;
	queue->request(true, [&](bool) { queue->request(false, again.callback()); });
	calls.answer(true);
	ASSERT_EQ(calls.out.size(), 1u);
	EXPECT_FALSE(calls.out[0].enable);

	int answered = 0;
	queue->request(true, [&](bool) {	// waits for the StopNotify
		++answered;
		queue.reset();
	});
	calls.answer(true);	// the StopNotify: the StartNotify goes out, then the callbacks run
	EXPECT_EQ(again.results, std::vector<bool>({ true }));
	ASSERT_EQ(calls.out.size(), 1u);
	calls.answer(true);
	EXPECT_EQ(answered, 1);
	EXPECT_EQ(queue, nullptr);
	EXPECT_EQ(calls.made, 3);
}
