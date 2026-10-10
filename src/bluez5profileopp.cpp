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

#include "bluez5profileopp.h"
#include "bluez5adapter.h"
#include "logging.h"
#include "asyncutils.h"
#include "bluez5obextransfer.h"
#include "bluez5obexsession.h"
#include "bluez5obexclient.h"
#include "bluez5busconfig.h"
#include "utils.h"
#include "obexpaths.h"

#include <sys/stat.h>

using namespace std::placeholders;

const std::string BLUETOOTH_PROFILE_OPP_UUID = "00001105-0000-1000-8000-00805f9b34fb";

// obexd's own namespace: it takes only org.bluez.obex.Error.Rejected as a refusal (obexd/src/manager.c). Any other
// error, org.bluez.Error.Rejected included, accepted the push under the sender's file name.
#define BLUEZ5_OBEX_AGENT_ERROR_CANCELED        "org.bluez.obex.Error.Canceled"
#define BLUEZ5_OBEX_AGENT_ERROR_REJECTED        "org.bluez.obex.Error.Rejected"
#define BLUEZ5_OBEX_AGENT_ERROR_NOT_IMPLEMENTED "org.bluez.obex.Error.NotImplemented"

// Below obexd's own 60 s agent timeout (obexd/src/manager.c).
const guint CONFIRMATION_TIMEOUT_SECONDS = 50;

// obexd's root folder, where it stores pushed files (obex.service: obexd -r /media/internal). The reply names a file
// in it that does not exist yet: obexd opens the name with O_TRUNC, replacing a file of the same name.
const std::string OPP_RECEIVE_FOLDER = "/media/internal";

Bluez5ProfileOpp::Bluez5ProfileOpp(Bluez5Adapter *adapter) :
	Bluez5ObexProfileBase(Bluez5ObexSession::Type::OPP, adapter, BLUETOOTH_PROFILE_OPP_UUID),
	mTranfserIdCounter(0),
	mInterface(0),
	mInvocation(nullptr),
	mPendingTransferId(BLUETOOTH_OPP_TRANSFER_ID_INVALID),
	mConfirmationTimeout(0)
{
}

Bluez5ProfileOpp::~Bluez5ProfileOpp()
{
	cancelPendingConfirmation();
}

void Bluez5ProfileOpp::cancelPendingConfirmation()
{
	if (mConfirmationTimeout)
		g_source_remove(mConfirmationTimeout);
	mConfirmationTimeout = 0;
	if (mInvocation)
		g_dbus_method_invocation_return_dbus_error(mInvocation, BLUEZ5_OBEX_AGENT_ERROR_REJECTED, "User rejected confirmation");
	mInvocation = nullptr;
	mPendingTransferId = BLUETOOTH_OPP_TRANSFER_ID_INVALID;
}

BluetoothOppTransferId Bluez5ProfileOpp::pushFile(const std::string &address,
								const std::string &sourcePath,
								BluetoothOppTransferResultCallback callback)
{
	const Bluez5ObexSession *session = findSession(address);
	if (!session || sourcePath.length() == 0)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID, 0, 0, false);
		return BLUETOOTH_OPP_TRANSFER_ID_INVALID;
	}

	BluezObexObjectPush1 *objectPushProxy = session->getObjectPushProxy();
	if (!objectPushProxy)
	{
		callback(BLUETOOTH_ERROR_FAIL, 0, 0, false);
		return BLUETOOTH_OPP_TRANSFER_ID_INVALID;
	}

	BluetoothOppTransferId transferId = nextTransferId();

	// Finished on the proxy the call was made on: the session's, borrowed here, is freed when the session goes.
	auto pushCallback = [this, alive = mLifetime.token(), transferId, callback](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		gboolean ret;
		char *objectPath = 0;

		ret = bluez_obex_object_push1_call_send_file_finish(BLUEZ_OBEX_OBJECT_PUSH1(source), &objectPath, NULL, result, &error);
		// The profile goes with its adapter, also while this call is out: nothing of it is touched.
		if (alive.expired() || !ret || error || !objectPath)
		{
			g_clear_error(&error);
			g_free(objectPath);
			callback(BLUETOOTH_ERROR_FAIL, 0, 0, false);
			return;
		}

		startTransfer(transferId, std::string(objectPath), callback, Bluez5ObexTransfer::TransferType::SENDING);
		g_free(objectPath);	// leaked on every push
	};

	bluez_obex_object_push1_call_send_file(objectPushProxy, sourcePath.c_str(),
												NULL, glibAsyncMethodWrapper,
												new GlibAsyncFunctionWrapper(pushCallback));

	return transferId;
}

void Bluez5ProfileOpp::cancelTransfer(BluetoothOppTransferId id, BluetoothResultCallback callback)
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

