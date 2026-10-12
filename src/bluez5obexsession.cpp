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

#include "bluez5obexsession.h"
#include "bluez5obexclient.h"
#include "dbusutils.h"
#include "asyncutils.h"
#include "logging.h"

// Nothing reads these proxies' cached properties: loading them took a synchronous GetAll for each of the six, on
// the service's only main loop. Made on obexd's unique name and the connection its answer came on, they are made
// without any call: on the well-known name each one first asked the bus, synchronously, StartServiceByName and
// GetNameOwner (GLib gdbusproxy.c, initable_init and async_initable_init_second_async).
static const GDBusProxyFlags PROXY_FLAGS = G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES;

Bluez5ObexSession::Bluez5ObexSession(Bluez5ObexClient *client, Type type, const std::string &objectPath,
                                     const std::string &deviceAddress, const ObexPeer &peer) :
	mClient(client),
	mType(type),
	mObjectPath(objectPath),
	mDeviceAddress(deviceAddress),
	mBusName(peer.name),
	mSessionProxy(0),
	mFileTransferProxy(0),
	mObjectPushProxy(0),
	mPhonebookAccessProxy(nullptr),
	mMessageAccessProxy(nullptr),
	mPropertiesProxy(nullptr),
	mLostRemote(false),
	mValid(false),
	mObjectWatch(new DBusUtils::ObjectWatch(peer.connection, peer.name, objectPath))
{
	GError *error = 0;

	if (!peer.isValid())
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_SESSION_PROXY, 0, "No obexd for session %s", mObjectPath.c_str());
		return;
	}
	GDBusConnection *conn = peer.connection;
	const char *name = peer.name.c_str();

	mSessionProxy = bluez_obex_session1_proxy_new_sync(conn, PROXY_FLAGS, name, mObjectPath.c_str(), NULL, &error);
	if (error)
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_SESSION_PROXY, 0,
			  "Failed to create dbus proxy for session client on path %s",
			  mObjectPath.c_str());
		g_error_free(error);
		return;
	}

	mFileTransferProxy = bluez_obex_file_transfer1_proxy_new_sync(conn, PROXY_FLAGS, name, mObjectPath.c_str(), NULL,
	                                                              &error);
	if (error)
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_FILE_TRANSFER_PROXY, 0,
			  "Failed to create dbus proxy for file transfer on path %s",
			  mObjectPath.c_str());
		g_error_free(error);
		return;
	}

	mObjectPushProxy = bluez_obex_object_push1_proxy_new_sync(conn, PROXY_FLAGS, name, mObjectPath.c_str(), NULL, &error);

	if (error)
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_PUSH_PROXY, 0,
			  "Failed to create dbus proxy for obex push on path %s",
			  mObjectPath.c_str());
		g_error_free(error);
		return;
	}

	mPhonebookAccessProxy = bluez_obex_phonebook_access1_proxy_new_sync(conn, PROXY_FLAGS, name, mObjectPath.c_str(),
	                                                                    NULL, &error);

	if (error)
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_PHONEBOOK_PROXY, 0,
			  "Failed to create dbus proxy for obex phonebook on path %s",
			  mObjectPath.c_str());
		g_error_free(error);
		return;
	}

	mMessageAccessProxy = bluez_obex_message_access1_proxy_new_sync(conn, PROXY_FLAGS, name, mObjectPath.c_str(), NULL,
	                                                                &error);

	if (error)
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_MESSAGE_PROXY, 0,
			  "Failed to create dbus proxy for obex message on path %s",
			  mObjectPath.c_str());
		g_error_free(error);
		return;
	}

	mPropertiesProxy = free_desktop_dbus_properties_proxy_new_sync(conn, PROXY_FLAGS, name, mObjectPath.c_str(), NULL,
	                                                               &error);
	if (error)
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_PHONEBOOK_PROXY, 0,
			  "Not able to get property interface on path %s",
			  mObjectPath.c_str());
		g_error_free(error);
		return;
	}

	mValid = mObjectWatch->isWatching();

	mObjectWatch->watchInterfaceRemoved([this](const std::string &name) {
		if (name != "org.bluez.obex.Session1" && name != "all")
			return;

		DEBUG("Session interface was removed for %s", mObjectPath.c_str());
		mLostRemote = true;

		if (mStatusCallback)
			mStatusCallback(true);
	});
}

Bluez5ObexSession::~Bluez5ObexSession()
{
	// A server session (an incoming push) is not the client's to remove: obexd refused it and logged an error.
	if (!mLostRemote && mObjectPath.find("/server/") == std::string::npos && mClient)
		mClient->destroySession(mObjectPath);
	if(mFileTransferProxy)
		g_object_unref(mFileTransferProxy);
	if (mObjectPushProxy)	// never released
		g_object_unref(mObjectPushProxy);
	if(mSessionProxy)
		g_object_unref(mSessionProxy);
	if(mPhonebookAccessProxy)
		g_object_unref(mPhonebookAccessProxy);
	if(mMessageAccessProxy)
		g_object_unref(mMessageAccessProxy);
	if(mPropertiesProxy)
		g_object_unref(mPropertiesProxy);

	delete mObjectWatch;
}

ObexPeer Bluez5ObexSession::getPeer() const
{
	ObexPeer peer;
	if (mSessionProxy)
	{
		peer.connection = g_dbus_proxy_get_connection(G_DBUS_PROXY(mSessionProxy));
		peer.name = mBusName;
	}
	return peer;
}

void Bluez5ObexSession::watch(Bluez5ObexSessionStatusCallback callback)
{
	mStatusCallback = callback;
}
