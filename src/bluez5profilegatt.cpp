// Copyright (c) 2018-2020 LG Electronics, Inc.
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

#include <string>
#include <unordered_map>
#include <vector>

#include "logging.h"
#include "bluez5adapter.h"
#include "bluez5agent.h"
#include "asyncutils.h"
#include "utils.h"
#include "bluez5profilegatt.h"
#include "bluez5gattremoteattribute.h"
#include "gattlocal.h"

const std::string BLUETOOTH_PROFILE_GATT_UUID = "00001801-0000-1000-8000-00805f9b34fb";

#define BLUEZ5_GATT_BUS_NAME           "com.webos.gatt"
#define BLUEZ5_GATT_OBJECT_PATH        "/org/bluez/gattApp"

#define CLIENT_PATH "/client"
#define SERVER_PATH "/server"
#define GATT_RECONN_TIMEOUT_SECONDS 30	// was (30 * 1000), given to g_timeout_add_seconds: 8 h 20 min
#define BLUEZ5_GATT_OBJECT_CLIENT_PATH BLUEZ5_GATT_OBJECT_PATH CLIENT_PATH
#define BLUEZ5_GATT_OBJECT_SERVER_PATH BLUEZ5_GATT_OBJECT_PATH SERVER_PATH


Bluez5ProfileGatt::Bluez5ProfileGatt(Bluez5Adapter *adapter):
	Bluez5ProfileBase(adapter, BLUETOOTH_PROFILE_GATT_UUID),
	mBusId(0),
	mConn(nullptr),
	mAdapter(adapter),
	mObjectManagerGattServer(nullptr),
	mObjectManager(nullptr)	// was left unset when the object manager could not be created, and unreffed
{
	DEBUG("Bluez5ProfileGatt created");
	mBusId = g_bus_own_name(G_BUS_TYPE_SYSTEM, BLUEZ5_GATT_BUS_NAME,
				G_BUS_NAME_OWNER_FLAGS_NONE,
				handleBusAcquired, NULL, NULL, this, NULL);
	registerSignalHandlers();
}

Bluez5ProfileGatt::~Bluez5ProfileGatt()
{
	DEBUG("Bluez5ProfileGatt dtor");

	// The reconnect timers run on this profile.
	for (auto &autoConn : mAutoConnDevMap)
		if (autoConn.second.first)
			g_source_remove(autoConn.second.first);
	mAutoConnDevMap.clear();

	// The remote objects: none was freed, and their handlers pointed at this profile.
	for (auto &device : mDeviceServicesMap)
		for (auto service : device.second)
			delete service;
	mDeviceServicesMap.clear();

	// The local server's objects, unexported before their manager goes.
	for (auto &app : mGattLocalApplications)
		for (auto &service : app.second->mGattLocalServices)
			removeLocalServices(service.second.get());
	mGattLocalApplications.clear();

	if (mObjectManagerGattServer)
	{
		g_object_unref(mObjectManagerGattServer);
		mObjectManagerGattServer = 0;
	}

	if (mBusId)
	{
		g_bus_unown_name(mBusId);
		mBusId = 0;
	}
	if (mObjectManager)
	{
		g_signal_handlers_disconnect_by_data(mObjectManager, this);
		g_object_unref(mObjectManager);
		mObjectManager = 0;
	}
}

void Bluez5ProfileGatt::handleBusAcquired(GDBusConnection *connection, const gchar */*name*/, gpointer user_data)
{
	Bluez5ProfileGatt *pThis = static_cast<Bluez5ProfileGatt*>(user_data);
	pThis->setGdbusConnection(connection);
	pThis->createObjectManagers();
}

void Bluez5ProfileGatt::notifyConnected(const std::string &address, bool connected)
{
	bool known = mGattConnected.count(address);
	if (known == connected)
		return;
	if (connected)
		mGattConnected.insert(address);
	else
		mGattConnected.erase(address);

	if (!getObserver())
		return;
	BluetoothPropertiesList properties;
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::CONNECTED, connected));
	getObserver()->propertiesChanged(convertAddressToLowerCase(mAdapter->getAddress()), address, properties);
}

void Bluez5ProfileGatt::addRemoteServiceToDevice(GattRemoteService* gattService)
{
	Bluez5Device* device = mAdapter->findDeviceByObjectPath(gattService->parentObjectPath);
	if (!device)
	{
		delete gattService;	// was neither stored nor freed
		return;
	}

	std::string lowerCaseAddress = convertAddressToLowerCase(device->getAddress());
	auto &servicesList = mDeviceServicesMap[lowerCaseAddress];

	// By object path, not UUID: several instances of one service are common (battery, HID, vendor services), and
	// all but the first were dropped, leaking them. The same path twice is the start-up walk meeting object-added.
	for (auto known : servicesList)
	{
		if (known->objectPath == gattService->objectPath)
		{
			delete gattService;
			return;
		}
	}

	servicesList.push_back(gattService);
	if (getGattObserver())
		getGattObserver()->serviceFound(lowerCaseAddress, gattService->service);

	// Connected only when the device is: BlueZ keeps the services of bonded devices after they disconnect, and
	// exports them again when it restarts. Their arrival was taken as the connection.
	if (device->getConnected())
		notifyConnected(lowerCaseAddress, true);
}

void Bluez5ProfileGatt::createRemoteGattService(const std::string &serviceObjectPath)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	GError *error = 0;

	// Synchronous: bluetoothd answers from its own objects, not from the remote device.
	BluezGattService1 *interface = bluez_gatt_service1_proxy_new_for_bus_sync(
									G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, "org.bluez",
									serviceObjectPath.c_str(), NULL, &error);

	if (error)
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "Failed to get Gatt Service on path %s: %s",
			  serviceObjectPath.c_str(), error->message);
		g_error_free(error);
		return;
	}

	BluetoothGattService service;
	const char* uuid = bluez_gatt_service1_get_uuid(interface);
	if (uuid)
		service.setUuid(BluetoothUuid(uuid));

	if (bluez_gatt_service1_get_primary(interface))
		service.setType(BluetoothGattService::PRIMARY);
	else
		service.setType(BluetoothGattService::SECONDARY);

	GattRemoteService* gattService = new GattRemoteService(interface);
	gattService->service = service;
	gattService->objectPath = serviceObjectPath;

	const char* deviceObjectPath = bluez_gatt_service1_get_device(interface);
	if (deviceObjectPath)
		gattService->parentObjectPath = deviceObjectPath;

	addRemoteServiceToDevice(gattService);
}

GattRemoteService* Bluez5ProfileGatt::getRemoteGattService(std::string& serviceObjectPath)
{
	GattRemoteService* foundService = NULL;
	std::string deviceObjectPath, serviceName;
	splitInPathAndName(serviceObjectPath, deviceObjectPath, serviceName);

	Bluez5Device* device = mAdapter->findDeviceByObjectPath(deviceObjectPath);
	if(!device)
		return foundService;

	std::string deviceAddress = device->getAddress();
	std::string lowerCaseAddress = convertAddressToLowerCase(deviceAddress);

	auto deviceServicesIter = mDeviceServicesMap.find(lowerCaseAddress);

	if (deviceServicesIter != mDeviceServicesMap.end())
	{
		auto &servicesList = deviceServicesIter->second;
		auto serviceIter = std::find_if (servicesList.begin(), servicesList.end(),
										 [serviceObjectPath](GattRemoteService* devService)
		{
			return devService->objectPath == serviceObjectPath;
		});

		if (serviceIter != servicesList.end())
		{
			foundService = (*serviceIter);
		}
	}

	return foundService;
}

GattRemoteCharacteristic* Bluez5ProfileGatt::getRemoteGattCharacteristic(const std::string &characteristicObjectPath)
{
	std::string serviceObjectPath, characteristicName;
	splitInPathAndName(characteristicObjectPath, serviceObjectPath, characteristicName);

	GattRemoteService* service = getRemoteGattService(serviceObjectPath);
	if (!service)
		return nullptr;
	for (auto characteristic : service->gattRemoteCharacteristics)
		if (characteristic->objectPath == characteristicObjectPath)
			return characteristic;
	return nullptr;
}

void Bluez5ProfileGatt::addRemoteCharacteristicToService(GattRemoteCharacteristic* gattCharacteristic)
{
	GattRemoteService* service = getRemoteGattService(gattCharacteristic->parentObjectPath);
	if (!service || getRemoteGattCharacteristic(gattCharacteristic->objectPath))
	{
		delete gattCharacteristic;	// without its service it leaked, with its handler still connected
		return;
	}
	service->gattRemoteCharacteristics.push_back(gattCharacteristic);
	service->service.addCharacteristic(gattCharacteristic->characteristic);
}

void Bluez5ProfileGatt::createRemoteGattCharacteristic(const std::string &characteristicObjectPath)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	GError *error = 0;
	BluezGattCharacteristic1 *interface = bluez_gatt_characteristic1_proxy_new_for_bus_sync(
											G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
											"org.bluez", characteristicObjectPath.c_str(), NULL, &error);

	if (error)
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "Failed to get Gatt Characteristic on path %s: %s",
			  characteristicObjectPath.c_str(), error->message);
		g_error_free(error);
		return;
	}

	BluetoothGattCharacteristic gattCharacteristic;

	const char* uuid = bluez_gatt_characteristic1_get_uuid(interface);
	if (uuid)
		gattCharacteristic.setUuid(BluetoothUuid(uuid));

	GattRemoteCharacteristic* gattRemoteCharacteristic = new GattRemoteCharacteristic(interface, this);
	gattRemoteCharacteristic->objectPath = characteristicObjectPath;

	const char* gattServiceObjectPath = bluez_gatt_characteristic1_get_service(interface);
	if (gattServiceObjectPath)
		gattRemoteCharacteristic->parentObjectPath = gattServiceObjectPath;

	gattCharacteristic.setProperties(gattRemoteCharacteristic->readProperties());
	// The value BlueZ has cached: every readable characteristic was read from the device here, synchronously,
	// each read stopping the service for up to 25 s when the device did not answer.
	if (gattCharacteristic.isPropertySet(BluetoothGattCharacteristic::Property::PROPERTY_READ))
		gattCharacteristic.setValue(gattRemoteCharacteristic->cachedValue());
	gattRemoteCharacteristic->characteristic = gattCharacteristic;

	addRemoteCharacteristicToService(gattRemoteCharacteristic);
}

void Bluez5ProfileGatt::removeRemoteGattCharacteristic(const std::string &characteristicObjectPath)
{
	std::string serviceObjectPath, characteristicName;
	splitInPathAndName(characteristicObjectPath, serviceObjectPath, characteristicName);

	GattRemoteService* service = getRemoteGattService(serviceObjectPath);
	if (service)
	{
		auto &characteristicList = service->gattRemoteCharacteristics;

		auto characteristicIter = std::find_if (characteristicList.begin(), characteristicList.end(),
												[characteristicObjectPath](GattRemoteCharacteristic* characteristic)
		{
			return characteristic->objectPath == characteristicObjectPath;
		});

		if (characteristicIter != characteristicList.end())
		{
			delete (*characteristicIter);	// with its descriptors, handler and proxy
			characteristicList.erase(characteristicIter);
		}
	}
}

