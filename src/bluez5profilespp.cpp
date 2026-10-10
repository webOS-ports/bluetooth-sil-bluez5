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

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <vector>

#include "bluez5profilespp.h"
#include "logging.h"
#include "bluez5adapter.h"
#include "bluez5agent.h"
#include "bluez5profilespp.h"
#include "utils.h"
#include "asyncutils.h"

const std::string BLUETOOTH_PROFILE_SPP_UUID = "00001101-0000-1000-8000-00805f9b34fb";
const std::string BASE_OBJ_PATH = "/bluetooth/profile/serial_port/";

// BlueZ keys external profiles by their object path, for the whole process, while channel ids are counted per
// adapter: with two adapters both registered ".../serial_port/<id>", and the second got AlreadyExists (an SPP
// connect between two local adapters failed). The adapter's name keeps them apart.
std::string Bluez5ProfileSpp::profileObjectPath(BluetoothSppChannelId channelId) const
{
	std::string adapterPath = mAdapter->getObjectPath();
	std::string adapterName = adapterPath.substr(adapterPath.rfind('/') + 1);
	return BASE_OBJ_PATH + adapterName + "/" + std::to_string(channelId);
}

Bluez5ProfileSpp::Bluez5ProfileSpp(Bluez5Adapter *adapter):
	Bluez5ProfileBase(adapter, BLUETOOTH_PROFILE_SPP_UUID), mAdapter(adapter), mConn(0)
{
	GError *error = nullptr;
	mConn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (error)
	{
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to connect on system bus %s", error->message);
		g_error_free(error);
	}
	initialiseChannelIds();
}

Bluez5ProfileSpp::~Bluez5ProfileSpp()
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	// Their fd watches and exported skeletons point at the channel infos freed with the map.
	std::vector<BluetoothSppChannelId> channels;
	for (auto &device : mConnectedDevices)
		channels.push_back(device.first);
	for (auto channelId : channels)
		releaseChannel(channelId);

	if (mConn)
		g_object_unref(mConn);
}

// Ends a channel's RFCOMM link: removes its watch, and shuts the GIOChannel down, which closes the fd (it was
// closed a second time after, by when the number could belong to another descriptor).
void Bluez5ProfileSpp::closeLink(SppDeviceInfo *deviceInfo, bool notify)
{
	bool connected = deviceInfo->mChannel != nullptr;

	if (deviceInfo->mIoWatchId)
	{
		g_source_remove(deviceInfo->mIoWatchId);
		deviceInfo->mIoWatchId = 0;
	}

	if (deviceInfo->mChannel)
	{
		GError *error = nullptr;
		g_io_channel_shutdown(deviceInfo->mChannel, TRUE, &error);
		if (error)
		{
			ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to shutdown channel %s", error->message);
			g_error_free(error);
		}
		g_io_channel_unref(deviceInfo->mChannel);
		deviceInfo->mChannel = nullptr;
	}
	else if (deviceInfo->mSockfd >= 0)
		close(deviceInfo->mSockfd);	// received but never wrapped in a channel
	deviceInfo->mSockfd = -1;

	if (notify && connected)
		getSppObserver()->channelStateChanged(deviceInfo->mAdapterAddress, deviceInfo->mDeviceAddress, deviceInfo->mUuid, deviceInfo->mChannelId, false);
}

// Ends a channel for good: its link, its exported profile object and its id; the entry goes last, as nothing
// may use it after (both disconnect paths read the freed entry's id).
void Bluez5ProfileSpp::releaseChannel(BluetoothSppChannelId channelId)
{
	SppDeviceInfo *deviceInfo = getSppDevice(channelId);
	if (!deviceInfo)
		return;

	closeLink(deviceInfo, false);

	if (deviceInfo->mInterface)
	{
		g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(deviceInfo->mInterface));
		g_object_unref(deviceInfo->mInterface);
		deviceInfo->mInterface = nullptr;
	}

	deallocateChannelId(channelId);
	removeConnectedDevice(channelId);
}

