// Copyright (c) 2018-2021 LG Electronics, Inc.
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

#ifndef BLUEZ5PROFILEA2DP_H
#define BLUEZ5PROFILEA2DP_H

#include <map>
#include <string>

#include <bluetooth-sil-api.h>
#include "bluez5profilebase.h"

class Bluez5Adapter;
class Bluez5Device;

class Bluez5ProfileA2dp : public Bluez5ProfileBase,
                         public BluetoothA2dpProfile
{
public:
	Bluez5ProfileA2dp(Bluez5Adapter *adapter);
	~Bluez5ProfileA2dp();
	void getProperties(const std::string &address, BluetoothPropertiesResultCallback  callback) override;
	void getProperty(const std::string &address, BluetoothProperty::Type type,
	                         BluetoothPropertyResultCallback callback) override;

	BluetoothError startStreaming(const std::string &address) override;
	BluetoothError stopStreaming(const std::string &address) override;
	void connect(const std::string &address, BluetoothResultCallback callback) override;
	void disconnect(const std::string &address, BluetoothResultCallback callback) override;
	bool updateA2dpUuid(const std::string &address);
	void enable(const std::string &uuid, BluetoothResultCallback callback) override;
	void disable(const std::string &uuid, BluetoothResultCallback callback) override;
	void updateConnectionStatus(const std::string &address, bool status,
								const std::string &uuid) override;
	BluetoothError setDelayReportingState(bool state) override;
	BluetoothError getDelayReportingState(bool &state) override;

	static void handleObjectAdded(GDBusObjectManager *objectManager, GDBusObject *object, void *user_data);
	static void handleObjectRemoved(GDBusObjectManager *objectManager, GDBusObject *object, void *user_data);
	static void handleBluezServiceStarted(GDBusConnection *conn, const gchar *name,
											const gchar *nameOwner, gpointer user_data);
	static void handleBluezServiceStopped(GDBusConnection *conn, const gchar *name,
											gpointer user_data);
	static void handlePropertiesChanged(FreeDesktopDBusProperties *propertiesProxy, gchar *interface,
										GVariant *changedProperties, GVariant *invalidatedProperties, gpointer userData);
	void setA2dpUuid(const std::string &uuid);

	// The transport of this device, NULL when it has none.
	BluezMediaTransport1* getMediaTransport(const std::string &address);

	void delayReportChanged(const std::string &adapterAddress, const std::string &deviceAddress, guint16 delay);

private:
	// One per MediaTransport1 object of this adapter, by its object path: with two devices, or a reconnect whose new
	// transport appears before the old one goes, there are several.
	struct Transport
	{
		BluezMediaTransport1 *transport;
		FreeDesktopDBusProperties *properties;
	};

	void addTransport(const std::string &objectPath);
	void removeTransport(const std::string &objectPath);
	void clearTransports();
	void releaseObjectManager();
	Bluez5Device *findTransportDevice(BluezMediaTransport1 *transport);
	void updateTransportProperties(BluezMediaTransport1 *transport);
#ifdef WEBOS_AUTO
	void selectRole(BluezMedia1 *mediaProxy, const std::string &role, const std::string &uuid,
					BluetoothResultCallback callback);
#endif

	bool mConnected;
	GDBusObjectManager *mObjectManager;
	BluetoothA2dpProfileState mState;
	std::map<std::string, Transport> mTransports;
	std::string mTransportUuid;
	guint mWatcherId;
};

#endif
