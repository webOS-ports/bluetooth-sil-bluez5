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

// The AVRCP browsing folder and player against a fake bluetoothd on a private bus (GTestDBus). The fake holds its
// replies to ChangeFolder, MediaItem1.Play, ListItems and the Set of a player setting until a test lets them go, as
// BlueZ does until the remote device has answered: the SIL must not wait for them, the folder must refuse a second
// request while one is out (BlueZ would fail it with EBUSY), and a reply that comes after the folder or player was
// deleted must not touch it. ChangeFolder and Play report the device's answer to a callback when given one.

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <gio/gio.h>

#include "bluez5mediafolder.h"
#include "bluez5mediaplayer.h"
#include "bluez5profileavrcp.h"

// The folder and player report through the profile only on property changes, which the fake never sends: the
// profile is never used.
std::string Bluez5ProfileAvcrp::getAdapterAddress()
{
	return "";
}

bool Bluez5ProfileAvcrp::isDeviceConnected(const std::string &) const
{
	return false;
}

void Bluez5ProfileAvcrp::updatePlayerInfo(const std::string &)
{
}

namespace {

const char *PLAYER_PATH = "/org/bluez/hci0/dev_00_11_22_33_44_55/player0";
const char *FOLDER_ITEM = "/org/bluez/hci0/dev_00_11_22_33_44_55/player0/Filesystem/item1";
const char *AUDIO_ITEM = "/org/bluez/hci0/dev_00_11_22_33_44_55/player0/Filesystem/item2";
// A player whose org.freedesktop.DBus.Properties the fake handles itself, to hold a Set.
const char *SETTINGS_PLAYER_PATH = "/org/bluez/hci0/dev_00_11_22_33_44_55/player1";

const char *INTROSPECTION =
	"<node>"
	"  <interface name='org.bluez.MediaFolder1'>"
	"    <method name='Search'>"
	"      <arg name='string' type='s' direction='in'/>"
	"      <arg name='filter' type='a{sv}' direction='in'/>"
	"      <arg name='folder' type='o' direction='out'/>"
	"    </method>"
	"    <method name='ListItems'>"
	"      <arg name='filter' type='a{sv}' direction='in'/>"
	"      <arg name='items' type='a{oa{sv}}' direction='out'/>"
	"    </method>"
	"    <method name='ChangeFolder'>"
	"      <arg name='folder' type='o' direction='in'/>"
	"    </method>"
	"    <property name='NumberOfItems' type='u' access='read'/>"
	"  </interface>"
	"  <interface name='org.bluez.MediaItem1'>"
	"    <method name='Play'/>"
	"    <method name='AddtoNowPlaying'/>"
	"    <property name='Type' type='s' access='read'/>"
	"    <property name='Playable' type='b' access='read'/>"
	"  </interface>"
	"  <interface name='org.freedesktop.DBus.Properties'>"
	"    <method name='Get'>"
	"      <arg name='interface' type='s' direction='in'/>"
	"      <arg name='name' type='s' direction='in'/>"
	"      <arg name='value' type='v' direction='out'/>"
	"    </method>"
	"    <method name='GetAll'>"
	"      <arg name='interface' type='s' direction='in'/>"
	"      <arg name='properties' type='a{sv}' direction='out'/>"
	"    </method>"
	"    <method name='Set'>"
	"      <arg name='interface' type='s' direction='in'/>"
	"      <arg name='name' type='s' direction='in'/>"
	"      <arg name='value' type='v' direction='in'/>"
	"    </method>"
	"  </interface>"
	"</node>";

// bluetoothd: its own thread and main context, as the folder makes synchronous calls (proxy set-up, GetAll) that
// a server on the test's context could never answer.
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

