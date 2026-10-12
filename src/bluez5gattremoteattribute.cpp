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

#include <utility>

#include "asyncutils.h"
#include "logging.h"
#include "utils.h"
#include "bluez5profilegatt.h"
#include "bluetooth-sil-api.h"
#include "bluez5gattremoteattribute.h"

const std::map <std::string, BluetoothGattCharacteristic::Property> GattRemoteCharacteristic::characteristicPropertyMap = {
	{ "read", BluetoothGattCharacteristic::PROPERTY_READ},
	{ "broadcast", BluetoothGattCharacteristic::PROPERTY_BROADCAST},
	{ "write-without-response", BluetoothGattCharacteristic::PROPERTY_WRITE_WITHOUT_RESPONSE},
	{ "write", BluetoothGattCharacteristic::PROPERTY_WRITE},
	{ "notify", BluetoothGattCharacteristic::PROPERTY_NOTIFY},
	{ "indicate", BluetoothGattCharacteristic::PROPERTY_INDICATE},
	{ "authenticated-signed-writes", BluetoothGattCharacteristic::PROPERTY_AUTHENTICATED_SIGNED_WRITES}
};

const std::map <BluetoothGattPermission, std::string> GattRemoteDescriptor::descriptorPermissionMap = {
	{ PERMISSION_READ, "read"},
	{ PERMISSION_READ_ENCRYPTED, "encrypt-read"},
	{ PERMISSION_READ_ENCRYPTED_MITM, "encrypt-authenticated-read"},
	{ PERMISSION_WRITE, "write"},
	{ PERMISSION_WRITE_ENCRYPTED, "encrypt-write"},
	{ PERMISSION_WRITE_ENCRYPTED_MITM, "encrypt-authenticated-write"},
	{ PERMISSION_WRITE_SIGNED, "secure-write"}
};

GattRemoteCharacteristic::GattRemoteCharacteristic(BluezGattCharacteristic1 *interface, Bluez5ProfileGatt *gattProfile)
	: mInterface(interface), mGattProfile(gattProfile),
	  mNotifyQueue([this](bool enable, GattNotifyQueue::Result done) { callNotify(enable, std::move(done)); })
{
	mPropertiesChangedHandler = g_signal_connect(G_DBUS_PROXY(interface), "g-properties-changed",
	                                             G_CALLBACK(GattRemoteCharacteristic::onCharacteristicPropertiesChanged), this);
}

// One call at a time per characteristic: BlueZ fails a second StartNotify of the same client while the first waits
// for the device (InProgress), and a StopNotify meanwhile removes the session under it. See GattNotifyQueue.
void GattRemoteCharacteristic::startNotify(GattRemoteResultCallback callback)
{
	mNotifyQueue.request(true, std::move(callback));
}

void GattRemoteCharacteristic::stopNotify(GattRemoteResultCallback callback)
{
	mNotifyQueue.request(false, std::move(callback));
}

// StartNotify writes the remote's Client Characteristic Configuration descriptor and is answered only when the
// device has acknowledged it. Like reads and writes, the callback keeps nothing of the characteristic: the call
// holds its own reference to the proxy, and the reply is finished on that. The queue is called only while the
// characteristic is there.
void GattRemoteCharacteristic::callNotify(bool enable, GattRemoteResultCallback done)
{
	auto notifyCallback = [done = std::move(done), enable, path = objectPath](GObject *source, GAsyncResult *result) {
		GError *error = NULL;
		gboolean ret = enable
			? bluez_gatt_characteristic1_call_start_notify_finish(BLUEZ_GATT_CHARACTERISTIC1(source), result, &error)
			: bluez_gatt_characteristic1_call_stop_notify_finish(BLUEZ_GATT_CHARACTERISTIC1(source), result, &error);
		if (!ret || error)
		{
			ERROR(MSGID_GATT_PROFILE_ERROR, 0, "%s failed on %s: %s", enable ? "StartNotify" : "StopNotify",
			      path.c_str(), error ? error->message : "no result");
			g_clear_error(&error);
			done(false);
			return;
		}
		done(true);
	};
	if (enable)
		bluez_gatt_characteristic1_call_start_notify(mInterface, NULL, glibAsyncMethodWrapper,
		                                             new GlibAsyncFunctionWrapper(notifyCallback));
	else
		bluez_gatt_characteristic1_call_stop_notify(mInterface, NULL, glibAsyncMethodWrapper,
		                                            new GlibAsyncFunctionWrapper(notifyCallback));
}

void GattRemoteCharacteristic::onCharacteristicPropertiesChanged(GDBusProxy */*proxy*/, GVariant *changed_properties, GStrv /*invalidated_properties*/, gpointer userdata)
{
	auto characteristic = static_cast<GattRemoteCharacteristic*>(userdata);
	if (characteristic && characteristic->mGattProfile)
		characteristic->mGattProfile->onCharacteristicPropertiesChanged(characteristic, changed_properties);
}

namespace {

GVariant *noOptions()
{
	GVariantDict dict;
	g_variant_dict_init(&dict, NULL);
	return g_variant_dict_end(&dict);
}

} // namespace

