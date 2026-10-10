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

#include "bluez5profileavrcp.h"
#include "bluez5adapter.h"
#include "logging.h"
#include "utils.h"
#include "bluez5mprisplayer.h"
#include "bluez5profilea2dp.h"
#include "bluez5mediaplayer.h"
#include <algorithm>
#include <string>


const std::string BLUETOOTH_PROFILE_AVRCP_TARGET_UUID = "0000110c-0000-1000-8000-00805f9b34fb";
const std::string BLUETOOTH_PROFILE_AVRCP_REMOTE_UUID = "0000110e-0000-1000-8000-00805f9b34fb";

const std::map <std::string, BluetoothAvrcpPassThroughKeyCode> Bluez5ProfileAvcrp::mKeyMap = {
	{ "POWER",		KEY_CODE_POWER },
	{ "VOLUME UP",	KEY_CODE_VOLUME_UP },
	{ "VOLUME DOWN",KEY_CODE_VOLUME_DOWN },
	{ "MUTE",		KEY_CODE_MUTE },
	{ "PLAY",		KEY_CODE_PLAY},
	{ "STOP",		KEY_CODE_STOP },
	{ "PAUSE",		KEY_CODE_PAUSE},
	{ "FORWARD",	KEY_CODE_NEXT},
	{ "BACKWARD",	KEY_CODE_PREVIOUS },
	{ "REWIND",		KEY_CODE_REWIND},
	{ "FAST FORWARD",KEY_CODE_FAST_FORWARD}
};

Bluez5ProfileAvcrp::Bluez5ProfileAvcrp(Bluez5Adapter* adapter):
Bluez5ProfileBase(adapter, BLUETOOTH_PROFILE_AVRCP_REMOTE_UUID),
mMetaDataRequestId(0),
mMediaPlayStatusRequestId(0),
mObjectManager(nullptr)
{
	mWatcherId = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.bluez", G_BUS_NAME_WATCHER_FLAGS_NONE,
		handleBluezServiceStarted, handleBluezServiceStopped, this, NULL);
}

Bluez5ProfileAvcrp::~Bluez5ProfileAvcrp()
{
	/* Stops watching a name: first, so that its callbacks do not run on what is torn down */
	if (mWatcherId)
	{
		g_bus_unwatch_name(mWatcherId);
		mWatcherId = 0;
	}

	if (mObjectManager)
	{
		g_signal_handlers_disconnect_by_data(mObjectManager, this);
		g_object_unref(mObjectManager);
		mObjectManager = nullptr;
	}

	// The players were never deleted: their proxies follow org.bluez, and when the phone played again after a
	// bluetoothd restart their handlers called into this deleted profile.
	for (auto player : mMediaPlayerList)
		delete player;
	mMediaPlayerList.clear();
}

void Bluez5ProfileAvcrp::connect(const std::string& address, BluetoothResultCallback callback)
{
	auto connectCallback = [address, callback](BluetoothError error) {
		if (error != BLUETOOTH_ERROR_NONE)
		{
			DEBUG("AVRCP connect failed");
			callback(error);
			return;
		}
		DEBUG("AVRCP connected succesfully");
		callback(BLUETOOTH_ERROR_NONE);
	};

	// This device: another device's AVRCP connection refused every connect.
	if (!mDevices.isConnected(address))
	{
		Bluez5ProfileBase::connect(address, connectCallback);
	}
	else
		connectCallback(BLUETOOTH_ERROR_DEVICE_ALREADY_CONNECTED);
}

