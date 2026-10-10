// Copyright (c) 2014-2019 LG Electronics, Inc.
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

#include <memory.h>

#include "bluez5profileftp.h"
#include "obextime.h"
#include "bluez5adapter.h"
#include "bluez5obexclient.h"
#include "bluez5obexsession.h"
#include "bluez5obextransfer.h"
#include "asyncutils.h"
#include "logging.h"
#include "obexpaths.h"

using namespace std::placeholders;

const std::string BLUETOOTH_PROFILE_FTP_UUID = "00001106-0000-1000-8000-00805f9b34fb";

Bluez5ProfileFtp::Bluez5ProfileFtp(Bluez5Adapter *adapter) :
	Bluez5ProfileBase(adapter, BLUETOOTH_PROFILE_FTP_UUID),
	mTranfserIdCounter(0)
{
}

// Its sessions and transfers were never freed, and their watches, bound to this profile, ran after it was gone.
Bluez5ProfileFtp::~Bluez5ProfileFtp()
{
	for (auto &transfer : mTransfers)
	{
		transfer.second->watch(nullptr);
		delete transfer.second;
	}
	mTransfers.clear();

	for (auto &session : mSessions)
	{
		session.second->watch(nullptr);
		delete session.second;
	}
	mSessions.clear();
}

void Bluez5ProfileFtp::getProperties(const std::string &/*address*/, BluetoothPropertiesResultCallback callback)
{
	BluetoothPropertiesList properties;

	callback(BLUETOOTH_ERROR_UNHANDLED, properties);
}

void Bluez5ProfileFtp::getProperty(const std::string &address, BluetoothProperty::Type type, BluetoothPropertyResultCallback callback)
{
	bool sessionExists = false;
	BluetoothProperty prop(type);

	auto sessionIter = mSessions.find(normaliseSessionKey(address));

	switch (type)
	{
	case BluetoothProperty::Type::CONNECTED:
		sessionExists = (sessionIter != mSessions.end());
		prop.setValue<bool>(sessionExists);
		break;
	default:
		callback(BLUETOOTH_ERROR_PARAM_INVALID, prop);
		return;
	}

	callback(BLUETOOTH_ERROR_NONE, prop);
}

void Bluez5ProfileFtp::notifySessionStatus(const std::string &address, bool createdOrRemoved)
{
	if (!observer)
		return;

	BluetoothPropertiesList properties;
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::CONNECTED, createdOrRemoved));

	observer->propertiesChanged(address, properties);
}

// Only this attempt failed: it went through disconnect(), which removed the session that was working.
void Bluez5ProfileFtp::handleFailedToCreateSession(const std::string &/*address*/, BluetoothResultCallback callback)
{
	callback(BLUETOOTH_ERROR_FAIL);
}

// False, storing nothing, when the address has a session already: insert() did nothing, and the new one leaked.
bool Bluez5ProfileFtp::storeSession(const std::string &address, Bluez5ObexSession *session)
{
	return mSessions.insert(std::pair<std::string, Bluez5ObexSession*>(normaliseSessionKey(address), session)).second;
}

void Bluez5ProfileFtp::handleObexSessionStatus(std::string address, bool lost)
{
	if (!lost)
		return;

	DEBUG("Session lost");

	// At this point we lost the session due to some internal bluez error
	// and need power down everything.
	removeSession(address);
}

// One session per address: a second connect made a second obexd session, which leaked, and whose end removed the
// stored one. A connect while one is being made is busy.
void Bluez5ProfileFtp::createSession(const std::string &address, BluetoothResultCallback callback)
{
	std::string key = normaliseSessionKey(address);
	if (findSession(key))
	{
		callback(BLUETOOTH_ERROR_NONE);
		return;
	}
	if (mPendingSessions.count(key))
	{
		callback(BLUETOOTH_ERROR_BUSY);
		return;
	}

	Bluez5ObexClient *obexClient = mAdapter->getObexClient();

	if (!obexClient)
	{
		handleFailedToCreateSession(key, callback);
		return;
	}

	auto sessionCreateCallback = [this, alive = mLifetime.token(), key, callback](Bluez5ObexSession *session) {
		// Called back after the profile may have gone with its adapter.
		if (alive.expired())
		{
			delete session;
			callback(BLUETOOTH_ERROR_FAIL);
			return;
		}
		mPendingSessions.erase(key);
		if (!session)
		{
			handleFailedToCreateSession(key, callback);
			return;
		}
		if (!storeSession(key, session))
		{
			delete session;
			callback(BLUETOOTH_ERROR_NONE);
			return;
		}

		session->watch(std::bind(&Bluez5ProfileFtp::handleObexSessionStatus, this, key, _1));
		notifySessionStatus(key, true);

		callback(BLUETOOTH_ERROR_NONE);
	};

	mPendingSessions.insert(key);
	obexClient->createSession(Bluez5ObexSession::Type::FTP, key, sessionCreateCallback);
}

