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

#include "bluez5obexagent.h"
#include "bluez5sil.h"
#include "dbusutils.h"
#include "asyncutils.h"
#include "logging.h"
#include "bluez5adapter.h"
#include "bluez5profileopp.h"
#include "bluez5busconfig.h"

const std::string OBEX_AGENT_PATH = "/obex/agent";

// obexd's own namespace: it takes only org.bluez.obex.Error.Rejected as a refusal (obexd/src/manager.c). Any other
// error, org.bluez.Error.Rejected included, accepted the push under the sender's file name.
#define BLUEZ5_OBEX_AGENT_ERROR_CANCELED        "org.bluez.obex.Error.Canceled"
#define BLUEZ5_OBEX_AGENT_ERROR_REJECTED        "org.bluez.obex.Error.Rejected"
#define BLUEZ5_OBEX_AGENT_ERROR_NOT_IMPLEMENTED "org.bluez.obex.Error.NotImplemented"

Bluez5ObexAgent::Bluez5ObexAgent(Bluez5SIL *sil) :
	mAgentManagerProxy(nullptr),
	mAgentInterface(nullptr),
	mNameWatch(BLUEZ5_OBEX_DBUS_BUS_TYPE, "org.bluez.obex"),
	mSIL(sil)
{
	DBusUtils::waitForBus(BLUEZ5_OBEX_DBUS_BUS_TYPE, [this, alive = mLifetime.token()](bool available) {
		if (alive.expired())
			return;
		if (!available) {
			// register an empty name watch handler
			mNameWatch.watch([](bool /*available*/) { });
			return;
		}

		DEBUG("DBus session bus is available now");

		connectWithObex();
	});
}

// The agent stayed exported at its path, its handlers pointing at the freed agent: after a bluetoothd restart
// (obexd still running) the next incoming push called into freed memory, and the new agent could neither register
// nor export.
Bluez5ObexAgent::~Bluez5ObexAgent()
{
	mNameWatch.watch(nullptr);

	if (mAgentManagerProxy && mAgentInterface)
		bluez_obex_agent_manager1_call_unregister_agent(mAgentManagerProxy, OBEX_AGENT_PATH.c_str(), NULL, NULL, NULL);

	deleteObexAgentManagerProxy();
}

void Bluez5ObexAgent::releaseAgentInterface()
{
	if (!mAgentInterface)
		return;

	g_signal_handlers_disconnect_by_data(mAgentInterface, this);
	g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(mAgentInterface));
	g_object_unref(mAgentInterface);
	mAgentInterface = nullptr;
}

void Bluez5ObexAgent::connectWithObex()
{
	DEBUG("Waiting for OBEX service to be available on the bus");

	mNameWatch.watch([this](bool available) {
		if (available)
		{
			createObexAgentManagerProxy();
		}
		else
		{
			deleteObexAgentManagerProxy();
		}
	});
}

void Bluez5ObexAgent::createObexAgentManagerProxy()
{
	if (mAgentManagerProxy)
	{
		WARNING(MSGID_PROXY_ALREADY_EXISTS, 0, "Proxy for OBEX agent mgr already exists. Removing it first");
		deleteObexAgentManagerProxy();
	}

	auto createProxyCallback = [this, alive = mLifetime.token()](GAsyncResult *result) {
		GError *error = 0;

		BluezObexAgentManager1 *proxy = bluez_obex_agent_manager1_proxy_new_for_bus_finish(result, &error);
		if (alive.expired())
		{
			g_clear_error(&error);
			if (proxy)
				g_object_unref(proxy);
			return;
		}
		mAgentManagerProxy = proxy;
		if (error)
		{
			ERROR(MSGID_FAILED_TO_CREATE_OBEX_AGENT_MGR_PROXY, 0,
			      "Failed to create dbus proxy for OBEX agent mgr: %s", error->message);
			g_error_free(error);
			return;
		}

		DEBUG("Successfully created agent manager for OBEX client");
		registerAgent();
	};

	bluez_obex_agent_manager1_proxy_new_for_bus(BLUEZ5_OBEX_DBUS_BUS_TYPE, G_DBUS_PROXY_FLAGS_NONE,
	                                     "org.bluez.obex", "/org/bluez/obex", NULL,
	                                     glibAsyncMethodWrapper,
	                                     new GlibAsyncFunctionWrapper(createProxyCallback));
}

