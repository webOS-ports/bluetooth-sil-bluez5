// Copyright (c) 2019 LG Electronics, Inc.
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

#ifndef BLUEZ5MPRISPLAYER_H
#define BLUEZ5MPRISPLAYER_H

#include <string>
#include <glib.h>
#include <bluetooth-sil-api.h>

#include "asyncutils.h"

extern "C" {
#include "freedesktop-interface.h"
#include "bluez-interface.h"
}

class Bluez5MprisPlayer
{
public:
	// The player is exported and registered under adapterPath + "/mpris/MediaPlayer2".
	Bluez5MprisPlayer(BluezMedia1* media, const std::string &adapterPath);

	~Bluez5MprisPlayer();

	Bluez5MprisPlayer(const Bluez5MprisPlayer&) = delete;
	Bluez5MprisPlayer& operator = (const Bluez5MprisPlayer&) = delete;

	static void handleBusAcquired(GDBusConnection *connection, const gchar *name, gpointer user_data);
	// Its own reference: the bus-acquired callback only lends the shared system bus connection.
	void setConnection(GDBusConnection *connection)
	{
		if (connection)
			g_object_ref(connection);
		if (mConn)
			g_object_unref(mConn);
		mConn = connection;
	}
	void createInterface();
	// Both asynchronous: they return before bluetoothd has answered.
	void registerPlayer();
	void unRegisterPlayer();
	bool isRegistered() const { return mRegistered; }

	bool setMediaPlayStatus(const BluetoothMediaPlayStatus &status);
	bool setMediaMetaData(const BluetoothMediaMetaData &metaData);
	bool setMediaMetaDataOnMprisInterface();
	bool setMediaPosition(uint64_t pos);
	bool setMediaDuration(uint64_t duration);

	void setTitle(const std::string &title) { mTitle = title; }
	void setArtist(const std::string &artist) { mArtist = artist; }
	void setAlbum(const std::string &album) { mAlbum = album; }
	void setGenre(const std::string &genre) { mGenre = genre; }
	void setTrackNumber(uint32_t trackNumber) { mTrackNumber = trackNumber; }
	void setDuration(uint64_t duration) { mLength = duration; }

	int32_t getTrackNumber() const { return mTrackNumber; }

	void buildMetaData(GVariant *variant, const char *key);

private:
	guint mBusId;
	GDBusConnection *mConn;
	std::string mPlayerPath;
	BluezMedia1 *mMediaProxy;
	BluezOrgMprisMediaPlayer2Player *mPlayerInterface;
	std::string mTitle;
	std::string mArtist;
	std::string mAlbum;
	std::string mGenre;
	int32_t mTrackNumber;
	uint64_t mLength;
	bool mRegistered;	// bluetoothd accepted RegisterPlayer
	LifetimeGuard mLifetime;	// last: replies after the player went see it expire first
};

#endif