void Bluez5ProfileSpp::initialiseChannelIds()
{
	mChannelIdList = {
		{22, false},
		{6 , false},
	};
}

int Bluez5ProfileSpp::registerProfile(spDeviceInfo &deviceInfo, std::string objPath, BluezProfileManager1 *proxy)
{
	GVariant *profileVariant;
	GVariantBuilder profileBuilder;
	GError *error = nullptr;

	if (!g_variant_is_object_path(objPath.c_str())) {
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "ObjectPath validation failed");
		return BLUETOOTH_ERROR_FAIL;
	}

	g_variant_builder_init(&profileBuilder, G_VARIANT_TYPE("(osa{sv})"));

	g_variant_builder_add (&profileBuilder, "o", objPath.c_str());

	g_variant_builder_add (&profileBuilder, "s", (deviceInfo->mUuid).c_str());

	g_variant_builder_open(&profileBuilder, G_VARIANT_TYPE("a{sv}"));

	g_variant_builder_open(&profileBuilder, G_VARIANT_TYPE("{sv}"));
	g_variant_builder_add (&profileBuilder, "s", "Channel");
	g_variant_builder_add (&profileBuilder, "v", g_variant_new_uint16(deviceInfo->mChannelId));
	g_variant_builder_close(&profileBuilder);

	g_variant_builder_open(&profileBuilder, G_VARIANT_TYPE("{sv}"));
	g_variant_builder_add (&profileBuilder, "s", "Service");
	g_variant_builder_add (&profileBuilder, "v",
			g_variant_new_string((deviceInfo->mUuid).c_str()));
	g_variant_builder_close(&profileBuilder);

	// A server channel takes connections from whoever is in range, and its data goes to the app: BlueZ is asked to
	// require an authenticated (paired) link and the agent's authorisation. Paired devices are unaffected. An
	// outgoing (client) link is ours, so it stays as it was.
	gboolean secure = deviceInfo->mDeviceRole == SERVER ? TRUE : FALSE;

	g_variant_builder_open(&profileBuilder, G_VARIANT_TYPE("{sv}"));
	g_variant_builder_add (&profileBuilder, "s", "RequireAuthorization");
	g_variant_builder_add (&profileBuilder, "v", g_variant_new_boolean(secure));
	g_variant_builder_close(&profileBuilder);

	g_variant_builder_open(&profileBuilder, G_VARIANT_TYPE("{sv}"));
	g_variant_builder_add (&profileBuilder, "s", "RequireAuthentication");
	g_variant_builder_add (&profileBuilder, "v", g_variant_new_boolean(secure));
	g_variant_builder_close(&profileBuilder);

	g_variant_builder_open(&profileBuilder, G_VARIANT_TYPE("{sv}"));
	g_variant_builder_add (&profileBuilder, "s", "Name");
	g_variant_builder_add (&profileBuilder, "v",
			g_variant_new_string((deviceInfo->mName).c_str()));
	g_variant_builder_close(&profileBuilder);

	g_variant_builder_open(&profileBuilder, G_VARIANT_TYPE("{sv}"));
	g_variant_builder_add (&profileBuilder, "s", "Role");

	if (deviceInfo->mDeviceRole) {
		g_variant_builder_add (&profileBuilder, "v",
				g_variant_new_string("server"));
	} else {
		g_variant_builder_add (&profileBuilder, "v",
				g_variant_new_string("client"));
	}

	g_variant_builder_close(&profileBuilder);

	g_variant_builder_close(&profileBuilder);
	profileVariant = g_variant_builder_end(&profileBuilder);

	GVariant *ret = g_dbus_proxy_call_sync (G_DBUS_PROXY(proxy),
				"RegisterProfile",
				profileVariant,
				G_DBUS_CALL_FLAGS_NONE,
				-1,
				NULL,
				&error);
	if (error)
	{
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to register profileManager due to  %s", error->message);
		g_error_free(error);
		return BLUETOOTH_ERROR_FAIL;
	}

	if (ret) g_variant_unref(ret);
	return BLUETOOTH_ERROR_NONE;
}

