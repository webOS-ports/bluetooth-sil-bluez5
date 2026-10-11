// Copyright (c) 2020-2021 LG Electronics, Inc.
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

#include "bluez5mediafolder.h"
#include "bluez5profileavrcp.h"
#include "utils.h"
#include "logging.h"
#include "asyncutils.h"

// The calls still made synchronously, which bluetoothd answers itself (BlueZ 5.86 profiles/audio/player.c): the
// NumberOfItems and GetAll property reads from its cache, and AddtoNowPlaying, replied to once the command is queued.
// They blocked the whole service for up to the default 25 s when bluetoothd hung; 3 s bounds that.
// ChangeFolder, MediaItem1.Play, ListItems and Search are answered only after the remote device (BlueZ's own AVCTP
// timeout is 10 s, a control request is retried once): they are asynchronous, with the default D-Bus timeout, as a
// shorter one gave up while BlueZ still held the request and refused the next one with EBUSY.
static const gint LOCAL_CALL_TIMEOUT_MS = 3000;

// A MediaItem1 proxy, owned for the scope: one leaked per changePath/playItem/addToNowPlaying, each with its own
// match rule on the bus.
typedef GObjectRef<BluezMediaItem1> MediaItemRef;

Bluez5MediaFolder::Bluez5MediaFolder(Bluez5ProfileAvcrp *avrcp,
		const std::string &playerPath) :
	mAvrcp(avrcp),
	mFolderInterface(nullptr),
	mPropertiesProxy(nullptr),
	mBrowseRequestPending(false)
{
	mPlayerObjPath = playerPath;
	GError *error = 0;
	DEBUG("Bluez5MediaFolder:: mPlayerObjPath: %s", mPlayerObjPath.c_str());
	mFolderInterface = bluez_media_folder1_proxy_new_for_bus_sync(
			G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
			"org.bluez", mPlayerObjPath.c_str(), NULL, &error);
	if (error)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Not able to get media folder interface: %s", error->message);
		g_error_free(error);
		return;
	}
	mPropertiesProxy = free_desktop_dbus_properties_proxy_new_for_bus_sync(
			G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, "org.bluez",
			mPlayerObjPath.c_str(), NULL, &error);
	if (error)
	{
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Not able to get property interface: %s", error->message);
		g_error_free(error);
		return;
	}

	g_dbus_proxy_set_default_timeout(G_DBUS_PROXY(mPropertiesProxy), LOCAL_CALL_TIMEOUT_MS);

	g_signal_connect(G_OBJECT(mPropertiesProxy), "properties-changed",
			G_CALLBACK(handlePropertiesChanged), this);
	GVariant *propsVar = nullptr;	// set only on success: garbage was unreffed on a failure
	free_desktop_dbus_properties_call_get_all_sync(
			mPropertiesProxy,
			"org.bluez.MediaFolder1", &propsVar, NULL, NULL);
	if (propsVar != NULL)
	{
		mediaFolderPropertiesChanged(propsVar);
		g_variant_unref(propsVar);
	}
}

Bluez5MediaFolder::~Bluez5MediaFolder()
{
	if (mFolderInterface)
	{
		g_signal_handlers_disconnect_by_data(mFolderInterface, this);
		g_object_unref(mFolderInterface);
		mFolderInterface = nullptr;
	}
	if (mPropertiesProxy)
	{
		g_signal_handlers_disconnect_by_data(mPropertiesProxy, this);	// it can outlive the folder
		g_object_unref(mPropertiesProxy);
		mPropertiesProxy = nullptr;
	}
}

void Bluez5MediaFolder::handlePropertiesChanged(
		BluezMediaFolder1 */*folderInterface*/,
		gchar *interface, GVariant *changedProperties,
		GVariant */*invalidatedProperties*/, gpointer userData)
{
	std::string interfaceName = interface;
	auto mediaFolder = static_cast<Bluez5MediaFolder *>(userData);

	if ("org.bluez.MediaFolder1" == interfaceName)
	{
		DEBUG("Bluez5MediaFolder::Media folder properties changed");
		mediaFolder->mediaFolderPropertiesChanged(changedProperties);
	}
}