// Everything is checked before the request is kept, and a request that is not kept is answered: obexd waits up to
// 60 s for an answer and refuses every other push meanwhile.
void Bluez5ProfileOpp::agentTransferConfirmationRequested(BluezObexAgent1 *interface, GDBusMethodInvocation *invocation, const gchar *arg_path)
{
	auto reject = [invocation]() {
		g_dbus_method_invocation_return_dbus_error(invocation, BLUEZ5_OBEX_AGENT_ERROR_REJECTED, "User rejected confirmation");
	};

	// obexd asks about one push at a time; one still pending was given up (its Cancel may not have reached us).
	cancelPendingConfirmation();

	GError *error = 0;
	GObjectRef<BluezObexTransfer1> transferProxy(bluez_obex_transfer1_proxy_new_for_bus_sync(BLUEZ5_OBEX_DBUS_BUS_TYPE,
	                                             G_DBUS_PROXY_FLAGS_NONE, "org.bluez.obex", arg_path, NULL, &error));
	g_clear_error(&error);
	if (!transferProxy)
	{
		reject();
		return;
	}

	// A PUT without a Name header has no name: rejected. Removing the session, as was tried, cannot work on a
	// server session.
	const gchar *sessionPath = bluez_obex_transfer1_get_session(transferProxy.get());
	const char *fileName = bluez_obex_transfer1_get_name(transferProxy.get());
	if (!sessionPath || !fileName)
	{
		reject();
		return;
	}
	guint64 size = bluez_obex_transfer1_get_size(transferProxy.get());

	GObjectRef<BluezObexSession1> sessionProxy(bluez_obex_session1_proxy_new_for_bus_sync(BLUEZ5_OBEX_DBUS_BUS_TYPE,
	                                           G_DBUS_PROXY_FLAGS_NONE, "org.bluez.obex", sessionPath, NULL, &error));
	g_clear_error(&error);
	const gchar *destination = sessionProxy ? bluez_obex_session1_get_destination(sessionProxy.get()) : nullptr;
	const gchar *source = sessionProxy ? bluez_obex_session1_get_source(sessionProxy.get()) : nullptr;
	auto device = destination && source ? mAdapter->findDevice(destination) : nullptr;
	if (!device || !getOppObserver())
	{
		reject();
		return;
	}

	// Stored under a free name; names starting with '.' are refused (obexd itself refuses '/', "." and "..").
	std::string storedName = uniqueReceivedName(fileName, [](const std::string &name) {
		struct stat st;
		return lstat((OPP_RECEIVE_FOLDER + "/" + name).c_str(), &st) == 0;
	});
	if (storedName.empty())
	{
		reject();
		return;
	}

	// Lower-case, as the service passes addresses and as outgoing sessions are keyed: obexd's upper-case
	// Destination made a second session for the same device, and getProperty(CONNECTED) missed it.
	std::string deviceAddress = convertAddressToLowerCase(destination);
	BluetoothOppTransferId transferId = nextTransferId();
	mInterface = interface;
	mInvocation = invocation;
	mPendingTransferId = transferId;
	mTransferObjPath = arg_path;
	mFileName = fileName;
	mStoredName = storedName;

	// Unanswered, obexd gives up after 60 s and then takes the push anyway, under the sender's name: refused here
	// first. The observer's request stays; answering it later fails.
	mConfirmationTimeout = g_timeout_add_seconds(CONFIRMATION_TIMEOUT_SECONDS, [](gpointer data) -> gboolean {
		auto opp = static_cast<Bluez5ProfileOpp*>(data);
		opp->mConfirmationTimeout = 0;
		opp->cancelPendingConfirmation();
		return G_SOURCE_REMOVE;
	}, this);

	// Another push from a device with a session already uses that one: a second session was stored over it and
	// leaked, and its end removed the first.
	if (!findSession(deviceAddress))
	{
		Bluez5ObexSession *session = new Bluez5ObexSession(mAdapter->getObexClient(), Bluez5ObexSession::Type::OPP, std::string(sessionPath), deviceAddress);
		if (session->isValid())
		{
			session->watch(std::bind(&Bluez5ObexProfileBase::handleObexSessionStatus, this, deviceAddress, _1));
			storeSession(deviceAddress, session);
			notifySessionStatus(deviceAddress, true);
		}
		else
			delete session;	// the push goes on; only its session is not tracked
	}
	mTransfersMap[transferId] = 0;
	std::string lowerCaseAddress = convertAddressToLowerCase(source);
	getOppObserver()->transferConfirmationRequested(transferId, lowerCaseAddress, deviceAddress, device->getName(), mFileName, size);
}

void Bluez5ProfileOpp::supplyTransferConfirmation(BluetoothOppTransferId transferId, bool accept, BluetoothResultCallback callback)
{
	// Only the push still waiting for an answer: not one obexd has given up on, nor another id. An accept for a
	// dead transfer started a transfer that never completed, and a reject with none pending was a GLib critical.
	if (!mInvocation || transferId != mPendingTransferId)
	{
		callback(BLUETOOTH_ERROR_PARAM_INVALID);
		return;
	}

	if (accept)
	{
		auto resultCallBack = [this, alive = mLifetime.token(), transferId](BluetoothError /*error*/, uint64_t bytesTransferred,
														   uint64_t /*totalSize*/, bool finished)
		{
			// Called back after the profile may have gone with its adapter.
			if (alive.expired())
			{
				return;
			}
			if (mTransfersMap[transferId])
				getOppObserver()->transferStateChanged(transferId, bytesTransferred - mTransfersMap[transferId], finished);
			else
				getOppObserver()->transferStateChanged(transferId, bytesTransferred, finished);

			if (finished)
				mTransfersMap[transferId] = 0;
			else
				mTransfersMap[transferId] = bytesTransferred;
		};

		startTransfer(transferId, mTransferObjPath, resultCallBack, Bluez5ObexTransfer::TransferType::RECEIVING);
		bluez_obex_agent1_complete_authorize_push(mInterface, mInvocation, mStoredName.c_str());
		mInvocation = nullptr;
		cancelPendingConfirmation();	// only the timer and the id are left
	}
	else
		cancelPendingConfirmation();

	callback(BLUETOOTH_ERROR_NONE);
}
