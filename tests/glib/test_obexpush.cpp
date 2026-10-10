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

// The properties an incoming OPP push is decided on, as obexd's GetAll returns them, and the time left to ask the
// user once they are read.

#include <gtest/gtest.h>

#include "obexpush.h"

namespace {

// An a{sv} from a GVariant text.
GVariant *dict(const char *text)
{
	GError *error = nullptr;
	GVariant *value = g_variant_parse(G_VARIANT_TYPE("a{sv}"), text, nullptr, nullptr, &error);
	EXPECT_EQ(error, nullptr) << (error ? error->message : "");
	g_clear_error(&error);
	return value;	// g_variant_parse gives a full reference
}

struct Dict
{
	explicit Dict(const char *text) : value(dict(text)) { }
	~Dict() { if (value) g_variant_unref(value); }
	GVariant *value;
};

// Counts GLib's and GIO's own criticals (failed g_return_if_fail checks) while it lives: input of the wrong type has to be
// turned away before it reaches GLib, not by GLib.
class CriticalCounter
{
public:
	CriticalCounter() :
		mGLib(g_log_set_handler("GLib", G_LOG_LEVEL_CRITICAL, count, this)),
		mGio(g_log_set_handler("GLib-GIO", G_LOG_LEVEL_CRITICAL, count, this)) { }
	~CriticalCounter()
	{
		g_log_remove_handler("GLib", mGLib);
		g_log_remove_handler("GLib-GIO", mGio);
	}
	CriticalCounter(const CriticalCounter &) = delete;
	CriticalCounter &operator=(const CriticalCounter &) = delete;
	int criticals = 0;

private:
	guint mGLib;
	guint mGio;
	static void count(const gchar *, GLogLevelFlags, const gchar *, gpointer self)
	{
		static_cast<CriticalCounter *>(self)->criticals++;
	}
};

} // namespace

TEST(ObexPush, TransferPropertiesRead)
{
	Dict props("{'Status': <'queued'>, 'Session': <objectpath '/org/bluez/obex/server/session3'>,"
	           " 'Name': <'photo.jpg'>, 'Size': <uint64 12345>, 'Transferred': <uint64 0>}");
	ObexPushRequest request;
	ASSERT_TRUE(parseObexTransferProperties(props.value, request));
	EXPECT_EQ(request.sessionPath, "/org/bluez/obex/server/session3");
	EXPECT_TRUE(request.hasName);
	EXPECT_EQ(request.name, "photo.jpg");
	EXPECT_EQ(request.size, 12345u);
}

TEST(ObexPush, TransferWithoutNameOrSize)
{
	// A PUT without a Name header: obexd leaves the property out (transfer_name_exists).
	Dict props("{'Session': <objectpath '/org/bluez/obex/server/session1'>}");
	ObexPushRequest request;
	request.name = "stale";
	request.size = 7;
	ASSERT_TRUE(parseObexTransferProperties(props.value, request));
	EXPECT_FALSE(request.hasName);
	EXPECT_EQ(request.name, "");
	EXPECT_EQ(request.size, 0u);
}

TEST(ObexPush, TransferWithoutSessionRefused)
{
	Dict none("{'Name': <'a.txt'>}");
	ObexPushRequest request;
	EXPECT_FALSE(parseObexTransferProperties(none.value, request));

	// A string where an object path belongs counts as absent.
	Dict wrongType("{'Session': <'/org/bluez/obex/server/session1'>, 'Name': <'a.txt'>}");
	EXPECT_FALSE(parseObexTransferProperties(wrongType.value, request));
}

TEST(ObexPush, TransferValuesOfOtherTypesIgnored)
{
	Dict props("{'Session': <objectpath '/s'>, 'Name': <uint32 5>, 'Size': <'big'>}");
	ObexPushRequest request;
	ASSERT_TRUE(parseObexTransferProperties(props.value, request));
	EXPECT_FALSE(request.hasName);
	EXPECT_EQ(request.size, 0u);
}