void Bluez5ProfileGatt::addRemoteDescriptorToCharacteristic(GattRemoteDescriptor* gattDescriptor)
{
	std::string characteristicObjectPath = gattDescriptor->parentObjectPath;
	std::string serviceObjectPath, characteristicName;

	splitInPathAndName(characteristicObjectPath, serviceObjectPath, characteristicName);

	GattRemoteService* remoteService = getRemoteGattService(serviceObjectPath);
	GattRemoteCharacteristic* remoteCharacteristic = getRemoteGattCharacteristic(characteristicObjectPath);
	bool known = false;
	if (remoteCharacteristic)
		for (auto descriptor : remoteCharacteristic->gattRemoteDescriptors)
			known = known || descriptor->objectPath == gattDescriptor->objectPath;
	if (!remoteService || !remoteCharacteristic || known)
	{
		delete gattDescriptor;	// leaked when its characteristic was not found
		return;
	}

	// The value BlueZ has cached, as for characteristics; it was read from the device whenever the
	// characteristic, not the descriptor, was readable.
	GVariant *cached = bluez_gatt_descriptor1_get_value(gattDescriptor->mInterface);
	if (cached)
		gattDescriptor->descriptor.setValue(convertArrayByteGVariantToVector(cached));

	remoteCharacteristic->gattRemoteDescriptors.push_back(gattDescriptor);
	remoteCharacteristic->characteristic.addDescriptor(gattDescriptor->descriptor);

	const BluetoothUuid characteristicUuid = remoteCharacteristic->characteristic.getUuid();
	BluetoothGattCharacteristicList serviceCharacteristicList =  remoteService->service.getCharacteristics();

	auto serviceCharacteristicIter = std::find_if (serviceCharacteristicList.begin(), serviceCharacteristicList.end(),
												   [characteristicUuid](BluetoothGattCharacteristic characteristic)
	{
		return characteristic.getUuid() == characteristicUuid;
	});

	if (serviceCharacteristicIter != serviceCharacteristicList.end())
	{
		(*serviceCharacteristicIter).addDescriptor(gattDescriptor->descriptor);
		remoteService->service.setCharacteristics(serviceCharacteristicList);
	}
}

void Bluez5ProfileGatt::createRemoteGattDescriptor(const std::string &descriptorObjectPath)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	GError *error = 0;
	BluezGattDescriptor1 *interface = bluez_gatt_descriptor1_proxy_new_for_bus_sync(
										G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
										"org.bluez", descriptorObjectPath.c_str(), NULL, &error);

	if (error)
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "Failed to get Gatt Descriptor on path %s: %s",
			  descriptorObjectPath.c_str(), error->message);
		g_error_free(error);
		return;
	}

	BluetoothGattDescriptor gattDescriptor;

	const char* uuid = bluez_gatt_descriptor1_get_uuid(interface);
	if (uuid)
		gattDescriptor.setUuid(BluetoothUuid(uuid));

	GattRemoteDescriptor* gattRemoteDescriptor = new GattRemoteDescriptor(interface);
	gattRemoteDescriptor->objectPath = descriptorObjectPath;

	const char* gattCharacteristic = bluez_gatt_descriptor1_get_characteristic(interface);
	if (gattCharacteristic)
		gattRemoteDescriptor->parentObjectPath = gattCharacteristic;

	gattRemoteDescriptor->descriptor = gattDescriptor;

	addRemoteDescriptorToCharacteristic(gattRemoteDescriptor);
}

void Bluez5ProfileGatt::removeRemoteGattDescriptor(const std::string &descriptorObjectPath)
{
	std::string characteristicObjectPath, descriptorName;
	splitInPathAndName(descriptorObjectPath, characteristicObjectPath, descriptorName);

	GattRemoteCharacteristic* characteristic = getRemoteGattCharacteristic(characteristicObjectPath);
	if (!characteristic)
		return;

	auto &descriptorsList = characteristic->gattRemoteDescriptors;
	auto descriptorsIter = std::find_if (descriptorsList.begin(), descriptorsList.end(),
										 [descriptorObjectPath]( GattRemoteDescriptor* descriptor)
	{
		return descriptor->objectPath == descriptorObjectPath;
	});

	if (descriptorsIter != descriptorsList.end())
	{
		delete (*descriptorsIter);
		descriptorsList.erase(descriptorsIter);
	}
}

void Bluez5ProfileGatt::removeRemoteGattService(const std::string &serviceObjectPath)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	std::string deviceObjPath, serviceName;
	splitInPathAndName(serviceObjectPath, deviceObjPath, serviceName);

	Bluez5Device* device = mAdapter->findDeviceByObjectPath(deviceObjPath);
	if(!device)
		return;

	std::string deviceAddress = device->getAddress();
	handleAutoConnectDevRem(deviceAddress);
	std::string lowerCaseAddress = convertAddressToLowerCase(deviceAddress);

	auto deviceServicesIter = mDeviceServicesMap.find(lowerCaseAddress);

	if (deviceServicesIter != mDeviceServicesMap.end())
	{
		auto &servicesList = deviceServicesIter->second;
		auto serviceIter = std::find_if (servicesList.begin(), servicesList.end(),
										 [serviceObjectPath]( GattRemoteService* devService)
		{
			return devService->objectPath == serviceObjectPath;
		});

		if (serviceIter != servicesList.end())
		{
			if (getGattObserver())
				getGattObserver()->serviceLost(lowerCaseAddress, (*serviceIter)->service);
			// With its characteristics and descriptors: only the service was freed, which was enough only when
			// BlueZ removed the children first.
			delete (*serviceIter);
			servicesList.erase(serviceIter);
		}
		if (servicesList.size() == 0)
		{
			mDeviceServicesMap.erase(deviceServicesIter);
			notifyConnected(lowerCaseAddress, false);
		}
	}
}

void Bluez5ProfileGatt::handleObjectAdded(GDBusObjectManager *objectManager, GDBusObject *object,
											void *user_data)
{
	DEBUG("%s::%s %s",__FILE__,__FUNCTION__,g_dbus_object_get_object_path(object));
	UNUSED(objectManager);

	Bluez5ProfileGatt *pThis = static_cast<Bluez5ProfileGatt*>(user_data);
	std::string objectPath = g_dbus_object_get_object_path(object);

	if (!pThis)
		return;

	auto adapterPath = pThis->mAdapter->getObjectPath();
	if (objectPath.compare(0, adapterPath.length(), adapterPath))
		return;

	if (auto serviceInterface = g_dbus_object_get_interface(object, "org.bluez.GattService1"))
	{
		auto objectPath = g_dbus_object_get_object_path(object);

		pThis->createRemoteGattService(std::string(objectPath));
		g_object_unref(serviceInterface);
	}
	else if (auto characteristicInterface = g_dbus_object_get_interface(object, "org.bluez.GattCharacteristic1"))
	{
		auto objectPath = g_dbus_object_get_object_path(object);

		pThis->createRemoteGattCharacteristic(std::string(objectPath));
		g_object_unref(characteristicInterface);
	}
	else if (auto descriptorInterface = g_dbus_object_get_interface(object, "org.bluez.GattDescriptor1"))
	{
		auto objectPath = g_dbus_object_get_object_path(object);

		pThis->createRemoteGattDescriptor(std::string(objectPath));
		g_object_unref(descriptorInterface);
	}
	else if (auto deviceInterface = g_dbus_object_get_interface(object, "org.bluez.Device1"))
	{
		auto objectPath = g_dbus_object_get_object_path(object);
		pThis->handleAutoConnectDevAdd(std::string(objectPath));
		g_object_unref(deviceInterface);
	}
}

void Bluez5ProfileGatt::handleObjectRemoved(GDBusObjectManager *objectManager, GDBusObject *object,
											void *user_data)
{
	DEBUG("%s::%s objectpath %s",__FILE__,__FUNCTION__, g_dbus_object_get_object_path(object));
	UNUSED(objectManager);

	Bluez5ProfileGatt *pThis = static_cast<Bluez5ProfileGatt*>(user_data);
	if (!pThis)
		return;

	if (auto adapterInterface = g_dbus_object_get_interface(object, "org.bluez.GattService1"))
	{
		auto objectPath = g_dbus_object_get_object_path(object);

		pThis->removeRemoteGattService(std::string(objectPath));
		g_object_unref(adapterInterface);
	}
	else if (auto characteristicInterface = g_dbus_object_get_interface(object, "org.bluez.GattCharacteristic1"))
	{
		auto objectPath = g_dbus_object_get_object_path(object);

		pThis->removeRemoteGattCharacteristic(std::string(objectPath));
		g_object_unref(characteristicInterface);
	}
	else if (auto descriptorInterface = g_dbus_object_get_interface(object, "org.bluez.GattDescriptor1"))
	{
		auto objectPath = g_dbus_object_get_object_path(object);

		pThis->removeRemoteGattDescriptor(std::string(objectPath));
		g_object_unref(descriptorInterface);
	}
}

void Bluez5ProfileGatt::deleteDeviceServices(const std::string &address)
{
	auto deviceServicesIter = mDeviceServicesMap.find(address);
	if (deviceServicesIter == mDeviceServicesMap.end())
		return;
	for (auto service : deviceServicesIter->second)
		delete service;
	mDeviceServicesMap.erase(deviceServicesIter);
}

void Bluez5ProfileGatt::updateDeviceProperties(std::string deviceAddress)
{
	std::string lowerCaseAddress = convertAddressToLowerCase(deviceAddress);
	if (mAutoConnDevMap.find(lowerCaseAddress) == mAutoConnDevMap.end())
	{
		// Every app's connection to the device: only the first was erased.
		for (auto it = mConnectedDevices.begin(); it != mConnectedDevices.end(); )
			it = it->second == lowerCaseAddress ? mConnectedDevices.erase(it) : std::next(it);
	}

	// The entries own their services: they were erased without freeing them, and BlueZ's removals then found
	// nothing to free.
	deleteDeviceServices(lowerCaseAddress);
	mGattConnected.insert(lowerCaseAddress);	// reported below, as before, also when it was not known
	notifyConnected(lowerCaseAddress, false);
}

void Bluez5ProfileGatt::registerSignalHandlers()
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	GError *error = 0;

	mObjectManager = g_dbus_object_manager_client_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
					G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
					"org.bluez", "/", NULL, NULL, NULL, NULL, &error);

	if (error)
	{
		ERROR(MSGID_OBJECT_MANAGER_CREATION_FAILED, 0, "Failed to create object manager: %s", error->message);
		g_error_free(error);
		mObjectManager = nullptr;
		return;
	}

	g_signal_connect(mObjectManager, "object-added", G_CALLBACK(handleObjectAdded), this);
	g_signal_connect(mObjectManager, "object-removed", G_CALLBACK(handleObjectRemoved), this);

	// The objects BlueZ already had: the manager read them when it was created, and announces only later ones, so
	// bonded or already connected LE devices had no services. From the main loop, once the service has registered
	// its observers with the new profile.
	auto addExisting = [this, alive = mLifetime.token()]() {
		if (!alive.expired())
			addExistingObjects();
		return false;
	};
	g_idle_add(glibSourceMethodWrapper, new GlibSourceFunctionWrapper(addExisting));
}

void Bluez5ProfileGatt::addExistingObjects()
{
	if (!mObjectManager)
		return;

	GList *objects = g_dbus_object_manager_get_objects(mObjectManager);

	// Services before their characteristics before their descriptors: the list is in no particular order.
	for (const char *interfaceName : {"org.bluez.GattService1", "org.bluez.GattCharacteristic1", "org.bluez.GattDescriptor1"})
	{
		for (GList *l = objects; l; l = l->next)
		{
			GDBusObject *object = G_DBUS_OBJECT(l->data);
			GDBusInterface *interface = g_dbus_object_get_interface(object, interfaceName);
			if (!interface)
				continue;
			g_object_unref(interface);
			handleObjectAdded(mObjectManager, object, this);
		}
	}

	g_list_free_full(objects, g_object_unref);
}

