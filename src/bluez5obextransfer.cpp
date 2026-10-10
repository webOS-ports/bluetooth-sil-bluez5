// Copyright (c) 2014-2020 LG Electronics, Inc.
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

#include <bluetooth-sil-api.h>

#include "bluez5obextransfer.h"
#include "bluez5obexsession.h"
#include "logging.h"
#include "asyncutils.h"
#include "bluez5busconfig.h"

Bluez5ObexTransfer::Bluez5ObexTransfer(const std::string &objectPath, TransferType type) :
	mObjectPath(objectPath),
	mTransferProxy(0),
	mPropertiesProxy(0),
	mWatchCallback(nullptr),
	mBytesTransferred(0),
	mFileSize(0),
	mState(INACTIVE),
	mTransferType(type)
{
	GError *error = 0;

	mTransferProxy = bluez_obex_transfer1_proxy_new_for_bus_sync(BLUEZ5_OBEX_DBUS_BUS_TYPE, G_DBUS_PROXY_FLAGS_NONE,
														"org.bluez.obex", mObjectPath.c_str(), NULL, &error);
	if (error)
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_TRANSFER_PROXY, 0,
			  "Failed to create dbus proxy for obex transfer on path %s",
			  mObjectPath.c_str());
		g_error_free(error);
		failSoon();
		return;
	}

	mPropertiesProxy = free_desktop_dbus_properties_proxy_new_for_bus_sync(BLUEZ5_OBEX_DBUS_BUS_TYPE, G_DBUS_PROXY_FLAGS_NONE,
																		   "org.bluez.obex", objectPath.c_str(), NULL, &error);
	if (error)
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_TRANSFER_PROXY, 0, "Failed to create dbus proxy for obex transfer on path %s: %s",
			  objectPath.c_str(), error->message);
		g_error_free(error);
		failSoon();
		return;
	}

	g_signal_connect(G_OBJECT(mPropertiesProxy), "properties-changed", G_CALLBACK(handlePropertiesChanged), this);

	// A transfer that ends quickly is deleted before this answer (its final state arrives first): the callback
	// read the freed transfer and reported its completion a second time.
	auto fetchPropertiesCallback = [this, alive = mLifetime.token()](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		gboolean ret;
		GVariant *properties = 0;

		ret = free_desktop_dbus_properties_call_get_all_finish(FREE_DESKTOP_DBUS_PROPERTIES(source), &properties, result, &error);
		if (!ret || error || alive.expired())
		{
			g_clear_error(&error);
			if (properties)
				g_variant_unref(properties);
			// Gone before it could be read: it ended (or failed) before its signals were watched, and nothing
			// would ever complete it. Reported as failed, unless its end was seen meanwhile.
			if (!alive.expired() && mState != COMPLETE && mState != ERROR)
			{
				mState = ERROR;
				notifyWatcherAboutChangedProperties();
			}
			return;
		}

		updateFromProperties(properties);

		g_variant_unref(properties);
	};

	free_desktop_dbus_properties_call_get_all(mPropertiesProxy, "org.bluez.obex.Transfer1", NULL,
											  glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(fetchPropertiesCallback));

}

Bluez5ObexTransfer::~Bluez5ObexTransfer()
{
	// If we didn't completed our transfer yet and no error has occured we
	// have to notify our watcher that the transfer was aborted.
	if (mState != COMPLETE && mState != ERROR)
	{
		mState = ERROR;
		if (mWatchCallback)
			mWatchCallback();
	}

	// A pending call keeps the proxy, and with it this handler, alive after the transfer.
	if (mPropertiesProxy)
	{
		g_signal_handlers_disconnect_by_data(mPropertiesProxy, this);
		g_object_unref(mPropertiesProxy);
	}
	if (mTransferProxy)	// NULL when construction failed
		g_object_unref(mTransferProxy);
}

// A transfer whose proxies could not be made is told as failed once its watch is set, which happens right after
// it is created: it stayed INACTIVE, and its request never completed.
void Bluez5ObexTransfer::failSoon()
{
	auto fail = [this, alive = mLifetime.token()]() {
		if (!alive.expired() && mState != COMPLETE && mState != ERROR)
		{
			mState = ERROR;
			notifyWatcherAboutChangedProperties();
		}
		return false;
	};
	g_idle_add(glibSourceMethodWrapper, new GlibSourceFunctionWrapper(fail));
}

void Bluez5ObexTransfer::watch(Bluez5ObexTransferWatchCallback callback)
{
	mWatchCallback = callback;
}