void Bluez5ProfileSpp::getProperties(const std::string &address, BluetoothPropertiesResultCallback callback)
{
	UNUSED(address);
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	callback(BLUETOOTH_ERROR_UNSUPPORTED, BluetoothPropertiesList());	// never answered
}

void Bluez5ProfileSpp::getProperty(const std::string &address, BluetoothProperty::Type type,
	                         BluetoothPropertyResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	BluetoothProperty prop(type);
	Bluez5Device *device = mAdapter->findDevice(address);
	if (!device)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID, prop);
		return;
	}
	bool adapterConnectionStatus = device->getConnected();
	prop.setValue<bool>(adapterConnectionStatus);
	callback(BLUETOOTH_ERROR_NONE, prop);
}

void Bluez5ProfileSpp::getChannelState(const std::string &address, const std::string &uuid, BluetoothChannelStateResultCallback callback)
{
	auto deviceIterator = mConnectedDevices.begin();
	std::string lowerCaseAddress = convertAddressToLowerCase(address);

	while (deviceIterator != mConnectedDevices.end())
	{
		if ((deviceIterator->second->mDeviceAddress == lowerCaseAddress) && (deviceIterator->second->mUuid == uuid))
		{
			callback(BLUETOOTH_ERROR_NONE, true);
			return;
		}

		++deviceIterator;
	}

	callback(BLUETOOTH_ERROR_NONE, false);
}

gboolean Bluez5ProfileSpp::onHandleNewConnection (BluezProfile1 *interface,
			GDBusMethodInvocation *invocation,
			const gchar           *device,
			const GVariant        *fd,
			const GVariant        *fd_props,
			gpointer              user_data)
{
	UNUSED(interface);
	SppDeviceInfo* sppDevice = static_cast<SppDeviceInfo*>(user_data);
	return sppDevice->mSppProfile->handleNewConnection(invocation, device, fd, fd_props, sppDevice);
}

gboolean Bluez5ProfileSpp::onHandleRequestDisconnection (BluezProfile1 *interface,
			GDBusMethodInvocation *invocation,
			const gchar           *device,
			gpointer              user_data)
{
	SppDeviceInfo* devieInfo = static_cast<SppDeviceInfo*>(user_data);
	return devieInfo->mSppProfile->handleRequestDisconnection(interface, invocation, device, devieInfo);
}

gboolean Bluez5ProfileSpp::onHandleRelease (BluezProfile1 *interface,
			GDBusMethodInvocation *invocation,
			gpointer              user_data)
{
	UNUSED(interface);
	UNUSED(user_data);

	SppDeviceInfo* devieInfo = static_cast<SppDeviceInfo*>(user_data);
	devieInfo->mSppProfile->handleRelease();

	// finished with method call; no reply sent
	g_dbus_method_invocation_return_value(invocation, NULL);
	return TRUE;
}

