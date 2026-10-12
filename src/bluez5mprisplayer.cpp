// Copyright (c) 2019-2020 LG Electronics, Inc.
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

#include "mpristime.h"
#include "glib.h"
#include "bluez5mprisplayer.h"
#include "logging.h"
#include "asyncutils.h"
#include <bluetooth-sil-api.h>
#include <string>
#include <map>

#define BLUEZ5_MEDIA_PLAYER_BUS_NAME    "com.webos.service.bluezMprisPlayer"
#define BLUEZ5_MEDIA_PLAYER_PATH        "/mpris/MediaPlayer2"
#define MEDIA_PLAYER_INTERFACE          "org.mpris.MediaPlayer2.Player"

std::map <BluetoothMediaPlayStatus::MediaPlayStatus, std::string> mediaPlayStatusMap =
{
	{ BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_PLAYING, "Playing"},
	{ BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_PAUSED, "Paused"},
	{ BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_STOPPED, "Stopped"},
	{ BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_FWD_SEEK, "forward-seek"},
	{ BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_REV_SEEK, "reverse-seek"}
};

Bluez5MprisPlayer::Bluez5MprisPlayer(BluezMedia1 *media, const std::string &adapterPath):
mBusId(0),
mConn(nullptr),
mPlayerPath(adapterPath + BLUEZ5_MEDIA_PLAYER_PATH),
mMediaProxy(media),
mPlayerInterface(0),
mTrackNumber(0),
mLength(0),
mRegistered(false)
{
		mBusId = g_bus_own_name(G_BUS_TYPE_SYSTEM, BLUEZ5_MEDIA_PLAYER_BUS_NAME,
				G_BUS_NAME_OWNER_FLAGS_NONE,
				handleBusAcquired, NULL, NULL, this, NULL);
}

Bluez5MprisPlayer::~Bluez5MprisPlayer()
{
	if (mPlayerInterface)
		g_object_unref(mPlayerInterface);

	if (mConn)
	{
		g_object_unref(mConn);
	}

	if (mBusId)
	{
		g_bus_unown_name(mBusId);
	}
}

void Bluez5MprisPlayer::handleBusAcquired(GDBusConnection *connection, const gchar */*name*/, gpointer user_data)
{
	Bluez5MprisPlayer *player = static_cast<Bluez5MprisPlayer*>(user_data);
	player->setConnection(connection);
	player->createInterface();
	// Not owned: it was unreffed here, and once more in the destructor.
}

void Bluez5MprisPlayer::createInterface()
{
	mPlayerInterface = bluez_org_mpris_media_player2_player_skeleton_new();
	GError *error = 0;

	if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(mPlayerInterface), mConn,
                                          mPlayerPath.c_str(), &error))
	{
		DEBUG("Failed to initialize agent on bus: %s",error->message);
		g_error_free(error);
		return;
	}
	registerPlayer();
}

void Bluez5MprisPlayer::registerPlayer()
{
	if (!mMediaProxy)
		return;

	GVariantBuilder *builder = 0;
	GVariant *arguments = 0;
	builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	arguments = g_variant_builder_end (builder);
	g_variant_builder_unref(builder);

	// Not waited for: the service's only main loop stood still while bluetoothd (or a hung one, for the default
	// 25 s) answered. Nothing waits for the result; it is logged, and kept for unRegisterPlayer().
	auto registerCallback = [this, alive = mLifetime.token(), path = mPlayerPath](GObject *source,
																					GAsyncResult *result) {
		GError *error = 0;
		gboolean ret = bluez_media1_call_register_player_finish(BLUEZ_MEDIA1(source), result, &error);
		if (!ret || error)
		{
			ERROR(MSGID_MEDIA_PLAYER_ERROR, 0, "Registration of player %s failed: %s", path.c_str(),
				  error ? error->message : "no reply");
			g_clear_error(&error);
			return;
		}
		if (!alive.expired())
			mRegistered = true;
	};
	bluez_media1_call_register_player(mMediaProxy, mPlayerPath.c_str(), arguments, NULL, glibAsyncMethodWrapper,
									  new GlibAsyncFunctionWrapper(registerCallback));
}

