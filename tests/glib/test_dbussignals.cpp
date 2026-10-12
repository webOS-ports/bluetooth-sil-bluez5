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

// The signals that tell an OBEX session is gone: InterfacesRemoved for its path, and its obexd leaving the bus.

#include <gtest/gtest.h>

#include "dbussignals.h"

using DBusUtils::interfacesRemovedAt;
using DBusUtils::nameLost;

namespace {

GVariant *parsed(const char *type, const char *text)
{
	GError *error = nullptr;
	GVariant *value = g_variant_parse(G_VARIANT_TYPE(type), text, nullptr, nullptr, &error);
	EXPECT_EQ(error, nullptr) << (error ? error->message : "");
	g_clear_error(&error);
	return value;	// g_variant_parse gives a full reference
}

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

TEST(DBusSignals, InterfacesRemovedForThePath)
{
	GVariant *params = parsed("(oas)", "(objectpath '/org/bluez/obex/client/session1',"
	                                   " ['org.bluez.obex.Session1', 'org.bluez.obex.ObjectPush1'])");
	auto removed = interfacesRemovedAt(params, "/org/bluez/obex/client/session1");
	ASSERT_EQ(removed.size(), 2u);
	EXPECT_EQ(removed[0], "org.bluez.obex.Session1");
	EXPECT_EQ(removed[1], "org.bluez.obex.ObjectPush1");
	g_variant_unref(params);
}

TEST(DBusSignals, InterfacesRemovedForAnotherPath)
{
	GVariant *params = parsed("(oas)", "(objectpath '/org/bluez/obex/client/session12', ['org.bluez.obex.Session1'])");
	EXPECT_TRUE(interfacesRemovedAt(params, "/org/bluez/obex/client/session1").empty());
	GVariant *below = parsed("(oas)", "(objectpath '/org/bluez/obex/client/session1/transfer2',"
	                                  " ['org.bluez.obex.Transfer1'])");
	EXPECT_TRUE(interfacesRemovedAt(below, "/org/bluez/obex/client/session1").empty());
	g_variant_unref(below);
	g_variant_unref(params);
}

TEST(DBusSignals, InterfacesRemovedOfAnotherType)
{
	CriticalCounter glib;
	EXPECT_TRUE(interfacesRemovedAt(nullptr, "/a").empty());
	GVariant *params = parsed("(sas)", "('/a', ['org.bluez.obex.Session1'])");
	EXPECT_TRUE(interfacesRemovedAt(params, "/a").empty());
	g_variant_unref(params);
	EXPECT_EQ(glib.criticals, 0);
}

TEST(DBusSignals, NameLost)
{
	GVariant *lost = parsed("(sss)", "(':1.7', ':1.7', '')");
	EXPECT_TRUE(nameLost(lost, ":1.7"));
	EXPECT_FALSE(nameLost(lost, ":1.70"));
	g_variant_unref(lost);

	GVariant *gained = parsed("(sss)", "(':1.7', '', ':1.7')");
	EXPECT_FALSE(nameLost(gained, ":1.7"));
	g_variant_unref(gained);

	GVariant *wellKnown = parsed("(sss)", "('org.bluez.obex', ':1.7', ':1.9')");
	EXPECT_FALSE(nameLost(wellKnown, ":1.7"));
	g_variant_unref(wellKnown);

	CriticalCounter glib;
	EXPECT_FALSE(nameLost(nullptr, ":1.7"));
	GVariant *wrongType = parsed("(ss)", "(':1.7', '')");
	EXPECT_FALSE(nameLost(wrongType, ":1.7"));
	g_variant_unref(wrongType);
	EXPECT_EQ(glib.criticals, 0);
}
