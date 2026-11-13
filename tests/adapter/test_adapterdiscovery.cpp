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

// The adapter's startDiscovery and cancelDiscovery against a fake bluetoothd on a private bus (GTestDBus). The fake
// keeps a discovery per D-Bus client as BlueZ 5.86 does (src/adapter.c): StartDiscovery from a client that has one
// fails with org.bluez.Error.InProgress, "Operation already in progress", and StopDiscovery from a client without
// one with org.bluez.Error.Failed, "No discovery started". It answers both at once and sends Discovering only when
// a test lets it, as bluetoothd's reply comes before its PropertiesChanged.

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <gio/gio.h>

#include "bluez5adapter.h"

namespace {

const char *ADAPTER_PATH = "/org/bluez/hci0";

const char *INTROSPECTION =
	"<node>"
	"  <interface name='org.bluez.Adapter1'>"
	"    <method name='StartDiscovery'/>"
	"    <method name='StopDiscovery'/>"
	"    <method name='SetDiscoveryFilter'>"
	"      <arg name='filter' type='a{sv}' direction='in'/>"
	"    </method>"
	"  </interface>"
	"</node>";

// bluetoothd on its own thread and main context: the adapter makes its proxies and calls synchronously, which a
// server on the test's context could never answer.
class FakeBluez
{
public:
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
		mOurDiscovery = false;
		mCalls.clear();
		mNoDiscovery = 0;
		mAlreadyStarted = 0;
		mFilterUuids.clear();
		mFailMethod.clear();
		mFailError.clear();
	}

	// The next call of method fails with this error, whatever the client's discovery.
	void failNext(const std::string &method, const std::string &name)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mFailMethod = method;
		mFailError = name;
	}

	// Discovering as bluetoothd sends it: when its stop has completed, or when another client starts one.
	void sendDiscovering(bool discovering)
	{
		GVariantBuilder changed;
		g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&changed, "{sv}", "Discovering", g_variant_new_boolean(discovering));
		g_dbus_connection_emit_signal(mConn, nullptr, ADAPTER_PATH, "org.freedesktop.DBus.Properties",
			"PropertiesChanged", g_variant_new("(sa{sv}as)", "org.bluez.Adapter1", &changed, nullptr), nullptr);
		g_dbus_connection_flush_sync(mConn, nullptr, nullptr);
	}

	// The calls that reached the fake, in order.
	std::vector<std::string> calls()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mCalls;
	}

	// StopDiscovery calls refused for a client without a discovery.
	int noDiscovery()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mNoDiscovery;
	}

	// StartDiscovery calls refused for a client that has a discovery.
	int alreadyStarted()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mAlreadyStarted;
	}

	// The number of UUIDs in each SetDiscoveryFilter.
	std::vector<size_t> filterUuids()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mFilterUuids;
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
		guint id = g_dbus_connection_register_object(conn, ADAPTER_PATH,
			g_dbus_node_info_lookup_interface(node, "org.bluez.Adapter1"), &vtable, self, nullptr, &error);
		g_assert_no_error(error);
		GVariant *owned = g_dbus_connection_call_sync(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.bluez", 0u), G_VARIANT_TYPE("(u)"),
			G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
		g_assert_no_error(error);
		g_variant_unref(owned);

		self->mConn = conn;
		self->mReady = true;
		g_main_loop_run(self->mLoop);

		g_dbus_connection_unregister_object(conn, id);
		g_dbus_node_info_unref(node);
		g_dbus_connection_flush_sync(conn, nullptr, nullptr);
		g_object_unref(conn);
		g_main_loop_unref(self->mLoop);
		g_main_context_pop_thread_default(self->mContext);
		g_main_context_unref(self->mContext);
		return nullptr;
	}

	// One client (the SIL) on the bus: its discovery, as start_discovery and stop_discovery keep it per sender.
	static void handleMethod(GDBusConnection *, const gchar *, const gchar *, const gchar *,
			const gchar *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
	{
		auto self = static_cast<FakeBluez *>(data);
		const std::string name = method;
		{
			std::lock_guard<std::mutex> lock(self->mMutex);
			self->mCalls.push_back(name);
			if (name == self->mFailMethod)
			{
				const std::string error = self->mFailError;
				self->mFailMethod.clear();
				self->mFailError.clear();
				g_dbus_method_invocation_return_dbus_error(invocation, error.c_str(), "Resource Not Ready");
				return;
			}
			if (name == "StartDiscovery")
			{
				if (self->mOurDiscovery)
				{
					++self->mAlreadyStarted;
					g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InProgress",
						"Operation already in progress");
					return;
				}
				self->mOurDiscovery = true;	// Discovering=true follows when the test sends it
			}
			else if (name == "StopDiscovery")
			{
				if (!self->mOurDiscovery)
				{
					++self->mNoDiscovery;
					g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed",
						"No discovery started");
					return;
				}
				self->mOurDiscovery = false;	// Discovering=false follows when the test sends it
			}
			else	// SetDiscoveryFilter
			{
				GVariant *filter = g_variant_get_child_value(parameters, 0);
				GVariant *uuids = g_variant_lookup_value(filter, "UUIDs", G_VARIANT_TYPE("as"));
				self->mFilterUuids.push_back(uuids ? g_variant_n_children(uuids) : 0);
				if (uuids)
					g_variant_unref(uuids);
				g_variant_unref(filter);
			}
		}
		g_dbus_method_invocation_return_value(invocation, nullptr);
	}

	std::string mAddress;
	GThread *mThread = nullptr;
	GMainContext *mContext = nullptr;
	GMainLoop *mLoop = nullptr;
	GDBusConnection *mConn = nullptr;
	std::atomic<bool> mReady { false };
	std::mutex mMutex;
	bool mOurDiscovery = false;
	std::vector<std::string> mCalls;
	int mNoDiscovery = 0;
	int mAlreadyStarted = 0;
	std::vector<size_t> mFilterUuids;
	std::string mFailMethod;
	std::string mFailError;
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