	// Calls held, as BlueZ holds them until the device answers.
	size_t held()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mHeld.size();
	}

	std::string lastMethod()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mLastMethod;
	}

	// The settings asked for, "Name=value", in order.
	std::vector<std::string> sets()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mSets;
	}

	void clearSets()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mSets.clear();
	}

	// Answers the oldest held call: an empty success, or a failure as BlueZ sends it.
	void answer(bool ok)
	{
		GDBusMethodInvocation *invocation = nullptr;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			if (mHeld.empty())
				return;
			invocation = mHeld.front();
			mHeld.erase(mHeld.begin());
		}
		const std::string method = g_dbus_method_invocation_get_method_name(invocation);
		if (!ok)
			g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed", "Connection timed out");
		else if (method == "ListItems")
			g_dbus_method_invocation_return_value(invocation, g_variant_new_parsed("(@a{oa{sv}} {},)"));
		else
			g_dbus_method_invocation_return_value(invocation, nullptr);
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

		static const GDBusInterfaceVTable vtable = { handleMethod, handleGetProperty, nullptr, { nullptr } };
		std::vector<guint> ids;
		ids.push_back(g_dbus_connection_register_object(conn, PLAYER_PATH,
			g_dbus_node_info_lookup_interface(node, "org.bluez.MediaFolder1"), &vtable, self, nullptr, &error));
		g_assert_no_error(error);
		for (const char *item : { FOLDER_ITEM, AUDIO_ITEM })
		{
			ids.push_back(g_dbus_connection_register_object(conn, item,
				g_dbus_node_info_lookup_interface(node, "org.bluez.MediaItem1"), &vtable, self, nullptr, &error));
			g_assert_no_error(error);
		}
		ids.push_back(g_dbus_connection_register_object(conn, SETTINGS_PLAYER_PATH,
			g_dbus_node_info_lookup_interface(node, "org.freedesktop.DBus.Properties"), &vtable, self, nullptr,
			&error));
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
		for (auto id : ids)
			g_dbus_connection_unregister_object(conn, id);
		g_dbus_node_info_unref(node);
		g_dbus_connection_flush_sync(conn, nullptr, nullptr);
		g_object_unref(conn);
		g_main_loop_unref(self->mLoop);
		g_main_context_pop_thread_default(self->mContext);
		g_main_context_unref(self->mContext);
		return nullptr;
	}

	static void handleMethod(GDBusConnection *, const gchar *, const gchar *, const gchar *,
			const gchar *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
	{
		auto self = static_cast<FakeBluez *>(data);
		const std::string name = method;
		if (name == "AddtoNowPlaying")	// answered at once, as bluetoothd does
		{
			g_dbus_method_invocation_return_value(invocation, nullptr);
			return;
		}
		if (name == "GetAll")	// the player's properties: none
		{
			g_dbus_method_invocation_return_value(invocation, g_variant_new_parsed("(@a{sv} {},)"));
			return;
		}
		if (name == "Get")
		{
			g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.InvalidArgs", "no");
			return;
		}
		std::lock_guard<std::mutex> lock(self->mMutex);
		self->mLastMethod = name;
		if (name == "Set")
		{
			const gchar *property = nullptr;
			GVariant *value = nullptr;
			g_variant_get(parameters, "(&s&sv)", nullptr, &property, &value);
			self->mSets.push_back(std::string(property) + "=" + g_variant_get_string(value, nullptr));
			g_variant_unref(value);
		}
		self->mHeld.push_back(invocation);
	}

	static GVariant *handleGetProperty(GDBusConnection *, const gchar *, const gchar *path, const gchar *,
			const gchar *property, GError **, gpointer)
	{
		const std::string name = property;
		if (name == "NumberOfItems")
			return g_variant_new_uint32(7);
		if (name == "Type")
			return g_variant_new_string(std::string(path) == FOLDER_ITEM ? "folder" : "audio");
		if (name == "Playable")
			return g_variant_new_boolean(std::string(path) == AUDIO_ITEM);
		return nullptr;
	}

	std::string mAddress;
	GThread *mThread = nullptr;
	GMainContext *mContext = nullptr;
	GMainLoop *mLoop = nullptr;
	std::atomic<bool> mReady { false };
	std::mutex mMutex;
	std::vector<GDBusMethodInvocation *> mHeld;
	std::string mLastMethod;
	std::vector<std::string> mSets;
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
		// The folder talks to the system bus.
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

class MediaFolder : public ::testing::Test
{
protected:
	void TearDown() override
	{
		while (fakeBluez->held())
			fakeBluez->answer(true);
		settle();
	}
};

} // namespace