void Bluez5MprisPlayer::unRegisterPlayer()
{
	if (!mMediaProxy || !mRegistered)
		return;
	mRegistered = false;
	// Not waited for, like RegisterPlayer.
	auto unregisterCallback = [path = mPlayerPath](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		if (!bluez_media1_call_unregister_player_finish(BLUEZ_MEDIA1(source), result, &error) || error)
		{
			ERROR(MSGID_MEDIA_PLAYER_ERROR, 0, "Unregistration of player %s failed: %s", path.c_str(),
				  error ? error->message : "no reply");
			g_clear_error(&error);
		}
	};
	bluez_media1_call_unregister_player(mMediaProxy, mPlayerPath.c_str(), NULL, glibAsyncMethodWrapper,
										new GlibAsyncFunctionWrapper(unregisterCallback));
}

bool Bluez5MprisPlayer::setMediaPlayStatus(const BluetoothMediaPlayStatus &status)
{
	if (status.getStatus() == BluetoothMediaPlayStatus::MEDIA_PLAYSTATUS_ERROR)
		return true;

	std::string playStatus = mediaPlayStatusMap[status.getStatus()];
	bluez_org_mpris_media_player2_player_set_playback_status(mPlayerInterface, playStatus.c_str());
	return true;
}

bool Bluez5MprisPlayer::setMediaPosition(uint64_t pos)
{
	bluez_org_mpris_media_player2_player_set_position(mPlayerInterface, msToMprisMicroseconds(pos));
	return true;
}

bool Bluez5MprisPlayer::setMediaDuration(uint64_t duration)
{
	setDuration(duration);
	setMediaMetaDataOnMprisInterface();
	return true;
}

bool Bluez5MprisPlayer::setMediaMetaData(const BluetoothMediaMetaData &metaData)
{
	setTitle(metaData.getTitle());
	setArtist(metaData.getArtist());
	setAlbum(metaData.getAlbum());
	setGenre(metaData.getGenre());
	setTrackNumber(metaData.getTrackNumber());
	setDuration(metaData.getDuration());
	setMediaMetaDataOnMprisInterface();
	return true;
}

bool Bluez5MprisPlayer::setMediaMetaDataOnMprisInterface()
{
	GVariantBuilder *builder = 0;
	builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

	//Title
	if (!mTitle.empty())
		g_variant_builder_add (builder, "{sv}", "xesam:title", g_variant_new_string (mTitle.c_str()));

	//Artist
	if (!mArtist.empty())
	{
		GVariantBuilder *artistArrayVarBuilder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
		g_variant_builder_add(artistArrayVarBuilder, "s", mArtist.c_str());
		GVariant *artist = g_variant_builder_end(artistArrayVarBuilder);
		g_variant_builder_unref (artistArrayVarBuilder);
		g_variant_builder_add (builder, "{sv}", "xesam:artist", artist);
	}

	//Album
	if (!mAlbum.empty())
		g_variant_builder_add (builder, "{sv}", "xesam:album", g_variant_new_string (mAlbum.c_str()));

	//Genre
	if (!mGenre.empty())
	{
		GVariantBuilder *genreArrayVarBuilder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
		g_variant_builder_add(genreArrayVarBuilder, "s", mGenre.c_str());
		GVariant *genre = g_variant_builder_end(genreArrayVarBuilder);
		g_variant_builder_unref (genreArrayVarBuilder);
		g_variant_builder_add (builder, "{sv}", "xesam:genre", genre);
	}

	//Track number
	g_variant_builder_add (builder, "{sv}", "xesam:trackNumber", g_variant_new_int32(mTrackNumber));

	//Duration
	g_variant_builder_add (builder, "{sv}", "mpris:length", g_variant_new_int64(msToMprisMicroseconds(mLength)));

	GVariant *arguments = g_variant_builder_end(builder);
	g_variant_builder_unref(builder);

	bluez_org_mpris_media_player2_player_set_metadata(mPlayerInterface, arguments);

	return true;
}

void Bluez5MprisPlayer::buildMetaData(GVariant *variant, const char *key)
{
	GVariantBuilder *builder = 0;
	GVariant *arguments = 0;
	builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (builder, "{sv}", key, variant);
	arguments = g_variant_builder_end(builder);
	g_variant_builder_unref(builder);

	bluez_org_mpris_media_player2_player_set_metadata(mPlayerInterface, arguments);
}