gboolean Bluez5ProfileSpp::handleNewConnection (GDBusMethodInvocation *invocation, const gchar *device, const GVariant *fd,
							const GVariant *fd_props, SppDeviceInfo* devieInfo)
{
	GDBusMessage *message;
	GError *error = nullptr;
	GUnixFDList *fd_list;

	UNUSED(fd);
	UNUSED(fd_props);
	message = g_dbus_method_invocation_get_message (invocation);
	fd_list = g_dbus_message_get_unix_fd_list (message);
	if (!fd_list)
	{
		g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InvalidArguments", "No file descriptor");
		return TRUE;
	}

	// A new link on a channel that still has one (a server's second client, or a link whose end was missed):
	// the old fd, channel and watch were overwritten and leaked, and both kept delivering data.
	if (devieInfo->mChannel || devieInfo->mSockfd >= 0)
		closeLink(devieInfo, true);

	devieInfo->mSockfd = g_unix_fd_list_get (fd_list, 0, &error);

	if (error)
	{
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to handle_new_connection due to  %s", error->message);
		g_error_free(error);
		devieInfo->mSockfd = -1;
		g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed", "No file descriptor");
		return TRUE;	// answered; on FALSE the skeleton answers it again
	}

	// finished with method call; no reply sent
	g_dbus_method_invocation_return_value(invocation, NULL);

	Bluez5Device *bluezDevice = mAdapter->findDeviceByObjectPath(device);
	std::string deviceAddress;
	// Lower case, as every other profile reports them: getChannelState(), which lower-cases its query, never
	// matched the upper-case address BlueZ gives.
	if (bluezDevice)
	{
		deviceAddress = convertAddressToLowerCase(bluezDevice->getAddress());
		devieInfo->mDeviceAddress = deviceAddress;
	}

	devieInfo->mAdapterAddress = convertAddressToLowerCase(mAdapter->getAddress());

	getSppObserver()->channelStateChanged(devieInfo->mAdapterAddress, deviceAddress, devieInfo->mUuid, devieInfo->mChannelId, true);

	devieInfo->mChannel = g_io_channel_unix_new (devieInfo->mSockfd);

	g_io_channel_set_encoding(devieInfo->mChannel, NULL, &error);
	if (error)
	{
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to set encoding due to  %s", error->message);
		g_error_free(error);
		error = nullptr;
	}
	// Reads return what is there: a buffered or blocking read waited for a full buffer on the main loop.
	g_io_channel_set_buffered(devieInfo->mChannel, FALSE);
	g_io_channel_set_flags(devieInfo->mChannel, G_IO_FLAG_NONBLOCK, &error);
	if (error)
	{
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to set non-blocking due to  %s", error->message);
		g_error_free(error);
	}

	// Hang-ups and errors too: on G_IO_IN alone a closed link stayed readable and spun the main loop forever.
	devieInfo->mIoWatchId = g_io_add_watch (devieInfo->mChannel, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
	                                        ioCallback, devieInfo);

	DEBUG("devieInfo->mIoWatchId = %d", devieInfo->mIoWatchId);
	return TRUE;
}

gboolean Bluez5ProfileSpp::handleRequestDisconnection (BluezProfile1 *interface, GDBusMethodInvocation *invocation, const gchar *device, SppDeviceInfo* devieInfo)
{
	GError *error = nullptr;

	UNUSED(device);
	UNUSED(interface);

	UNUSED(error);
	getSppObserver()->channelStateChanged(devieInfo->mAdapterAddress, devieInfo->mDeviceAddress, devieInfo->mUuid, devieInfo->mChannelId, false);
	closeLink(devieInfo, false);

	// finished with method call; no reply sent
	g_dbus_method_invocation_return_value(invocation, NULL);

	if ((devieInfo->mDeviceRole == CLIENT) && devieInfo->mInterface)
	{
		std::string objPath = profileObjectPath(devieInfo->mChannelId);
		auto unRegisterCallback = [this, alive = mLifetime.token()](GAsyncResult *result) {
			// The profile goes with its adapter, also while this call is out: nothing of it is touched.
			if (alive.expired())
			{
				return;
			}
			GError *error = nullptr;
			[[maybe_unused]] gboolean ret;

			ret = bluez_profile_manager1_call_unregister_profile_finish(mAdapter->getProfileManager(), result, &error);
			if (error)
			{
				g_error_free(error);
				return;
			}
		};
		bluez_profile_manager1_call_unregister_profile(mAdapter->getProfileManager(), objPath.c_str(), NULL, glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(unRegisterCallback));
		releaseChannel(devieInfo->mChannelId);	// read the entry's id after freeing it
	}

	return TRUE;
}

gboolean Bluez5ProfileSpp::handleRelease()
{
	// Watches stayed with freed channel infos, channels never connected were unreffed while NULL, and the ids
	// were never freed, so SPP had none left after a bluetoothd restart.
	std::vector<BluetoothSppChannelId> channels;
	for (auto &device : mConnectedDevices)
		channels.push_back(device.first);
	for (auto channelId : channels)
	{
		if (SppDeviceInfo *deviceInfo = getSppDevice(channelId))
			closeLink(deviceInfo, true);
		releaseChannel(channelId);
	}
	return TRUE;
}

