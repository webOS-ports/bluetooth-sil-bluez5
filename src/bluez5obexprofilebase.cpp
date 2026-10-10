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

#include <memory.h>

#include "bluez5obexprofilebase.h"
#include "bluez5adapter.h"
#include "bluez5obexclient.h"
#include "bluez5obexsession.h"
#include "bluez5obextransfer.h"
#include "asyncutils.h"
#include "logging.h"
#include "utils.h"
#include "obexpaths.h"

using namespace std::placeholders;

Bluez5ObexProfileBase::Bluez5ObexProfileBase(Bluez5ObexSession::Type type, Bluez5Adapter *adapter, const std::string &uuid) :
	Bluez5ProfileBase(adapter, uuid),
	mType(type)
{
}

// Sessions and transfers were never freed with the profile, and their watches and signal handlers, bound to it,
// ran after it was gone (bluetoothd restarting, the adapter removed). They are detached first: a deleted transfer
// reports itself aborted to its watch.
Bluez5ObexProfileBase::~Bluez5ObexProfileBase()
{
	for (auto &transfer : mTransfers)
	{
		transfer.second->watch(nullptr);
		delete transfer.second;
	}
	mTransfers.clear();

	for (auto &session : mSessions)
	{
		detachSession(session.second);
		delete session.second;
	}
	mSessions.clear();
}

// The session's status watch and its properties-changed handler point at this profile.
void Bluez5ObexProfileBase::detachSession(Bluez5ObexSession *session)
{
	session->watch(nullptr);
	if (session->getObjectPropertiesProxy())
		g_signal_handlers_disconnect_by_data(session->getObjectPropertiesProxy(), this);
}

void Bluez5ObexProfileBase::notifySessionStatus(const std::string &address, bool createdOrRemoved)
{
	Bluez5Device *device = mAdapter->findDevice(address);
	BluetoothPropertiesList properties;
	properties.push_back(BluetoothProperty(BluetoothProperty::Type::CONNECTED, createdOrRemoved));
	getObserver()->propertiesChanged(convertAddressToLowerCase(mAdapter->getAddress()), convertAddressToLowerCase(address), properties);

	if (device)
	{
		mAdapter->handleDevicePropertiesChanged(device);
	}
}

// Only this attempt failed: it went through disconnect(), which removed the session that was working.
void Bluez5ObexProfileBase::handleFailedToCreateSession(const std::string &address, BluetoothResultCallback callback)
{
	DEBUG("Failed to connect with device %s on uuid %s profile", address.c_str(), getProfileUuid().c_str());
	callback(BLUETOOTH_ERROR_FAIL);
}

void Bluez5ObexProfileBase::removeFromSessionList(std::string address)
{
	auto sessionIter = mSessions.find(normaliseSessionKey(address));
	if (sessionIter == mSessions.end())
		return;

	Bluez5ObexSession *session = sessionIter->second;
	if (!session)
		return;

	mSessions.erase(sessionIter);
	removeSessionTransfers(session);
	detachSession(session);
	// This will unregister the session with the obex daemon
	delete session;

}

void Bluez5ObexProfileBase::removeSession(const std::string &address)
{
	auto sessionIter = mSessions.find(normaliseSessionKey(address));
	if (sessionIter == mSessions.end())
		return;

	Bluez5ObexSession *session = sessionIter->second;
	if (!session)
		return;

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
	removeSessionTransfers(session);

	notifySessionStatus(address, false);
	detachSession(session);
	// This will unregister the session with the obex daemon
	delete session;

}


void Bluez5ObexProfileBase::updateProperties(GVariant */*changedProperties*/)
{
	DEBUG("Bluez5ObexProfileBase::updateProperties");

	return;
}

void Bluez5ObexProfileBase::handlePropertiesChanged(BluezObexSession1 *proxy, gchar */*interface*/,  GVariant *changedProperties, GVariant */*invalidatedProperties*/, gpointer userData)
{
	DEBUG("handlePropertiesChanged Bluez5ObexProfileBase");

	Bluez5ObexProfileBase *profileBase = static_cast<Bluez5ObexProfileBase*>(userData);

	// The emitter is the session's properties proxy (connected in createSession).
	profileBase->updateSessionProperties(G_OBJECT(proxy), changedProperties);
}

const Bluez5ObexSession* Bluez5ObexProfileBase::findSessionByPropertiesProxy(GObject *propertiesProxy) const
{
	for (auto &session : mSessions)
		if (G_OBJECT(session.second->getObjectPropertiesProxy()) == propertiesProxy)
			return session.second;
	return nullptr;
}