void Bluez5ObexTransfer::notifyWatcherAboutChangedProperties()
{
	if (mWatchCallback)
		mWatchCallback();
}

// Types are checked: a value of another type gave NULL from g_variant_get_string, and std::string(NULL) throws.
bool Bluez5ObexTransfer::parsePropertyFromVariant(const std::string &key, GVariant *valueVar)
{
	bool changed = false;
	bool isString = g_variant_is_of_type(valueVar, G_VARIANT_TYPE_STRING);
	bool isUint64 = g_variant_is_of_type(valueVar, G_VARIANT_TYPE_UINT64);

	if (key == "Transferred" && isUint64)
	{
		mBytesTransferred = g_variant_get_uint64(valueVar);
		changed = true;
	}
	else if (key == "Size" && isUint64)
	{
		mFileSize = g_variant_get_uint64(valueVar);
	}
	else if (key == "Status" && isString)
	{
		std::string state = g_variant_get_string(valueVar, NULL);

		if (state == "queued")
			mState = QUEUED;
		else if (state == "active")
			mState = ACTIVE;
		else if (state == "suspended")
			mState = SUSPENDED;
		else if (state == "complete")
			mState = COMPLETE;
		else if (state == "error")
			mState = ERROR;

		changed = true;
	}
	else if (key == "Name" && isString)
	{
		mFileName = g_variant_get_string(valueVar, NULL);
	}
	else if (key == "Filename" && isString)
	{
		mFilePath = g_variant_get_string(valueVar, NULL);
	}
	else if (key == "MessageHandle" && isString)
	{
		mMessageHandle = g_variant_get_string(valueVar, NULL);
	}

	// Done: for small files obexd may send no Transferred update before the final state. Both directions, as an
	// incoming file was reported complete with fewer bytes than it had.
	if (mState == COMPLETE && mBytesTransferred < mFileSize)
		mBytesTransferred = mFileSize;

	return changed;
}

void Bluez5ObexTransfer::updateFromProperties(GVariant *properties)
{
	bool changed = false;

	for (size_t n = 0; n < g_variant_n_children(properties); n++)
	{
		GVariant *propertyVar = g_variant_get_child_value(properties, n);
		GVariant *keyVar = g_variant_get_child_value(propertyVar, 0);
		GVariant *valueVar = g_variant_get_child_value(propertyVar, 1);
		GVariant *realValueVar = g_variant_get_variant(valueVar);

		std::string key = g_variant_get_string(keyVar, NULL);

		changed |= parsePropertyFromVariant(key, realValueVar);

		g_variant_unref(valueVar);
		g_variant_unref(keyVar);
		g_variant_unref(propertyVar);
		g_variant_unref(realValueVar);
	}

	if (changed)
		notifyWatcherAboutChangedProperties();
}

void Bluez5ObexTransfer::handlePropertiesChanged(BluezObexTransfer1 *, gchar */*interface*/,  GVariant *changedProperties,
												   GVariant */*invalidatedProperties*/, gpointer userData)
{
	auto transfer = static_cast<Bluez5ObexTransfer*>(userData);

	transfer->updateFromProperties(changedProperties);
}

void Bluez5ObexTransfer::cancel(BluetoothResultCallback callback)
{
	if (!mTransferProxy)
	{
		callback(BLUETOOTH_ERROR_FAIL);
		return;
	}

	// Answered when the remote acknowledges, or the session dies, by when the transfer may be deleted: nothing
	// of it is used here.
	auto cancelCallback = [callback](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		gboolean ret;

		ret = bluez_obex_transfer1_call_cancel_finish(BLUEZ_OBEX_TRANSFER1(source), result, &error);
		if (!ret || error)
		{
			g_clear_error(&error);
			callback(BLUETOOTH_ERROR_FAIL);
			return;
		}

		callback(BLUETOOTH_ERROR_NONE);
	};

	bluez_obex_transfer1_call_cancel(mTransferProxy, NULL, glibAsyncMethodWrapper,
									 new GlibAsyncFunctionWrapper(cancelCallback));
}

bool Bluez5ObexTransfer::isPartOfSession(Bluez5ObexSession *session)
{
	// Followed by a '/': as a plain prefix, session1 also claimed the transfers of session12.
	const std::string &sessionPath = session->getObjectPath();
	return mObjectPath.size() > sessionPath.size() && mObjectPath.compare(0, sessionPath.size(), sessionPath) == 0 &&
	       mObjectPath[sessionPath.size()] == '/';
}