gboolean Bluez5ProfileSpp::ioCallback(GIOChannel * io, GIOCondition condition, gpointer data)
{
	SppDeviceInfo* sppDevice = static_cast<SppDeviceInfo*>(data);

	return sppDevice->mSppProfile->handleRxData(io, condition, sppDevice);
}

gboolean Bluez5ProfileSpp::handleRxData(GIOChannel *io, GIOCondition condition, SppDeviceInfo *deviceInfo)
{
	char buf[1024];
	bool closed = (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) && !(condition & G_IO_IN);

	while (!closed)
	{
		gsize bytesRead = 0;
		GError *error = nullptr;
		GIOStatus status = g_io_channel_read_chars (io, buf, sizeof(buf), &bytesRead, &error);

		if (bytesRead > 0)
		{
			getSppObserver()->dataReceived(deviceInfo->mChannelId, deviceInfo->mAdapterAddress, (const uint8_t *)buf, bytesRead);
			DEBUG("received %" G_GSIZE_FORMAT " bytes", bytesRead);	// the buffer is binary, and not terminated
		}

		if (error)
		{
			ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to read data due to %s",error->message);
			g_error_free(error);
		}

		if (status == G_IO_STATUS_AGAIN)
			return G_SOURCE_CONTINUE;
		if (status == G_IO_STATUS_EOF || status == G_IO_STATUS_ERROR)
			closed = true;
	}

	// The peer closed the link, which BlueZ does not report with RequestDisconnection: the channel ends here.
	deviceInfo->mIoWatchId = 0;	// this source ends with G_SOURCE_REMOVE
	closeLink(deviceInfo, true);
	return G_SOURCE_REMOVE;
}


void Bluez5ProfileSpp::connectUuid(const std::string &address, const std::string &uuid, BluetoothChannelResultCallback callback)
{
	Bluez5Device *device = mAdapter->findDevice(address);
	if (!device)
	{
		DEBUG("Could not find device with address %s while trying to connect", address.c_str());
		callback(BLUETOOTH_ERROR_NOT_READY, BLUETOOTH_SPP_CHANNEL_ID_INVALID);
		return;
	}

	BluetoothSppChannelId channelId = allocateChannelId();
	if (!channelId)
	{
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to allocate Channel Id");
		callback(BLUETOOTH_ERROR_FAIL, BLUETOOTH_SPP_CHANNEL_ID_INVALID);
		return;
	}

	SppDeviceInfo * sppDevInfo = new (std::nothrow) SppDeviceInfo(this, channelId, CLIENT, "SerialPort", uuid);
	if (!sppDevInfo)
	{
		//Remove ChannelId used
		deallocateChannelId(channelId);
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to allocate memory for sppDevice");
		callback(BLUETOOTH_ERROR_NOMEM, BLUETOOTH_SPP_CHANNEL_ID_INVALID);
		return;
	}

	spDeviceInfo channelInfo(sppDevInfo);
	BluetoothError error = createSkeletonAndExport(uuid, channelInfo);

	if (error != BLUETOOTH_ERROR_NONE)
	{
		callback(BLUETOOTH_ERROR_NOT_READY, BLUETOOTH_SPP_CHANNEL_ID_INVALID);
		return;
	}

	auto ConnectedCallback = [this, alive = mLifetime.token(), callback, channelId](BluetoothError error)
	{
		// Called back after the profile may have gone with its adapter.
		if (alive.expired())
		{
			callback(BLUETOOTH_ERROR_FAIL, BLUETOOTH_SPP_CHANNEL_ID_INVALID);
			return;
		}
		if (error == BLUETOOTH_ERROR_NONE)
		{
			callback(BLUETOOTH_ERROR_NONE, channelId);
		}
		else
		{
			std::string objPath = profileObjectPath(channelId);
			bluez_profile_manager1_call_unregister_profile_sync(mAdapter->getProfileManager(), objPath.c_str(), NULL, NULL);
			// The entry went too: it stayed, with its released skeleton, which later paths released again.
			releaseChannel(channelId);
			callback(BLUETOOTH_ERROR_NOT_READY, BLUETOOTH_SPP_CHANNEL_ID_INVALID);
		}
	};

	this->mConnectedDevices[channelInfo->mChannelId] = std::move(channelInfo);
	device->connect(uuid, ConnectedCallback);
}