void Bluez5ProfileFtp::removeSession(const std::string &address)
{
	auto sessionIter = mSessions.find(normaliseSessionKey(address));
	if (sessionIter == mSessions.end())
		return;

	Bluez5ObexSession *session = sessionIter->second;

	for (auto transferIter = mTransfers.begin(); transferIter != mTransfers.end(); )
	{
		Bluez5ObexTransfer *transfer = transferIter->second;

		if (transfer->isPartOfSession(session))
		{
			// We don't have to cancel the transfer here. If the session goes down
			// the transfer will too. This will also trigger a possible watcher
			// of the transfer so he gets notified about the aborted transfer.
			mTransfers.erase(transferIter++);
			delete transfer;

			continue;
		}

		transferIter++;
	}

	mSessions.erase(sessionIter);

	// This will unregister the session with the obex daemon
	delete session;

	notifySessionStatus(address, false);
}

void Bluez5ProfileFtp::removeTransfer(BluetoothFtpTransferId id)
{
	auto transferIter = mTransfers.find(id);
	if (transferIter == mTransfers.end())
		return;

	Bluez5ObexTransfer *transfer = transferIter->second;

	mTransfers.erase(transferIter);

	delete transfer;
}

Bluez5ObexSession* Bluez5ProfileFtp::findSession(const std::string &address)
{
	auto sessionIter = mSessions.find(normaliseSessionKey(address));
	if (sessionIter == mSessions.end())
		return 0;

	Bluez5ObexSession *session = sessionIter->second;

	return session;
}

Bluez5ObexTransfer* Bluez5ProfileFtp::findTransfer(BluetoothFtpTransferId id)
{
	auto transferIter = mTransfers.find(id);
	if (transferIter == mTransfers.end())
		return 0;

	Bluez5ObexTransfer *transfer = transferIter->second;

	return transfer;
}

void Bluez5ProfileFtp::connect(const std::string &address, BluetoothResultCallback callback)
{
	DEBUG("Connecting with device %s on FTP profile", address.c_str());

	auto connectCallback = [this, alive = mLifetime.token(), address, callback](BluetoothError error) {
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

		createSession(address, callback);
	};

	Bluez5ProfileBase::connect(address, connectCallback);
}

void Bluez5ProfileFtp::disconnect(const std::string &address, BluetoothResultCallback callback)
{
	DEBUG("Disconnecting from device %s on FTP profile", address.c_str());

	callback(BLUETOOTH_ERROR_NONE);

	removeSession(address);
}

static uint8_t decodePermissionString(const std::string &str)
{
	uint8_t permission = BluetoothFtpElement::Permission::NONE;

	if (str.find("R") != std::string::npos)
		permission |= BluetoothFtpElement::Permission::READ;
	if (str.find("W") != std::string::npos)
		permission |= BluetoothFtpElement::Permission::WRITE;
	if (str.find("D") != std::string::npos)
		permission |= BluetoothFtpElement::Permission::DELETE;

	return permission;
}