void Bluez5ObexAgent::deleteObexAgentManagerProxy()
{
	DEBUG("Destroying proxy for OBEX agent manager");

	if (mAgentManagerProxy)
	{
		g_object_unref(mAgentManagerProxy);
		mAgentManagerProxy = nullptr;
	}

	releaseAgentInterface();	// was unreffed while still exported
}

void Bluez5ObexAgent::registerAgent()
{
	DEBUG("registerAgent with OBEX agent manager");

	std::string objectPath(OBEX_AGENT_PATH);

	auto registerCallback = [this, alive = mLifetime.token(), objectPath](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		gboolean ret;

		ret = bluez_obex_agent_manager1_call_register_agent_finish(BLUEZ_OBEX_AGENT_MANAGER1(source), result, &error);
		if (alive.expired())
		{
			g_clear_error(&error);
			return;
		}
		if (!ret || error)
		{
			ERROR(MSGID_FAILED_TO_CREATE_OBEX_AGENT_MGR_PROXY, 0, "Failed to register obex agent on path %s",
				  objectPath.c_str());
			g_clear_error(&error);
			return;
		}

		createAgentInterface(objectPath);
	};

	bluez_obex_agent_manager1_call_register_agent(mAgentManagerProxy, objectPath.c_str(), NULL,
	                                       glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(registerCallback));
}

void Bluez5ObexAgent::createAgentInterface(const std::string &objectPath)
{
	DEBUG("creating interface OBEX agent");

	GError *error = nullptr;
	[[maybe_unused]] gboolean ret;

	// The connection the manager proxy is on, where obexd will call the agent (borrowed). It was taken with
	// g_bus_get_sync.
	GDBusConnection *mConn = mAgentManagerProxy ? g_dbus_proxy_get_connection(G_DBUS_PROXY(mAgentManagerProxy)) : nullptr;
	if (!mConn)
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_AGENT_MGR_PROXY, 0, "Failed to create obex agent on path");
		return;
	}

	releaseAgentInterface();

	mAgentInterface = bluez_obex_agent1_skeleton_new();

	g_signal_connect(mAgentInterface,
		"handle_authorize_push",
		G_CALLBACK (onHandleAuthorizePush),
		this);

	g_signal_connect(mAgentInterface,
		"handle_cancel",
		G_CALLBACK (onHandleCancel),
		this);

	g_signal_connect(mAgentInterface,
		"handle_release",
		G_CALLBACK (onHandleRelease),
		this);

	ret = g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (mAgentInterface),
									  mConn,
									  objectPath.c_str(),
									  &error);
	if (error)
	{
		ERROR(MSGID_FAILED_TO_CREATE_OBEX_AGENT_MGR_PROXY, 0, "Failed to export obex interface skeleton on path %s error:%s",objectPath.c_str(), error->message);
		g_error_free(error);
		return;
	}

}

namespace {

// Each lookup is answered by obexd itself (obexd/src/manager.c: the Transfer1 and Session1 properties are its own
// data), from the nested main loop it runs while it waits for the agent: milliseconds. The limit only keeps a hung
// obexd from holding the request; the time taken counts against the 50 s the user is given (Bluez5ProfileOpp).
const int PUSH_LOOKUP_TIMEOUT_MS = 5000;

typedef std::function<void(GVariant *properties)> PropertiesCallback;

// Properties.GetAll(interface) of obexd's object at path, asynchronously: done gets the a{sv}, or NULL on failure.
// It ran synchronously, through two proxies made for the purpose, each first asking the bus who owns
// org.bluez.obex, all on the service's only main loop.
void getObexProperties(const ObexPeer &peer, const std::string &path, const char *interface, PropertiesCallback done)
{
	auto reply = [done, path](GObject *source, GAsyncResult *result) {
		GError *error = nullptr;
		GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
		if (!ret)
		{
			ERROR(MSGID_FAILED_TO_CREATE_OBEX_TRANSFER_PROXY, 0, "Cannot read %s of an incoming push: %s", path.c_str(),
			      error ? error->message : "no reply");
			g_clear_error(&error);
			done(nullptr);
			return;
		}
		GVariant *properties = g_variant_get_child_value(ret, 0);
		done(properties);
		g_variant_unref(properties);
		g_variant_unref(ret);
	};

	g_dbus_connection_call(peer.connection, peer.name.c_str(), path.c_str(), "org.freedesktop.DBus.Properties", "GetAll",
	                       g_variant_new("(s)", interface), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
	                       PUSH_LOOKUP_TIMEOUT_MS, NULL, glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(reply));
}

void rejectPush(GDBusMethodInvocation *invocation)
{
	g_dbus_method_invocation_return_dbus_error(invocation, BLUEZ5_OBEX_AGENT_ERROR_REJECTED, "User rejected confirmation");
}

} // namespace