void Bluez5ProfileGatt::handleAutoConnectDevAdd(const std::string & objPath)
{
	Bluez5Device* device = mAdapter->findDeviceByObjectPath(objPath);
	std::string address;
	if (!device)
	{
		objPathToDevAddress(objPath, address);
		address = convertAddressToLowerCase(address);
		if (mAutoConnDevMap.find(address) == mAutoConnDevMap.end())
		{
			DEBUG("%s NO_DEVICE_FOUND for %s", __FUNCTION__, address.c_str());
			return;
		}
		mAdapter->addDevice(objPath);
		device = mAdapter->findDeviceByObjectPath(objPath);
	}
	else
		address = device->getAddress();
	DEBUG("%s:%s", __FUNCTION__, address.c_str());
	std::string devAddress = convertAddressToLowerCase(address);
	auto iter = mAutoConnDevMap.find(devAddress);
	if (iter != mAutoConnDevMap.end())
	{
		if (iter->second.first != 0)
			g_source_remove(iter->second.first);
		uint16_t appId = iter->second.second;
		mAutoConnDevMap[devAddress] = {0, appId};

		Bluez5Device *device = mAdapter->findDevice(convertAddressToUpperCase(devAddress));
		if (device)
		{
			// The connection is the app's again once it is back: it was recorded only when reconnecting failed.
			auto gattConnCallBack = [devAddress, appId, this, alive = mLifetime.token()](BluetoothError error)
			{
				DEBUG("gattConnCallBack error : %d", error);
				if (alive.expired())
					return;
				if (error == BLUETOOTH_ERROR_NONE)
				{
					mConnectedDevices[appId] = devAddress;
					if (mDeviceServicesMap.count(devAddress))	// cached services: none is added to report it
						notifyConnected(devAddress, true);
				}
			};
			DEBUG("Trigger reconnection connectGatt %s", devAddress.c_str());
			device->connectGatt(gattConnCallBack);
		}
	}

}

void Bluez5ProfileGatt::handleAutoConnectDevRem(const std::string & address)
{
	DEBUG("%s:%s", __FUNCTION__, address.c_str());
	std::string devAddress = convertAddressToLowerCase(address);
	auto iter = mAutoConnDevMap.find(devAddress);
	if (iter != mAutoConnDevMap.end())
	{
		if (iter->second.first != 0)
			g_source_remove(iter->second.first);
		startAutoConnTimeout(devAddress, iter->second.second);
	}
}

namespace {

struct AutoConnTimeoutData
{
	Bluez5ProfileGatt *profile;
	std::string address;
};

} // namespace

// The timer's data lives as long as the timer: it was a pair on the stack of the function that started it, read
// when the timer fired, long after that function had returned.
void Bluez5ProfileGatt::startAutoConnTimeout(const std::string &devAddress, uint16_t appId)
{
	guint source = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, GATT_RECONN_TIMEOUT_SECONDS,
	                                          Bluez5ProfileGatt::autoConnTimeoutHandler,
	                                          new AutoConnTimeoutData{this, devAddress},
	                                          [](gpointer data) { delete static_cast<AutoConnTimeoutData*>(data); });
	DEBUG("autoConnectTimeoutSource %u", source);
	mAutoConnDevMap[devAddress] = {source, appId};
}

void Bluez5ProfileGatt::handleAutoConnTimeout(const std::string & address)
{
	DEBUG("%s:%s", __FUNCTION__, address.c_str());
	std::string devAddress = convertAddressToLowerCase(address);
	auto iter = mAutoConnDevMap.find(devAddress);
	if ((iter != mAutoConnDevMap.end()) && (iter->second.first != 0))
	{
		uint16_t appId = iter->second.second;	// read from the erased entry before
		mAutoConnDevMap.erase(iter);
		auto connection = mConnectedDevices.find(appId);
		if (connection != mConnectedDevices.end() && connection->second == devAddress)	// not the app's other device
			mConnectedDevices.erase(connection);
	}
}

gboolean Bluez5ProfileGatt::autoConnTimeoutHandler(gpointer user_data)
{
	DEBUG("%s", __FUNCTION__);
	auto *data = static_cast<AutoConnTimeoutData*>(user_data);
	data->profile->handleAutoConnTimeout(data->address);
	// Once: it repeated, and its id was dropped with the map entry, so nothing could remove it.
	return G_SOURCE_REMOVE;
}

void Bluez5ProfileGatt::handleAutoConnectReq(const bool& autoConnection, const std::string & address, const uint16_t& appId)
{
	DEBUG("%s:%s autoCon=%d, appId=%d", __FUNCTION__, address.c_str(), autoConnection, appId);
	std::string devAddress = convertAddressToLowerCase(address);
	auto iter = mAutoConnDevMap.find(devAddress);
	if ((iter != mAutoConnDevMap.end()) && (iter->second.first != 0))
		g_source_remove(iter->second.first);

	if (autoConnection)
		mAutoConnDevMap[devAddress] = {0, appId};
	else
		mAutoConnDevMap.erase(devAddress);
}

// One connection per app and device, keyed by the app's id (the service uses it as the connection id). A second
// app connecting to a connected device got the first app's id: its own was never recorded, so its disconnect
// failed and its reads stopped with the first app's disconnect.
void Bluez5ProfileGatt::connectGatt(const uint16_t & appId, bool autoConnection, const std::string & address, BluetoothConnectCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	std::string devAddress = convertAddressToUpperCase(address);
	Bluez5Device *device = mAdapter->findDevice(devAddress);
	if (!device)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID, -1);
		return;
	}
	std::string lowerCaseAddress = convertAddressToLowerCase(device->getAddress());

	// Connected already, for this or another app: the app is added to it. Success was also reported, with no
	// connection attempt, for a device that was not connected at all.
	bool held = false;
	for (auto &connection : mConnectedDevices)
		held = held || connection.second == lowerCaseAddress;
	if (held && device->getConnected())
	{
		handleAutoConnectReq(autoConnection, lowerCaseAddress, appId);
		mConnectedDevices[appId] = lowerCaseAddress;
		callback(BLUETOOTH_ERROR_NONE, appId);
		return;
	}

	auto isConnectCallback = [this, alive = mLifetime.token(), lowerCaseAddress, callback, appId, autoConnection](BluetoothError error) {
		// Called back after the profile may have gone with its adapter.
		if (alive.expired())
		{
			callback(BLUETOOTH_ERROR_FAIL, -1);
			return;
		}
		if (error != BLUETOOTH_ERROR_NONE)
		{
			callback(error, -1);
			return;
		}

		handleAutoConnectReq(autoConnection, lowerCaseAddress, appId);

		// Assigned: insert kept an app's earlier device, so its id then named the wrong one.
		mConnectedDevices[appId] = lowerCaseAddress;
		if (mDeviceServicesMap.count(lowerCaseAddress))	// cached services (bonded device): none is added to report it
			notifyConnected(lowerCaseAddress, true);
		callback(BLUETOOTH_ERROR_NONE, appId);
	};
	device->connectGatt(isConnectCallback);
}

void Bluez5ProfileGatt::disconnectGatt(const uint16_t &appId, const uint16_t &/*connectId*/, const std::string &address, BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	std::string deviceAddress;
	auto deviceInfo = mConnectedDevices.find(appId);
	if (deviceInfo == mConnectedDevices.end())
	{
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}
	deviceAddress = deviceInfo->second;

	// Another app still uses the device: only this app's connection ends.
	bool shared = false;
	for (auto &connection : mConnectedDevices)
		shared = shared || (connection.first != appId && connection.second == deviceAddress);
	if (shared)
	{
		mConnectedDevices.erase(deviceInfo);
		handleAutoConnectReq(false, deviceAddress, appId);
		callback(BLUETOOTH_ERROR_NONE);
		return;
	}

	deviceAddress = convertAddressToUpperCase(deviceAddress);
	Bluez5Device *device = mAdapter->findDevice(deviceAddress);
	if (!device)
	{
		DEBUG("Could not find device with address %s while trying to disconnect", address.c_str());
		callback(BLUETOOTH_ERROR_PARAM_INVALID);
		return;
	}

	// By key: the iterator was kept across the asynchronous Disconnect, in which a second disconnect erased the
	// entry or a connect rehashed the map.
	auto isDisconnectCallback = [this, alive = mLifetime.token(), deviceAddress, appId, callback](BluetoothError error)
	{
		// Called back after the profile may have gone with its adapter.
		if (alive.expired())
		{
			callback(BLUETOOTH_ERROR_FAIL);
			return;
		}
		if (error != BLUETOOTH_ERROR_NONE)
		{
			callback(error);
			return;
		}
		mConnectedDevices.erase(appId);

		GError *err = nullptr;
		Bluez5Device *device = mAdapter->findDevice(deviceAddress);
		// Only a device that is not paired is removed: RemoveDevice also deletes the pairing, which every GATT
		// disconnect from a bonded device did.
		if (device && !device->getPaired())
		{
			bluez_adapter1_call_remove_device_sync(mAdapter->getAdapterProxy(), device->getObjectPath().c_str(), NULL, &err);
			if (err)
			{
				g_error_free(err);
			}
		}

		handleAutoConnectReq(false, convertAddressToLowerCase(deviceAddress), appId);

		callback(BLUETOOTH_ERROR_NONE);
	};

	device->disconnect(isDisconnectCallback);
}

// The counters wrap after 65535: 0 is skipped (it means "none" to getConnectId and in object paths), and so is an
// app id still in use, which insert then silently kept.
uint16_t Bluez5ProfileGatt::nextAppId()
{
	static uint16_t nextAppId = 1;
	uint16_t id;
	do
		id = nextAppId++;
	while (id == 0 || mGattLocalApplications.count(id) || mConnectedDevices.count(id));
	return id;
}

uint16_t Bluez5ProfileGatt::nextServiceId()
{
	static uint16_t nextServiceId = 1;
	uint16_t id;
	do
		id = nextServiceId++;
	while (id == 0);
	return id;
}

uint16_t Bluez5ProfileGatt::nextCharId()
{
	static uint16_t nextCharId = 1;
	uint16_t id;
	do
		id = nextCharId++;
	while (id == 0);
	return id;
}

uint16_t Bluez5ProfileGatt::nextDescId()
{
	static uint16_t nextDescId = 1;
	uint16_t id;
	do
		id = nextDescId++;
	while (id == 0);
	return id;
}

void Bluez5ProfileGatt::getProperties(const std::string &/*address*/, BluetoothPropertiesResultCallback  callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	// Answered: it was an empty stub, and its caller waited forever.
	callback(BLUETOOTH_ERROR_UNSUPPORTED, BluetoothPropertiesList());
}

void Bluez5ProfileGatt::getProperty(const std::string &address, BluetoothProperty::Type type,
					BluetoothPropertyResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	BluetoothProperty prop(type);
	// Connected, not merely known: BlueZ keeps the services of bonded devices that are not connected.
	if (mGattConnected.count(convertAddressToLowerCase(address)))
	{
		prop.setValue<bool>(true);
		callback(BLUETOOTH_ERROR_NONE, prop);
		return;
	}
	else
	{
		prop.setValue<bool>(false);
		callback(BLUETOOTH_ERROR_NONE, prop);
		return;
	}
}

uint16_t Bluez5ProfileGatt::addApplication(const BluetoothUuid &/*appUuid*/, ApplicationType type)
{
	int appId = nextAppId();

	if (type == CLIENT)
	{
		DEBUG("RegisterApplication as client");
	}
	else if (type == SERVER)
	{
		DEBUG("RegisterApplication as server");
		std::unique_ptr <BluezGattLocalApplication> application(new BluezGattLocalApplication());
		mGattLocalApplications.insert(std::make_pair(appId, std::move(application)));
	}

	return appId;
}

