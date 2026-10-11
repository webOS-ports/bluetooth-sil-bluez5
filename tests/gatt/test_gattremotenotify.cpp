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

// StartNotify and StopNotify of a remote characteristic against a fake bluetoothd on a private bus (GTestDBus). The
// fake keeps one notify session per D-Bus client as BlueZ 5.86 does (src/gatt-client.c): a StartNotify while the
// client's session waits for the device fails at once with org.bluez.Error.InProgress, one on an active session
// succeeds at once, and a StopNotify without a session fails. It holds StartNotify and StopNotify until a test
// lets them go, as BlueZ holds StartNotify until the device has acknowledged its CCCD write.

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <gio/gio.h>

#include "bluez5gattremoteattribute.h"
#include "bluez5profilegatt.h"

// The fake sends no property changes: the profile is never used.
void Bluez5ProfileGatt::onCharacteristicPropertiesChanged(GattRemoteCharacteristic *, GVariant *)
{
}

namespace {

const char *CHAR_PATH = "/org/bluez/hci0/dev_00_11_22_33_44_55/service0010/char0011";

const char *INTROSPECTION =
	"<node>"
	"  <interface name='org.bluez.GattCharacteristic1'>"
	"    <method name='StartNotify'/>"
	"    <method name='StopNotify'/>"
	"  </interface>"
	"</node>";

// bluetoothd on its own thread and main context: the proxy is made synchronously (GetAll), which a server on the
// test's context could never answer.
class FakeBluez
{
public:
	enum class Session { None, Starting, Active };

	void start(const std::string &address)
	{
		mAddress = address;
		mThread = g_thread_new("fake-bluez", run, this);
		while (!mReady)
			g_usleep(1000);
	}

	void stop()
	{
		g_main_context_invoke(mContext, [](gpointer data) -> gboolean {
			g_main_loop_quit(static_cast<FakeBluez *>(data)->mLoop);
			return G_SOURCE_REMOVE;
		}, this);
		g_thread_join(mThread);
	}

	void reset()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mSession = Session::None;
		mCalls.clear();
		mInProgress = 0;
		mStopWhileStarting = 0;
		mNoSession = 0;
	}

	size_t held()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mHeld.size();
	}

	// The calls that reached the fake, in order.
	std::vector<std::string> calls()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mCalls;
	}

	Session session()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mSession;
	}

	int inProgress()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mInProgress;
	}

	int stopWhileStarting()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mStopWhileStarting;
	}

	// Answers the oldest held call: success, or a failure as BlueZ sends it when the device refused the write.
	void answer(bool ok)
	{
		GDBusMethodInvocation *invocation = nullptr;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			if (mHeld.empty())
				return;
			invocation = mHeld.front();
			mHeld.erase(mHeld.begin());
			const std::string method = g_dbus_method_invocation_get_method_name(invocation);
			if (method == "StartNotify" && mSession == Session::Starting)
				mSession = ok ? Session::Active : Session::None;
		}
		if (ok)
			g_dbus_method_invocation_return_value(invocation, nullptr);
		else
			g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed", "Operation failed with ATT error: 0x0e");
	}