void Bluez5ProfileAvcrp::handleBluezServiceStarted(GDBusConnection* conn, const gchar* /*name*/,
	const gchar* /*nameOwner*/, gpointer user_data)
{
	DEBUG("handleBluezServiceStarted");
	Bluez5ProfileAvcrp* avrcp = static_cast<Bluez5ProfileAvcrp*>(user_data);

	GError* error = 0;
	avrcp->mObjectManager = g_dbus_object_manager_client_new_sync(conn, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
		"org.bluez", "/", NULL, NULL, NULL, NULL, &error);
	if (error)
	{
		ERROR(MSGID_OBJECT_MANAGER_CREATION_FAILED, 0, "Failed to create object manager: %s", error->message);
		g_error_free(error);
		return;
	}

	g_signal_connect(avrcp->mObjectManager, "object-added", G_CALLBACK(handleObjectAdded), avrcp);
	g_signal_connect(avrcp->mObjectManager, "object-removed", G_CALLBACK(handleObjectRemoved), avrcp);

	GList* objects = g_dbus_object_manager_get_objects(avrcp->mObjectManager);
	for (size_t n = 0; n < g_list_length(objects); n++)
	{
		auto object = static_cast<GDBusObject*>(g_list_nth(objects, n)->data);
		std::string objectPath = g_dbus_object_get_object_path(object);
		auto adapterPath = avrcp->mAdapter->getObjectPath();
		if (objectPath.compare(0, adapterPath.length(), adapterPath))
			continue;

		auto mediaPlayerInterface = g_dbus_object_get_interface(object, "org.bluez.MediaPlayer1");
		if (mediaPlayerInterface)
		{
			DEBUG("MediaPlayer interface");
			avrcp->addMediaPlayer(object);
			g_object_unref(mediaPlayerInterface);
		}
		g_object_unref(object);
	}
	g_list_free(objects);
}

void Bluez5ProfileAvcrp::handleBluezServiceStopped(GDBusConnection* /*conn*/, const gchar* /*name*/,
	gpointer /*user_data*/)
{
}

void Bluez5ProfileAvcrp::handleObjectAdded(GDBusObjectManager* /*objectManager*/, GDBusObject* object, void* userData)
{
	DEBUG("handleObjectAdded");
	Bluez5ProfileAvcrp* avrcp = static_cast<Bluez5ProfileAvcrp*>(userData);

	std::string objectPath = g_dbus_object_get_object_path(object);

	auto adapterPath = avrcp->mAdapter->getObjectPath();
	if (objectPath.compare(0, adapterPath.length(), adapterPath))
		return;

	auto mediaPlayerInterface = g_dbus_object_get_interface(object, "org.bluez.MediaPlayer1");
	if (mediaPlayerInterface)
	{
		DEBUG("Added: %s", objectPath.c_str());
		avrcp->addMediaPlayer(object);
		g_object_unref(mediaPlayerInterface);
	}
}

void Bluez5ProfileAvcrp::handleObjectRemoved(GDBusObjectManager* /*objectManager*/, GDBusObject* object, void* userData)
{
	Bluez5ProfileAvcrp* avrcp = static_cast<Bluez5ProfileAvcrp*>(userData);
	/* We are not unrefing the mPropertiesProxy and mPlayerInterface here.
	 * When new player gets added, object-added event is received first and
	 * then object-removed for the old one. Hence to avoid unrefing the reference
	 * to newly added player, unref the existing player reference whenever new player
	 * is added and get the reference to new player in handleObjectAdded function
	 */
	std::string objectPath = g_dbus_object_get_object_path(object);
	auto adapterPath = avrcp->mAdapter->getObjectPath();
	if (objectPath.compare(0, adapterPath.length(), adapterPath))
		return;
	auto mediaPlayerInterface = g_dbus_object_get_interface(object, "org.bluez.MediaPlayer1");
	if (mediaPlayerInterface)
	{
		avrcp->removeMediaPlayer(objectPath);
		g_object_unref(mediaPlayerInterface);
	}
}

void Bluez5ProfileAvcrp::disconnect(const std::string& address, BluetoothResultCallback callback)
{
	auto disConnectCallback = [address, callback](BluetoothError error) {
		if (error != BLUETOOTH_ERROR_NONE)
		{
			DEBUG("AVRCP disconnect failed");
			callback(error);
			return;
		}
		DEBUG("AVRCP disconnected succesfully");
		callback(BLUETOOTH_ERROR_NONE);
	};
	Bluez5ProfileBase::disconnect(address, disConnectCallback);
}

void Bluez5ProfileAvcrp::enable(const std::string &uuid, BluetoothResultCallback callback)
{
	DEBUG("enable: %s", uuid.c_str());
	mAdapter->notifyAvrcpRoleChange(uuid);
	if (BLUETOOTH_PROFILE_AVRCP_TARGET_UUID == uuid)
	{
		mUuid = BLUETOOTH_PROFILE_AVRCP_REMOTE_UUID;
	}
	else
	{
		mUuid = BLUETOOTH_PROFILE_AVRCP_TARGET_UUID;
	}
	callback(BLUETOOTH_ERROR_NONE);
}

