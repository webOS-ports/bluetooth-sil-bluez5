// Copyright (c) 2014-2020 LG Electronics, Inc.
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

#include "bluez5obexclient.h"
#include "bluez5obexsession.h"
#include "dbusutils.h"
#include "asyncutils.h"
#include "logging.h"
#include "bluez5busconfig.h"
#include "bluez5adapter.h"


Bluez5ObexClient::Bluez5ObexClient(Bluez5Adapter *adapter) :
    mClientProxy(0),
    mNameWatch(BLUEZ5_OBEX_DBUS_BUS_TYPE, "org.bluez.obex"),
    mAdapter(adapter)
{
	// The adapter deletes its client when bluetoothd drops the adapter, which can be before the bus answers.
	DBusUtils::waitForBus(BLUEZ5_OBEX_DBUS_BUS_TYPE, [this, alive = mLifetime.token()](bool available) {
		if (alive.expired())
			return;
		if (!available) {
			// register an empty name watch handler
			mNameWatch.watch([](bool /*available*/) { });
			return;
		}

		DEBUG("DBus session bus is available now");

		connectWithObex();
	});
}

Bluez5ObexClient::~Bluez5ObexClient()
{
	destroyProxy();
}

void Bluez5ObexClient::connectWithObex()
{
	DEBUG("Waiting for OBEX service to be available on the bus");

	mNameWatch.watch([this](bool available) {
		if (available)
			createProxy();
		else
			destroyProxy();
	});
}

void Bluez5ObexClient::createProxy()
{
	if (mClientProxy)
	{
		WARNING(MSGID_PROXY_ALREADY_EXISTS, 0, "Proxy for OBEX client already exists. Removing it first");
		destroyProxy();
	}

	DEBUG("Creating proxy for OBEX client ..");

	auto createProxyCallback = [this, alive = mLifetime.token()](GAsyncResult *result) {
		GError *error = 0;

		BluezObexClient1 *proxy = bluez_obex_client1_proxy_new_for_bus_finish(result, &error);
		if (alive.expired())
		{
			g_clear_error(&error);
			if (proxy)
				g_object_unref(proxy);
			return;
		}
		mClientProxy = proxy;
		if (error)
		{
			ERROR(MSGID_FAILED_TO_CREATE_OBEX_CLIENT_PROXY, 0,
			      "Failed to create dbus proxy for OBEX client: %s", error->message);
			g_error_free(error);
			return;
		}

		DEBUG("Successfully created proxy for OBEX client");
	};

	bluez_obex_client1_proxy_new_for_bus(BLUEZ5_OBEX_DBUS_BUS_TYPE, G_DBUS_PROXY_FLAGS_NONE,
	                                     "org.bluez.obex", "/org/bluez/obex", NULL,
	                                     glibAsyncMethodWrapper,
	                                     new GlibAsyncFunctionWrapper(createProxyCallback));
}

void Bluez5ObexClient::destroyProxy()
{
	DEBUG("Destroying proxy for OBEX client");

	if (mClientProxy)
	{
		g_object_unref(mClientProxy);
		mClientProxy = 0;
	}
}

std::string sessionTypeToString(Bluez5ObexSession::Type type)
{
	std::string result = "";

	switch (type) {
	case Bluez5ObexSession::Type::FTP:
		result = "ftp";
		break;
	case Bluez5ObexSession::Type::MAP:
		result = "map";
		break;
	case Bluez5ObexSession::Type::OPP:
		result = "opp";
		break;
	case Bluez5ObexSession::Type::PBAP:
		result = "pbap";
		break;
	case Bluez5ObexSession::Type::SYNC:
		result = "sync";
		break;
	default:
		break;
	}

	return result;
}

void Bluez5ObexClient::createSession(Bluez5ObexSession::Type type, const std::string &deviceAddress, Bluez5ObexSessionCreateCallback callback, const std::string &instanceName)
{
	if (!mClientProxy)
	{
		callback(0);
		return;
	}

	// Finished on the proxy the call was made on: when obexd went away meanwhile, mClientProxy was NULL or
	// another proxy, the finish failed without setting an error, and the NULL path became a std::string, which
	// throws through GLib and ends the service.
	auto createSessionCallback = [this, alive = mLifetime.token(), callback, type, deviceAddress](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		gboolean ret;
		gchar *objectPath = 0;

		ret = bluez_obex_client1_call_create_session_finish(BLUEZ_OBEX_CLIENT1(source), &objectPath, result, &error);
		if (alive.expired())	// the client, and the profiles it was created for, are gone
		{
			g_clear_error(&error);
			g_free(objectPath);
			return;
		}
		if (!ret || error || !objectPath)
		{
			ERROR(MSGID_FAILED_TO_CREATE_OBEX_SESSION_PROXY, 0, "Failed to create OBEX session with %s: %s",
			      deviceAddress.c_str(), error ? error->message : "no session path");
			g_clear_error(&error);
			g_free(objectPath);
			callback(0);
			return;
		}

		// The obexd that answered: its objects are reached by its unique name, which needs no call to the bus.
		Bluez5ObexSession *session = new Bluez5ObexSession(this, type, std::string(objectPath), deviceAddress,
		                                                   obexPeerOfOwner(source));
		g_free(objectPath);	// allocated by the finish; it leaked

		// Partly made, its callers used NULL proxies, and its end went unnoticed: a failure, which also removes
		// the session from obexd.
		if (!session->isValid())
		{
			ERROR(MSGID_FAILED_TO_CREATE_OBEX_SESSION_PROXY, 0, "OBEX session %s with %s could not be set up",
			      session->getObjectPath().c_str(), deviceAddress.c_str());
			delete session;
			callback(0);
			return;
		}

		// The caller has to take care to release the memory for the session when it isn't used anymore
		callback(session);
	};

	GVariantBuilder *builder = 0;
	GVariant *arguments = 0;

	std::string typeStr = sessionTypeToString(type);
	std::string adapterAddress = mAdapter->getAddress();

	builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (builder, "{sv}", "Target", g_variant_new_string (typeStr.c_str()));
	g_variant_builder_add (builder, "{sv}", "Source", g_variant_new_string (adapterAddress.c_str()));
	if(!instanceName.empty())
		g_variant_builder_add (builder, "{sv}", "InstanceName", g_variant_new_string (instanceName.c_str()));
	arguments = g_variant_builder_end (builder);
	g_variant_builder_unref(builder);

	bluez_obex_client1_call_create_session(mClientProxy, deviceAddress.c_str(), arguments, NULL,
	                                       glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(createSessionCallback));
}

void Bluez5ObexClient::destroySession(const std::string &objectPath)
{
	if (!mClientProxy)
		return;

	auto removeSessionCallback = [objectPath](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		[[maybe_unused]] gboolean ret;

		ret = bluez_obex_client1_call_remove_session_finish(BLUEZ_OBEX_CLIENT1(source), result, &error);
		if (error)
		{
			ERROR(MSGID_FAILED_TO_REMOVE_OBEX_SESSION_PROXY, 0, "Failed to remove obex session on path %s",
				  objectPath.c_str());
			g_error_free(error);
			return;
		}
	};

	bluez_obex_client1_call_remove_session(mClientProxy, objectPath.c_str(), NULL,
	                                       glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(removeSessionCallback));
}