private:
	static gpointer run(gpointer data)
	{
		auto self = static_cast<FakeBluez *>(data);
		self->mContext = g_main_context_new();
		g_main_context_push_thread_default(self->mContext);
		self->mLoop = g_main_loop_new(self->mContext, FALSE);

		GError *error = nullptr;
		GDBusConnection *conn = g_dbus_connection_new_for_address_sync(self->mAddress.c_str(),
			GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
								 G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
			nullptr, nullptr, &error);
		g_assert_no_error(error);
		GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(INTROSPECTION, &error);
		g_assert_no_error(error);

		static const GDBusInterfaceVTable vtable = { handleMethod, nullptr, nullptr, { nullptr } };
		guint id = g_dbus_connection_register_object(conn, CHAR_PATH,
			g_dbus_node_info_lookup_interface(node, "org.bluez.GattCharacteristic1"), &vtable, self, nullptr, &error);
		g_assert_no_error(error);
		GVariant *owned = g_dbus_connection_call_sync(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.bluez", 0u), G_VARIANT_TYPE("(u)"),
			G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
		g_assert_no_error(error);
		g_variant_unref(owned);

		self->mReady = true;
		g_main_loop_run(self->mLoop);

		{
			std::lock_guard<std::mutex> lock(self->mMutex);
			for (auto invocation : self->mHeld)
				g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed", "gone");
			self->mHeld.clear();
		}
		g_dbus_connection_unregister_object(conn, id);
		g_dbus_node_info_unref(node);
		g_dbus_connection_flush_sync(conn, nullptr, nullptr);
		g_object_unref(conn);
		g_main_loop_unref(self->mLoop);
		g_main_context_pop_thread_default(self->mContext);
		g_main_context_unref(self->mContext);
		return nullptr;
	}

	// One client (the SIL) on the bus: its session, as characteristic_start_notify and characteristic_stop_notify
	// keep it per sender.
	static void handleMethod(GDBusConnection *, const gchar *, const gchar *, const gchar *,
			const gchar *method, GVariant *, GDBusMethodInvocation *invocation, gpointer data)
	{
		auto self = static_cast<FakeBluez *>(data);
		const std::string name = method;
		std::lock_guard<std::mutex> lock(self->mMutex);
		self->mCalls.push_back(name);
		if (name == "StartNotify")
		{
			if (self->mSession == Session::Starting)
			{
				++self->mInProgress;
				g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InProgress", "In Progress");
				return;
			}
			if (self->mSession == Session::Active)
			{
				g_dbus_method_invocation_return_value(invocation, nullptr);
				return;
			}
			self->mSession = Session::Starting;
		}
		else	// StopNotify
		{
			if (self->mSession == Session::None)
			{
				++self->mNoSession;
				g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed", "No notify session started");
				return;
			}
			// BlueZ drops the session under a StartNotify still waiting for the device.
			if (self->mSession == Session::Starting)
				++self->mStopWhileStarting;
			self->mSession = Session::None;
		}
		self->mHeld.push_back(invocation);
	}

	std::string mAddress;
	GThread *mThread = nullptr;
	GMainContext *mContext = nullptr;
	GMainLoop *mLoop = nullptr;
	std::atomic<bool> mReady { false };
	std::mutex mMutex;
	std::vector<GDBusMethodInvocation *> mHeld;
	std::vector<std::string> mCalls;
	Session mSession = Session::None;
	int mInProgress = 0;
	int mStopWhileStarting = 0;
	int mNoSession = 0;
};

GTestDBus *testBus;
FakeBluez *fakeBluez;

class BusEnvironment : public ::testing::Environment
{
public:
	void SetUp() override
	{
		testBus = g_test_dbus_new(G_TEST_DBUS_NONE);
		g_test_dbus_up(testBus);
		g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(testBus), TRUE);
		fakeBluez = new FakeBluez;
		fakeBluez->start(g_test_dbus_get_bus_address(testBus));
	}

	void TearDown() override
	{
		fakeBluez->stop();
		delete fakeBluez;
		g_test_dbus_stop(testBus);
		g_object_unref(testBus);
	}
};

const auto *environment = ::testing::AddGlobalTestEnvironment(new BusEnvironment);

bool runUntil(const std::function<bool()> &done)
{
	gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	while (!done() && g_get_monotonic_time() < end)
	{
		while (g_main_context_iteration(nullptr, FALSE))
			;
		g_usleep(1000);
	}
	return done();
}

// Lets the calls before this point reach the fake and their replies come back.
void settle()
{
	gint64 end = g_get_monotonic_time() + 100 * 1000;
	while (g_get_monotonic_time() < end)
	{
		while (g_main_context_iteration(nullptr, FALSE))
			;
		g_usleep(1000);
	}
}

struct Answers
{
	std::vector<bool> results;

	GattRemoteResultCallback callback()
	{
		return [this](bool success) { results.push_back(success); };
	}
};

class GattRemoteNotify : public ::testing::Test
{
protected:
	void SetUp() override
	{
		fakeBluez->reset();
		GError *error = nullptr;
		BluezGattCharacteristic1 *proxy = bluez_gatt_characteristic1_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
			G_DBUS_PROXY_FLAGS_NONE, "org.bluez", CHAR_PATH, nullptr, &error);
		ASSERT_EQ(error, nullptr);
		mCharacteristic = new GattRemoteCharacteristic(proxy, nullptr);	// owns the proxy
		mCharacteristic->objectPath = CHAR_PATH;
	}

	void TearDown() override
	{
		while (fakeBluez->held())
		{
			fakeBluez->answer(true);
			settle();
		}
		delete mCharacteristic;
		settle();
	}

	GattRemoteCharacteristic *mCharacteristic = nullptr;
	// Here, not in the tests: a test that stops early leaves calls whose replies come in TearDown.
	Answers mFirst, mSecond, mThird;
};

} // namespace