TEST(ObexPush, NotADictionary)
{
	CriticalCounter glib;
	ObexPushRequest request;
	EXPECT_FALSE(parseObexTransferProperties(nullptr, request));
	EXPECT_FALSE(parseObexSessionProperties(nullptr, request));

	GVariant *tuple = g_variant_ref_sink(g_variant_new("(s)", "x"));
	EXPECT_FALSE(parseObexTransferProperties(tuple, request));
	EXPECT_FALSE(parseObexSessionProperties(tuple, request));
	g_variant_unref(tuple);
	EXPECT_EQ(glib.criticals, 0);
}

TEST(ObexPush, SessionPropertiesRead)
{
	Dict props("{'Source': <'00:11:22:33:44:55'>, 'Destination': <'AA:BB:CC:DD:EE:FF'>, 'Root': <'/media/internal'>}");
	ObexPushRequest request;
	ASSERT_TRUE(parseObexSessionProperties(props.value, request));
	EXPECT_EQ(request.source, "00:11:22:33:44:55");
	EXPECT_EQ(request.destination, "AA:BB:CC:DD:EE:FF");
}

TEST(ObexPush, SessionNeedsSourceAndDestination)
{
	ObexPushRequest request;
	Dict noSource("{'Destination': <'AA:BB:CC:DD:EE:FF'>}");
	EXPECT_FALSE(parseObexSessionProperties(noSource.value, request));
	Dict noDestination("{'Source': <'00:11:22:33:44:55'>}");
	EXPECT_FALSE(parseObexSessionProperties(noDestination.value, request));
	Dict wrongType("{'Source': <'00:11:22:33:44:55'>, 'Destination': <uint64 1>}");
	EXPECT_FALSE(parseObexSessionProperties(wrongType.value, request));
}

TEST(ObexPush, RemainingTimeCountsFromTheRequest)
{
	const gint64 s = G_USEC_PER_SEC;
	EXPECT_EQ(obexConfirmationRemainingMs(100 * s, 100 * s, 50000), 50000u);
	EXPECT_EQ(obexConfirmationRemainingMs(100 * s, 100 * s + 1500, 50000), 49999u);	// whole ms used
	EXPECT_EQ(obexConfirmationRemainingMs(100 * s, 103 * s, 50000), 47000u);
	EXPECT_EQ(obexConfirmationRemainingMs(100 * s, 150 * s - 1000, 50000), 1u);
	EXPECT_EQ(obexConfirmationRemainingMs(100 * s, 150 * s, 50000), 0u);
	EXPECT_EQ(obexConfirmationRemainingMs(100 * s, 150 * s + 1000, 50000), 0u);	// just past: not a wrap-around
	EXPECT_EQ(obexConfirmationRemainingMs(100 * s, 400 * s, 50000), 0u);
	// A clock that seems to go back gives the whole time, not more.
	EXPECT_EQ(obexConfirmationRemainingMs(100 * s, 90 * s, 50000), 50000u);
}

TEST(ObexPush, PeerNeedsConnectionAndUniqueName)
{
	ObexPeer peer;
	EXPECT_FALSE(peer.isValid());
	peer.connection = reinterpret_cast<GDBusConnection *>(&peer);	// only compared with NULL
	peer.name = "org.bluez.obex";	// well-known: proxies on it ask the bus who owns it
	EXPECT_FALSE(peer.isValid());
	peer.name = "";
	EXPECT_FALSE(peer.isValid());
	peer.name = ":1.42";
	EXPECT_TRUE(peer.isValid());
	peer.connection = nullptr;
	EXPECT_FALSE(peer.isValid());
}

TEST(ObexPush, PeerOfSomethingElse)
{
	CriticalCounter glib;
	EXPECT_FALSE(obexPeerOfProxy(nullptr).isValid());
	EXPECT_FALSE(obexPeerOfOwner(nullptr).isValid());
	GObject *notAProxy = G_OBJECT(g_simple_action_new("x", nullptr));
	EXPECT_FALSE(obexPeerOfProxy(notAProxy).isValid());
	EXPECT_FALSE(obexPeerOfOwner(notAProxy).isValid());
	g_object_unref(notAProxy);
	EXPECT_EQ(glib.criticals, 0);
}
