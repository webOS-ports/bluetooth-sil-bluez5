// Copyright (c) 2014-2019 LG Electronics, Inc.
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

#ifndef DBUSCONNECTION_H
#define DBUSCONNECTION_H

#include <cstdint>
#include <functional>
#include <string>

#include <glib.h>
#include <gio/gio.h>

#include "asyncutils.h"



namespace DBusUtils
{
	typedef std::function<void(bool)> StatusCallback;

	void checkBus(GBusType busType, StatusCallback callback);
	void waitForBus(GBusType busType, StatusCallback callback);

	class NameWatch
	{
	public:
		NameWatch(GBusType busType, const std::string &busName);
		~NameWatch();

		void watch(StatusCallback callback);

	private:
		uint32_t mWatch;
		StatusCallback mCallback;
		enum class State { UNKNOWN, PRESENT, ABSENT } mState = State::UNKNOWN;	// as last reported

		static void handleNameAppeared(GDBusConnection *conn, const gchar *name, const gchar *nameOwner, gpointer user_data);
		static void handleNameDisappeared(GDBusConnection *conn, const gchar *name, gpointer user_data);
	};

	typedef std::function<void (const std::string &)> InterfaceStatusCallback;

	// Tells when interfaces of the object at path go, or the whole object ("all") because its owner left the bus.
	// Signal subscriptions only: no call waits for an answer. A GDBusObjectManagerClient for this read all of
	// obexd's objects (GetManagedObjects) synchronously for every session. busName is the owner's unique name.
	class ObjectWatch
	{
	public:
		ObjectWatch(GDBusConnection *connection, const std::string &busName, const std::string &path);
		~ObjectWatch();

		ObjectWatch(const ObjectWatch&) = delete;
		ObjectWatch& operator = (const ObjectWatch&) = delete;

		void watchInterfaceRemoved(InterfaceStatusCallback callback);
		bool isWatching() const { return mConnection != nullptr; }	// false without a connection or unique name

	private:
		std::string mPath;
		std::string mBusName;
		GDBusConnection *mConnection;
		guint mRemovedSubscription;
		guint mOwnerSubscription;
		InterfaceStatusCallback mInterfaceRemovedCallback;
		LifetimeGuard mLifetime;	// a callback can delete the watch

		static void handleInterfacesRemoved(GDBusConnection *connection, const gchar *sender, const gchar *path,
		                                    const gchar *interface, const gchar *signal, GVariant *parameters,
		                                    gpointer user_data);
		static void handleNameOwnerChanged(GDBusConnection *connection, const gchar *sender, const gchar *path,
		                                   const gchar *interface, const gchar *signal, GVariant *parameters,
		                                   gpointer user_data);
	};
}

#endif // DBUSCONNECTION_H