std::vector<BluetoothFtpElement> Bluez5ProfileFtp::buildElementList(GVariant *entries)
{
	std::vector<BluetoothFtpElement> elements;

	for (size_t n = 0; n < g_variant_n_children(entries); n++)
	{
		GVariant *entry = g_variant_get_child_value(entries, n);
		BluetoothFtpElement currentElement;

		for (size_t m = 0; m < g_variant_n_children(entry); m++)
		{
			GVariant *propertyVar = g_variant_get_child_value(entry, m);

			GVariant *keyVar = g_variant_get_child_value(propertyVar, 0);
			GVariant *valueVar = g_variant_get_child_value(propertyVar, 1);
			GVariant *value = g_variant_get_variant(valueVar);

			std::string key = g_variant_get_string(keyVar, NULL);
			// Types are checked: a value of another type gave NULL, and std::string(NULL) throws.
			bool isString = g_variant_is_of_type(value, G_VARIANT_TYPE_STRING);
			std::string text = isString ? g_variant_get_string(value, NULL) : "";

			if (key == "Name" && isString)
			{
				currentElement.setName(text);
			}
			else if (key == "Type" && isString)
			{
				if (text == "folder")
					currentElement.setType(BluetoothFtpElement::Type::FOLDER);
				else if (text == "file")
					currentElement.setType(BluetoothFtpElement::Type::FILE);
			}
			else if (key == "Size" && g_variant_is_of_type(value, G_VARIANT_TYPE_UINT64))
			{
				currentElement.setSize(g_variant_get_uint64(value));
			}
			else if (key == "User-perm" && isString)
			{
				currentElement.setUserPermission(decodePermissionString(text));
			}
			else if (key == "Group-perm" && isString)
			{
				currentElement.setGroupPermission(decodePermissionString(text));
			}
			else if (key == "Other-perm" && isString)
			{
				currentElement.setOtherPermission(decodePermissionString(text));
			}
			else if (key == "Modified" && isString)
			{
				currentElement.setModifiedTime(decodeObexTime(text));
			}
			else if (key == "Accessed" && isString)
			{
				currentElement.setAccessedTime(decodeObexTime(text));
			}
			else if (key == "Created" && isString)
			{
				currentElement.setCreatedTime(decodeObexTime(text));
			}

			g_variant_unref(value);
			g_variant_unref(valueVar);
			g_variant_unref(keyVar);
			g_variant_unref(propertyVar);
		}

		elements.push_back(currentElement);

		g_variant_unref(entry);
	}

	return elements;
}

void Bluez5ProfileFtp::listFolder(const std::string &address, const std::string &path, BluetoothFtpListFolderResultCallback callback)
{
	Bluez5ObexSession *session = findSession(address);
	if (!session)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID, std::vector<BluetoothFtpElement>());
		return;
	}

	// Checked, as pullFile and pushFile do: with NULL the call returned at once and the callback never came.
	BluezObexFileTransfer1 *fileTransferProxy = session->getFileTransferProxy();
	if (!fileTransferProxy)
	{
		callback(BLUETOOTH_ERROR_FAIL, std::vector<BluetoothFtpElement>());
		return;
	}

	// Each reply is finished on the proxy it was made on: the session's, captured here before, is freed when the
	// session goes. The listing was leaked on every call.
	auto changeFolderCallback = [this, alive = mLifetime.token(), callback](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		gboolean ret;

		ret = bluez_obex_file_transfer1_call_change_folder_finish(BLUEZ_OBEX_FILE_TRANSFER1(source), result, &error);
		// The profile goes with its adapter, also while this call is out: nothing of it is touched.
		if (alive.expired() || !ret || error)
		{
			g_clear_error(&error);
			callback(BLUETOOTH_ERROR_FAIL, std::vector<BluetoothFtpElement>());
			return;
		}

		auto listFolderCallback = [this, alive = mLifetime.token(), callback](GObject *source, GAsyncResult *result) {
			GError *error = 0;
			gboolean ret;
			GVariant *entries = 0;

			ret = bluez_obex_file_transfer1_call_list_folder_finish(BLUEZ_OBEX_FILE_TRANSFER1(source), &entries, result, &error);
			if (alive.expired() || !ret || error || !entries)
			{
				g_clear_error(&error);
				if (entries)
					g_variant_unref(entries);
				callback(BLUETOOTH_ERROR_FAIL, std::vector<BluetoothFtpElement>());
				return;
			}

			auto elements = buildElementList(entries);
			g_variant_unref(entries);
			callback(BLUETOOTH_ERROR_NONE, elements);
		};

		bluez_obex_file_transfer1_call_list_folder(BLUEZ_OBEX_FILE_TRANSFER1(source), NULL, glibAsyncMethodWrapper,
												  new GlibAsyncFunctionWrapper(listFolderCallback));
	};

	// First we have to change to the folder we want to list, then we can
	// ask for it's content
	bluez_obex_file_transfer1_call_change_folder(fileTransferProxy, path.c_str(),
												NULL, glibAsyncMethodWrapper,
												new GlibAsyncFunctionWrapper(changeFolderCallback));
}