void Bluez5ProfileAvcrp::disable(const std::string &/*uuid*/, BluetoothResultCallback callback)
{
	callback(BLUETOOTH_ERROR_UNSUPPORTED);
}

void Bluez5ProfileAvcrp::getProperties(const std::string &/*address*/, BluetoothPropertiesResultCallback  callback)
{
	// Answered: it was an empty stub, and its caller waited forever.
	callback(BLUETOOTH_ERROR_UNSUPPORTED, BluetoothPropertiesList());
}

void Bluez5ProfileAvcrp::getProperty(const std::string &address, BluetoothProperty::Type type, BluetoothPropertyResultCallback callback)
{
	BluetoothProperty prop(type);
	Bluez5Device *device = mAdapter->findDevice(address);
	if (!device)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID, prop);
		return;
	}

	bool connected = isDeviceConnected(device->getAddress());
	prop.setValue<bool>(connected);
	callback(BLUETOOTH_ERROR_NONE, prop);
}

void Bluez5ProfileAvcrp::supplyMediaMetaData(BluetoothAvrcpRequestId /*requestId*/, const BluetoothMediaMetaData &metaData, BluetoothResultCallback callback)
{
	// No player until org.bluez.Media1 is there, and none again once it goes: it was dereferenced anyway.
	auto player = mAdapter->getPlayer();
	if (!player)
	{
		callback(BLUETOOTH_ERROR_NOT_READY);
		return;
	}
	player->setMediaMetaData(metaData);
	callback(BLUETOOTH_ERROR_NONE);
}

void Bluez5ProfileAvcrp::supplyMediaPlayStatus(BluetoothAvrcpRequestId /*requestId*/, const BluetoothMediaPlayStatus &playStatus, BluetoothResultCallback callback)
{
	auto player = mAdapter->getPlayer();
	if (!player)
	{
		callback(BLUETOOTH_ERROR_NOT_READY);
		return;
	}
	player->setMediaPlayStatus(playStatus);
	if (playStatus.getStatus() == BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_STOPPED)
		player->setMediaPosition(0);
	else
		player->setMediaPosition(playStatus.getPosition());
	player->setMediaDuration(playStatus.getDuration());
	callback(BLUETOOTH_ERROR_NONE);
}

void Bluez5ProfileAvcrp::notifyMediaPlayStatus(const BluetoothMediaPlayStatus &playStatus, BluetoothResultCallback callback)
{
	if (!mDevices.anyTarget())
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Not connected as target");
		callback(BLUETOOTH_ERROR_NOT_ALLOWED);
		return;
	}
	auto player = mAdapter->getPlayer();
	if (!player)
	{
		callback(BLUETOOTH_ERROR_NOT_READY);
		return;
	}
	player->setMediaPlayStatus(playStatus);
	if (playStatus.getStatus() == BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_STOPPED)
		player->setMediaPosition(0);
	else
		player->setMediaPosition(playStatus.getPosition());
	player->setMediaDuration(playStatus.getDuration());
	callback(BLUETOOTH_ERROR_NONE);
}


void Bluez5ProfileAvcrp::mediaPlayStatusRequested(const std::string &address)
{
	getAvrcpObserver()->mediaPlayStatusRequested(generateMediaPlayStatusRequestId(),
		convertAddressToLowerCase(mAdapter->getAddress()), convertAddressToLowerCase(address));
}

void Bluez5ProfileAvcrp::mediaMetaDataRequested(const std::string &address)
{
	getAvrcpObserver()->mediaMetaDataRequested(generateMetaDataRequestId(),
		convertAddressToLowerCase(mAdapter->getAddress()), convertAddressToLowerCase(address));
}