TEST_F(MediaFolder, NumberOfItemsIsReadAtOnce)
{
	Bluez5MediaFolder folder(nullptr, PLAYER_PATH);
	int calls = 0;
	uint32_t items = 0;
	folder.getNumberOfItems([&](BluetoothError error, uint32_t n) {
		++calls;
		EXPECT_EQ(error, BLUETOOTH_ERROR_NONE);
		items = n;
	});
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(items, 7u);
}

// The service's main loop went on while the device had not answered: the call returned before the reply.
TEST_F(MediaFolder, ChangeFolderDoesNotWaitForTheDevice)
{
	Bluez5MediaFolder folder(nullptr, PLAYER_PATH);
	gint64 start = g_get_monotonic_time();
	EXPECT_EQ(folder.changePath(FOLDER_ITEM), BLUETOOTH_ERROR_NONE);
	EXPECT_LT(g_get_monotonic_time() - start, G_USEC_PER_SEC);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	EXPECT_EQ(fakeBluez->lastMethod(), "ChangeFolder");

	// Refused here, as BlueZ would with EBUSY, while the first waits.
	EXPECT_EQ(folder.changePath(FOLDER_ITEM), BLUETOOTH_ERROR_BUSY);
	EXPECT_EQ(folder.playItem(AUDIO_ITEM), BLUETOOTH_ERROR_BUSY);
	int calls = 0;
	folder.getFolderItems(0, 9, [&calls](BluetoothError error, BluetoothFolderItemList) {
		++calls;
		EXPECT_EQ(error, BLUETOOTH_ERROR_BUSY);
	});
	EXPECT_EQ(calls, 1);
	folder.search("x", [&calls](BluetoothError error, const std::string &) {
		++calls;
		EXPECT_EQ(error, BLUETOOTH_ERROR_BUSY);
	});
	EXPECT_EQ(calls, 2);
	EXPECT_EQ(fakeBluez->held(), 1u);

	fakeBluez->answer(true);
	settle();
	EXPECT_EQ(folder.changePath(FOLDER_ITEM), BLUETOOTH_ERROR_NONE);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
}

// The device refused or did not answer in time: the next request is not refused for ever.
TEST_F(MediaFolder, FailedReplyEndsTheRequest)
{
	Bluez5MediaFolder folder(nullptr, PLAYER_PATH);
	EXPECT_EQ(folder.playItem(AUDIO_ITEM), BLUETOOTH_ERROR_NONE);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	EXPECT_EQ(fakeBluez->lastMethod(), "Play");
	fakeBluez->answer(false);
	settle();
	EXPECT_EQ(folder.playItem(AUDIO_ITEM), BLUETOOTH_ERROR_NONE);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
}

// A listing out holds the folder in BlueZ as well: a ChangeFolder sent meanwhile would fail there unseen.
TEST_F(MediaFolder, ListingHoldsTheFolderToo)
{
	Bluez5MediaFolder folder(nullptr, PLAYER_PATH);
	int calls = 0;
	BluetoothError result = BLUETOOTH_ERROR_FAIL;
	folder.getFolderItems(0, 9, [&](BluetoothError error, BluetoothFolderItemList) {
		++calls;
		result = error;
	});
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	EXPECT_EQ(fakeBluez->lastMethod(), "ListItems");
	EXPECT_EQ(folder.changePath(FOLDER_ITEM), BLUETOOTH_ERROR_BUSY);
	EXPECT_EQ(calls, 0);

	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([&calls]() { return calls > 0; }));
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(result, BLUETOOTH_ERROR_NONE);
	EXPECT_EQ(folder.changePath(FOLDER_ITEM), BLUETOOTH_ERROR_NONE);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
}