void Bluez5MediaFolder::mediaFolderPropertiesChanged(GVariant* changedProperties)
{
	DEBUG("mediaFolderPropertiesChanged");
	for (size_t n = 0; n < g_variant_n_children(changedProperties); n++)
	{
		GVariant *propertyVar = g_variant_get_child_value(changedProperties, n);
		GVariant *keyVar = g_variant_get_child_value(propertyVar, 0);
		GVariant *valueVar = g_variant_get_child_value(propertyVar, 1);

		std::string key = g_variant_get_string(keyVar, NULL);
		if ("Name" == key)
		{
			GVariant *nameVar = g_variant_get_variant(valueVar);	// leaked
			std::string currentFolder = g_variant_get_string(nameVar, NULL);
			g_variant_unref(nameVar);
			DEBUG("Bluez5MediaFolder:: CurrentFolder: %s", currentFolder.c_str());
			// The device of this folder's player, not the one address the profile keeps for all devices.
			std::string deviceAddress;
			objPathToDevAddress(mPlayerObjPath, deviceAddress);
			mAvrcp->getAvrcpObserver()->currentFolderReceived(
					currentFolder,
					convertAddressToLowerCase(mAvrcp->getAdapterAddress()),
					convertAddressToLowerCase(deviceAddress));
		}
		g_variant_unref(valueVar);
		g_variant_unref(keyVar);
		g_variant_unref(propertyVar);
	}
}

void Bluez5MediaFolder::getNumberOfItems(BluetoothAvrcpBrowseTotalNumberOfItemsCallback callback)
{
	GError *error = 0;
	GVariant *var = nullptr;
	if (mPropertiesProxy)
		free_desktop_dbus_properties_call_get_sync(
			mPropertiesProxy,
			"org.bluez.MediaFolder1", "NumberOfItems",
			&var, NULL, &error);
	// With no proxy no error was set, and the uninitialised result was used.
	if (error || !var)
	{
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "get numberOfItems failed: %s", error ? error->message : "no proxy");
		g_clear_error(&error);
		if (var)
			g_variant_unref(var);
		callback(BLUETOOTH_ERROR_FAIL, 0);
		return;
	}

	GVariant *realPropVar = g_variant_get_variant(var);
	uint32_t numOfItems = g_variant_is_of_type(realPropVar, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(realPropVar) : 0;
	g_variant_unref(realPropVar);	// both leaked
	g_variant_unref(var);
	DEBUG("Bluez5MediaFolder: Number of items: %d", numOfItems);

	callback(BLUETOOTH_ERROR_NONE, numOfItems);

	return;
}