void Bluez5ProfileAvcrp::updateConnectionStatus(const std::string &address, bool status, const std::string &uuid)
{
	DEBUG("AVRCP isConnected %d:%s", status, uuid.c_str());
	BluetoothPropertiesList properties;

	// The remote's target connected: the local controller role.
	auto change = mDevices.setRole(address, BLUETOOTH_PROFILE_AVRCP_TARGET_UUID == uuid, status);
	if (change == AvrcpDevices::Change::CONNECTED)
	{
		DEBUG("AVRCP: Notifying upper layer avrcp connected");
		/* Inform upper layer when the first of this device's roles connects. */
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::CONNECTED, true));
		getObserver()->propertiesChanged(convertAddressToLowerCase(mAdapter->getAddress()),
				convertAddressToLowerCase(address), properties);
	}
	else if (change == AvrcpDevices::Change::DISCONNECTED)
	{
		DEBUG("AVRCP: Notifying upper layer avrcp disconnected");
		/* Inform upper layer when both of this device's roles are disconnected. */
		properties.push_back(BluetoothProperty(BluetoothProperty::Type::CONNECTED, false));
		getObserver()->propertiesChanged(convertAddressToLowerCase(mAdapter->getAddress()),
				convertAddressToLowerCase(address), properties);
	}
}

bool Bluez5ProfileAvcrp::isDeviceConnected(const std::string &address) const
{
	return mDevices.isConnected(address);
}

void Bluez5ProfileAvcrp::updateSupportedNotificationEvents(uint16_t notificationEvents, const std::string& address)
{
	DEBUG("notificationEvents: %x\n", notificationEvents);
	BluetoothAvrcpSupportedNotificationEventList eventsList;
	if (notificationEvents & (1 << EVENT_STATUS_CHANGED))
	{
		eventsList.push_back(EVENT_STATUS_CHANGED);
	}
	if (notificationEvents & (1 << EVENT_TRACK_CHANGED))
	{
		eventsList.push_back(EVENT_TRACK_CHANGED);
	}
	if (notificationEvents & (1 << EVENT_TRACK_REACHED_END))
	{
		eventsList.push_back(EVENT_TRACK_REACHED_END);
	}
	if (notificationEvents & (1 << EVENT_TRACK_REACHED_START))
	{
		eventsList.push_back(EVENT_TRACK_REACHED_START);
	}
	if (notificationEvents & (1 << EVENT_PLAYBACK_POS_CHANGED))
	{
		eventsList.push_back(EVENT_PLAYBACK_POS_CHANGED);
	}
	if (notificationEvents & (1 << EVENT_BATTERY_STATUS_CHANGED))
	{
		eventsList.push_back(EVENT_BATTERY_STATUS_CHANGED);
	}
	if (notificationEvents & (1 << EVENT_SYSTEM_STATUS_CHANGED))
	{
		eventsList.push_back(EVENT_SYSTEM_STATUS_CHANGED);
	}
	if (notificationEvents & (1 << EVENT_PLAYER_APPLICATION_SETTING_CHANGED))
	{
		eventsList.push_back(EVENT_PLAYER_APPLICATION_SETTING_CHANGED);
	}
	if (notificationEvents & (1 << EVENT_NOW_PLAYING_CHANGED))
	{
		eventsList.push_back(EVENT_NOW_PLAYING_CHANGED);
	}
	if (notificationEvents & (1 << EVENT_AVAILABLE_PLAYERS_CHANGED))
	{
		eventsList.push_back(EVENT_AVAILABLE_PLAYERS_CHANGED);
	}
	if (notificationEvents & (1 << EVENT_ADDRESSED_PLAYERS_CHANGED))
	{
		eventsList.push_back(EVENT_ADDRESSED_PLAYERS_CHANGED);
	}
	if (notificationEvents & (1 << EVENT_UIDS_CHANGED))
	{
		eventsList.push_back(EVENT_UIDS_CHANGED);
	}
	if (notificationEvents & (1 << EVENT_VOLUME_CHANGED))
	{
		eventsList.push_back(EVENT_VOLUME_CHANGED);
	}
	getAvrcpObserver()->supportedNotificationEventsReceived(eventsList, convertAddressToLowerCase(mAdapter->getAddress()),
		convertAddressToLowerCase(address));
}