// The service answers its caller with what the device answered: success or failure, once, and only then.
TEST_F(MediaFolder, ChangeFolderAndPlayReportTheDevicesAnswer)
{
	Bluez5MediaFolder folder(nullptr, PLAYER_PATH);
	for (bool ok : { true, false })
	{
		int calls = 0;
		BluetoothError result = BLUETOOTH_ERROR_PARAM_INVALID;
		auto done = [&](BluetoothError error) {
			++calls;
			result = error;
		};
		EXPECT_EQ(folder.changePath(FOLDER_ITEM, done), BLUETOOTH_ERROR_NONE);
		ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
		EXPECT_EQ(fakeBluez->lastMethod(), "ChangeFolder");
		settle();
		EXPECT_EQ(calls, 0);	// not before the device has answered
		fakeBluez->answer(ok);
		ASSERT_TRUE(runUntil([&calls]() { return calls > 0; }));
		settle();
		EXPECT_EQ(calls, 1);
		EXPECT_EQ(result, ok ? BLUETOOTH_ERROR_NONE : BLUETOOTH_ERROR_FAIL);

		calls = 0;
		result = BLUETOOTH_ERROR_PARAM_INVALID;
		EXPECT_EQ(folder.playItem(AUDIO_ITEM, done), BLUETOOTH_ERROR_NONE);
		ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
		EXPECT_EQ(fakeBluez->lastMethod(), "Play");
		settle();
		EXPECT_EQ(calls, 0);
		fakeBluez->answer(ok);
		ASSERT_TRUE(runUntil([&calls]() { return calls > 0; }));
		settle();
		EXPECT_EQ(calls, 1);
		EXPECT_EQ(result, ok ? BLUETOOTH_ERROR_NONE : BLUETOOTH_ERROR_FAIL);
	}
}

// A request that was not sent is answered by the return value alone: the callback is not called as well.
TEST_F(MediaFolder, RequestNotSentDoesNotCallBack)
{
	Bluez5MediaFolder folder(nullptr, PLAYER_PATH);
	int calls = 0;
	auto done = [&calls](BluetoothError) { ++calls; };
	EXPECT_EQ(folder.changePath(AUDIO_ITEM, done), BLUETOOTH_ERROR_AVRCP_NOT_A_FOLDER);
	EXPECT_EQ(folder.playItem(FOLDER_ITEM, done), BLUETOOTH_ERROR_AVRCP_ITEM_NOT_PLAYABLE);
	EXPECT_EQ(folder.changePath(FOLDER_ITEM, done), BLUETOOTH_ERROR_NONE);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	EXPECT_EQ(folder.changePath(FOLDER_ITEM, done), BLUETOOTH_ERROR_BUSY);
	EXPECT_EQ(folder.playItem(AUDIO_ITEM, done), BLUETOOTH_ERROR_BUSY);
	settle();
	EXPECT_EQ(calls, 0);
	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([&calls]() { return calls > 0; }));
	settle();
	EXPECT_EQ(calls, 1);	// the one that went out
}

// The phone switched player before it answered: the caller still gets the answer, once, and the deleted folder is
// not touched (AddressSanitizer reports it if it is).
TEST_F(MediaFolder, AnswerReachesTheCallerAfterTheFolderWent)
{
	auto folder = new Bluez5MediaFolder(nullptr, PLAYER_PATH);
	int calls = 0;
	BluetoothError result = BLUETOOTH_ERROR_NONE;
	EXPECT_EQ(folder->playItem(AUDIO_ITEM, [&](BluetoothError error) {
		++calls;
		result = error;
	}), BLUETOOTH_ERROR_NONE);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	delete folder;
	fakeBluez->answer(false);
	ASSERT_TRUE(runUntil([&calls]() { return calls > 0; }));
	settle();
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(result, BLUETOOTH_ERROR_FAIL);
}