bool runUntil(const std::function<bool()> &done, int seconds = 5)
{
	gint64 end = g_get_monotonic_time() + gint64(seconds) * G_USEC_PER_SEC;
	while (!done() && g_get_monotonic_time() < end)
	{
		while (g_main_context_iteration(nullptr, FALSE))
			;
		g_usleep(1000);
	}
	return done();
}

// Lets the signals sent before this point reach the adapter.
void settle()
{
	gint64 end = g_get_monotonic_time() + G_USEC_PER_SEC / 10;
	while (g_get_monotonic_time() < end)
	{
		while (g_main_context_iteration(nullptr, FALSE))
			;
		g_usleep(1000);
	}
}

class DiscoveryObserver : public BluetoothAdapterStatusObserver
{
public:
	void discoveryStateChanged(bool active) override { states.push_back(active); }
	std::vector<bool> states;
};

struct Answers
{
	std::vector<BluetoothError> results;

	BluetoothResultCallback callback()
	{
		return [this](BluetoothError error) { results.push_back(error); };
	}
};

const std::vector<BluetoothError> NONE = { BLUETOOTH_ERROR_NONE };
const std::vector<BluetoothError> FAIL = { BLUETOOTH_ERROR_FAIL };

class AdapterDiscovery : public ::testing::Test
{
protected:
	void SetUp() override
	{
		fakeBluez->reset();
		mAdapter = new Bluez5Adapter(ADAPTER_PATH);
		mAdapter->registerObserver(&mObserver);
	}

	void TearDown() override
	{
		delete mAdapter;
		settle();
	}

	// Discovery started through the adapter, as adapter/startDiscovery does.
	void startDiscovery()
	{
		ASSERT_EQ(mAdapter->startDiscovery(), BLUETOOTH_ERROR_NONE);
		fakeBluez->sendDiscovering(true);
		ASSERT_TRUE(runUntil([this]() { return mObserver.states == std::vector<bool>({ true }); }));
	}

	Bluez5Adapter *mAdapter = nullptr;
	DiscoveryObserver mObserver;
	// Here, not in the tests: a test that stops early leaves callbacks the adapter's deletion drops.
	Answers mFirst, mSecond;
};

} // namespace

// Settings stops the search and pairs at once; bluetooth2's pair cancels discovery again before bluetoothd's
// Discovering=false for the first stop has come. The second StopDiscovery is refused ("No discovery started") and
// the pair failed with "Failed to stop discovery". Both now wait for Discovering=false.
TEST_F(AdapterDiscovery, SecondCancelWhileTheStopIsUnderWay)
{
	Answers &settings = mFirst, &pair = mSecond;
	startDiscovery();
	mAdapter->cancelDiscovery(settings.callback());
	mAdapter->cancelDiscovery(pair.callback());
	EXPECT_EQ(fakeBluez->calls(), std::vector<std::string>({ "StartDiscovery", "StopDiscovery", "StopDiscovery" }));
	EXPECT_EQ(fakeBluez->noDiscovery(), 1);
	EXPECT_TRUE(settings.results.empty());
	EXPECT_TRUE(pair.results.empty());

	fakeBluez->sendDiscovering(false);
	ASSERT_TRUE(runUntil([&]() { return !pair.results.empty(); }));
	settle();
	EXPECT_EQ(settings.results, NONE);
	EXPECT_EQ(pair.results, NONE);
	EXPECT_EQ(mObserver.states, std::vector<bool>({ true, false }));
}