void Bluez5ProfileAvcrp::updateRemoteFeatures(uint8_t features, const std::string &role, const std::string &address)
{
	Bluez5Device *device = mAdapter->findDevice(address);
	if (!device)
	{
		return;
	}

	BluetoothAvrcpRemoteFeatures remoteFeatures = FEATURE_NONE;
	if (features & REMOTE_DEVICE_AVRCP_FEATURE_BROWSE)
	{
		remoteFeatures = FEATURE_BROWSE;
		if (isDeviceConnected(address))
			getAvrcpObserver()->remoteFeaturesReceived(remoteFeatures,
				convertAddressToLowerCase(mAdapter->getAddress()),
				convertAddressToLowerCase(address), role);
	}
	if (features & REMOTE_DEVICE_AVRCP_FEATURE_ABSOLUTE_VOLUME)
	{
		remoteFeatures = FEATURE_ABSOLUTE_VOLUME;
		if (isDeviceConnected(address))
			getAvrcpObserver()->remoteFeaturesReceived(remoteFeatures,
				convertAddressToLowerCase(mAdapter->getAddress()),
				convertAddressToLowerCase(address), role);

	}
	if (features & REMOTE_DEVICE_AVRCP_FEATURE_METADATA)
	{
		remoteFeatures = FEATURE_METADATA;
		if (isDeviceConnected(address))
			getAvrcpObserver()->remoteFeaturesReceived(remoteFeatures,
				convertAddressToLowerCase(mAdapter->getAddress()),
				convertAddressToLowerCase(address), role);
	}
}

void Bluez5ProfileAvcrp::updateVolume(const std::string &address, int volume)
{
	DEBUG("updateVolume %d", volume);

	Bluez5Device *device = mAdapter->findDevice(address);
	if (!device)
	{
		DEBUG("Bluez5ProfileAvcrp::updateVolume not handled");
		return;
	}

	if (mDevices.isConnected(address))	// this device, not any device
		getAvrcpObserver()->volumeChanged(volume, convertAddressToLowerCase(mAdapter->getAddress()), convertAddressToLowerCase(address));
}

void Bluez5ProfileAvcrp::recievePassThroughCommand(std::string address, std::string key, std::string state)
{
	DEBUG("Bluez5ProfileAvcrp::recievePassThroughCommand %s %s", key.c_str(), state.c_str());
	BluetoothAvrcpPassThroughKeyStatus keyStatus = KEY_STATUS_UNKNOWN;
	BluetoothAvrcpPassThroughKeyCode keyCode = KEY_CODE_UNKNOWN;

	if (state == "pressed")
		keyStatus = KEY_STATUS_PRESSED;
	else if (state == "released")
		keyStatus = KEY_STATUS_RELEASED;

	auto keyCodeIt = mKeyMap.find(key);
	if (keyCodeIt != mKeyMap.end())
		keyCode = keyCodeIt->second;

	Bluez5Device *device = mAdapter->findDevice(address);
	if (!device)
	{
		DEBUG("Bluez5ProfileAvcrp::recievePassThroughCommand not handled");
		return;
	}
	// This device in the target role: another device's target role let a controller-only device through.
	if (mDevices.isTarget(address))
	{
		getAvrcpObserver()->passThroughCommandReceived(keyCode, keyStatus, convertAddressToLowerCase(mAdapter->getAddress()), convertAddressToLowerCase(address));

		//TODO: Remove setMediaPlayStatus() call from this code block, after
		//proper implementation in media application
		BluetoothMediaPlayStatus status;
		if (!mAdapter->getPlayer())
			return;
		if (key == "PLAY")
		{
			status.setStatus(BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_PLAYING);
			mAdapter->getPlayer()->setMediaPlayStatus(status);
		}
		else if (key == "PAUSE")
		{
			status.setStatus(BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_PAUSED);
			mAdapter->getPlayer()->setMediaPlayStatus(status);
		}
		else if (key == "STOP")
		{
			status.setStatus(BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_STOPPED);
			mAdapter->getPlayer()->setMediaPlayStatus(status);
		}
	}
}