// Every request is answered, exactly once: obexd waits up to 60 s for an unanswered one, refusing all other pushes
// meanwhile, and then takes the push. The transfer's and its session's properties are read asynchronously; the
// request is then handed to the OPP profile of the adapter it came in on, or refused.
gboolean Bluez5ObexAgent::handleAuthorizePush(BluezObexAgent1 *object, GDBusMethodInvocation *invocation,
	                                      const gchar *arg_path)
{
	const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
	ObexPeer peer;
	peer.connection = g_dbus_method_invocation_get_connection(invocation);
	peer.name = sender ? sender : "";
	if (!peer.isValid() || !arg_path || !g_variant_is_object_path(arg_path))
	{
		rejectPush(invocation);
		return TRUE;
	}

	auto request = std::make_shared<ObexPushRequest>();
	request->transferPath = arg_path;
	request->receivedAt = g_get_monotonic_time();

	// A newer request, or obexd's Cancel, makes this one void (obexd asks about one push at a time).
	unsigned int pushId = ++mPushId;
	// Kept for the answer: the skeleton is replaced when the agent registers again.
	std::shared_ptr<BluezObexAgent1> skeleton(BLUEZ_OBEX_AGENT1(g_object_ref(object)), g_object_unref);
	// The invocation (and with it the connection in peer) lives until it is answered, which every path below does.
	auto stillWanted = [this, alive = mLifetime.token(), pushId]() {
		return !alive.expired() && pushId == mPushId;
	};

	getObexProperties(peer, request->transferPath, "org.bluez.obex.Transfer1",
	                  [this, stillWanted, peer, request, skeleton, invocation](GVariant *transfer) {
		if (!stillWanted() || !parseObexTransferProperties(transfer, *request))
		{
			rejectPush(invocation);
			return;
		}

		getObexProperties(peer, request->sessionPath, "org.bluez.obex.Session1",
		                  [this, stillWanted, peer, request, skeleton, invocation](GVariant *session) {
			if (!stillWanted() || !parseObexSessionProperties(session, *request))
			{
				rejectPush(invocation);
				return;
			}
			dispatchPush(skeleton.get(), invocation, *request, peer);
		});
	});

	return TRUE;
}

void Bluez5ObexAgent::dispatchPush(BluezObexAgent1 *object, GDBusMethodInvocation *invocation,
                                   const ObexPushRequest &request, const ObexPeer &peer)
{
	// No adapter while bluez is back but the adapter is not yet created; no OPP when it is not enabled.
	Bluez5Adapter *adapter = mSIL->getBluez5AdapterbyAddress(request.source);
	Bluez5ProfileOpp *oppProfile = adapter ? dynamic_cast<Bluez5ProfileOpp*>(adapter->getProfile(BLUETOOTH_PROFILE_ID_OPP))
	                                       : nullptr;
	if (oppProfile)
		oppProfile->agentTransferConfirmationRequested(object, invocation, request, peer);
	else
		rejectPush(invocation);
}

// obexd gave up on the push it asked about (timeout, or the sender cancelled): the pending confirmation is void.
gboolean Bluez5ObexAgent::handleCancel(BluezObexAgent1 *object, GDBusMethodInvocation *invocation)
{
	++mPushId;	// one still being looked up is refused when its lookup ends
	for (auto adapter : mSIL->getAdapters())
	{
		auto bluez5Adapter = static_cast<Bluez5Adapter*>(adapter);
		auto oppProfile = dynamic_cast<Bluez5ProfileOpp*>(bluez5Adapter->getProfile(BLUETOOTH_PROFILE_ID_OPP));
		if (oppProfile)
			oppProfile->cancelPendingConfirmation();
	}
	bluez_obex_agent1_complete_cancel(object, invocation);
	return TRUE;
}

gboolean Bluez5ObexAgent::handleRelease(BluezObexAgent1 *object, GDBusMethodInvocation *invocation)
{
	bluez_obex_agent1_complete_release(object, invocation);
	return TRUE;
}
