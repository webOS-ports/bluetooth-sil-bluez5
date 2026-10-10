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

#include "bluez5adapter.h"
#include "logging.h"
#include "bluez5profilea2dp.h"
#include "utils.h"

const std::string BLUETOOTH_PROFILE_A2DP_SOURCE_UUID = "0000110a-0000-1000-8000-00805f9b34fb";
const std::string BLUETOOTH_PROFILE_A2DP_SINK_UUID = "0000110b-0000-1000-8000-00805f9b34fb";

Bluez5ProfileA2dp::Bluez5ProfileA2dp(Bluez5Adapter *adapter) :
	Bluez5ProfileBase(adapter, BLUETOOTH_PROFILE_A2DP_SINK_UUID),
	mConnected(false),
	mObjectManager(0),
	mState(NOT_PLAYING)
{
	mWatcherId = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.bluez", G_BUS_NAME_WATCHER_FLAGS_NONE,
					 handleBluezServiceStarted, handleBluezServiceStopped, this, NULL);
}

void Bluez5ProfileA2dp::delayReportChanged(const std::string &adapterAddress, const std::string &deviceAddress, guint16 delay)
{
	getA2dpObserver()->delayReportChanged(adapterAddress, deviceAddress, delay);
}

Bluez5ProfileA2dp::~Bluez5ProfileA2dp()
{
	/* Stops watching a name */
	if (mWatcherId)
		g_bus_unwatch_name(mWatcherId);
	clearTransports();
	releaseObjectManager();
}

void Bluez5ProfileA2dp::releaseObjectManager()
{
	if (!mObjectManager)
		return;
	g_signal_handlers_disconnect_by_data(mObjectManager, this);
	g_object_unref(mObjectManager);
	mObjectManager = nullptr;
}

void Bluez5ProfileA2dp::addTransport(const std::string &objectPath)
{
	// Under this adapter only; "/org/bluez/hci1" is also a prefix of hci10's paths.
	std::string adapterPath = mAdapter->getObjectPath() + "/";
	if (objectPath.compare(0, adapterPath.length(), adapterPath) || mTransports.count(objectPath))
		return;

	GError *error = 0;
	BluezMediaTransport1 *transport = bluez_media_transport1_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
	                                  G_DBUS_PROXY_FLAGS_NONE, "org.bluez", objectPath.c_str(), NULL, &error);
	if (!transport)
	{
		DEBUG("Not able to get media transport interface");
		g_clear_error(&error);
		return;
	}

	FreeDesktopDBusProperties *properties = free_desktop_dbus_properties_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
	                                        G_DBUS_PROXY_FLAGS_NONE, "org.bluez", objectPath.c_str(), NULL, &error);
	if (!properties)
	{
		DEBUG("Not able to get property interface");
		g_clear_error(&error);
		g_object_unref(transport);
		return;
	}

	g_signal_connect(G_OBJECT(properties), "properties-changed", G_CALLBACK(handlePropertiesChanged), this);
	mTransports[objectPath] = {transport, properties};
	updateTransportProperties(properties);
}

// Only the transport that went: removing any one dropped the newest, even when that was another device's.
void Bluez5ProfileA2dp::removeTransport(const std::string &objectPath)
{
	auto it = mTransports.find(objectPath);
	if (it == mTransports.end())
		return;

	g_signal_handlers_disconnect_by_data(it->second.properties, this);
	g_object_unref(it->second.properties);
	g_object_unref(it->second.transport);
	mTransports.erase(it);
	if (mTransports.empty())
		mTransportUuid = "";
}

void Bluez5ProfileA2dp::clearTransports()
{
	while (!mTransports.empty())
		removeTransport(mTransports.begin()->first);
}

Bluez5Device *Bluez5ProfileA2dp::findTransportDevice(BluezMediaTransport1 *transport)
{
	const char *deviceObjectPath = bluez_media_transport1_get_device(transport);
	return deviceObjectPath ? mAdapter->findDeviceByObjectPath(deviceObjectPath) : nullptr;
}

