// Copyright (c) 2018 LG Electronics, Inc.
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

#ifndef BLUEZ5GATTREMOTEATTRIBUTE_H
#define BLUEZ5GATTREMOTEATTRIBUTE_H

#include <gio/gio.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <bluetooth-sil-api.h>

extern "C" {
#include "freedesktop-interface.h"
#include "bluez-interface.h"
}

class Bluez5ProfileGatt;

// Remote GATT attributes, as BlueZ exports them. Each owns its proxy and its children: they were never freed on
// several paths, and the characteristics' signal handler then pointed at a deleted profile.
//
// Reads, writes and StartNotify/StopNotify are asynchronous: they were synchronous calls on the only main loop, so a
// peripheral that did not answer stopped the service for the 25 s D-Bus timeout per attribute. Their callbacks capture nothing of the
// attribute, which may be gone by the answer: the caller looks it up again.
typedef std::function<void(bool success, const std::vector<unsigned char> &value)> GattRemoteReadCallback;
typedef std::function<void(bool success)> GattRemoteResultCallback;
typedef GattRemoteResultCallback GattRemoteWriteCallback;

class GattRemoteDescriptor
{
public:
	GattRemoteDescriptor(BluezGattDescriptor1 *interface)
		: mInterface(interface) {
	}
	~GattRemoteDescriptor();
	GattRemoteDescriptor(const GattRemoteDescriptor &) = delete;
	GattRemoteDescriptor &operator=(const GattRemoteDescriptor &) = delete;

	void readValue(GattRemoteReadCallback callback);
	void writeValue(const std::vector<unsigned char> &descriptorValue, GattRemoteWriteCallback callback);

	static const std::map <BluetoothGattPermission, std::string> descriptorPermissionMap;
	std::string parentObjectPath;
	std::string objectPath;
	BluetoothGattDescriptor descriptor;
	BluezGattDescriptor1 *mInterface;
};

class GattRemoteCharacteristic
{
public:
	static void onCharacteristicPropertiesChanged(GDBusProxy *proxy, GVariant *changed_properties,
                                                  GStrv invalidated_properties, gpointer userdata);
	GattRemoteCharacteristic(BluezGattCharacteristic1 *interface, Bluez5ProfileGatt *gattProfile)
		: mInterface(interface), mGattProfile(gattProfile) {
		mPropertiesChangedHandler = g_signal_connect(G_DBUS_PROXY(interface), "g-properties-changed",
						 G_CALLBACK(GattRemoteCharacteristic::onCharacteristicPropertiesChanged), this);
	}
	~GattRemoteCharacteristic();
	GattRemoteCharacteristic(const GattRemoteCharacteristic &) = delete;
	GattRemoteCharacteristic &operator=(const GattRemoteCharacteristic &) = delete;

	// Asynchronous as well: StartNotify waits for the device to acknowledge its CCCD write.
	void startNotify(GattRemoteResultCallback callback);
	void stopNotify(GattRemoteResultCallback callback);
	void readValue(GattRemoteReadCallback callback);
	// withoutResponse: a Write Command, for characteristics that only allow those.
	void writeValue(const std::vector<unsigned char> &characteristicValue, bool withoutResponse,
	                GattRemoteWriteCallback callback);
	BluetoothGattCharacteristicProperties readProperties();
	// BlueZ's cached value: reading the remote during discovery stalled it the same way.
	std::vector<unsigned char> cachedValue();

	static const std::map <std::string, BluetoothGattCharacteristic::Property> characteristicPropertyMap;
	std::string parentObjectPath;
	std::string objectPath;
	BluetoothGattCharacteristic characteristic;
	BluezGattCharacteristic1 *mInterface;
	Bluez5ProfileGatt *mGattProfile;
	gulong mPropertiesChangedHandler = 0;
	std::vector<GattRemoteDescriptor*> gattRemoteDescriptors;
};

class GattRemoteService
{
public:
	GattRemoteService(BluezGattService1 *interface)
		: mInterface(interface) {
	}
	~GattRemoteService();
	GattRemoteService(const GattRemoteService &) = delete;
	GattRemoteService &operator=(const GattRemoteService &) = delete;

	std::string parentObjectPath;
	std::string objectPath;
	BluetoothGattService service;
	BluezGattService1 *mInterface;
	std::vector<GattRemoteCharacteristic*> gattRemoteCharacteristics;
};

#endif
