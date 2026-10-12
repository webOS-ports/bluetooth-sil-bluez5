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

#include "obexpush.h"

ObexPeer obexPeerOfProxy(GObject *proxy)
{
	ObexPeer peer;
	if (!proxy || !G_IS_DBUS_PROXY(proxy))
		return peer;

	const gchar *name = g_dbus_proxy_get_name(G_DBUS_PROXY(proxy));
	peer.connection = g_dbus_proxy_get_connection(G_DBUS_PROXY(proxy));
	peer.name = name ? name : "";
	return peer;
}

ObexPeer obexPeerOfOwner(GObject *proxy)
{
	ObexPeer peer;
	if (!proxy || !G_IS_DBUS_PROXY(proxy))
		return peer;

	gchar *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(proxy));
	peer.connection = g_dbus_proxy_get_connection(G_DBUS_PROXY(proxy));
	peer.name = owner ? owner : "";
	g_free(owner);
	return peer;
}

namespace {

// The value of key in an a{sv} if it has type, else NULL (to be unreffed).
GVariant *lookup(GVariant *properties, const char *key, const GVariantType *type)
{
	if (!properties || !g_variant_is_of_type(properties, G_VARIANT_TYPE("a{sv}")))
		return nullptr;
	return g_variant_lookup_value(properties, key, type);
}

bool lookupString(GVariant *properties, const char *key, const GVariantType *type, std::string &value)
{
	GVariant *variant = lookup(properties, key, type);
	if (!variant)
		return false;
	value = g_variant_get_string(variant, nullptr);
	g_variant_unref(variant);
	return true;
}

} // namespace

bool parseObexTransferProperties(GVariant *properties, ObexPushRequest &request)
{
	if (!lookupString(properties, "Session", G_VARIANT_TYPE_OBJECT_PATH, request.sessionPath))
		return false;

	request.hasName = lookupString(properties, "Name", G_VARIANT_TYPE_STRING, request.name);
	if (!request.hasName)
		request.name.clear();

	request.size = 0;
	if (GVariant *size = lookup(properties, "Size", G_VARIANT_TYPE_UINT64))
	{
		request.size = g_variant_get_uint64(size);
		g_variant_unref(size);
	}
	return true;
}

bool parseObexSessionProperties(GVariant *properties, ObexPushRequest &request)
{
	return lookupString(properties, "Source", G_VARIANT_TYPE_STRING, request.source) &&
	       lookupString(properties, "Destination", G_VARIANT_TYPE_STRING, request.destination);
}

guint obexConfirmationRemainingMs(gint64 receivedAt, gint64 now, guint limitMs)
{
	gint64 elapsedMs = now > receivedAt ? (now - receivedAt) / 1000 : 0;
	return elapsedMs >= (gint64) limitMs ? 0 : (guint) ((gint64) limitMs - elapsedMs);
}