BluezMediaTransport1 *Bluez5ProfileA2dp::getMediaTransport(const std::string &address)
{
	Bluez5Device *device = mAdapter->findDevice(address);
	if (!device)
		return nullptr;
	for (auto &entry : mTransports)
		if (findTransportDevice(entry.second.transport) == device)
			return entry.second.transport;
	return nullptr;
}

void Bluez5ProfileA2dp::getProperties(const std::string &/*address*/, BluetoothPropertiesResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	// Answered: it was an empty stub, and its caller waited forever.
	callback(BLUETOOTH_ERROR_UNSUPPORTED, BluetoothPropertiesList());
}

void Bluez5ProfileA2dp::getProperty(const std::string &address, BluetoothProperty::Type type,
	                         BluetoothPropertyResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	BluetoothProperty prop(type);
	bool isConnected = false;
	Bluez5Device *device = mAdapter->findDevice(address);

	if (!device)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID, prop);
		return;
	}

	for (auto &entry : mTransports)
		if (findTransportDevice(entry.second.transport) == device)
			isConnected = true;

	prop.setValue<bool>(isConnected);
	callback(BLUETOOTH_ERROR_NONE, prop);
}

BluetoothError Bluez5ProfileA2dp::startStreaming(const std::string &/*address*/)
{
	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5ProfileA2dp::stopStreaming(const std::string &/*address*/)
{
	return BLUETOOTH_ERROR_NONE;
}

void Bluez5ProfileA2dp::connect(const std::string &address, BluetoothResultCallback callback)
{
	auto connectCallback = [address, callback](BluetoothError error) {
		if (error != BLUETOOTH_ERROR_NONE)
		{
			callback(error);
			return;
		}
		DEBUG("A2DP connected succesfully");
		callback(BLUETOOTH_ERROR_NONE);
	};

	if (!mConnected)
	{
		// Unknown address: answered here once. It was answered by updateA2dpUuid and again by the base class.
		if (!updateA2dpUuid(address))
		{
			callback(BLUETOOTH_ERROR_PARAM_INVALID);
			return;
		}
		Bluez5ProfileBase::connect(address, connectCallback);
	}
	else
		callback(BLUETOOTH_ERROR_DEVICE_ALREADY_CONNECTED);
}

void Bluez5ProfileA2dp::disconnect(const std::string &address, BluetoothResultCallback callback)
{
	auto disConnectCallback = [address, callback](BluetoothError error) {
		if (error != BLUETOOTH_ERROR_NONE)
		{
			callback(error);
			return;
		}
		DEBUG("A2DP disconnected succesfully");
		callback(BLUETOOTH_ERROR_NONE);
	};
	if (!updateA2dpUuid(address))
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID);
		return;
	}
	Bluez5ProfileBase::disconnect(address, disConnectCallback);
}

// False when the address is unknown.
bool Bluez5ProfileA2dp::updateA2dpUuid(const std::string &address)
{
	Bluez5Device *device = mAdapter->findDevice(address);
	if (!device)
		return false;

#ifndef WEBOS_AUTO
	for(auto tempUuid : device->getUuids())
	{
		if ((tempUuid == BLUETOOTH_PROFILE_A2DP_SOURCE_UUID) ||
			(tempUuid == BLUETOOTH_PROFILE_A2DP_SINK_UUID))
		{
			mUuid = tempUuid;
			break;
		}
	}
#else
	for(auto tempUuid : mAdapter->getAdapterSupportedUuid())
	{
		if ((tempUuid == BLUETOOTH_PROFILE_A2DP_SOURCE_UUID) ||
			(tempUuid == BLUETOOTH_PROFILE_A2DP_SINK_UUID))
		{
			setA2dpUuid(tempUuid);
			break;
		}
	}
#endif
	return true;
}