BluetoothError Bluez5ProfileAvcrp::setAbsoluteVolume(const std::string &address, int volume)
{
	// AVRCP absolute volume is 0..127: -1 became 255 and 256 became 0, and BlueZ refuses >127 later while NONE
	// was returned. Without a transport the property was set on NULL.
	if (volume < 0 || volume > 127)
		return BLUETOOTH_ERROR_PARAM_INVALID;

	Bluez5ProfileA2dp *a2dp = dynamic_cast<Bluez5ProfileA2dp*> (mAdapter->getProfile(BLUETOOTH_PROFILE_ID_A2DP));
	BluezMediaTransport1 *transport = a2dp ? a2dp->getMediaTransport(address) : nullptr;
	if (!transport)
		return BLUETOOTH_ERROR_NOT_READY;

	bluez_media_transport1_set_volume(transport, (guint16)volume);
	return BLUETOOTH_ERROR_NONE;
}

void Bluez5ProfileAvcrp::setPlayerApplicationSettingsProperties(
	const BluetoothPlayerApplicationSettingsPropertiesList &properties,
	BluetoothResultCallback callback)
{
	const std::string device = mDevices.currentDevice();
	if (!mDevices.anyController())
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Not connected as controller");
		callback(BLUETOOTH_ERROR_NOT_ALLOWED);
		return;
	}
	Bluez5MediaPlayer *player = controllerPlayer(device);
	if (!player)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Addressed player is not there");
		callback(BLUETOOTH_ERROR_NOT_ALLOWED);
		return;
	}
	callback(player->setPlayerApplicationSettingsProperties(properties));
}

BluetoothError Bluez5ProfileAvcrp::sendPassThroughCommand(
	const std::string &address,
	BluetoothAvrcpPassThroughKeyCode keyCode,
	BluetoothAvrcpPassThroughKeyStatus keyStatus)
{
	DEBUG("AVRCP: sendPassThroughCommand");
	Bluez5Device* device = mAdapter->findDevice(address);
	if (!device)
	{
		DEBUG("AVRCP: device not found.");
		return BLUETOOTH_ERROR_UNKNOWN_DEVICE_ADDR;
	}
	/* process the command if avrcp is connected in CT role.  */
	// To this device's addressed player: the command went to the player addressed last, of any device.
	if (!mDevices.isController(address))
	{
		DEBUG("AVRCP: Not connected as controller");
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}
	Bluez5MediaPlayer *player = controllerPlayer(address);
	if (!player)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Addressed player is not there");
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}
	return player->sendPassThroughCommand(keyCode, keyStatus);
}

Bluez5MediaPlayer *Bluez5ProfileAvcrp::findMediaPlayer(const std::string &playerPath) const
{
	if (playerPath.empty())
		return nullptr;
	auto it = std::find_if(mMediaPlayerList.begin(), mMediaPlayerList.end(),
		[&playerPath](const Bluez5MediaPlayer *player) { return player->getPlayerObjPath() == playerPath; });
	return it != mMediaPlayerList.end() ? *it : nullptr;
}

Bluez5MediaPlayer *Bluez5ProfileAvcrp::controllerPlayer(const std::string &address) const
{
	if (!mDevices.isController(address))
		return nullptr;
	return findMediaPlayer(mDevices.addressedPlayer(address));
}

void Bluez5ProfileAvcrp::addMediaPlayer(GDBusObject *object)
{
	std::string objectPath = g_dbus_object_get_object_path(object);
	Bluez5MediaPlayer *mediaPlayer = new Bluez5MediaPlayer(this, object);
	mMediaPlayerList.push_back(mediaPlayer);
	addressedPlayerChanged(objectPath);
	mediaPlayer->getAllProperties();
}

void Bluez5ProfileAvcrp::removeMediaPlayer(const std::string &playerPath)
{
	mDevices.removePlayer(playerPath);
	for (auto player = mMediaPlayerList.begin();
		 player != mMediaPlayerList.end(); ++player)
	{
		if ((*player)->getPlayerObjPath() == playerPath)
		{
			auto playerToRemove = *player;
			mMediaPlayerList.erase(player);
			delete playerToRemove;
			break;
		}
	}
	std::string deviceAddress;
	objPathToDevAddress(playerPath, deviceAddress);
	updatePlayerInfo(deviceAddress);
}

std::string Bluez5ProfileAvcrp::getAdapterAddress()
{
	 return mAdapter->getAddress();
}