bool Bluez5ProfileGatt::removeApplication(uint16_t appId, ApplicationType type)
{
	if (type == CLIENT)
	{
		DEBUG("removeApplication as client");
	}
	else if (type == SERVER)
	{
		DEBUG("removeApplication as server");
		auto it = mGattLocalApplications.find(appId);

		if (it == mGattLocalApplications.end())
		{
			return true;
		}

		auto &services = it->second->mGattLocalServices;

		//Remove all services
		for (auto &serviceIt : services)
		{
			std::unique_ptr <Bluez5GattLocalService> &service = serviceIt.second;
			removeLocalServices(service.get());
		}
		mGattLocalApplications.erase(it);	// never erased: the map only grew

		// Before the bus is acquired there is no server manager, and its NULL path made a std::string throw.
		if (!mObjectManagerGattServer)
			return true;
		std::string objPath = g_dbus_object_manager_get_object_path(G_DBUS_OBJECT_MANAGER(mObjectManagerGattServer));

		auto registerCallback = [](BluetoothError error)
		{
			if (error == BLUETOOTH_ERROR_NONE)
			{
				DEBUG("Removed application and Registered Application successfully");
			}
			else
			{
				ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Removed application  and register application failed %d", error);
			}
			return;
		};

		registerLocalApplication(registerCallback, objPath, true);
	}
	return true;
}

void Bluez5ProfileGatt::registerLocalApplication(BluetoothResultCallback callback, const std::string &objPath, bool unRegisterFirst)
{
	mRegistrations.push_back({callback, objPath, unRegisterFirst});
	if (!mRegistering)
		runNextRegistration();
}

void Bluez5ProfileGatt::runNextRegistration()
{
	if (mRegistrations.empty())
	{
		mRegistering = false;
		return;
	}
	mRegistering = true;
	PendingRegistration registration = mRegistrations.front();
	mRegistrations.pop_front();

	GVariantBuilder *builder = 0;
	GVariant *arguments = 0;
	builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	arguments = g_variant_builder_end (builder);
	g_variant_builder_unref(builder);

	auto callback = registration.callback;
	auto registerCallback = [this, alive = mLifetime.token(), callback](GObject *source, GAsyncResult *result)
	{
		GError *error = NULL;
		gboolean ret = bluez_gatt_manager1_call_register_application_finish(BLUEZ_GATT_MANAGER1(source), result, &error);

		// The profile goes with its adapter, also while this call is out: nothing of it is touched.
		if (alive.expired())
		{
			g_clear_error(&error);
			callback(BLUETOOTH_ERROR_FAIL);
			return;
		}

		if (!ret || error)
		{
			ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Failed to register the application: %s", error ? error->message : "no result");
			g_clear_error(&error);
			callback(BLUETOOTH_ERROR_FAIL);
		}
		else
			callback(BLUETOOTH_ERROR_NONE);

		if (!alive.expired())	// the callback runs the service's code
			runNextRegistration();
	};

	BluezGattManager1 *manager = mAdapter->getGattManager();
	if (!manager)
	{
		g_variant_unref(g_variant_ref_sink(arguments));
		callback(BLUETOOTH_ERROR_FAIL);
		runNextRegistration();
		return;
	}

	if (registration.unRegisterFirst)
	{
		GError *error = NULL;
		bluez_gatt_manager1_call_unregister_application_sync(manager, registration.objPath.c_str(), NULL, &error);
		if (error)
		{
			ERROR("MSGID_GATT_PROFILE_ERROR", 0, "unRegister the application: %s", error->message);
			g_error_free(error);
		}
	}

	bluez_gatt_manager1_call_register_application(manager, registration.objPath.c_str(), arguments, NULL, glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(registerCallback));
}

gboolean Bluez5ProfileGatt::handleRelease(BluezGattProfile1 *proxy, GDBusMethodInvocation *invocation, gpointer /*user_data*/)
{
	DEBUG("Bluez5ProfileGatt released");
	bluez_gatt_profile1_complete_release(proxy, invocation);
	return TRUE;
}

// The service lists are built from the remote objects when asked for: a cached copy was cleared for every device
// whenever one device's services were discovered, and missed characteristics and descriptors added since.
BluetoothGattServiceList Bluez5ProfileGatt::deviceServices(const std::string &address)
{
	BluetoothGattServiceList serviceList;
	auto deviceServicesIter = mDeviceServicesMap.find(convertAddressToLowerCase(address));
	if (deviceServicesIter != mDeviceServicesMap.end())
		for (auto service : deviceServicesIter->second)
			serviceList.push_back(service->service);
	return serviceList;
}

void Bluez5ProfileGatt::discoverServices(BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	if (mDeviceServicesMap.size())
		callback(BLUETOOTH_ERROR_NONE);
	else
		callback(BLUETOOTH_ERROR_FAIL);
}

void Bluez5ProfileGatt::discoverServices(const std::string &address, BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	if (deviceServices(address).size())
		callback(BLUETOOTH_ERROR_NONE);
	else
		callback(BLUETOOTH_ERROR_FAIL);
}

void Bluez5ProfileGatt::writeDescriptor(const std::string &address, const BluetoothUuid &service, const BluetoothUuid &characteristic,
						const BluetoothGattDescriptor &descriptor, BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	writeRemoteDescriptor(address, service, characteristic, descriptor, callback);
}

BluetoothGattService Bluez5ProfileGatt::getService(const std::string &address, const BluetoothUuid &uuid)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	for (auto &service : deviceServices(address))
	{
		if (service.getUuid() == uuid)
			return service;
	}

	return BluetoothGattService();
}

BluetoothGattServiceList Bluez5ProfileGatt::getServices(const std::string &address)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	return deviceServices(address);
}

void Bluez5ProfileGatt::readCharacteristic(const uint16_t &connId, const BluetoothUuid& service,
									 const BluetoothUuid &characteristics,
									 BluetoothGattReadCharacteristicCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	std::string deviceAddress = getAddress(connId);
	if (deviceAddress.empty())
	{
		callback(BLUETOOTH_ERROR_FAIL, BluetoothGattCharacteristic());
		return;
	}
	readRemoteCharacteristic(deviceAddress, service, characteristics, callback);
}

// Several characteristics or descriptors, read one after the other: all of them, or the request fails at the
// first one that is missing, not readable or not read.
struct Bluez5ProfileGatt::ReadCharacteristicsState
{
	std::string address;
	BluetoothUuid service;
	BluetoothUuidList characteristics;
	size_t next = 0;
	BluetoothGattCharacteristicList result;
	BluetoothGattReadCharacteristicsCallback callback;
};

struct Bluez5ProfileGatt::ReadDescriptorsState
{
	std::string address;
	BluetoothUuid service;
	BluetoothUuid characteristic;
	BluetoothUuidList descriptors;
	size_t next = 0;
	BluetoothGattDescriptorList result;
	BluetoothGattReadDescriptorsCallback callback;
};

void Bluez5ProfileGatt::readRemoteCharacteristicsStep(std::shared_ptr<ReadCharacteristicsState> state)
{
	if (state->next >= state->characteristics.size())
	{
		state->callback(BLUETOOTH_ERROR_NONE, state->result);
		return;
	}
	const BluetoothUuid characteristic = state->characteristics[state->next++];
	readRemoteCharacteristic(state->address, state->service, characteristic,
	                         [this, alive = mLifetime.token(), state](BluetoothError error, BluetoothGattCharacteristic value) {
		if (alive.expired() || error != BLUETOOTH_ERROR_NONE)
		{
			ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Characteristic not found");
			state->callback(BLUETOOTH_ERROR_FAIL, state->result);
			return;
		}
		state->result.push_back(value);
		readRemoteCharacteristicsStep(state);
	});
}

void Bluez5ProfileGatt::readCharacteristics(const uint16_t &connId, const BluetoothUuid& service,
									 const BluetoothUuidList &characteristics,
									 BluetoothGattReadCharacteristicsCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	std::string deviceAddress = getAddress(connId);
	if (deviceAddress.empty())
	{
		callback(BLUETOOTH_ERROR_FAIL, BluetoothGattCharacteristicList());
		return;
	}
	readCharacteristics(deviceAddress, service, characteristics, callback);
}

void Bluez5ProfileGatt::writeCharacteristic(const uint16_t &connId, const BluetoothUuid& service,
									 const BluetoothGattCharacteristic &characteristic,
									 BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	std::string deviceAddress = getAddress(connId);
	if (deviceAddress.empty())
	{
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}
	writeRemoteCharacteristic(deviceAddress, service, characteristic, callback);
}

void Bluez5ProfileGatt::readDescriptor(const uint16_t &connId, const BluetoothUuid& service, const BluetoothUuid &characteristic,
								 const BluetoothUuid &descriptor, BluetoothGattReadDescriptorCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	std::string deviceAddress = getAddress(connId);
	if (deviceAddress.empty())
	{
		callback(BLUETOOTH_ERROR_FAIL, BluetoothGattDescriptor());
		return;
	}
	readRemoteDescriptor(deviceAddress, service, characteristic, descriptor, callback);
}

void Bluez5ProfileGatt::readDescriptors(const uint16_t &connId, const BluetoothUuid& service, const BluetoothUuid &characteristic,
								 const BluetoothUuidList &descriptors, BluetoothGattReadDescriptorsCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	std::string deviceAddress = getAddress(connId);
	if (deviceAddress.empty())
	{
		callback(BLUETOOTH_ERROR_FAIL, BluetoothGattDescriptorList());
		return;
	}
	readDescriptors(deviceAddress, service, characteristic, descriptors, callback);
}

void Bluez5ProfileGatt::writeDescriptor(const uint16_t &connId, const BluetoothUuid &service, const BluetoothUuid &characteristic,
								 const BluetoothGattDescriptor &descriptor, BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	std::string deviceAddress = getAddress(connId);
	if (deviceAddress.empty())
	{
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}
	writeRemoteDescriptor(deviceAddress, service, characteristic, descriptor, callback);
}

void Bluez5ProfileGatt::changeCharacteristicWatchStatus(const std::string &address, const BluetoothUuid &service,
										const BluetoothUuid &characteristic, bool enabled,
										BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	auto deviceServicesIter = mDeviceServicesMap.find(convertAddressToLowerCase(address));

	if (deviceServicesIter == mDeviceServicesMap.end())
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "Device is not connected");
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	// The characteristic in every instance of the service, as before.
	std::vector<GattRemoteCharacteristic*> targets;
	for (auto remoteService : deviceServicesIter->second)
	{
		if (remoteService->service.getUuid() != service)
			continue;
		GattRemoteCharacteristic *remoteChar = findCharacteristic(remoteService, characteristic);
		if (remoteChar)
			targets.push_back(remoteChar);
	}
	if (targets.empty())
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "remote GATT characteristic not found");
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	// Asynchronous: StartNotify is answered when the device has acknowledged its CCCD write, and a device that did
	// not answer stopped the service for 25 s. One answer, after the last call, failing when any failed (the last
	// call's result was taken before), or when the profile has gone meanwhile, as for reads and writes.
	ResultJoin join(targets.size(), [alive = mLifetime.token(), callback](bool allSucceeded) {
		callback(allSucceeded && !alive.expired() ? BLUETOOTH_ERROR_NONE : BLUETOOTH_ERROR_FAIL);
	});
	for (auto remoteChar : targets)
	{
		if (enabled)
			remoteChar->startNotify(join.part());
		else
			remoteChar->stopNotify(join.part());
	}
}