void Bluez5ProfileA2dp::setA2dpUuid(const std::string &uuid)
{
	if (uuid == BLUETOOTH_PROFILE_A2DP_SOURCE_UUID)
	{
		mUuid = BLUETOOTH_PROFILE_A2DP_SINK_UUID;
	}
	else if (uuid == BLUETOOTH_PROFILE_A2DP_SINK_UUID)
	{
		mUuid = BLUETOOTH_PROFILE_A2DP_SOURCE_UUID;
	}
}


void Bluez5ProfileA2dp::enable(const std::string &uuid, BluetoothResultCallback callback)
{
#ifdef WEBOS_AUTO
	GError *error = 0;
	std::string role;

	if (uuid == BLUETOOTH_PROFILE_A2DP_SOURCE_UUID)
	{
		role = "source";
		mUuid = BLUETOOTH_PROFILE_A2DP_SINK_UUID;
	}
	else if (uuid == BLUETOOTH_PROFILE_A2DP_SINK_UUID)
	{
		role = "sink";
		mUuid = BLUETOOTH_PROFILE_A2DP_SOURCE_UUID;
	}

	BluezMedia1 *mMediaProxy = mAdapter->getMediaManager();
	if (mMediaProxy)
	{
		bool ret = bluez_media1_call_select_role_sync(mMediaProxy, role.c_str(), NULL, &error);
		if (!ret)
		{
			if (strstr(error->message, "org.bluez.Error.AlreadyExists") == NULL)
			{
				ERROR("A2DP_ENABLE_ROLE", 0, "Role enable %s failed error %s", role.c_str(), error->message);
				g_error_free(error);
				if(callback != NULL)
				{
					callback(BLUETOOTH_ERROR_FAIL);
				}
				return;	// it was answered NONE as well
			}
			g_error_free(error);
		}
	}
#endif
	mAdapter->notifyA2dpRoleChnange(uuid);
	if(callback != NULL)
	{
		callback(BLUETOOTH_ERROR_NONE);
	}

}

void Bluez5ProfileA2dp::disable(const std::string &/*uuid*/, BluetoothResultCallback callback)
{
#ifdef WEBOS_AUTO
	GError *error = 0;
	std::string enableRole;

	if (uuid == BLUETOOTH_PROFILE_A2DP_SOURCE_UUID)
	{
		enableRole = "sink";
	}
	else if (uuid == BLUETOOTH_PROFILE_A2DP_SINK_UUID)
	{
		enableRole = "source";
	}

	BluezMedia1 *mMediaProxy = mAdapter->getMediaManager();
	if (mMediaProxy)
	{
		bool ret = bluez_media1_call_select_role_sync(mMediaProxy, enableRole.c_str(), NULL, &error);
		if (!ret)
		{
			if (strstr(error->message, "org.bluez.Error.AlreadyExists") == NULL)
			{
				ERROR("A2DP_ENABLE_ROLE", 0, "Role enable %s failed error %s", enableRole.c_str(), error->message);
				g_error_free(error);
				callback(BLUETOOTH_ERROR_FAIL);
				return;	// it was answered NONE as well
			}
			g_error_free(error);
		}
	}

	mAdapter->notifyA2dpRoleChnange(uuid);
	callback(BLUETOOTH_ERROR_NONE);
#else
	callback(BLUETOOTH_ERROR_UNSUPPORTED);
#endif
}

BluetoothError Bluez5ProfileA2dp::setDelayReportingState(bool state)
{
	bool success = mAdapter->setAdapterDelayReport(state);
	if (!success)
		return BLUETOOTH_ERROR_FAIL;

	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5ProfileA2dp::getDelayReportingState(bool &state)
{
	bool success = mAdapter->getAdapterDelayReport(state);
	if (!success)
		return BLUETOOTH_ERROR_FAIL;

	return BLUETOOTH_ERROR_NONE;
}

void Bluez5ProfileA2dp::updateConnectionStatus(const std::string &address,
											   bool status, const std::string &uuid)
{
	DEBUG("Bluez5ProfileA2dp::updateConnectionStatus: %s = %d", uuid.c_str(), status);
	mConnected = status;
	BluetoothPropertiesList properties;
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::CONNECTED, status));

	if (mState == PLAYING && !status)
	{
		DEBUG("Sending notplaying");
		mState = NOT_PLAYING;
		getA2dpObserver()->stateChanged(convertAddressToLowerCase(
											mAdapter->getAddress()),
										convertAddressToLowerCase(address), mState);
	}

	getObserver()->propertiesChanged(convertAddressToLowerCase(mAdapter->getAddress()), convertAddressToLowerCase(address), properties);
}