void Bluez5ProfileFtp::updateActiveTransfer(BluetoothFtpTransferId id, Bluez5ObexTransfer *transfer, BluetoothFtpTransferResultCallback callback)
{
	bool cleanup = false;

	if (transfer->getState() == Bluez5ObexTransfer::State::ACTIVE)
	{
		callback(BLUETOOTH_ERROR_NONE, transfer->getBytesTransferred(), false);
	}
	else if (transfer->getState() == Bluez5ObexTransfer::State::COMPLETE)
	{
		callback(BLUETOOTH_ERROR_NONE, transfer->getBytesTransferred(), true);
		cleanup = true;
	}
	else if (transfer->getState() == Bluez5ObexTransfer::State::ERROR)
	{
		DEBUG("File transfer failed");
		callback(BLUETOOTH_ERROR_FAIL, transfer->getBytesTransferred(), false);
		cleanup = true;
	}

	if (cleanup)
		removeTransfer(id);
}

void Bluez5ProfileFtp::startTransfer(BluetoothFtpTransferId id, const std::string &objectPath, const ObexPeer &peer, BluetoothFtpTransferResultCallback callback)
{
	// NOTE: ownership of the transfer object is passed to updateActiveTransfer which
	// will delete it once there is nothing to left to do with it
	Bluez5ObexTransfer *transfer = new Bluez5ObexTransfer(std::string(objectPath), peer);
	mTransfers.insert(std::pair<BluetoothFtpTransferId, Bluez5ObexTransfer*>(id, transfer));
	transfer->watch(std::bind(&Bluez5ProfileFtp::updateActiveTransfer, this, id, transfer, callback));
}

BluetoothFtpTransferId Bluez5ProfileFtp::pullFile(const std::string &address, const std::string &sourcePath, const std::string &targetPath, BluetoothFtpTransferResultCallback callback)
{
	Bluez5ObexSession *session = findSession(address);
	if (!session || sourcePath.length() == 0 || targetPath.length() == 0)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID, 0, false);
		return BLUETOOTH_FTP_TRANSFER_ID_INVALID;
	}

	BluezObexFileTransfer1 *fileTransferProxy = session->getFileTransferProxy();
	if (!fileTransferProxy)
	{
		callback(BLUETOOTH_ERROR_FAIL, 0, false);
		return BLUETOOTH_FTP_TRANSFER_ID_INVALID;
	}

	BluetoothFtpTransferId transferId = nextTransferId();
        gchar *dirName = g_path_get_dirname(sourcePath.c_str());
	gchar *fileName = g_path_get_basename(sourcePath.c_str());

	std::string basePath(dirName);
	std::string sourceFileName(fileName);

	g_free(dirName);
	g_free(fileName);

	// Each reply is finished on the proxy it was made on (the session's is freed when the session goes); the
	// transfer's path was leaked.
	auto changeFolderCallback = [this, alive = mLifetime.token(), sourceFileName, targetPath, transferId, callback](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		gboolean ret;

		ret = bluez_obex_file_transfer1_call_change_folder_finish(BLUEZ_OBEX_FILE_TRANSFER1(source), result, &error);
		// The profile goes with its adapter, also while this call is out: nothing of it is touched.
		if (alive.expired() || !ret || error)
		{
			g_clear_error(&error);
			callback(BLUETOOTH_ERROR_FAIL, 0, false);
			return;
		}

		auto getFileCallback = [this, alive = mLifetime.token(), transferId, callback](GObject *source, GAsyncResult *result) {
			GError *error = 0;
			gboolean ret;
			gchar *objectPath = 0;

			ret = bluez_obex_file_transfer1_call_get_file_finish(BLUEZ_OBEX_FILE_TRANSFER1(source), &objectPath, NULL, result, &error);
			if (alive.expired() || !ret || error || !objectPath)
			{
				g_clear_error(&error);
				g_free(objectPath);
				callback(BLUETOOTH_ERROR_FAIL, 0, false);
				return;
			}

			startTransfer(transferId, std::string(objectPath), obexPeerOfProxy(source), callback);
			g_free(objectPath);
		};

		bluez_obex_file_transfer1_call_get_file(BLUEZ_OBEX_FILE_TRANSFER1(source), targetPath.c_str(), sourceFileName.c_str(),
											   NULL, glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(getFileCallback));
	};

	// First we have to change to the folder we want to get the file from and
	// then we can ask to retrieve the file we want
	bluez_obex_file_transfer1_call_change_folder(fileTransferProxy, basePath.c_str(),
												NULL, glibAsyncMethodWrapper,
												new GlibAsyncFunctionWrapper(changeFolderCallback));

	return transferId;
}