void GattRemoteCharacteristic::readValue(GattRemoteReadCallback callback)
{
	auto readCallback = [callback, path = objectPath](GObject *source, GAsyncResult *result) {
		GError *error = NULL;
		GVariant *value = nullptr;
		gboolean ret = bluez_gatt_characteristic1_call_read_value_finish(BLUEZ_GATT_CHARACTERISTIC1(source), &value, result, &error);
		if (!ret || error)
		{
			ERROR(MSGID_GATT_PROFILE_ERROR, 0, "ReadValue failed on %s: %s", path.c_str(), error ? error->message : "no result");
			g_clear_error(&error);
			callback(false, std::vector<unsigned char>());
			return;
		}
		std::vector<unsigned char> bytes = convertArrayByteGVariantToVector(value);
		g_variant_unref(value);
		callback(true, bytes);
	};
	bluez_gatt_characteristic1_call_read_value(mInterface, noOptions(), NULL, glibAsyncMethodWrapper,
	                                           new GlibAsyncFunctionWrapper(readCallback));
}

void GattRemoteCharacteristic::writeValue(const std::vector<unsigned char> &characteristicValue, bool withoutResponse,
                                          GattRemoteWriteCallback callback)
{
	GVariantDict dict;
	g_variant_dict_init(&dict, NULL);
	if (withoutResponse)
		g_variant_dict_insert_value(&dict, "type", g_variant_new_string("command"));
	GVariant *options = g_variant_dict_end(&dict);

	auto writeCallback = [callback, path = objectPath](GObject *source, GAsyncResult *result) {
		GError *error = NULL;
		gboolean ret = bluez_gatt_characteristic1_call_write_value_finish(BLUEZ_GATT_CHARACTERISTIC1(source), result, &error);
		if (!ret || error)
		{
			ERROR(MSGID_GATT_PROFILE_ERROR, 0, "WriteValue failed on %s: %s", path.c_str(), error ? error->message : "no result");
			g_clear_error(&error);
			callback(false);
			return;
		}
		callback(true);
	};
	bluez_gatt_characteristic1_call_write_value(mInterface, convertVectorToArrayByteGVariant(characteristicValue), options,
	                                            NULL, glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(writeCallback));
}

std::vector<unsigned char> GattRemoteCharacteristic::cachedValue()
{
	GVariant *value = bluez_gatt_characteristic1_get_value(mInterface);	// owned by the proxy
	return value ? convertArrayByteGVariantToVector(value) : std::vector<unsigned char>();
}

GattRemoteCharacteristic::~GattRemoteCharacteristic()
{
	for (auto descriptor : gattRemoteDescriptors)
		delete descriptor;
	if (mPropertiesChangedHandler)
		g_signal_handler_disconnect(mInterface, mPropertiesChangedHandler);
	if (mInterface)
		g_object_unref(mInterface);
}

GattRemoteService::~GattRemoteService()
{
	for (auto characteristic : gattRemoteCharacteristics)
		delete characteristic;
	if (mInterface)
		g_object_unref(mInterface);
}

GattRemoteDescriptor::~GattRemoteDescriptor()
{
	if (mInterface)
		g_object_unref(mInterface);
}

BluetoothGattCharacteristicProperties GattRemoteCharacteristic::readProperties()
{
	BluetoothGattCharacteristicProperties properties = 0;
	GVariant* flagVariant = bluez_gatt_characteristic1_get_flags(mInterface);
	if (flagVariant) {
		std::vector <std::string> characteristicFlags = convertArrayStringGVariantToVector(flagVariant);

		for (auto it = characteristicFlags.begin(); it != characteristicFlags.end(); it++)
		{
			auto property = characteristicPropertyMap.find(*it);
			if (property != characteristicPropertyMap.end())
				properties |= property->second;
		}
	}
	return 	properties;
}

void GattRemoteDescriptor::readValue(GattRemoteReadCallback callback)
{
	auto readCallback = [callback, path = objectPath](GObject *source, GAsyncResult *result) {
		GError *error = NULL;
		GVariant *value = nullptr;
		gboolean ret = bluez_gatt_descriptor1_call_read_value_finish(BLUEZ_GATT_DESCRIPTOR1(source), &value, result, &error);
		if (!ret || error)
		{
			ERROR(MSGID_GATT_PROFILE_ERROR, 0, "ReadValue failed on %s: %s", path.c_str(), error ? error->message : "no result");
			g_clear_error(&error);
			callback(false, std::vector<unsigned char>());
			return;
		}
		std::vector<unsigned char> bytes = convertArrayByteGVariantToVector(value);
		g_variant_unref(value);
		callback(true, bytes);
	};
	bluez_gatt_descriptor1_call_read_value(mInterface, noOptions(), NULL, glibAsyncMethodWrapper,
	                                       new GlibAsyncFunctionWrapper(readCallback));
}

void GattRemoteDescriptor::writeValue(const std::vector<unsigned char> &descriptorValue, GattRemoteWriteCallback callback)
{
	auto writeCallback = [callback, path = objectPath](GObject *source, GAsyncResult *result) {
		GError *error = NULL;
		gboolean ret = bluez_gatt_descriptor1_call_write_value_finish(BLUEZ_GATT_DESCRIPTOR1(source), result, &error);
		if (!ret || error)
		{
			ERROR(MSGID_GATT_PROFILE_ERROR, 0, "WriteValue failed on %s: %s", path.c_str(), error ? error->message : "no result");
			g_clear_error(&error);
			callback(false);
			return;
		}
		callback(true);
	};
	bluez_gatt_descriptor1_call_write_value(mInterface, convertVectorToArrayByteGVariant(descriptorValue), noOptions(),
	                                        NULL, glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(writeCallback));
}