void Bluez5MediaFolder::getFolderItems(uint32_t startIndex, uint32_t endIndex,
										BluetoothAvrcpBrowseFolderItemsCallback callback)
{
	if (!mFolderInterface)
	{
		// The call failed on the NULL proxy without its completion: never answered, wrapper and callback leaked.
		callback(BLUETOOTH_ERROR_FAIL, BluetoothFolderItemList());
		return;
	}
	if (mBrowseRequestPending)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "List items: another browsing request is waiting for the device");
		callback(BLUETOOTH_ERROR_BUSY, BluetoothFolderItemList());
		return;
	}

	GVariantBuilder *builderFilter = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(builderFilter, "{sv}", "start", g_variant_new_uint32(startIndex));
	g_variant_builder_add(builderFilter, "{sv}", "end", g_variant_new_uint32(endIndex));

	GVariant *filters = g_variant_builder_end(builderFilter);
	g_variant_builder_unref(builderFilter);
	BluetoothFolderItemList itemList;

	// Answered over AVRCP browsing, possibly after the phone switched player and this folder was deleted: the
	// reply finishes on its own proxy (it read the freed folder's interface), and touches the folder only while
	// it is there.
	auto listItemsCallback = [this, alive = mLifetime.token(), callback](GObject *source, GAsyncResult *result) {
		DEBUG("listItemsCallback");
		if (!alive.expired())
			mBrowseRequestPending = false;
		GError *error = 0;
		gboolean ret;
		BluetoothFolderItemList itemList;

		GVariant *items = NULL;
		ret = bluez_media_folder1_call_list_items_finish(BLUEZ_MEDIA_FOLDER1(source), &items, result, &error);
		if (!error && (!ret || !items))
		{
			callback(BLUETOOTH_ERROR_FAIL, itemList);	// was never answered
			return;
		}
		if (error)
		{
			ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "List items failed: %s", error->message);
			g_error_free(error);
			callback(BLUETOOTH_ERROR_FAIL, itemList);
			return;
		}

		if (items)
		{
			g_autoptr(GVariantIter) iter1 = NULL;
			g_autoptr(GVariantIter) iter2 = NULL;
			const gchar *itemObject = NULL;
			g_variant_get(items, "a{oa{sv}}", &iter1);

			while (g_variant_iter_loop(iter1, "{oa{sv}}", &itemObject, &iter2))
			{
				BluetoothFolderItem item;
				const gchar *key;
				g_autoptr(GVariant) value = NULL;
				DEBUG("Object: %s", itemObject);
				/* Modify the item path to not include bluez info */
				std::string itemPath = itemObject;
				size_t pos = itemPath.find("player");
				if (pos != std::string::npos)
				{
					itemPath.erase(0, pos);
				}
				DEBUG("Object: %s", itemPath.c_str());
				item.setPath(itemPath);
				while (g_variant_iter_loop(iter2, "{sv}", &key, &value))
				{
					std::string keyString(key);
					if ("Name" == keyString)
					{
						item.setName(g_variant_get_string(value, NULL));
					}
					else if ("Playable" == keyString)
					{
						item.setPlayable(g_variant_get_boolean(value));
					}
					else if ("Type" == keyString)
					{
						item.setType(itemTypeStringToEnum(g_variant_get_string(value, NULL)));
					}
					else if ("Metadata" == keyString)
					{
						DEBUG("Item: Metadata");
						GVariant *valueTrack = NULL;
						gchar *keyVar = NULL;
						g_autoptr(GVariantIter) iter3 = NULL;
						BluetoothMediaMetaData mediaMetadata;
						if (NULL == value)
						{
							DEBUG("Value is NULL");
						}
						g_variant_get(value, "a{sv}", &iter3);
						if (NULL == iter3)
						{
							DEBUG("iter3 is NULL");
						}
						while (g_variant_iter_loop(iter3, "{sv}", &keyVar, &valueTrack))
						{
							std::string keyTrack(keyVar);
							DEBUG("keyTrack: %s", keyTrack.c_str());
							if ("Duration" == keyTrack)
							{
								mediaMetadata.setDuration(g_variant_get_uint32(valueTrack));
							}
							else if ("Title" == keyTrack)
							{
								mediaMetadata.setTitle(g_variant_get_string(valueTrack, NULL));
							}
							else if ("Album" == keyTrack)
							{
								mediaMetadata.setAlbum(g_variant_get_string(valueTrack, NULL));
							}
							else if ("Artist" == keyTrack)
							{
								mediaMetadata.setArtist(g_variant_get_string(valueTrack, NULL));
							}
							else if ("Genre" == keyTrack)
							{
								mediaMetadata.setGenre(g_variant_get_string(valueTrack, NULL));
							}
							else if ("NumberOfTracks" == keyTrack)
							{
								mediaMetadata.setTrackCount(g_variant_get_uint32(valueTrack));
							}
							else if ("TrackNumber" == keyTrack)
							{
								mediaMetadata.setTrackNumber(g_variant_get_uint32(valueTrack));
							}
						}
						DEBUG("Calling setMetadata");
						item.setMetadata(mediaMetadata);
					}
				}
				itemList.push_back(item);
			}
			g_variant_unref(items);	// leaked on every listing
			callback(BLUETOOTH_ERROR_NONE, itemList);
		}
		return;
	};

	mBrowseRequestPending = true;
	bluez_media_folder1_call_list_items(mFolderInterface, filters, NULL,
										glibAsyncMethodWrapper,
										new GlibAsyncFunctionWrapper(listItemsCallback));
	return;
}

BluetoothAvrcpItemType Bluez5MediaFolder::itemTypeStringToEnum(const std::string type)
{
	if ("audio" == type)
	{
		return BluetoothAvrcpItemType::ITEM_TYPE_AUDIO;
	}
	if ("video" == type)
	{
		return BluetoothAvrcpItemType::ITEM_TYPE_VIDEO;
	}
	if ("folder" == type)
	{
		return BluetoothAvrcpItemType::ITEM_TYPE_FOLDER;
	}
	return BluetoothAvrcpItemType::ITEM_TYPE_AUDIO;
}