// One session per key: a second connect made a second obexd session, which leaked, and whose end removed the
// stored one. A connect while one is being made is busy.
void Bluez5ObexProfileBase::createSession(const std::string &address, Bluez5ObexSession::Type type, BluetoothResultCallback callback)
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
		if (!storeSession(key, session))	// one came in meanwhile (an incoming push)
		{
			delete session;
			callback(BLUETOOTH_ERROR_NONE);
			return;
		}

		session->watch(std::bind(&Bluez5ObexProfileBase::handleObexSessionStatus, this, key, _1));
		DEBUG("createSession g_signal_connect");
		g_signal_connect(G_OBJECT(session->getObjectPropertiesProxy()), "properties-changed", G_CALLBACK(handlePropertiesChanged), this);

		notifySessionStatus(key, true);

		callback(BLUETOOTH_ERROR_NONE);
	};

	mPendingSessions.insert(key);
	obexClient->createSession(type, key, sessionCreateCallback);
}

void Bluez5ObexProfileBase::handleObexSessionStatus(const std::string &address, bool lost)
{
	if (!lost)
		return;

	DEBUG("Session lost for address %s", address.c_str());

	// At this point we lost the session due to some internal bluez error
	// and need power down everything.
	removeSession(address);
}

// False, storing nothing, when the key has a session already: insert() did nothing, and the new session leaked.
bool Bluez5ObexProfileBase::storeSession(const std::string &address, Bluez5ObexSession *session)
{
	return mSessions.insert(std::pair<std::string, Bluez5ObexSession*>(normaliseSessionKey(address), session)).second;
}

void Bluez5ObexProfileBase::connect(const std::string &address, BluetoothResultCallback callback)
{
	DEBUG("Connecting with device %s on uuid %s profile", address.c_str(), getProfileUuid().c_str());

	createSession(address, mType, callback);
}

void Bluez5ObexProfileBase::disconnect(const std::string& address, BluetoothResultCallback callback)
{
	DEBUG("Disconnecting from device %s on uuid %s profile", address.c_str(), getProfileUuid().c_str());

	callback(BLUETOOTH_ERROR_NONE);

	removeSession(address);
}

void Bluez5ObexProfileBase::getProperties(const std::string &/*address*/, BluetoothPropertiesResultCallback callback)
{
	BluetoothPropertiesList properties;

	callback(BLUETOOTH_ERROR_UNHANDLED, properties);
}

void Bluez5ObexProfileBase::getProperty(const std::string &address, BluetoothProperty::Type type, BluetoothPropertyResultCallback callback)
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

const Bluez5ObexSession* Bluez5ObexProfileBase::findSession(const std::string &address) const
{
	auto sessionIter = mSessions.find(normaliseSessionKey(address));
	if (sessionIter == mSessions.end())
		return nullptr;

	return sessionIter->second;
}

void Bluez5ObexProfileBase::startTransfer(BluetoothFtpTransferId id, const std::string &objectPath, BluetoothOppTransferResultCallback callback, Bluez5ObexTransfer::TransferType type)
{
	// NOTE: ownership of the transfer object is passed to updateActiveTransfer which
	// will delete it once there is nothing to left to do with it
	Bluez5ObexTransfer *transfer = new Bluez5ObexTransfer(std::string(objectPath), type);
	mTransfers.insert(std::pair<BluetoothFtpTransferId, Bluez5ObexTransfer*>(id, transfer));
	transfer->watch(std::bind(&Bluez5ObexProfileBase::updateActiveTransfer, this, id, transfer, callback));
}

void Bluez5ObexProfileBase::updateActiveTransfer(BluetoothFtpTransferId id, Bluez5ObexTransfer *transfer, BluetoothOppTransferResultCallback callback)
{
	bool cleanup = false;

	if (transfer->getState() == Bluez5ObexTransfer::State::ACTIVE)
	{
		callback(BLUETOOTH_ERROR_NONE, transfer->getBytesTransferred(), transfer->getFileSize(), false);
	}
	// Final whatever the byte count: a zero-byte file, or a small one done before any progress, never completed.
	else if (transfer->getState() == Bluez5ObexTransfer::State::COMPLETE)
	{
		callback(BLUETOOTH_ERROR_NONE, transfer->getBytesTransferred(), transfer->getFileSize(), true);
		cleanup = true;
	}
	else if (transfer->getState() == Bluez5ObexTransfer::State::ERROR)
	{
		DEBUG("File transfer failed");
		callback(BLUETOOTH_ERROR_FAIL, transfer->getBytesTransferred(), transfer->getFileSize(), false);
		cleanup = true;
	}

	if (cleanup)
		removeTransfer(id);
}

void Bluez5ObexProfileBase::removeTransfer(BluetoothFtpTransferId id)
{
	auto transferIter = mTransfers.find(id);
	if (transferIter == mTransfers.end())
		return;

	Bluez5ObexTransfer *transfer = transferIter->second;

	mTransfers.erase(transferIter);

	delete transfer;
}

Bluez5ObexTransfer* Bluez5ObexProfileBase::findTransfer(BluetoothFtpTransferId id)
{
	auto transferIter = mTransfers.find(id);
	if (transferIter == mTransfers.end())
		return 0;

	Bluez5ObexTransfer *transfer = transferIter->second;

	return transfer;
}