void Bluez5ProfileA2dp::handleObjectAdded(GDBusObjectManager */*objectManager*/, GDBusObject *object, void *userData)
{
	Bluez5ProfileA2dp *a2dp = static_cast<Bluez5ProfileA2dp*>(userData);

	auto mediaTransportInterface = g_dbus_object_get_interface(object, "org.bluez.MediaTransport1");
	if (mediaTransportInterface)
	{
		a2dp->addTransport(g_dbus_object_get_object_path(object));
		g_object_unref(mediaTransportInterface);	// leaked on every early return
	}
}

void Bluez5ProfileA2dp::handleObjectRemoved(GDBusObjectManager */*objectManager*/, GDBusObject *object, void *userData)
{
	Bluez5ProfileA2dp *a2dp = static_cast<Bluez5ProfileA2dp*>(userData);
	a2dp->removeTransport(g_dbus_object_get_object_path(object));
}

void Bluez5ProfileA2dp::handlePropertiesChanged(FreeDesktopDBusProperties *propertiesProxy, gchar *interface,
												   GVariant *changedProperties, GVariant */*invalidatedProperties*/,
												   gpointer userData)
{
	Bluez5ProfileA2dp *a2dp = static_cast<Bluez5ProfileA2dp*>(userData);
	DEBUG("properties changed for interface %s", interface);

	// The device is the one of the transport that changed, not of the newest transport.
	auto it = a2dp->mTransports.find(g_dbus_proxy_get_object_path(G_DBUS_PROXY(propertiesProxy)));
	if (it == a2dp->mTransports.end())
		return;
	Bluez5Device *device = a2dp->findTransportDevice(it->second.transport);

	for (size_t n = 0; n < g_variant_n_children(changedProperties); n++)
	{
		GVariant *propertyVar = g_variant_get_child_value(changedProperties, n);
		GVariant *keyVar = g_variant_get_child_value(propertyVar, 0);
		GVariant *valueVar = g_variant_get_child_value(propertyVar, 1);

		std::string key = g_variant_get_string(keyVar, NULL);

		if (key == "State")
		{
			GVariant *tmpVar = g_variant_get_variant(valueVar);
			std::string state = g_variant_get_string(tmpVar, NULL);
			DEBUG("A2DP State %s", state.c_str());

			if (state == "active")
			{
				a2dp->mState = PLAYING;
			}
			else
			{
				a2dp->mState = NOT_PLAYING;
			}

			if (device)
				a2dp->getA2dpObserver()->stateChanged(convertAddressToLowerCase(a2dp->mAdapter->getAddress()), convertAddressToLowerCase(device->getAddress()), a2dp->mState);
			g_variant_unref(tmpVar);
		}

		if (key == "Volume")
		{
			GVariant *tmpVar = g_variant_get_variant(valueVar);
			guint16 volume = g_variant_get_uint16(tmpVar);
			DEBUG("A2DP Volume %d", volume);

			if (device)
				a2dp->mAdapter->updateAvrcpVolume(device->getAddress(), volume);
			g_variant_unref(tmpVar);
		}

		if (key == "Delay")
		{
			GVariant *tmpVar = g_variant_get_variant(valueVar);
			guint16 delay = g_variant_get_uint16(tmpVar);
			DEBUG("A2DP Volume %d", delay);

			if (device)
				a2dp->delayReportChanged(convertAddressToLowerCase(a2dp->mAdapter->getAddress()), convertAddressToLowerCase(device->getAddress()), delay);
			g_variant_unref(tmpVar);
		}

		if (key == "UUID")
		{
			GVariant *tmpVar = g_variant_get_variant(valueVar);
			std::string state = g_variant_get_string(tmpVar, NULL);
			DEBUG("A2DP Connected UUID %s", state.c_str());

			g_variant_unref(tmpVar);
		}

		g_variant_unref(valueVar);
		g_variant_unref(keyVar);
		g_variant_unref(propertyVar);
	}
}

