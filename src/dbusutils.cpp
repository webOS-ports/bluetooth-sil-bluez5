// Copyright (c) 2014-2018 LG Electronics, Inc.
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

#include <map>
#include <stdint.h>

#include "dbusutils.h"
#include "asyncutils.h"
#include "logging.h"
#include "dbussignals.h"

namespace DBusUtils
{

void checkBus(GBusType busType, StatusCallback callback)
{
	if (!callback)
		return;

	auto busGetCallback = [callback](GAsyncResult *result) {
		GError *error = 0;

		GDBusConnection *conn = g_bus_get_finish(result, &error);
		if (error) {
			g_error_free(error);
		}

		bool busAvailable = (conn != 0);
		callback(busAvailable);

		if (conn)
			g_object_unref(conn);
	};

	g_bus_get(busType, NULL, glibAsyncMethodWrapper,
	          new GlibAsyncFunctionWrapper(busGetCallback));
}

void waitForBus(GBusType busType, StatusCallback callback)
{
	auto busStatusCallback = [busType, callback](bool available) {
		if (!available)
		{
			auto timeoutCallback = [busType, callback]() {
				waitForBus(busType, callback);
				return false;
			};

			g_timeout_add(250, glibSourceMethodWrapper,
			              new GlibSourceFunctionWrapper(timeoutCallback));
			return;
		}

		callback(true);
	};

	checkBus(busType, busStatusCallback);
}

NameWatch::NameWatch(GBusType busType, const std::string &name) :
    mWatch(0)
{
	mWatch = g_bus_watch_name(busType, name.c_str(), G_BUS_NAME_WATCHER_FLAGS_NONE,
	                          handleNameAppeared, handleNameDisappeared, this, NULL);
}

NameWatch::~NameWatch()
{
	if (mWatch > 0)
		g_bus_unwatch_name(mWatch);
}

// The watch starts in the constructor, but its users set their callback later, after an asynchronous bus check.
// The first event could come in between and was lost: with obexd already running at a service start, the OBEX
// client and agent never learnt it was there, and MAP, PBAP, FTP and incoming OPP stayed dead until obexd
// restarted (seen on a TouchPad after a service restart). The last state is kept and given to a later callback.
void NameWatch::watch(StatusCallback callback)
{
	mCallback = callback;
	if (mCallback && mState != State::UNKNOWN)
		mCallback(mState == State::PRESENT);
}

void NameWatch::handleNameAppeared(GDBusConnection */*conn*/, const gchar */*name*/, const gchar */*nameOwner*/, gpointer user_data)
{
	NameWatch *self = static_cast<NameWatch*>(user_data);

	self->mState = State::PRESENT;
	if (self->mCallback)
		self->mCallback(true);
}

void NameWatch::handleNameDisappeared(GDBusConnection */*conn*/, const gchar */*name*/, gpointer user_data)
{
	NameWatch *self = static_cast<NameWatch*>(user_data);

	self->mState = State::ABSENT;
	if (self->mCallback)
		self->mCallback(false);
}

ObjectWatch::ObjectWatch(GDBusConnection *connection, const std::string &busName, const std::string &path) :
    mPath(path),
    mBusName(busName),
    mConnection(nullptr),
    mRemovedSubscription(0),
    mOwnerSubscription(0)
{
	if (!connection || !g_dbus_is_unique_name(busName.c_str()))
	{
		ERROR(MSGID_OBJECT_MANAGER_CREATION_FAILED, 0, "Cannot watch %s: no connection or no unique name",
		      path.c_str());
		return;
	}
	mConnection = G_DBUS_CONNECTION(g_object_ref(connection));

	// The sender is the unique name: another client cannot fake the signal, and a restarted owner's objects
	// are not taken for these. Its object manager is at "/" (bluez's gdbus, g_dbus_attach_object_manager).
	mRemovedSubscription = g_dbus_connection_signal_subscribe(mConnection, busName.c_str(),
	                                                          "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
	                                                          "/", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
	                                                          handleInterfacesRemoved, this, NULL);
	// The object manager client told this as the removal of every object.
	mOwnerSubscription = g_dbus_connection_signal_subscribe(mConnection, "org.freedesktop.DBus",
	                                                        "org.freedesktop.DBus", "NameOwnerChanged",
	                                                        "/org/freedesktop/DBus", busName.c_str(),
	                                                        G_DBUS_SIGNAL_FLAGS_NONE, handleNameOwnerChanged, this, NULL);

	DEBUG("Created object watch for %s of %s", path.c_str(), busName.c_str());
}

ObjectWatch::~ObjectWatch()
{
	if (!mConnection)	// construction failed
		return;
	// After this no handler runs, also not one already queued (GLib checks the subscription before calling).
	g_dbus_connection_signal_unsubscribe(mConnection, mRemovedSubscription);
	g_dbus_connection_signal_unsubscribe(mConnection, mOwnerSubscription);
	g_object_unref(mConnection);
}

void ObjectWatch::watchInterfaceRemoved(InterfaceStatusCallback callback)
{
	mInterfaceRemovedCallback = callback;
}

void ObjectWatch::handleInterfacesRemoved(GDBusConnection */*connection*/, const gchar */*sender*/,
                                          const gchar */*path*/, const gchar */*interface*/, const gchar */*signal*/,
                                          GVariant *parameters, gpointer user_data)
{
	const ObjectWatch *watch = static_cast<const ObjectWatch*>(user_data);

	// A copy: the callback may delete the watch.
	InterfaceStatusCallback callback = watch->mInterfaceRemovedCallback;
	if (!callback)
		return;

	auto alive = watch->mLifetime.token();
	for (const std::string &name : interfacesRemovedAt(parameters, watch->mPath))
	{
		callback(name);
		if (alive.expired())
			return;
	}
}

void ObjectWatch::handleNameOwnerChanged(GDBusConnection */*connection*/, const gchar */*sender*/,
                                         const gchar */*path*/, const gchar */*interface*/, const gchar */*signal*/,
                                         GVariant *parameters, gpointer user_data)
{
	const ObjectWatch *watch = static_cast<const ObjectWatch*>(user_data);

	InterfaceStatusCallback callback = watch->mInterfaceRemovedCallback;
	if (callback && nameLost(parameters, watch->mBusName))
		callback("all");
}


} // namespace DBusUtils