void Bluez5ProfileSpp::disconnectUuid(const BluetoothSppChannelId channelId, BluetoothResultCallback callback)
{
	auto deviceIterator = mConnectedDevices.find(channelId);
	if (deviceIterator == mConnectedDevices.end())
	{
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	SppDeviceInfo *sppConnectionInfo = (deviceIterator->second).get();

	if (!sppConnectionInfo)
	{
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	Bluez5Device *device = mAdapter->findDevice(sppConnectionInfo->mDeviceAddress);
	if (!device)
	{
		DEBUG("Could not find device with address %s while trying to connect", sppConnectionInfo->mDeviceAddress.c_str());
		callback(BLUETOOTH_ERROR_NOT_READY);
		return;
	}

	auto disConnectedCallback = [callback](BluetoothError error)
	{
		if (error == BLUETOOTH_ERROR_NONE)
		{
			callback(BLUETOOTH_ERROR_NONE);
		}
		else
		{
			callback(BLUETOOTH_ERROR_NOT_READY);
		}
	};

	device->disconnect(sppConnectionInfo->mUuid, disConnectedCallback);
}

void Bluez5ProfileSpp::writeData(const BluetoothSppChannelId channelId, const uint8_t *data, const uint32_t size, BluetoothResultCallback callback)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	UNUSED(data);
	UNUSED(size);
	auto deviceIterator = mConnectedDevices.find(channelId);
	if (deviceIterator == mConnectedDevices.end())
	{
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	SppDeviceInfo *sppConnectionInfo = (deviceIterator->second).get();
	if (sppConnectionInfo->mSockfd < 0)
	{
		callback(BLUETOOTH_ERROR_NOT_READY);	// written to fd -1 and reported as sent
		return;
	}

	// All of it, or an error: short writes were dropped and failures reported as success.
	uint32_t written = 0;
	while (written < size)
	{
		ssize_t n = write(sppConnectionInfo->mSockfd, data + written, size - written);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			struct pollfd pfd = { sppConnectionInfo->mSockfd, POLLOUT, 0 };
			if (poll(&pfd, 1, 1000) > 0)
				continue;
		}
		if (n <= 0)
		{
			ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to write to channel %d: %s", channelId, strerror(errno));
			callback(BLUETOOTH_ERROR_FAIL);
			return;
		}
		written += (uint32_t)n;
	}

	callback(BLUETOOTH_ERROR_NONE);
}

BluetoothError Bluez5ProfileSpp::createChannel(const std::string &name, const std::string &uuid)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	BluetoothSppChannelId channelId = allocateChannelId();
	if (!channelId)
	{
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to get channelId");
		return BLUETOOTH_ERROR_FAIL;
	}

	SppDeviceInfo * sppDevInfo = new (std::nothrow) SppDeviceInfo(this, channelId, SERVER, name, uuid);

	if (!sppDevInfo)
	{
		deallocateChannelId(channelId);
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to allocate memory for sppDevInfo");
		return BLUETOOTH_ERROR_NOMEM;
	}

	spDeviceInfo channelInfo(sppDevInfo);

	BluetoothError error = createSkeletonAndExport(uuid, channelInfo);

	if (error != BLUETOOTH_ERROR_NONE)
	{
		return error;
	}

	mConnectedDevices[channelId] = std::move(channelInfo);
	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5ProfileSpp::removeChannel(const std::string &uuid)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	SppDeviceInfo *sppConnectionInfo = getSppDevice(uuid);

	if (!sppConnectionInfo)
	{
		return BLUETOOTH_ERROR_FAIL;
	}

	if (sppConnectionInfo->mInterface)
	{
		std::string objPath = profileObjectPath(sppConnectionInfo->mChannelId);
		bluez_profile_manager1_call_unregister_profile_sync(mAdapter->getProfileManager(), objPath.c_str(), NULL, NULL);
		getSppObserver()->channelStateChanged(sppConnectionInfo->mAdapterAddress, sppConnectionInfo->mDeviceAddress, uuid, sppConnectionInfo->mChannelId, false);
		releaseChannel(sppConnectionInfo->mChannelId);	// read the entry's id after freeing it
	}

	return BLUETOOTH_ERROR_NONE;
}