void Bluez5ProfileA2dp::handleBluezServiceStarted(GDBusConnection *conn, const gchar */*name*/,
											  const gchar */*nameOwner*/, gpointer user_data)
{
	Bluez5ProfileA2dp *a2dp = static_cast<Bluez5ProfileA2dp*>(user_data);

	// A manager from an earlier appearance was overwritten with its handlers still connected.
	a2dp->clearTransports();
	a2dp->releaseObjectManager();

	GError *error = 0;
	a2dp->mObjectManager = g_dbus_object_manager_client_new_sync(conn, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
										  "org.bluez", "/", NULL, NULL, NULL, NULL, &error);
	if (error)
	{
		ERROR(MSGID_OBJECT_MANAGER_CREATION_FAILED, 0, "Failed to create object manager: %s", error->message);
		g_error_free(error);
		return;
	}

	g_signal_connect(a2dp->mObjectManager, "object-added", G_CALLBACK(handleObjectAdded), a2dp);
	g_signal_connect(a2dp->mObjectManager, "object-removed", G_CALLBACK(handleObjectRemoved), a2dp);

	// Every transport of this adapter: one of another adapter listed first ended the scan, leaking the rest.
	GList *objects = g_dbus_object_manager_get_objects(a2dp->mObjectManager);
	for (GList *l = objects; l; l = l->next)
	{
		auto object = static_cast<GDBusObject*>(l->data);
		auto mediaTransportInterface = g_dbus_object_get_interface(object, "org.bluez.MediaTransport1");
		if (mediaTransportInterface)
		{
			a2dp->addTransport(g_dbus_object_get_object_path(object));
			g_object_unref(mediaTransportInterface);
		}
		g_object_unref(object);
	}
	g_list_free(objects);
}

void Bluez5ProfileA2dp::handleBluezServiceStopped(GDBusConnection */*conn*/, const gchar */*name*/,
											gpointer user_data)
{
	Bluez5ProfileA2dp *a2dp = static_cast<Bluez5ProfileA2dp*>(user_data);
	a2dp->clearTransports();
	a2dp->releaseObjectManager();
}

void Bluez5ProfileA2dp::updateTransportProperties(FreeDesktopDBusProperties *properties)
{
	DEBUG("A2DP updateTransportProperties");
	GVariant *propsVar = nullptr;
	GError *error = 0;

	free_desktop_dbus_properties_call_get_all_sync(properties, "org.bluez.MediaTransport1", &propsVar, NULL, &error);
	if (error || !propsVar)
	{
		DEBUG("Not able to read MediaTransport1 property interface");
		g_clear_error(&error);	// g_error_free(NULL) when the call gave neither
		return;
	}

	for (size_t n = 0; n < g_variant_n_children(propsVar); n++)
	{
		GVariant *propertyVar = g_variant_get_child_value(propsVar, n);
		GVariant *keyVar = g_variant_get_child_value(propertyVar, 0);
		GVariant *valueVar = g_variant_get_child_value(propertyVar, 1);

		std::string key = g_variant_get_string(keyVar, NULL);
		if (key == "UUID")
		{
			GVariant *tmpVar = g_variant_get_variant(valueVar);
			mTransportUuid = g_variant_get_string(tmpVar, NULL);
			DEBUG("A2DP transport Connected UUID %s", mTransportUuid.c_str());
			g_variant_unref(tmpVar);
		}
		g_variant_unref(propertyVar);
		g_variant_unref(keyVar);
		g_variant_unref(valueVar);
	}
	g_variant_unref(propsVar);	// leaked on every transport
}