void Bluez5ProfileGatt::readCharacteristic(const std::string &address, const BluetoothUuid& service,
									 const BluetoothUuid &characteristic,
									 BluetoothGattReadCharacteristicCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	readRemoteCharacteristic(address, service, characteristic, callback);
}

void Bluez5ProfileGatt::readCharacteristics(const std::string &address, const BluetoothUuid& service,
									const BluetoothUuidList &characteristics,
									BluetoothGattReadCharacteristicsCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	if (characteristics.empty())	// failed before, as nothing was found
	{
		callback(BLUETOOTH_ERROR_FAIL, BluetoothGattCharacteristicList());
		return;
	}
	auto state = std::make_shared<ReadCharacteristicsState>();
	state->address = address;
	state->service = service;
	state->characteristics = characteristics;
	state->callback = callback;
	readRemoteCharacteristicsStep(state);
}

void Bluez5ProfileGatt::writeCharacteristic(const std::string &address, const BluetoothUuid& service,
							const BluetoothGattCharacteristic &characteristic,
							BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	writeRemoteCharacteristic(address, service, characteristic, callback);
}

void Bluez5ProfileGatt::readDescriptor(const std::string &address, const BluetoothUuid& service, const BluetoothUuid &characteristic,
								 const BluetoothUuid &descriptor, BluetoothGattReadDescriptorCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	readRemoteDescriptor(address, service, characteristic, descriptor, callback);
}

void Bluez5ProfileGatt::readRemoteDescriptorsStep(std::shared_ptr<ReadDescriptorsState> state)
{
	if (state->next >= state->descriptors.size())
	{
		state->callback(BLUETOOTH_ERROR_NONE, state->result);
		return;
	}
	const BluetoothUuid descriptor = state->descriptors[state->next++];
	readRemoteDescriptor(state->address, state->service, state->characteristic, descriptor,
	                     [this, alive = mLifetime.token(), state](BluetoothError error, BluetoothGattDescriptor value) {
		if (alive.expired() || error != BLUETOOTH_ERROR_NONE)
		{
			ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Descriptor not found");
			state->callback(BLUETOOTH_ERROR_FAIL, state->result);
			return;
		}
		state->result.push_back(value);
		readRemoteDescriptorsStep(state);
	});
}

void Bluez5ProfileGatt::readDescriptors(const std::string &address, const BluetoothUuid& service, const BluetoothUuid &characteristic,
						const BluetoothUuidList &descriptors, BluetoothGattReadDescriptorsCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	if (descriptors.empty())	// failed before, as nothing was found
	{
		callback(BLUETOOTH_ERROR_FAIL, BluetoothGattDescriptorList());
		return;
	}
	auto state = std::make_shared<ReadDescriptorsState>();
	state->address = address;
	state->service = service;
	state->characteristic = characteristic;
	state->descriptors = descriptors;
	state->callback = callback;
	readRemoteDescriptorsStep(state);
}

uint16_t Bluez5ProfileGatt::getConnectId(const std::string &address)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	std::string lowerCaseAddress = convertAddressToLowerCase(address);

	for ( auto it = mConnectedDevices.begin(); it != mConnectedDevices.end(); ++it )
	{
		if (lowerCaseAddress == it->second)
		{
			return it->first;
		}
	}
	return 0;
}

std::string Bluez5ProfileGatt::getAddress(const uint16_t &connId)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	std::string deviceAddress;
	if (mConnectedDevices.find(connId) == mConnectedDevices.end())
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "Device not connected");
	}
	else
	{
		deviceAddress = mConnectedDevices.find(connId)->second;
	}
	return deviceAddress;
}

void Bluez5ProfileGatt::readRemoteCharacteristic(const std::string &address, const BluetoothUuid &service,
                                                 const BluetoothUuid &characteristic, BluetoothGattReadCharacteristicCallback callback)
{
	GattRemoteService* remoteService = findService(address, service);
	GattRemoteCharacteristic* remoteChar = remoteService ? findCharacteristic(remoteService, characteristic) : nullptr;
	if (!remoteChar || !remoteChar->characteristic.isPropertySet(BluetoothGattCharacteristic::Property::PROPERTY_READ))
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "remote GATT characteristic not found or not readable");
		callback(BLUETOOTH_ERROR_FAIL, BluetoothGattCharacteristic());
		return;
	}

	BluetoothGattCharacteristicProperties properties = remoteChar->readProperties();
	remoteChar->readValue([this, alive = mLifetime.token(), address, service, characteristic, properties, callback]
	                      (bool success, const std::vector<unsigned char> &value) {
		if (alive.expired() || !success)
		{
			callback(BLUETOOTH_ERROR_FAIL, BluetoothGattCharacteristic());
			return;
		}

		BluetoothGattCharacteristic readCharacteristicValue;
		readCharacteristicValue.setProperties(properties);
		readCharacteristicValue.setUuid(characteristic);
		readCharacteristicValue.setValue(value);

		// Looked up again: the device may have gone meanwhile.
		GattRemoteService* remoteService = findService(address, service);
		if (remoteService)
			remoteService->service.updateCharacteristicValue(characteristic, value);

		callback(BLUETOOTH_ERROR_NONE, readCharacteristicValue);
	});
}

void Bluez5ProfileGatt::readRemoteDescriptor(const std::string &address, const BluetoothUuid &service, const BluetoothUuid &characteristic,
                                             const BluetoothUuid &descriptor, BluetoothGattReadDescriptorCallback callback)
{
	GattRemoteService* remoteService = findService(address, service);
	GattRemoteCharacteristic* remoteChar = remoteService ? findCharacteristic(remoteService, characteristic) : nullptr;
	GattRemoteDescriptor* remoteDesc = remoteChar ? findDescriptor(remoteChar, descriptor) : nullptr;
	// Not gated on the characteristic's READ property, which says nothing about its descriptors: BlueZ answers
	// NotPermitted where reading is not allowed.
	if (!remoteDesc)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Descriptor not found");
		callback(BLUETOOTH_ERROR_FAIL, BluetoothGattDescriptor());
		return;
	}

	remoteDesc->readValue([this, alive = mLifetime.token(), address, service, characteristic, descriptor, callback]
	                      (bool success, const std::vector<unsigned char> &value) {
		if (alive.expired() || !success)
		{
			callback(BLUETOOTH_ERROR_FAIL, BluetoothGattDescriptor());
			return;
		}

		BluetoothGattDescriptor readDescriptorValue;
		readDescriptorValue.setUuid(descriptor);
		readDescriptorValue.setValue(value);

		GattRemoteService* remoteService = findService(address, service);
		GattRemoteCharacteristic* remoteChar = remoteService ? findCharacteristic(remoteService, characteristic) : nullptr;
		if (remoteChar)
		{
			remoteChar->characteristic.updateDescriptorValue(descriptor, value);
			remoteService->service.updateDescriptorValue(characteristic, descriptor, value);
		}

		callback(BLUETOOTH_ERROR_NONE, readDescriptorValue);
	});
}

void Bluez5ProfileGatt::writeRemoteCharacteristic(const std::string &address, const BluetoothUuid &service,
                                                  const BluetoothGattCharacteristic &characteristic, BluetoothResultCallback callback)
{
	GattRemoteService* remoteService = findService(address, service);
	GattRemoteCharacteristic* remoteChar = remoteService ? findCharacteristic(remoteService, characteristic.getUuid()) : nullptr;
	// Write, or Write Without Response (a command): characteristics with only the latter, common for UART and HID
	// output, could not be written at all.
	bool withResponse = remoteChar && remoteChar->characteristic.isPropertySet(BluetoothGattCharacteristic::Property::PROPERTY_WRITE);
	bool withoutResponse = remoteChar &&
		remoteChar->characteristic.isPropertySet(BluetoothGattCharacteristic::Property::PROPERTY_WRITE_WITHOUT_RESPONSE);
	if (!withResponse && !withoutResponse)
	{
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	remoteChar->writeValue(characteristic.getValue(), !withResponse,
	                       [this, alive = mLifetime.token(), address, service, characteristic, callback](bool success) {
		if (alive.expired() || !success)
		{
			callback(BLUETOOTH_ERROR_FAIL);
			return;
		}

		GattRemoteService* remoteService = findService(address, service);
		if (remoteService)
			remoteService->service.updateCharacteristicValue(characteristic.getUuid(), characteristic.getValue());
		if (getGattObserver())
			getGattObserver()->characteristicValueChanged(address, service, characteristic, convertAddressToLowerCase(mAdapter->getAddress()));
		callback(BLUETOOTH_ERROR_NONE);
	});
}

void Bluez5ProfileGatt::writeRemoteDescriptor(const std::string &address, const BluetoothUuid &service, const BluetoothUuid &characteristic,
                                              const BluetoothGattDescriptor &descriptor, BluetoothResultCallback callback)
{
	GattRemoteService* remoteService = findService(address, service);
	GattRemoteCharacteristic* remoteChar = remoteService ? findCharacteristic(remoteService, characteristic) : nullptr;
	GattRemoteDescriptor* remoteDesc = remoteChar ? findDescriptor(remoteChar, descriptor.getUuid()) : nullptr;
	// Not gated on the characteristic's WRITE property: the CCCD of a notify-only characteristic could not be
	// written. BlueZ answers NotPermitted where writing is not allowed.
	if (!remoteDesc)
	{
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	remoteDesc->writeValue(descriptor.getValue(),
	                       [this, alive = mLifetime.token(), address, service, characteristic, descriptor, callback](bool success) {
		if (alive.expired() || !success)
		{
			callback(BLUETOOTH_ERROR_FAIL);
			return;
		}

		GattRemoteService* remoteService = findService(address, service);
		GattRemoteCharacteristic* remoteChar = remoteService ? findCharacteristic(remoteService, characteristic) : nullptr;
		if (remoteChar)
		{
			remoteChar->characteristic.updateDescriptorValue(descriptor.getUuid(), descriptor.getValue());
			remoteService->service.updateDescriptorValue(characteristic, descriptor.getUuid(), descriptor.getValue());
		}
		callback(BLUETOOTH_ERROR_NONE);
	});
}

GattRemoteService* Bluez5ProfileGatt::findService(const std::string &address, const BluetoothUuid& service)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	auto deviceServicesIter = mDeviceServicesMap.find(convertAddressToLowerCase(address));

	if (deviceServicesIter == mDeviceServicesMap.end())
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "Device not connected");
		return NULL;
	}

	auto &servicesList = deviceServicesIter->second;
	for (auto serviceIt : servicesList)
	{
		if (serviceIt->service.getUuid() == service)
		{
			return serviceIt;
		}
	}
	return NULL;
}

GattRemoteCharacteristic* Bluez5ProfileGatt::findCharacteristic(GattRemoteService* service, const BluetoothUuid &characteristic)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	for (auto charIt = service->gattRemoteCharacteristics.begin(); charIt != service->gattRemoteCharacteristics.end(); charIt++)
	{
		if ((*charIt)->characteristic.getUuid() == characteristic)
		{
			return (*charIt);
		}
	}
	return NULL;
}

GattRemoteDescriptor* Bluez5ProfileGatt::findDescriptor(GattRemoteCharacteristic* characteristic, const BluetoothUuid &descriptor)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	for (auto descIt = characteristic->gattRemoteDescriptors.begin(); descIt != characteristic->gattRemoteDescriptors.end(); descIt++)
	{
		if ((*descIt)->descriptor.getUuid() == descriptor)
		{
			return (*descIt);
		}
	}
	return NULL;
}

