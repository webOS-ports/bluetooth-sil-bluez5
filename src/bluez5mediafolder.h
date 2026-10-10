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

#ifndef BLUEZ5MEDIAFOLDER_H
#define BLUEZ5MEDIAFOLDER_H

#include <bluetooth-sil-api.h>
#include <string>

#include "asyncutils.h"

extern "C" {
#include "freedesktop-interface.h"
#include "bluez-interface.h"
}

class Bluez5ProfileAvcrp;

class Bluez5MediaFolder
{
public:
	Bluez5MediaFolder(Bluez5ProfileAvcrp *avrcp, const std::string &playerPath);
	~Bluez5MediaFolder();

	/* AVRCP CT Browse APIs, called from Bluez5MediaPlayer */
	void getNumberOfItems(BluetoothAvrcpBrowseTotalNumberOfItemsCallback callback);
	void getFolderItems(uint32_t startIndex, uint32_t endIndex,
						BluetoothAvrcpBrowseFolderItemsCallback callback);
	// Sent without waiting for the remote device: NONE means the request went out, and its outcome shows in the
	// folder's Name (currentFolderReceived) and the player's status and track. BUSY while another browsing request
	// of this folder is waiting for the remote device.
	BluetoothError changePath(const std::string &itemPath);
	BluetoothError playItem(const std::string &itemPath);
	BluetoothError addToNowPlaying(const std::string &itemPath);
	void search(const std::string &searchString,
                BluetoothAvrcpBrowseSearchListCallback callback);

private:
	std::string mPlayerObjPath;
	Bluez5ProfileAvcrp *mAvrcp;
	BluezMediaFolder1 *mFolderInterface;
	FreeDesktopDBusProperties *mPropertiesProxy;

	static void handlePropertiesChanged(
			BluezMediaFolder1 *folderInterface, gchar *,
			GVariant *changedProperties, GVariant *invalidatedProperties,
			gpointer userData);
	void mediaFolderPropertiesChanged(GVariant* changedProperties);
	static BluetoothAvrcpItemType itemTypeStringToEnum(const std::string type);
	// BlueZ keeps one browsing request per folder (ChangeFolder, MediaItem1.Play, ListItems, Search) and fails
	// another with EBUSY: true while one of ours is waiting for its reply.
	bool mBrowseRequestPending;
	LifetimeGuard mLifetime;	// last: replies after the folder went see it expire first

};
#endif //BLUEZ5MEDIAFOLDER_H