TEST_F(MediaFolder, ItemChecksStayImmediate)
{
	Bluez5MediaFolder folder(nullptr, PLAYER_PATH);
	EXPECT_EQ(folder.changePath(AUDIO_ITEM), BLUETOOTH_ERROR_AVRCP_NOT_A_FOLDER);
	EXPECT_EQ(folder.playItem(FOLDER_ITEM), BLUETOOTH_ERROR_AVRCP_ITEM_NOT_PLAYABLE);
	EXPECT_EQ(folder.addToNowPlaying(AUDIO_ITEM), BLUETOOTH_ERROR_NONE);	// answered by bluetoothd at once
	settle();
	EXPECT_EQ(fakeBluez->held(), 0u);
}

// The phone switched player while a request was out: the reply comes to a deleted folder. Under AddressSanitizer
// a reply that touched it is reported.
TEST_F(MediaFolder, ReplyAfterTheFolderWentTouchesNothing)
{
	auto folder = new Bluez5MediaFolder(nullptr, PLAYER_PATH);
	EXPECT_EQ(folder->changePath(FOLDER_ITEM), BLUETOOTH_ERROR_NONE);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	delete folder;
	fakeBluez->answer(true);
	settle();

	folder = new Bluez5MediaFolder(nullptr, PLAYER_PATH);
	int calls = 0;
	BluetoothError result = BLUETOOTH_ERROR_FAIL;
	folder->getFolderItems(0, 9, [&](BluetoothError error, BluetoothFolderItemList) {
		++calls;
		result = error;
	});
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	delete folder;
	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([&calls]() { return calls > 0; }));
	settle();
	EXPECT_EQ(calls, 1);	// answered once, though the folder went
	EXPECT_EQ(result, BLUETOOTH_ERROR_NONE);
}

namespace {

class MediaPlayer : public ::testing::Test
{
protected:
	void SetUp() override
	{
		fakeBluez->clearSets();
		GError *error = nullptr;
		mBus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
		ASSERT_EQ(error, nullptr);
		// An object with no interfaces: a player without a browsing folder.
		mObject = G_DBUS_OBJECT(g_dbus_object_proxy_new(mBus, SETTINGS_PLAYER_PATH));
	}

	void TearDown() override
	{
		while (fakeBluez->held())
			fakeBluez->answer(true);
		settle();
		g_object_unref(mObject);
		g_object_unref(mBus);
	}

	static BluetoothPlayerApplicationSettingsPropertiesList repeatAndShuffle()
	{
		BluetoothPlayerApplicationSettingsPropertiesList properties;
		properties.push_back(BluetoothPlayerApplicationSettingsProperty(
			BluetoothPlayerApplicationSettingsProperty::Type::REPEAT,
			BluetoothPlayerApplicationSettingsRepeat::REPEAT_ALL_TRACKS));
		properties.push_back(BluetoothPlayerApplicationSettingsProperty(
			BluetoothPlayerApplicationSettingsProperty::Type::SHUFFLE,
			BluetoothPlayerApplicationSettingsShuffle::SHUFFLE_OFF));
		return properties;
	}

	GDBusConnection *mBus = nullptr;
	GDBusObject *mObject = nullptr;
};

} // namespace

TEST_F(MediaPlayer, NoSettingsIsDoneAtOnce)
{
	Bluez5MediaPlayer player(nullptr, mObject);
	int calls = 0;
	player.setPlayerApplicationSettingsProperties({}, [&calls](BluetoothError error) {
		++calls;
		EXPECT_EQ(error, BLUETOOTH_ERROR_NONE);
	});
	EXPECT_EQ(calls, 1);
}

// Each Set waits for the device in BlueZ: the call returns at once, and the settings go one after the other.
TEST_F(MediaPlayer, SettingsAreSetOneAfterTheOther)
{
	Bluez5MediaPlayer player(nullptr, mObject);
	int calls = 0;
	BluetoothError result = BLUETOOTH_ERROR_FAIL;
	player.setPlayerApplicationSettingsProperties(repeatAndShuffle(), [&](BluetoothError error) {
		++calls;
		result = error;
	});
	EXPECT_EQ(calls, 0);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	EXPECT_EQ(fakeBluez->sets(), std::vector<std::string>({ "Repeat=alltracks" }));

	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([]() { return fakeBluez->sets().size() == 2 && fakeBluez->held() == 1; }));
	EXPECT_EQ(fakeBluez->sets()[1], "Shuffle=off");
	EXPECT_EQ(calls, 0);

	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([&calls]() { return calls > 0; }));
	settle();
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(result, BLUETOOTH_ERROR_NONE);
}