void Bluez5ProfileGatt::addService(uint16_t appId, const BluetoothGattService &service, BluetoothGattAddCallback callback)
{
	if (!mObjectManagerGattServer)
	{
		callback(BLUETOOTH_ERROR_FAIL, -1);
		return;
	}

	BluetoothUuid uuid = service.getUuid();
	auto serviceId = nextServiceId();

	bool isPrimary = (service.getType() == BluetoothGattService::PRIMARY) ? true : false;

	std::string objPath = g_dbus_object_manager_get_object_path(G_DBUS_OBJECT_MANAGER(mObjectManagerGattServer));
	std::string serviceObjPath = objPath  + "/App" + std::to_string(appId) + "/Service" + std::to_string(serviceId);

	// Includes holds object paths (ao): those of this application's services with the included UUIDs, which are
	// skipped when it has none. It was given the UUIDs themselves, as pointers into temporaries freed at once, in an
	// array on the stack sized by the client.
	std::vector<std::string> includePaths;
	auto app = mGattLocalApplications.find(appId);
	if (app != mGattLocalApplications.end())
	{
		for (auto &included : service.getIncludedServices())
		{
			std::string includedUuid = included.toString();
			for (auto &local : app->second->mGattLocalServices)
			{
				Bluez5GattLocalService *localService = local.second.get();
				if (localService->mServiceInterface && localService->mServiceObject &&
				    g_strcmp0(bluez_gatt_service1_get_uuid(localService->mServiceInterface), includedUuid.c_str()) == 0)
				{
					includePaths.push_back(g_dbus_object_get_object_path(localService->mServiceObject));
					break;
				}
			}
		}
	}
	std::vector<const char *> includes;
	for (auto &path : includePaths)
		includes.push_back(path.c_str());
	includes.push_back(nullptr);

	BluezObjectSkeleton *object = bluez_object_skeleton_new(serviceObjPath.c_str());

	BluezGattService1 *skeletonGattService = bluez_gatt_service1_skeleton_new();
	bluez_gatt_service1_set_uuid(skeletonGattService, uuid.toString().c_str());
	bluez_gatt_service1_set_primary(skeletonGattService, isPrimary);
	bluez_gatt_service1_set_includes(skeletonGattService, includes.data());
	bluez_object_skeleton_set_gatt_service1(object, skeletonGattService);
	g_dbus_object_manager_server_export(mObjectManagerGattServer, G_DBUS_OBJECT_SKELETON (object));
	g_dbus_object_manager_server_set_connection (mObjectManagerGattServer, mConn);

	auto registerCallback = [this, alive = mLifetime.token(), callback, object, skeletonGattService, appId, serviceId](BluetoothError error)
	{
		// Called back after the profile may have gone with its adapter.
		if (alive.expired())
		{
			callback(BLUETOOTH_ERROR_FAIL, -1);
			return;
		}
		auto appIt = mGattLocalApplications.find(appId);
		if (error == BLUETOOTH_ERROR_NONE && appIt != mGattLocalApplications.end())
		{
			DEBUG("Register application successfully");
			std::unique_ptr <Bluez5GattLocalService> service(new Bluez5GattLocalService(G_DBUS_OBJECT(object)));
			service->mServiceInterface = skeletonGattService;
			auto &services = ((appIt->second).get())->mGattLocalServices;
			services.insert(make_pair(serviceId, std::move(service)));
			callback(BLUETOOTH_ERROR_NONE, serviceId);
			return;
		}

		// Unpublished and released: the service stayed exported, appearing in the next registration, and our
		// references leaked. An app removed meanwhile was not even answered.
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Register application failed %d, or the application is gone", error);
		if (mObjectManagerGattServer)
			g_dbus_object_manager_server_unexport(mObjectManagerGattServer, g_dbus_object_get_object_path(G_DBUS_OBJECT(object)));
		g_object_unref(object);
		g_object_unref(skeletonGattService);
		callback(BLUETOOTH_ERROR_FAIL, -1);
	};

	registerLocalApplication(registerCallback, objPath, true);
}

void Bluez5ProfileGatt::removeService(uint16_t appId, uint16_t serviceId, BluetoothResultCallback callback)
{
	if (!mObjectManagerGattServer)
	{
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	std::string objPath = g_dbus_object_manager_get_object_path(G_DBUS_OBJECT_MANAGER(mObjectManagerGattServer));
	auto it = mGattLocalApplications.find(appId);

	if (it == mGattLocalApplications.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "appId not present");
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	auto &services = it->second->mGattLocalServices;

	auto serviceIt = services.find(serviceId);
	if (serviceIt == services.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "service not present");
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	removeLocalServices(serviceIt->second.get());
	services.erase(serviceId);

	auto registerCallback = [callback](BluetoothError error)
	{
		if (error == BLUETOOTH_ERROR_NONE)
		{
			DEBUG("Removed service successfully");
		}
		else
		{
			ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Removed service failed %d", error);
		}
		callback(error);
		return;
	};

	registerLocalApplication(registerCallback, objPath, true);
}

void Bluez5ProfileGatt::removeLocalServices(Bluez5GattLocalService *service)
{
	if (service->mServiceObject)
	{
		removeLocalCharacteristics(service);
		g_dbus_object_manager_server_unexport (mObjectManagerGattServer, g_dbus_object_get_object_path(service->mServiceObject));

		if (service->mServiceInterface)
		{
			g_object_unref(service->mServiceInterface);
			service->mServiceInterface = 0;
		}

		g_object_unref(service->mServiceObject);
		service->mServiceObject = 0;
	}
}

void Bluez5ProfileGatt::createObjectManagers()
{
	std::string objectPath = mAdapter->getObjectPath() + SERVER_PATH;
	mObjectManagerGattServer = g_dbus_object_manager_server_new(objectPath.c_str());

	if (!mObjectManagerGattServer)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Failed to create Object Manager for GATT Server");
	}
}

void Bluez5ProfileGatt::addCharacteristic(uint16_t appId, uint16_t serviceId, const BluetoothGattCharacteristic &characteristic, BluetoothGattAddCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	if (!mObjectManagerGattServer)	// its object path is read below; NULL made a std::string throw
	{
		callback(BLUETOOTH_ERROR_FAIL, -1);
		return;
	}

	auto appIt = mGattLocalApplications.find(appId);
	if (appIt == mGattLocalApplications.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Application not present for addCharacteristic");
		callback(BLUETOOTH_ERROR_PARAM_INVALID, -1);
		return;
	}

	auto &services = appIt->second->mGattLocalServices;

	auto srvIt = services.find(serviceId);

	if (srvIt == services.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Service is not present list for addCharacteristic");
		callback(BLUETOOTH_ERROR_PARAM_INVALID, -1);
		return;
	}

	BluezGattCharacteristic1 *skeletonGattChar = bluez_gatt_characteristic1_skeleton_new();
	if (!skeletonGattChar)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Failed to allocate memroy for gatt characteristic interface");
		callback(BLUETOOTH_ERROR_NOMEM, -1);
		return;
	}

	uint16_t charId = nextCharId();

	std::string objPath = g_dbus_object_manager_get_object_path(G_DBUS_OBJECT_MANAGER(mObjectManagerGattServer));
	std::string serviceObjPath = objPath  + "/App" + std::to_string(appId) + "/Service" + std::to_string(serviceId);
	std::string charObjPath = serviceObjPath + "/Char"+ std::to_string(charId);

	BluezObjectSkeleton *object = bluez_object_skeleton_new(charObjPath.c_str());

	if (!object)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Memory allocation failed %d", __LINE__);
		if (skeletonGattChar) g_object_unref(skeletonGattChar);
		callback(BLUETOOTH_ERROR_NOMEM, -1);
		return;
	}

	bluez_gatt_characteristic1_set_service(skeletonGattChar, serviceObjPath.c_str());
	bluez_gatt_characteristic1_set_uuid(skeletonGattChar, characteristic.getUuid().toString().c_str());

	std::vector<const char *> flags = propertyFlags(characteristic);

	BluetoothGattValue value = characteristic.getValue();

	GVariantBuilder *dataBuilder = g_variant_builder_new (G_VARIANT_TYPE ("ay"));

	if (!dataBuilder)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Memory allocation failed %d", __LINE__);
		if (object) g_object_unref(object);
		if (skeletonGattChar) g_object_unref(skeletonGattChar);
		callback(BLUETOOTH_ERROR_NOMEM, -1);
		return;
	}

	for (auto &byteIt : value)
		g_variant_builder_add(dataBuilder, "y", byteIt);

	GVariant *dataValue = g_variant_builder_end(dataBuilder);
	g_variant_builder_unref(dataBuilder);

	bluez_gatt_characteristic1_set_value(skeletonGattChar, dataValue);

	GVariantBuilder *builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
	if (!builder)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Memory allocation failed %d", __LINE__);
		if (object) g_object_unref(object);
		if (skeletonGattChar) g_object_unref(skeletonGattChar);
		callback(BLUETOOTH_ERROR_NOMEM, -1);
		return;
	}

	for (const char *flag : flags)
		g_variant_builder_add(builder, "s", flag);

	GVariant *flagsVariant = g_variant_builder_end(builder);
	g_variant_builder_unref(builder);

	bluez_gatt_characteristic1_set_flags(skeletonGattChar, flagsVariant);

	bluez_object_skeleton_set_gatt_characteristic1(object, skeletonGattChar);

	g_signal_connect(skeletonGattChar,
		"handle_read_value",
		G_CALLBACK (Bluez5GattLocalCharacteristic::onHandleReadValue),
		this);

	g_signal_connect(skeletonGattChar,
		"handle_write_value",
		G_CALLBACK (Bluez5GattLocalCharacteristic::onHandleWriteValue),
		this);

	g_signal_connect(skeletonGattChar,
		"handle_start_notify",
		G_CALLBACK (Bluez5GattLocalCharacteristic::onHandleStartNotify),
		this);

	g_signal_connect(skeletonGattChar,
		"handle_stop_notify",
		G_CALLBACK (Bluez5GattLocalCharacteristic::onHandleStopNotify),
		this);

	g_dbus_object_manager_server_export(mObjectManagerGattServer, G_DBUS_OBJECT_SKELETON (object));
	g_dbus_object_manager_server_set_connection (mObjectManagerGattServer, mConn);

	// The service is looked up again when the registration is answered: it held a reference to the service's
	// characteristics, which removing the service in the meantime freed.
	auto registerCallback = [this, alive = mLifetime.token(), callback, appId, serviceId, charId, charObjPath, object, skeletonGattChar](BluetoothError error)
	{
		// Called back after the profile may have gone with its adapter.
		if (alive.expired())
		{
			callback(BLUETOOTH_ERROR_FAIL, -1);
			return;
		}
		Bluez5GattLocalService *localService = findLocalService(appId, serviceId);
		if (error == BLUETOOTH_ERROR_NONE && localService)
		{
			DEBUG("Characterstic registered successfully");
			std::unique_ptr<Bluez5GattLocalCharacteristic> character(new Bluez5GattLocalCharacteristic(G_DBUS_OBJECT(object)));
			character->mInterface = skeletonGattChar;
			localService->mCharacteristics.insert(std::make_pair(charId, std::move(character)));
			localService->mLastCharId = charId;
			callback(BLUETOOTH_ERROR_NONE, charId);
		}
		else
		{
			ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Removed application  and register application failed %d", error);
			// Unpublished: it stayed exported, and appeared in the next registration.
			g_dbus_object_manager_server_unexport(mObjectManagerGattServer, charObjPath.c_str());
			g_object_unref(object);
			g_object_unref(skeletonGattChar);
			callback(BLUETOOTH_ERROR_FAIL, -1);
		}
		return;
	};

	registerLocalApplication(registerCallback, objPath, true);
}