BluetoothError Bluez5MediaFolder::changePath(const std::string &itemPath, BluetoothResultCallback done)
{
	GError *error = 0;
	MediaItemRef mediaItemRef(bluez_media_item1_proxy_new_for_bus_sync(
		G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, "org.bluez", itemPath.c_str(), NULL, &error));
	BluezMediaItem1 *mediaItem = mediaItemRef.get();
	if (error)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0,
			  "Not able to get media item interface: %s", error->message);
		g_error_free(error);
		return BLUETOOTH_ERROR_FAIL;
	}
	if (!mediaItem)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "MediaItem is NULL");
		return BLUETOOTH_ERROR_FAIL;
	}
	const gchar *type = bluez_media_item1_get_type_(mediaItem);
	DEBUG("MediaItem type : %s", type);
	if (!type)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "MediaItem type is NULL");
		return BLUETOOTH_ERROR_FAIL;
	}

	if (strcmp(type, "folder"))
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Not a folder: %s", itemPath.c_str());
		return BLUETOOTH_ERROR_AVRCP_NOT_A_FOLDER;
	}

	if (!mFolderInterface)
		return BLUETOOTH_ERROR_FAIL;
	if (mBrowseRequestPending)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Change folder: another browsing request is waiting for the device");
		return BLUETOOTH_ERROR_BUSY;
	}

	// BlueZ answers once the device has: waited for, a phone that did not answer held the whole service.
	// done gets the answer also when the folder went meanwhile: it needs nothing of the folder.
	auto changeFolderCallback = [this, alive = mLifetime.token(), itemPath, done](GObject *source,
																				   GAsyncResult *result) {
		if (!alive.expired())
			mBrowseRequestPending = false;
		GError *error = 0;
		gboolean ret = bluez_media_folder1_call_change_folder_finish(BLUEZ_MEDIA_FOLDER1(source), result, &error);
		BluetoothError answer = BLUETOOTH_ERROR_NONE;
		if (error)
		{
			ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Not able to change folder to %s: %s", itemPath.c_str(),
				  error->message);
			g_error_free(error);
			answer = BLUETOOTH_ERROR_FAIL;
		}
		else if (!ret)
		{
			ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Not able to change folder to %s", itemPath.c_str());
			answer = BLUETOOTH_ERROR_FAIL;
		}
		if (done)
			done(answer);
	};
	mBrowseRequestPending = true;
	bluez_media_folder1_call_change_folder(mFolderInterface, itemPath.c_str(), NULL, glibAsyncMethodWrapper,
										   new GlibAsyncFunctionWrapper(changeFolderCallback));
	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5MediaFolder::playItem(const std::string &itemPath, BluetoothResultCallback done)
{
	GError *error = 0;
	MediaItemRef mediaItemRef(bluez_media_item1_proxy_new_for_bus_sync(
		G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, "org.bluez", itemPath.c_str(), NULL, &error));
	BluezMediaItem1 *mediaItem = mediaItemRef.get();

	if (error)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0,
			  "Not able to get media item interface: %s", error->message);
		g_error_free(error);
		return BLUETOOTH_ERROR_FAIL;
	}
	if (!mediaItem)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "MediaItem is NULL");
		return BLUETOOTH_ERROR_FAIL;
	}
	const gboolean playable = bluez_media_item1_get_playable(mediaItem);
	DEBUG("MediaItem playable : %d", playable);
	if (!playable)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "MediaItem is not playable");
		return BLUETOOTH_ERROR_AVRCP_ITEM_NOT_PLAYABLE;
	}
	if (mBrowseRequestPending)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Play item: another browsing request is waiting for the device");
		return BLUETOOTH_ERROR_BUSY;
	}

	// Answered once the device has, like ChangeFolder. The call holds its own reference to the item proxy.
	auto playCallback = [this, alive = mLifetime.token(), itemPath, done](GObject *source, GAsyncResult *result) {
		if (!alive.expired())
			mBrowseRequestPending = false;
		GError *error = 0;
		gboolean ret = bluez_media_item1_call_play_finish(BLUEZ_MEDIA_ITEM1(source), result, &error);
		BluetoothError answer = BLUETOOTH_ERROR_NONE;
		if (error)
		{
			ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Not able to play media item %s: %s", itemPath.c_str(),
				  error->message);
			g_error_free(error);
			answer = BLUETOOTH_ERROR_FAIL;
		}
		else if (!ret)
		{
			ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Not able to play media item %s", itemPath.c_str());
			answer = BLUETOOTH_ERROR_FAIL;
		}
		if (done)
			done(answer);
	};
	mBrowseRequestPending = true;
	bluez_media_item1_call_play(mediaItem, NULL, glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(playCallback));
	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5MediaFolder::addToNowPlaying(const std::string &itemPath)
{
	GError *error = 0;
	MediaItemRef mediaItemRef(bluez_media_item1_proxy_new_for_bus_sync(
		G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, "org.bluez", itemPath.c_str(), NULL, &error));
	BluezMediaItem1 *mediaItem = mediaItemRef.get();
	if (mediaItem)
		g_dbus_proxy_set_default_timeout(G_DBUS_PROXY(mediaItem), LOCAL_CALL_TIMEOUT_MS);

	if (error)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0,
			  "Not able to get media item interface: %s", error->message);
		g_error_free(error);
		return BLUETOOTH_ERROR_FAIL;
	}
	if (!mediaItem)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "MediaItem is NULL");
		return BLUETOOTH_ERROR_FAIL;
	}
	const gboolean playable = bluez_media_item1_get_playable(mediaItem);
	DEBUG("MediaItem playable : %d", playable);
	if (!playable)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "MediaItem is not playable");
		return BLUETOOTH_ERROR_AVRCP_ITEM_NOT_PLAYABLE;
	}
	// Synchronous: bluetoothd replies once the command is queued, without waiting for the device.
	bluez_media_item1_call_addto_now_playing_sync(mediaItem, NULL, &error);
	if (error)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0,
			  "Not able to add to NowPlaying: %s", error->message);
		if (strstr(error->message, "org.bluez.Error.NotSupported"))
		{
			g_error_free(error);
			return BLUETOOTH_ERROR_NOT_ALLOWED;
		}
		g_error_free(error);
		return BLUETOOTH_ERROR_FAIL;
	}
	return BLUETOOTH_ERROR_NONE;
}