// Two subscribers while the device has not acknowledged the first StartNotify: one call, both told it worked. The
// second StartNotify used to be sent and failed with InProgress.
TEST_F(GattRemoteNotify, TwoEnablesDuringOnePendingCall)
{
	Answers &first = mFirst, &second = mSecond;
	mCharacteristic->startNotify(first.callback());
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	mCharacteristic->startNotify(second.callback());
	settle();
	EXPECT_EQ(fakeBluez->inProgress(), 0);
	EXPECT_TRUE(second.results.empty());

	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([&]() { return !second.results.empty(); }));
	settle();
	EXPECT_EQ(first.results, std::vector<bool>({ true }));
	EXPECT_EQ(second.results, std::vector<bool>({ true }));
	EXPECT_EQ(fakeBluez->calls(), std::vector<std::string>({ "StartNotify" }));
	EXPECT_EQ(fakeBluez->session(), FakeBluez::Session::Active);
}

TEST_F(GattRemoteNotify, ErrorReplyReachesBoth)
{
	Answers &first = mFirst, &second = mSecond;
	mCharacteristic->startNotify(first.callback());
	mCharacteristic->startNotify(second.callback());
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	fakeBluez->answer(false);
	ASSERT_TRUE(runUntil([&]() { return !second.results.empty(); }));
	settle();
	EXPECT_EQ(first.results, std::vector<bool>({ false }));
	EXPECT_EQ(second.results, std::vector<bool>({ false }));
	EXPECT_EQ(fakeBluez->inProgress(), 0);
	EXPECT_EQ(fakeBluez->session(), FakeBluez::Session::None);
}

// The last subscriber went while StartNotify waited: the StopNotify goes after it, and notifications end off. Sent
// alongside, it removed the session under the pending StartNotify.
TEST_F(GattRemoteNotify, EnableThenDisableBeforeTheReply)
{
	Answers &enable = mFirst, &disable = mSecond;
	mCharacteristic->startNotify(enable.callback());
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	mCharacteristic->stopNotify(disable.callback());
	settle();
	EXPECT_EQ(fakeBluez->calls(), std::vector<std::string>({ "StartNotify" }));

	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	EXPECT_EQ(fakeBluez->calls(), std::vector<std::string>({ "StartNotify", "StopNotify" }));
	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([&]() { return !disable.results.empty(); }));
	settle();
	EXPECT_EQ(enable.results, std::vector<bool>({ true }));
	EXPECT_EQ(disable.results, std::vector<bool>({ true }));
	EXPECT_EQ(fakeBluez->stopWhileStarting(), 0);
	EXPECT_EQ(fakeBluez->session(), FakeBluez::Session::None);
}

TEST_F(GattRemoteNotify, DisableThenEnable)
{
	Answers &start = mFirst, &disable = mSecond, &enable = mThird;
	mCharacteristic->startNotify(start.callback());
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([&]() { return !start.results.empty(); }));

	mCharacteristic->stopNotify(disable.callback());
	mCharacteristic->startNotify(enable.callback());
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	settle();
	EXPECT_EQ(fakeBluez->calls(), std::vector<std::string>({ "StartNotify", "StopNotify" }));
	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	EXPECT_EQ(fakeBluez->calls(), std::vector<std::string>({ "StartNotify", "StopNotify", "StartNotify" }));
	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([&]() { return !enable.results.empty(); }));
	settle();
	EXPECT_EQ(disable.results, std::vector<bool>({ true }));
	EXPECT_EQ(enable.results, std::vector<bool>({ true }));
	EXPECT_EQ(fakeBluez->session(), FakeBluez::Session::Active);
}

// The device's services went while StartNotify was out and a StopNotify waited: the reply answers each caller once,
// and nothing is sent for the deleted characteristic (under AddressSanitizer a reply that touched it is reported).
TEST_F(GattRemoteNotify, ReplyAfterDestruction)
{
	Answers &enable = mFirst, &joined = mSecond, &disable = mThird;
	mCharacteristic->startNotify(enable.callback());
	mCharacteristic->startNotify(joined.callback());
	mCharacteristic->stopNotify(disable.callback());
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	delete mCharacteristic;
	mCharacteristic = nullptr;

	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([&]() { return !disable.results.empty(); }));
	settle();
	EXPECT_EQ(enable.results, std::vector<bool>({ true }));
	EXPECT_EQ(joined.results, std::vector<bool>({ true }));
	EXPECT_EQ(disable.results, std::vector<bool>({ false }));
	EXPECT_EQ(fakeBluez->calls(), std::vector<std::string>({ "StartNotify" }));
	EXPECT_EQ(fakeBluez->held(), 0u);
}