void Bluez5ProfileGatt::removeLocalCharacteristics(Bluez5GattLocalService *service)
{
	if (!service)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "removeCharacteristics service object is null");
		return;
	}

	auto &chars = service->mCharacteristics;

	for (auto &it : chars)
	{
		removeLocalDescriptors(it.second.get());

		if (it.second->mCharObject)
		{
			g_dbus_object_manager_server_unexport (mObjectManagerGattServer, g_dbus_object_get_object_path(it.second->mCharObject));

			if (it.second->mInterface)
			{
				g_object_unref(it.second->mInterface);
				it.second->mInterface = 0;
			}

			if (it.second->mCharObject)
			{
				g_object_unref(it.second->mCharObject);
				it.second->mCharObject = 0;
			}
		}

	}
	chars.clear();
}

void Bluez5ProfileGatt::addDescriptor(uint16_t appId, uint16_t serviceId, const BluetoothGattDescriptor &descriptor, BluetoothGattAddCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	Bluez5GattLocalService *localService = findLocalService(appId, serviceId);
	id_type charId = localService ? localService->mLastCharId : 0;
	if (!mObjectManagerGattServer || !getLocalDescriptorList(appId, serviceId, charId))
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Failed to get desc list");
		callback(BLUETOOTH_ERROR_PARAM_INVALID, -1);
		return;	// went on to register the descriptor anyway, answer again and use the NULL list
	}

	BluezGattDescriptor1 *skeletonGattDesc = bluez_gatt_descriptor1_skeleton_new();
	if (!skeletonGattDesc)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Failed to allocate memroy for gatt descriptor interface");
		callback(BLUETOOTH_ERROR_NOMEM, -1);
		return;
	}

	uint16_t descId = nextDescId();
	std::string objPath = g_dbus_object_manager_get_object_path(G_DBUS_OBJECT_MANAGER(mObjectManagerGattServer));
	std::string serviceObjPath = objPath  + "/App" + std::to_string(appId) + "/Service" + std::to_string(serviceId);
	std::string charObjPath = serviceObjPath + "/Char"+ std::to_string(charId);
	std::string descObjPath = charObjPath + "/Desc" + std::to_string(descId);

	BluezObjectSkeleton *object = bluez_object_skeleton_new(descObjPath.c_str());
	if (!object)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Memory allocation failed %d", __LINE__);
		if (skeletonGattDesc) g_object_unref(skeletonGattDesc);
		callback(BLUETOOTH_ERROR_FAIL, -1);
		return;
	}

	BluetoothGattValue value = descriptor.getValue();

	std::vector<const char *> flags = permissionFlags(descriptor);

	GVariantBuilder *dataBuilder = g_variant_builder_new (G_VARIANT_TYPE ("ay"));
	for (auto &byteIt : value)
		g_variant_builder_add(dataBuilder, "y", byteIt);

	GVariant *dataValue = g_variant_builder_end(dataBuilder);
	g_variant_builder_unref(dataBuilder);

	bluez_gatt_descriptor1_set_value(skeletonGattDesc, dataValue);

	bluez_gatt_descriptor1_set_characteristic(skeletonGattDesc, charObjPath.c_str());
	bluez_gatt_descriptor1_set_uuid(skeletonGattDesc, descriptor.getUuid().toString().c_str());

	GVariantBuilder *builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
	if (!builder)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Memory allocation failed %d", __LINE__);
		if (object) g_object_unref(object);
		if (skeletonGattDesc) g_object_unref(skeletonGattDesc);
		callback(BLUETOOTH_ERROR_NOMEM, -1);
		return;
	}

	for (const char *flag : flags)
		g_variant_builder_add(builder, "s", flag);

	GVariant *flagsVariant = g_variant_builder_end(builder);
	g_variant_builder_unref(builder);

	bluez_gatt_descriptor1_set_flags(skeletonGattDesc, flagsVariant);

	bluez_object_skeleton_set_gatt_descriptor1(object, skeletonGattDesc);

	g_signal_connect(skeletonGattDesc,
		"handle_read_value",
		G_CALLBACK (Bluez5GattLocalDescriptor::onHandleReadValue),
		this);

	g_signal_connect(skeletonGattDesc,
		"handle_write_value",
		G_CALLBACK (Bluez5GattLocalDescriptor::onHandleWriteValue),
		this);

	g_dbus_object_manager_server_export(mObjectManagerGattServer, G_DBUS_OBJECT_SKELETON (object));
	g_dbus_object_manager_server_set_connection (mObjectManagerGattServer, mConn);

	auto registerCallback = [this, alive = mLifetime.token(), callback, appId, serviceId, charId, descId, descObjPath, object, skeletonGattDesc](BluetoothError error)
	{
		// Called back after the profile may have gone with its adapter.
		if (alive.expired())
		{
			callback(BLUETOOTH_ERROR_FAIL, -1);
			return;
		}
		// Looked up again, as for characteristics.
		GattLocalDescriptorsMap *descs = getLocalDescriptorList(appId, serviceId, charId);
		if (error == BLUETOOTH_ERROR_NONE && descs)
		{
			DEBUG("Descriptor registered successfully");
			std::unique_ptr<Bluez5GattLocalDescriptor> desc(new Bluez5GattLocalDescriptor(G_DBUS_OBJECT(object)));
			desc->mInterface = skeletonGattDesc;
			descs->insert(std::make_pair(descId, std::move(desc)));
			callback(BLUETOOTH_ERROR_NONE, descId);
		}
		else
		{
			ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Descriptor register failed %d", error);
			g_dbus_object_manager_server_unexport(mObjectManagerGattServer, descObjPath.c_str());
			if (object) g_object_unref(object);
			if (skeletonGattDesc) g_object_unref(skeletonGattDesc);
			callback(BLUETOOTH_ERROR_FAIL, -1);
		}
		return;
	};

	registerLocalApplication(registerCallback, objPath, true);
}

void Bluez5ProfileGatt::removeLocalDescriptors(Bluez5GattLocalCharacteristic *characteristic)
{
	if (!characteristic)
		return;

	auto &descs = characteristic->mDescriptors;

	for (auto &it: descs)
	{
		if (it.second->mDescObject)
			g_dbus_object_manager_server_unexport (mObjectManagerGattServer, g_dbus_object_get_object_path(it.second->mDescObject));
		else
			ERROR("MSGID_GATT_PROFILE_ERROR", 0, "removeDescriptors trying remove null object");

		if (it.second->mInterface)
		{
			g_object_unref(it.second->mInterface);
			it.second->mInterface = 0;
		}

		if (it.second->mDescObject)
		{
			g_object_unref(it.second->mDescObject);
			it.second->mDescObject = 0;
		}
	}

	descs.clear();
}

void Bluez5ProfileGatt::notifyCharacteristicValueChanged(uint16_t appId, uint16_t serviceId, BluetoothGattCharacteristic characteristic, uint16_t charId)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	auto appIt = mGattLocalApplications.find(appId);
	if (appIt == mGattLocalApplications.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Application not present for notifyCharacteristicValueChanged");
		return;
	}

	auto &services = appIt->second->mGattLocalServices;

	auto srvIt = services.find(serviceId);

	if (srvIt == services.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Service is not present list for notifyCharacteristicValueChanged");
		return;
	}

	auto &chars = srvIt->second->mCharacteristics;

	auto charIt = chars.find(charId);

	if (charIt == chars.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Characteristic is not present list for notifyCharacteristicValueChanged");
		return;
	}

	GVariant *characteristicVariant= convertVectorToArrayByteGVariant(characteristic.getValue());
	bluez_gatt_characteristic1_set_value(charIt->second->mInterface, characteristicVariant);
	return;
}

void Bluez5ProfileGatt::notifyDescriptorValueChanged(uint16_t appId, uint16_t serviceId, uint16_t descId, BluetoothGattDescriptor descriptor, uint16_t charId)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	auto appIt = mGattLocalApplications.find(appId);
	if (appIt == mGattLocalApplications.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Application not present for notifyDescriptorValueChanged");
		return;
	}

	auto &services = appIt->second->mGattLocalServices;

	auto srvIt = services.find(serviceId);

	if (srvIt == services.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Service is not present list for notifyDescriptorValueChanged");
		return;
	}

	auto &chars = srvIt->second->mCharacteristics;

	auto charIt = chars.find(charId);

	if (charIt == chars.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Characteristic is not present list for notifyDescriptorValueChanged");
		return;
	}

	auto &descs = charIt->second->mDescriptors;

	auto descIt = descs.find(descId);

	if (descIt == descs.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Descriptor is not present list for notifyDescriptorValueChanged");
		return;
	}

	GVariant *descVariant = convertVectorToArrayByteGVariant(descriptor.getValue());
	bluez_gatt_descriptor1_set_value(descIt->second->mInterface, descVariant);
	return;
}

void Bluez5ProfileGatt::startService(uint16_t /*serviceId*/, BluetoothGattTransportMode /*mode*/, BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	if (callback) callback(BLUETOOTH_ERROR_NONE);
}

void Bluez5ProfileGatt::startService(uint16_t /*appId*/, uint16_t /*serviceId*/, BluetoothGattTransportMode /*mode*/, BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	if (callback) callback(BLUETOOTH_ERROR_NONE);
}

Bluez5ProfileGatt::Bluez5GattLocalService *Bluez5ProfileGatt::findLocalService(uint16_t appId, uint16_t serviceId)
{
	auto appIt = mGattLocalApplications.find(appId);
	if (appIt == mGattLocalApplications.end())
		return nullptr;
	auto srvIt = appIt->second->mGattLocalServices.find(serviceId);
	return srvIt == appIt->second->mGattLocalServices.end() ? nullptr : srvIt->second.get();
}

Bluez5ProfileGatt::GattLocalDescriptorsMap* Bluez5ProfileGatt::getLocalDescriptorList(uint16_t appId, uint16_t serviceId, uint16_t charId)
{
	auto appIt = mGattLocalApplications.find(appId);
	if (appIt == mGattLocalApplications.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Application not present %d", __LINE__);
		return nullptr;
	}

	auto &services = appIt->second->mGattLocalServices;

	auto srvIt = services.find(serviceId);

	if (srvIt == services.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Service is not present in list %d", __LINE__);
		return nullptr;
	}

	auto &chars = srvIt->second->mCharacteristics;

	auto charIt = chars.find(charId);
	if (charIt == chars.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Characteristic is not present list %d",__LINE__);
		return nullptr;
	}

	return &charIt->second->mDescriptors;
}

// The flag names of a characteristic or descriptor. They were written into a fixed array whose terminator was
// written only while index < sizeof(the pointer): 4 on 32 bit, so with four flags or more the array was not
// terminated, and the loops reading it, bounded by its size in bytes, read on past its end.
std::vector<const char *> Bluez5ProfileGatt::propertyFlags(const BluetoothGattCharacteristic &characteristic)
{
	std::vector<const char *> flags;
	for (auto &propIt : GattRemoteCharacteristic::characteristicPropertyMap)
		if (characteristic.isPropertySet(propIt.second))
			flags.push_back(propIt.first.c_str());
	return flags;
}

std::vector<const char *> Bluez5ProfileGatt::permissionFlags(const BluetoothGattDescriptor &descriptor)
{
	std::vector<const char *> flags;
	for (auto &propIt : GattRemoteDescriptor::descriptorPermissionMap)
		if (descriptor.isPermissionSet(propIt.first))
			flags.push_back(propIt.second.c_str());
	return flags;
}