BluetoothFtpTransferId Bluez5ProfileFtp::pushFile(const std::string &address, const std::string &sourcePath, const std::string &targetPath, BluetoothFtpTransferResultCallback callback)
{
	Bluez5ObexSession *session = findSession(address);
	if (!session || sourcePath.length() == 0 || targetPath.length() == 0)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID, 0, false);
		return BLUETOOTH_FTP_TRANSFER_ID_INVALID;
	}

	BluezObexFileTransfer1 *fileTransferProxy = session->getFileTransferProxy();
	if (!fileTransferProxy)
	{
		callback(BLUETOOTH_ERROR_FAIL, 0, false);
		return BLUETOOTH_FTP_TRANSFER_ID_INVALID;
	}

	BluetoothFtpTransferId transferId = nextTransferId();

	gchar *dirName = g_path_get_dirname(targetPath.c_str());
	gchar *fileName = g_path_get_basename(targetPath.c_str());

	std::string basePath(dirName);
	std::string targetFileName(fileName);

	g_free(dirName);
	g_free(fileName);

	// Each reply is finished on the proxy it was made on (the session's is freed when the session goes); the
	// transfer's path was leaked.
	auto changeFolderCallback = [this, alive = mLifetime.token(), sourcePath, targetFileName, transferId, callback](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		gboolean ret;

		ret = bluez_obex_file_transfer1_call_change_folder_finish(BLUEZ_OBEX_FILE_TRANSFER1(source), result, &error);
		// The profile goes with its adapter, also while this call is out: nothing of it is touched.
		if (alive.expired() || !ret || error)
		{
			g_clear_error(&error);
			callback(BLUETOOTH_ERROR_FAIL, 0, false);
			return;
		}

		auto putFileCallback = [this, alive = mLifetime.token(), transferId, callback](GObject *source, GAsyncResult *result) {
			GError *error = 0;
			gboolean ret;
			gchar *objectPath = 0;

			ret = bluez_obex_file_transfer1_call_put_file_finish(BLUEZ_OBEX_FILE_TRANSFER1(source), &objectPath, NULL, result, &error);
			if (alive.expired() || !ret || error || !objectPath)
			{
				g_clear_error(&error);
				g_free(objectPath);
				callback(BLUETOOTH_ERROR_FAIL, 0, false);
				return;
			}

			startTransfer(transferId, std::string(objectPath), obexPeerOfProxy(source), callback);
			g_free(objectPath);
		};

		bluez_obex_file_transfer1_call_put_file(BLUEZ_OBEX_FILE_TRANSFER1(source), sourcePath.c_str(), targetFileName.c_str(),
											   NULL, glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(putFileCallback));
	};

	// First we have to change to the folder we want to put the file into and
	// then we can ask to send the file
	bluez_obex_file_transfer1_call_change_folder(fileTransferProxy, basePath.c_str(),
												NULL, glibAsyncMethodWrapper,
												new GlibAsyncFunctionWrapper(changeFolderCallback));

	return transferId;
}

void Bluez5ProfileFtp::cancelTransfer(BluetoothFtpTransferId id, BluetoothResultCallback callback)
{
	Bluez5ObexTransfer *transfer = findTransfer(id);
	if (!transfer)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID);
		return;
	}

	auto cancelCallback = [this, alive = mLifetime.token(), id, callback](BluetoothError error) {
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

		removeTransfer(id);

		callback(BLUETOOTH_ERROR_NONE);
	};

	transfer->cancel(cancelCallback);
}
