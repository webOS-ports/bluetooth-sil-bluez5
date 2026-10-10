// Copyright (c) 2020 LG Electronics, Inc.
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

#ifndef BLUEZ5MEDIAPLAYER_H
#define BLUEZ5MEDIAPLAYER_H

#include <bluetooth-sil-api.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "asyncutils.h"

extern "C" {
#include "freedesktop-interface.h"
#include "bluez-interface.h"
}

class Bluez5ProfileAvcrp;
class Bluez5MediaFolder;

typedef gboolean (* bluezSendPassThroughCommand)(
		BluezMediaPlayer1 *proxy,
		GCancellable *cancellable,
		GError **error);

class Bluez5MediaPlayer
{
public:
	static const std::map<BluetoothAvrcpPassThroughKeyCode, bluezSendPassThroughCommand> mPassThroughCmd;
	static const std::map<std::string, BluetoothMediaPlayStatus::MediaPlayStatus> mPlayStatus;

	Bluez5MediaPlayer(Bluez5ProfileAvcrp *avrcp, GDBusObject *object);
	~Bluez5MediaPlayer();

	/* AVRCP CT player control APIs */
	BluetoothError sendPassThroughCommand(BluetoothAvrcpPassThroughKeyCode keyCode,
			BluetoothAvrcpPassThroughKeyStatus keyStatus);
	// Answered with callback, once, after the remote device has answered every setting (BlueZ completes the Set
	// only then) or the first one failed.
	void setPlayerApplicationSettingsProperties(
			const BluetoothPlayerApplicationSettingsPropertiesList &properties,
			BluetoothResultCallback callback);

	/* AVRCP CT browse APIs called from Bluez5ProfileAvcrp */
	void getNumberOfItems(BluetoothAvrcpBrowseTotalNumberOfItemsCallback callback);
	void getFolderItems(uint32_t startIndex, uint32_t endIndex,
						BluetoothAvrcpBrowseFolderItemsCallback callback);
	BluetoothError changePath(const std::string &itemPath);
	BluetoothError playItem(const std::string &itemPath);
	BluetoothError addToNowPlaying(const std::string &itemPath);
	void search(const std::string &searchString,
                BluetoothAvrcpBrowseSearchListCallback callback);

	/* AVRCP CT helper functions called from AVRCP profile class */
	std::string getPlayerObjPath() const { return mPlayerInfo.getPath(); }
	const std::string &getDeviceAddress() const { return mDeviceAddress; }
	bool getAddressed() const { return mPlayerInfo.getAddressed(); }
	BluetoothPlayerInfo getPlayerInfo() const { return mPlayerInfo; }
	void getAllProperties();
	void setAddressed(bool addressed) { mPlayerInfo.setAddressed(addressed); }

private:
	Bluez5ProfileAvcrp *mAvrcp;
	BluezMediaPlayer1 *mPlayerInterface;
	FreeDesktopDBusProperties *mPropertiesProxy;
	GDBusObject *mObject;	// for its interface-added/removed handlers
	// The device this player belongs to, from its object path: events were reported under the one address the
	// profile kept, whichever device sent them.
	std::string mDeviceAddress;
	BluetoothMediaPlayStatus mMediaPlayStatus;
	BluetoothPlayerInfo mPlayerInfo;
	Bluez5MediaFolder *mMediaFolder;

	static void handlePropertiesChanged(BluezMediaPlayer1 *transportInterface,
			gchar *, GVariant *changedProperties, GVariant *invalidatedProperties,
			gpointer userData);
	static void handleInterfaceAdded(GDBusObject *object,
			GDBusInterface *interface, gpointer userData);
	static void handleInterfaceRemoved(GDBusObject *object,
			GDBusInterface *interface, gpointer userData);
	void mediaPlayerPropertiesChanged(GVariant *changedProperties);
	void updateMediaFolder(GDBusObject *object);
	bool reporting() const;

	/* Player application settings helper functions */
	BluetoothPlayerApplicationSettingsRepeat repeatStringToEnum(std::string repeatVal);
	BluetoothPlayerApplicationSettingsShuffle shuffleStringToEnum(std::string shuffleVal);
	BluetoothPlayerApplicationSettingsScan scanStringToEnum(std::string scanVal);
	BluetoothPlayerApplicationSettingsEqualizer equalizerStringToEnum(std::string equalizerVal);
	std::string equalizerEnumToString(BluetoothPlayerApplicationSettingsEqualizer equalizer);
	std::string repeatEnumToString(BluetoothPlayerApplicationSettingsRepeat repeat);
	std::string shuffleEnumToString(BluetoothPlayerApplicationSettingsShuffle shuffle);
	std::string scanEnumToString(BluetoothPlayerApplicationSettingsScan scan);
	bool updatePlayerProperties();
	BluetoothAvrcpPlayerType playerTypeStringToEnum(const std::string type);

	// {property, value} for org.bluez.MediaPlayer1, set one after the other.
	typedef std::vector<std::pair<std::string, std::string>> PlayerSettings;
	void setNextPlayerSetting(std::shared_ptr<const PlayerSettings> settings, size_t index,
			BluetoothResultCallback callback);

	LifetimeGuard mLifetime;	// last: replies after the player went see it expire first
};

#endif //BLUEZ5MEDIAPLAYER_H