// The player's device only: a new player of device B un-addressed device A's player.
void Bluez5ProfileAvcrp::addressedPlayerChanged(const std::string &playerPath)
{
	std::string deviceAddress;
	objPathToDevAddress(playerPath, deviceAddress);
	mDevices.setAddressedPlayer(deviceAddress, playerPath);

	for (auto player : mMediaPlayerList)
	{
		if (AvrcpDevices::key(player->getDeviceAddress()) == AvrcpDevices::key(deviceAddress))
			player->setAddressed(player->getPlayerObjPath() == playerPath);
	}

	updatePlayerInfo(deviceAddress);
	getAvrcpObserver()->currentFolderReceived(
		"",
		convertAddressToLowerCase(mAdapter->getAddress()),
		convertAddressToLowerCase(deviceAddress));
}

void Bluez5ProfileAvcrp::updatePlayerInfo(const std::string &address)
{
	// Each device's own players, under its own address: all players were reported under one device.
	BluetothPlayerInfoList playerInfoList;
	for (auto player : mMediaPlayerList)
	{
		if (AvrcpDevices::key(player->getDeviceAddress()) == AvrcpDevices::key(address))
			playerInfoList.push_back(player->getPlayerInfo());
	}
	DEBUG("Calling observer API for playerInfo");
	getAvrcpObserver()->playerInfoReceived(
		playerInfoList,
		convertAddressToLowerCase(mAdapter->getAddress()),
		convertAddressToLowerCase(address));
}

void Bluez5ProfileAvcrp::getNumberOfItems(BluetoothAvrcpBrowseTotalNumberOfItemsCallback callback)
{
	if (callback)
	{
		Bluez5MediaPlayer *player = controllerPlayer(mDevices.currentDevice());
		if (!player)
		{
			ERROR(MSGID_AVRCP_PROFILE_ERROR, 0,
				  "Not connected as controller/addressed player not there");
			callback(BLUETOOTH_ERROR_NOT_ALLOWED, 0);
			return;
		}
		player->getNumberOfItems(callback);
	}
	return;
}

void Bluez5ProfileAvcrp::getFolderItems(uint32_t startIndex, uint32_t endIndex,
										BluetoothAvrcpBrowseFolderItemsCallback callback)
{
	if (callback)
	{
		BluetoothFolderItemList itemList;

		Bluez5MediaPlayer *player = controllerPlayer(mDevices.currentDevice());
		if (!player)
		{
			ERROR(MSGID_AVRCP_PROFILE_ERROR, 0,
				  "Not connected as controller/addressed player not there");
			callback(BLUETOOTH_ERROR_NOT_ALLOWED, itemList);
			return;
		}
		player->getFolderItems(startIndex, endIndex, callback);
	}
	return;
}

BluetoothError Bluez5ProfileAvcrp::changePath(const std::string &itemPath)
{
	Bluez5MediaPlayer *player = controllerPlayer(mDevices.currentDevice());
	if (!player)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0,
			  "Not connected as controller/addressed player not there");
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}

	return player->changePath(itemPath);
}

BluetoothError Bluez5ProfileAvcrp::playItem(const std::string &itemPath)
{
	Bluez5MediaPlayer *player = controllerPlayer(mDevices.currentDevice());
	if (!player)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0,
			  "Not connected as controller/addressed player not there");
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}

	return player->playItem(itemPath);
}

BluetoothError Bluez5ProfileAvcrp::addToNowPlaying(const std::string &itemPath)
{
	Bluez5MediaPlayer *player = controllerPlayer(mDevices.currentDevice());
	if (!player)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0,
			  "Not connected as controller/addressed player not there");
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}

	return player->addToNowPlaying(itemPath);
}

void Bluez5ProfileAvcrp::search(const std::string &searchString,
                BluetoothAvrcpBrowseSearchListCallback callback)
{
	Bluez5MediaPlayer *player = controllerPlayer(mDevices.currentDevice());
	if (!player)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0,
			  "Not connected as controller/addressed player not there");
		callback(BLUETOOTH_ERROR_NOT_ALLOWED, "");
		return;
	}

	return player->search(searchString, callback);
}