void Bluez5ProfileGatt::onCharacteristicPropertiesChanged(GattRemoteCharacteristic* characteristic, GVariant *changed_properties)
{
	std::string deviceObjPath, serviceName;
	splitInPathAndName(characteristic->parentObjectPath, deviceObjPath, serviceName);

	Bluez5Device* device = mAdapter->findDeviceByObjectPath(deviceObjPath);

	if (!device)
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "onCharacteristicPropertiesChanged device is not present");
		return;
	}

	std::string deviceAddress = device->getAddress();
	std::string lowerCaseAddress = convertAddressToLowerCase(deviceAddress);

	GattRemoteService* service = getRemoteGattService(characteristic->parentObjectPath);

	if (!service)
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "onCharacteristicPropertiesChanged unable to get service instance for deviceAddress %s",lowerCaseAddress.c_str());
		return;
	}

	// UUIDs may be missing from the proxies' caches: std::string(NULL) threw.
	const char* uuid = bluez_gatt_service1_get_uuid(service->mInterface);
	const char* charUuidStr = bluez_gatt_characteristic1_get_uuid(characteristic->mInterface);
	if (!uuid || !charUuidStr || !getGattObserver())
		return;
	BluetoothUuid service_uuid(uuid, BluetoothUuid::UUID128);

	if(g_variant_n_children(changed_properties) > 0)
	{
		GVariantIter *iter = NULL;

		GVariant *value;
		const gchar *key;
		g_variant_get(changed_properties, "a{sv}", &iter);

		while (iter != nullptr && g_variant_iter_loop(iter, "{&sv}", &key, &value))
		{
			if (g_ascii_strncasecmp(key, "value", 5) == 0)
			{
				BluetoothGattValue charValue = convertArrayByteGVariantToVector(value);
				BluetoothGattCharacteristic remoteChar;
				BluetoothUuid charUuid(charUuidStr, BluetoothUuid::UUID128);
				remoteChar.setUuid(charUuid);
				remoteChar.setValue(charValue);
				getGattObserver()->characteristicValueChanged(lowerCaseAddress, service_uuid, remoteChar, convertAddressToLowerCase(mAdapter->getAddress()));
			}
		}
		g_variant_iter_free (iter);
	}
}

namespace {

// The offset BlueZ passes for Read Blob and Prepare Write: ignored, so a long read returned the start again and
// again, and every fragment of a long write replaced the whole value.
uint16_t optionOffset(GVariant *options)
{
	uint16_t offset = 0;
	if (options)
		g_variant_lookup(options, "offset", "q", &offset);
	return offset;
}

std::vector<uint8_t> bytesOf(GVariant *value)
{
	return value ? convertArrayByteGVariantToVector(value) : std::vector<uint8_t>();
}

void returnValueFrom(GDBusMethodInvocation *invocation, GVariant *value, GVariant *options)
{
	std::vector<uint8_t> out;
	if (!gattValueFromOffset(bytesOf(value), optionOffset(options), out))
	{
		g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InvalidOffset", "Invalid offset");
		return;
	}
	GVariant *result = convertVectorToArrayByteGVariant(out);
	g_dbus_method_invocation_return_value(invocation, g_variant_new_tuple(&result, 1));
}

// The value after a write: false (answered with InvalidOffset) when the offset is past the current value.
bool valueAfterWrite(GDBusMethodInvocation *invocation, GVariant *current, GVariant *written, GVariant *options,
                     GVariant **result)
{
	std::vector<uint8_t> out;
	if (!gattValueWriteAt(bytesOf(current), optionOffset(options), bytesOf(written), out))
	{
		g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InvalidOffset", "Invalid offset");
		return false;
	}
	*result = convertVectorToArrayByteGVariant(out);
	return true;
}

} // namespace

gboolean Bluez5ProfileGatt::Bluez5GattLocalCharacteristic::onHandleReadValue(BluezGattCharacteristic1* interface,
																			 GDBusMethodInvocation *invocation,
																			 GVariant *arg_options,
																			 gpointer /*user_data*/)
{
	returnValueFrom(invocation, bluez_gatt_characteristic1_get_value(interface), arg_options);
	return true;
}

gboolean Bluez5ProfileGatt::Bluez5GattLocalCharacteristic::onHandleWriteValue(BluezGattCharacteristic1* interface,
																			  GDBusMethodInvocation *invocation,
																			  GVariant *arg_value,
																			  GVariant *arg_options,
																			  gpointer user_data)
{
	GVariant *value = nullptr;
	if (!valueAfterWrite(invocation, bluez_gatt_characteristic1_get_value(interface), arg_value, arg_options, &value))
		return true;
	bluez_gatt_characteristic1_set_value(interface, value);
	g_dbus_method_invocation_return_value(invocation, NULL);
	Bluez5ProfileGatt *pThis = static_cast<Bluez5ProfileGatt *> (user_data);
	pThis->onHandleCharacteriscticWriteValue(interface, bluez_gatt_characteristic1_get_value(interface));
	return true;
}

void Bluez5ProfileGatt::onHandleCharacteriscticWriteValue(BluezGattCharacteristic1* charInterface, GVariant * charValue)
{
	BluetoothGattCharacteristic characteristic;

	const char* charUuid = bluez_gatt_characteristic1_get_uuid(charInterface);
	if (charUuid)
		characteristic.setUuid(BluetoothUuid(charUuid));

	BluetoothGattCharacteristicProperties properties = 0;
	GVariant* flagVariant = bluez_gatt_characteristic1_get_flags(charInterface);
	if (flagVariant)
	{
		std::vector <std::string> characteristicFlags = convertArrayStringGVariantToVector(flagVariant);

		for (auto it = characteristicFlags.begin(); it != characteristicFlags.end(); it++)
		{
			auto property = GattRemoteCharacteristic::characteristicPropertyMap.find(*it);
			if (property != GattRemoteCharacteristic::characteristicPropertyMap.end())
				properties |= property->second;
		}
	}

	characteristic.setProperties(properties);

	BluetoothGattValue result;

	result = convertArrayByteGVariantToVector(charValue);

	characteristic.setValue(result);

	// The ids from the service's path, all of their digits: they were the last character of each component, so
	// from id 10 on writes went to another service or were dropped. A NULL path made a std::string throw.
	const char *servicePathStr = bluez_gatt_characteristic1_get_service(charInterface);
	std::string servicePath = servicePathStr ? servicePathStr : "";
	LocalGattIds ids;
	if (!parseLocalGattPath(servicePath, ids))
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "unexpected service path for handleWriteCharacteristic: %s", servicePath.c_str());
		return;
	}
	uint16_t appId = ids.appId;
	uint16_t serviceId = ids.serviceId;

	auto appIt = mGattLocalApplications.find(appId);

	if (appIt == mGattLocalApplications.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "application not present for handleWriteCharacteristic");
		return;
	}

	auto &services = appIt->second->mGattLocalServices;

	auto srvIt = services.find(serviceId);

	if (srvIt == services.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Service is not present list for handleWriteCharacteristic");
		return;
	}

	const char* uuid = bluez_gatt_service1_get_uuid(srvIt->second->mServiceInterface);
	if (!uuid)
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "Failed to get Gatt Service uuid on %s",
			  servicePath.c_str());
		return;
	}
	BluetoothUuid service(uuid);

	if (getGattObserver())
		getGattObserver()->characteristicValueChanged(service, characteristic, convertAddressToLowerCase(mAdapter->getAddress()));
	return;
}

gboolean Bluez5ProfileGatt::Bluez5GattLocalCharacteristic::onHandleStartNotify(BluezGattCharacteristic1 *object,
																			   GDBusMethodInvocation *invocation,
																			   gpointer /*user_data*/)
{
	bluez_gatt_characteristic1_set_notifying(object, true);
	g_dbus_method_invocation_return_value(invocation, NULL);
	return true;
}

gboolean Bluez5ProfileGatt::Bluez5GattLocalCharacteristic::onHandleStopNotify(BluezGattCharacteristic1 *object,
																			  GDBusMethodInvocation *invocation,
																			  gpointer /*user_data*/)
{
	bluez_gatt_characteristic1_set_notifying(object, false);
	g_dbus_method_invocation_return_value(invocation, NULL);
	return true;
}

gboolean Bluez5ProfileGatt::Bluez5GattLocalDescriptor::onHandleReadValue(BluezGattDescriptor1* interface,
																		 GDBusMethodInvocation *invocation,
																		 GVariant *arg_options,
																		 gpointer /*user_data*/)
{
	returnValueFrom(invocation, bluez_gatt_descriptor1_get_value(interface), arg_options);
	return true;
}

gboolean Bluez5ProfileGatt::Bluez5GattLocalDescriptor::onHandleWriteValue(BluezGattDescriptor1* interface,
																		  GDBusMethodInvocation *invocation,
																		  GVariant *arg_value,
																		  GVariant *arg_options,
																		  gpointer user_data)
{
	GVariant *value = nullptr;
	if (!valueAfterWrite(invocation, bluez_gatt_descriptor1_get_value(interface), arg_value, arg_options, &value))
		return true;
	bluez_gatt_descriptor1_set_value(interface, value);
	g_dbus_method_invocation_return_value(invocation, NULL);
	Bluez5ProfileGatt *pThis = static_cast<Bluez5ProfileGatt *> (user_data);
	pThis->onHandleDescrptorWriteValue(interface, bluez_gatt_descriptor1_get_value(interface));
	return true;
}

void Bluez5ProfileGatt::onHandleDescrptorWriteValue(BluezGattDescriptor1* interface, GVariant * descValue)
{
	const char* descUuid = bluez_gatt_descriptor1_get_uuid(interface);

	// The ids from the characteristic's path, as for characteristic writes.
	const char *charPathStr = bluez_gatt_descriptor1_get_characteristic(interface);
	std::string charPath = charPathStr ? charPathStr : "";
	LocalGattIds ids;
	if (!descUuid || !parseLocalGattPath(charPath, ids) || !ids.charId)
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "unexpected descriptor for onHandleDescrptorWriteValue: %s", charPath.c_str());
		return;
	}
	uint16_t charId = ids.charId;
	uint16_t serviceId = ids.serviceId;
	std::string servicePath = charPath.substr(0, charPath.find_last_of('/'));
	uint16_t appId = ids.appId;
	auto appIt = mGattLocalApplications.find(appId);

	if (appIt == mGattLocalApplications.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "application not present for onHandleDescrptorWriteValue");
		return;
	}

	auto &services = appIt->second->mGattLocalServices;

	auto srvIt = services.find(serviceId);

	if (srvIt == services.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "Service is not present list for onHandleDescrptorWriteValue");
		return;
	}

	const char* serviceUuid = bluez_gatt_service1_get_uuid(srvIt->second->mServiceInterface);
	if (!serviceUuid)
	{
		ERROR(MSGID_GATT_PROFILE_ERROR, 0, "Failed to get Gatt Service uuid on %s",
			  servicePath.c_str());
		return;
	}

	auto &chars = srvIt->second->mCharacteristics;

	auto charIt = chars.find(charId);

	if (charIt == chars.end())
	{
		ERROR("MSGID_GATT_PROFILE_ERROR", 0, "char is not present list for onHandleDescrptorWriteValue");
		return;
	}

	const char* charUuid = bluez_gatt_characteristic1_get_uuid(charIt->second->mInterface);
	if (!charUuid || !getGattObserver())
		return;

	BluetoothGattDescriptor desc;
	desc.setUuid(BluetoothUuid(descUuid));
	BluetoothGattValue result = convertArrayByteGVariantToVector(descValue);
	desc.setValue(result);
	getGattObserver()->descriptorValueChanged(BluetoothUuid(serviceUuid), BluetoothUuid(charUuid), desc);
}