BluetoothSppChannelId Bluez5ProfileSpp::allocateChannelId()
{
	BluetoothSppChannelId retChId = 0;
	for (auto& ids : mChannelIdList)
	{
		if (!ids.second)
		{
			retChId = ids.first;
			ids.second = true;
			break;
		}
	}

	return retChId;
}

void Bluez5ProfileSpp::deallocateChannelId(BluetoothSppChannelId channelId)
{
	 if(mChannelIdList[channelId])
		mChannelIdList[channelId] = false;
}

bool Bluez5ProfileSpp::removeConnectedDevice(BluetoothSppChannelId channelId)
{
	return mConnectedDevices.erase(channelId);
}

Bluez5ProfileSpp::SppDeviceInfo* Bluez5ProfileSpp::getSppDevice(const BluetoothSppChannelId channelId)
{
	SppDeviceInfo* deviceInfo = nullptr;

	auto deviceIterator = mConnectedDevices.find(channelId);
	if (deviceIterator != mConnectedDevices.end())
		deviceInfo = (deviceIterator->second).get();

	return deviceInfo;
}

Bluez5ProfileSpp::SppDeviceInfo* Bluez5ProfileSpp::getSppDevice(const std::string& uuid)
{
	SppDeviceInfo* deviceInfo = nullptr;

	auto matchUuid = [&uuid](std::pair<const unsigned char, std::unique_ptr<Bluez5ProfileSpp::SppDeviceInfo>>& device)
	{
		if((device.second)->mUuid == uuid)
			return true;

		return false;
	};

	ConnectedDevice::iterator deviceIterator = std::find_if(mConnectedDevices.begin(), mConnectedDevices.end(), matchUuid);

	if (deviceIterator != mConnectedDevices.end())
		deviceInfo = (deviceIterator->second).get();

	return deviceInfo;
}

BluetoothError Bluez5ProfileSpp::createSkeletonAndExport(std::string uuid, spDeviceInfo &channelInfo)
{
	GError *error = nullptr;
	UNUSED(uuid);
	std::string objPath = profileObjectPath(channelInfo->mChannelId);

	if (registerProfile(channelInfo, objPath, mAdapter->getProfileManager()))
	{
		deallocateChannelId(channelInfo->mChannelId);
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to register to profile to manager");
		return BLUETOOTH_ERROR_NOT_READY;
	}

	channelInfo->mInterface = bluez_profile1_skeleton_new();

	g_signal_connect(channelInfo->mInterface,
		"handle_new_connection",
		G_CALLBACK (onHandleNewConnection),
		channelInfo.get());

	g_signal_connect(channelInfo->mInterface,
		"handle_request_disconnection",
		G_CALLBACK (onHandleRequestDisconnection),
		channelInfo.get());

	g_signal_connect(channelInfo->mInterface,
		"handle_release",
		G_CALLBACK (onHandleRelease),
		channelInfo.get());


	if (mConn && !g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (channelInfo->mInterface),
						mConn,
						objPath.c_str(),
						&error))
	{
		deallocateChannelId(channelInfo->mChannelId);
		g_object_unref(channelInfo->mInterface);
		channelInfo->mInterface = nullptr;
		ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Failed to export profile on system bus");
		if (error)
		{
			ERROR(MSGID_PROFILE_MANAGER_ERROR, 0, "Error message %s", error->message);
			g_error_free(error);
		}
		return BLUETOOTH_ERROR_NOT_READY;
	}

	return BLUETOOTH_ERROR_NONE;
}