void Bluez5MediaFolder::search(const std::string &searchString,
                BluetoothAvrcpBrowseSearchListCallback callback)
{
	if (!mFolderInterface)
	{
		callback(BLUETOOTH_ERROR_FAIL, "");	// never answered, as for ListItems
		return;
	}
	if (mBrowseRequestPending)
	{
		ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Search: another browsing request is waiting for the device");
		callback(BLUETOOTH_ERROR_BUSY, "");
		return;
	}

	GVariantBuilder *builderFilter = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));

	GVariant *filters = g_variant_builder_end(builderFilter);
	g_variant_builder_unref(builderFilter);

	auto searchCallback = [this, alive = mLifetime.token(), callback](GObject *source, GAsyncResult *result) {
		if (!alive.expired())
			mBrowseRequestPending = false;
		GError *error = 0;
		gboolean ret;
		gchar *outFolder = NULL;

		ret = bluez_media_folder1_call_search_finish(
			BLUEZ_MEDIA_FOLDER1(source), &outFolder, result, &error);
		if (!error && (!ret || !outFolder))
		{
			g_free(outFolder);
			callback(BLUETOOTH_ERROR_FAIL, "");	// a NULL folder made a std::string
			return;
		}
		if (error)
		{
			ERROR(MSGID_AVRCP_PROFILE_ERROR, 0, "Search failed: %s", error->message);
			g_error_free(error);
			callback(BLUETOOTH_ERROR_FAIL, "");
			return;
		}

		/* Modify the item path to not include bluez info */
		std::string itemPath = outFolder;
		g_free(outFolder);
		size_t pos = itemPath.find("player");
		if (pos != std::string::npos)
		{
			itemPath.erase(0, pos);
		}
		DEBUG("Search path: %s", itemPath.c_str());
		callback(BLUETOOTH_ERROR_NONE, itemPath);

	};

	mBrowseRequestPending = true;
	bluez_media_folder1_call_search(mFolderInterface, searchString.c_str(), filters,
										NULL, glibAsyncMethodWrapper,
										new GlibAsyncFunctionWrapper(searchCallback));
	return;
}