TEST_F(MediaPlayer, FirstRefusedSettingEndsTheRequest)
{
	Bluez5MediaPlayer player(nullptr, mObject);
	int calls = 0;
	BluetoothError result = BLUETOOTH_ERROR_NONE;
	player.setPlayerApplicationSettingsProperties(repeatAndShuffle(), [&](BluetoothError error) {
		++calls;
		result = error;
	});
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	fakeBluez->answer(false);
	ASSERT_TRUE(runUntil([&calls]() { return calls > 0; }));
	settle();
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(result, BLUETOOTH_ERROR_FAIL);
	EXPECT_EQ(fakeBluez->sets().size(), 1u);	// Shuffle not tried
}

// The device went while a setting was waiting: the caller is answered once, and the deleted player is not used for
// the next setting (under AddressSanitizer that is reported).
TEST_F(MediaPlayer, PlayerGoneBeforeTheReply)
{
	auto player = new Bluez5MediaPlayer(nullptr, mObject);
	int calls = 0;
	BluetoothError result = BLUETOOTH_ERROR_NONE;
	player->setPlayerApplicationSettingsProperties(repeatAndShuffle(), [&](BluetoothError error) {
		++calls;
		result = error;
	});
	ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
	delete player;
	fakeBluez->answer(true);
	ASSERT_TRUE(runUntil([&calls]() { return calls > 0; }));
	settle();
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(result, BLUETOOTH_ERROR_FAIL);
	EXPECT_EQ(fakeBluez->sets().size(), 1u);
	EXPECT_EQ(fakeBluez->held(), 0u);
}

// Through the player, as the profile calls it: the item path is relative to the device, and the callback reaches
// the folder's request.
TEST_F(MediaPlayer, BrowsingThroughThePlayerReportsTheAnswer)
{
	GDBusObjectSkeleton *object = g_dbus_object_skeleton_new(PLAYER_PATH);
	BluezMediaFolder1 *folderInterface = bluez_media_folder1_skeleton_new();
	g_dbus_object_skeleton_add_interface(object, G_DBUS_INTERFACE_SKELETON(folderInterface));
	{
		Bluez5MediaPlayer player(nullptr, G_DBUS_OBJECT(object));
		const std::string folderItem = std::string(FOLDER_ITEM).substr(std::string(PLAYER_PATH).rfind('/') + 1);
		const std::string audioItem = std::string(AUDIO_ITEM).substr(std::string(PLAYER_PATH).rfind('/') + 1);
		int calls = 0;
		BluetoothError result = BLUETOOTH_ERROR_NONE;
		auto done = [&](BluetoothError error) {
			++calls;
			result = error;
		};
		EXPECT_EQ(player.changePath(folderItem, done), BLUETOOTH_ERROR_NONE);
		ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
		EXPECT_EQ(fakeBluez->lastMethod(), "ChangeFolder");
		fakeBluez->answer(false);
		ASSERT_TRUE(runUntil([&calls]() { return calls > 0; }));
		EXPECT_EQ(result, BLUETOOTH_ERROR_FAIL);

		EXPECT_EQ(player.playItem(audioItem, done), BLUETOOTH_ERROR_NONE);
		ASSERT_TRUE(runUntil([]() { return fakeBluez->held() == 1; }));
		EXPECT_EQ(fakeBluez->lastMethod(), "Play");
		fakeBluez->answer(true);
		ASSERT_TRUE(runUntil([&calls]() { return calls > 1; }));
		settle();
		EXPECT_EQ(calls, 2);
		EXPECT_EQ(result, BLUETOOTH_ERROR_NONE);
	}
	g_object_unref(folderInterface);
	g_object_unref(object);
}