// Discovering=false never comes (another client keeps discovery going): the timeout answers both.
TEST_F(AdapterDiscovery, SecondCancelAnsweredByTheTimeout)
{
	Answers &settings = mFirst, &pair = mSecond;
	startDiscovery();
	mAdapter->cancelDiscovery(settings.callback());
	mAdapter->cancelDiscovery(pair.callback());
	EXPECT_EQ(fakeBluez->noDiscovery(), 1);
	EXPECT_TRUE(pair.results.empty());

	ASSERT_TRUE(runUntil([&]() { return !pair.results.empty(); }, 8));
	EXPECT_EQ(settings.results, NONE);
	EXPECT_EQ(pair.results, NONE);
}

// Discovery of another client only, with a BlueZ filter of ours left set: the filter is still cleared, and the
// request waits for Discovering=false.
TEST_F(AdapterDiscovery, OtherClientsDiscoveryClearsOurFilter)
{
	Answers &cancel = mFirst;
	BluetoothLeServiceUuid uuid;
	uuid.setUuid("180d");
	BluetoothLeDiscoveryFilter filter;
	filter.setServiceUuid(uuid);
	const int32_t scanId = mAdapter->addLeDiscoveryFilter(filter);
	ASSERT_GT(scanId, 0);
	ASSERT_EQ(mAdapter->removeLeDiscoveryFilter(static_cast<uint32_t>(scanId)), BLUETOOTH_ERROR_NONE);
	ASSERT_EQ(fakeBluez->filterUuids(), std::vector<size_t>({ 1 }));

	fakeBluez->sendDiscovering(true);
	ASSERT_TRUE(runUntil([this]() { return mObserver.states == std::vector<bool>({ true }); }));
	mAdapter->cancelDiscovery(cancel.callback());
	EXPECT_EQ(fakeBluez->noDiscovery(), 1);
	EXPECT_EQ(fakeBluez->filterUuids(), std::vector<size_t>({ 1, 0 }));
	EXPECT_TRUE(cancel.results.empty());

	fakeBluez->sendDiscovering(false);
	ASSERT_TRUE(runUntil([&]() { return !cancel.results.empty(); }));
	EXPECT_EQ(cancel.results, NONE);
}

// Any other error is still a failure, at once.
TEST_F(AdapterDiscovery, OtherStopErrorFails)
{
	Answers &cancel = mFirst;
	startDiscovery();
	fakeBluez->failNext("StopDiscovery", "org.bluez.Error.NotReady");
	mAdapter->cancelDiscovery(cancel.callback());
	EXPECT_EQ(cancel.results, FAIL);
	EXPECT_EQ(fakeBluez->noDiscovery(), 0);

	// Not answered a second time when discovery ends after all.
	fakeBluez->sendDiscovering(false);
	ASSERT_TRUE(runUntil([this]() { return mObserver.states.size() == 2; }));
	settle();
	EXPECT_EQ(cancel.results, FAIL);
}

// Two adapter/startDiscovery in the same second: the second comes before bluetoothd's Discovering=true for the
// first, and is refused as this client already has a discovery. It used to fail with 115; it is the discovery
// asked for.
TEST_F(AdapterDiscovery, SecondStartWhileTheStartIsUnderWay)
{
	EXPECT_EQ(mAdapter->startDiscovery(), BLUETOOTH_ERROR_NONE);
	EXPECT_EQ(mAdapter->startDiscovery(), BLUETOOTH_ERROR_NONE);
	EXPECT_EQ(fakeBluez->calls(), std::vector<std::string>({ "StartDiscovery", "StartDiscovery" }));
	EXPECT_EQ(fakeBluez->alreadyStarted(), 1);

	// And it is the one discovery: one stop ends it.
	fakeBluez->sendDiscovering(true);
	ASSERT_TRUE(runUntil([this]() { return mObserver.states == std::vector<bool>({ true }); }));
	Answers &cancel = mFirst;
	mAdapter->cancelDiscovery(cancel.callback());
	fakeBluez->sendDiscovering(false);
	ASSERT_TRUE(runUntil([&]() { return !cancel.results.empty(); }));
	EXPECT_EQ(cancel.results, NONE);
	EXPECT_EQ(fakeBluez->noDiscovery(), 0);
}

// Any other StartDiscovery error is still a failure.
TEST_F(AdapterDiscovery, OtherStartErrorFails)
{
	fakeBluez->failNext("StartDiscovery", "org.bluez.Error.NotReady");
	EXPECT_EQ(mAdapter->startDiscovery(), BLUETOOTH_ERROR_FAIL);
	EXPECT_EQ(fakeBluez->alreadyStarted(), 0);
}
