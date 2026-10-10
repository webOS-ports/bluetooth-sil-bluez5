// Copyright (c) 2021 LG Electronics, Inc.
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

#include <cinttypes>
//#include <dbus/dbus.h>
#include "bluez5meshadv.h"
#include "bluez5profilemesh.h"
#include "bluez5adapter.h"
#include "utils.h"
#include "logging.h"
#include "asyncutils.h"
#include "bluez5meshelement.h"
#include "bluez5meshadvprovisioner.h"
#include "bluez5meshadvprovagent.h"
#include "bluez5meshapplication.h"
#include <uuid/uuid.h>
#include <cstdint>
#include <string>
#include <vector>
#include "meshconfig.h"

#define BLUEZ_MESH_NAME "org.bluez.mesh"
#define BLUEZ_MESH_APP_PATH "/" //"/silbluez/mesh"
#define BLUEZ_MESH_ELEMENT_PATH "/element"
#define ONE_SECOND				1000
#define RESPOND_WAIT_DURATION	2
#define LOCAL_NODE_ADDRESS 001


/* Generic OnOff model opcodes */
#define OP_GENERIC_ONOFF_GET				0x8201
#define OP_GENERIC_ONOFF_SET				0x8202
#define OP_GENERIC_ONOFF_SET_UNACK			0x8203
#define OP_GENERIC_ONOFF_STATUS				0x8204

#define TTL_MASK	0x7f

Bluez5MeshAdv::Bluez5MeshAdv(Bluez5ProfileMesh *mesh, Bluez5Adapter *adapter):
mDbusConn(nullptr),
mObjectManager(nullptr),
mNetworkInterface(nullptr),
mMgmtInterface(nullptr),
mNodeInterface(nullptr),
mMeshAdvProv(nullptr),
mMeshAdvProvAgent(nullptr),
mMeshApplication(nullptr),
mTransacId(0),
mReqExpTimerId(0),
mWatcherId(0),
mMesh(mesh),
mAdapter(adapter),
mToken(0)
{
	GError *error = 0;

	mDbusConn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "Failed to setup dbus: %s", error->message);
		g_error_free(error);
		return;
	}

	mMeshAdvProv = new Bluez5MeshAdvProvisioner(mAdapter, mMesh);
	mMeshAdvProvAgent = new Bluez5MeshAdvProvAgent(mAdapter, mMesh);
	mMeshApplication = new Bluez5MeshApplication(mAdapter, mMesh);

	mWatcherId = g_bus_watch_name(G_BUS_TYPE_SYSTEM, BLUEZ_MESH_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
					 handleBluezMeshServiceStarted, handleBluezMeshServiceStopped, this, NULL);
}

Bluez5MeshAdv::~Bluez5MeshAdv()
{
	/* Stops watching a name: first, so that its callbacks do not run on what is torn down */
	if (mWatcherId)
	{
		g_bus_unwatch_name(mWatcherId);
		mWatcherId = 0;
	}

	if (mReqExpTimerId)
	{
		g_source_remove(mReqExpTimerId);	// it fires on this object
		mReqExpTimerId = 0;
	}

	for (auto &job : mKeyRefreshJobs)
		if (job->timer)
			g_source_remove(job->timer);
	mKeyRefreshJobs.clear();

	// The exported objects' handlers point at the helpers deleted below.
	releaseObjectManagerServer();

	// Plain C++ objects: they were given to g_object_unref, which failed G_IS_OBJECT on every adapter teardown
	// (each service stop) and leaked them.
	delete mMeshAdvProv;
	mMeshAdvProv = 0;
	delete mMeshAdvProvAgent;
	mMeshAdvProvAgent = 0;
	delete mMeshApplication;
	mMeshApplication = 0;

	releaseMeshProxies();

	if (mDbusConn)
		g_object_unref(mDbusConn);	// a new reference from g_bus_get_sync
}

void Bluez5MeshAdv::releaseObjectManagerServer()
{
	if (!mObjectManagerServer)
		return;

	g_dbus_object_manager_server_set_connection(mObjectManagerServer, NULL);	// unexports everything
	g_object_unref(mObjectManagerServer);
	mObjectManagerServer = nullptr;
}

// The proxies for meshd's objects, and the manager that reports them; the node proxy was never released.
void Bluez5MeshAdv::releaseMeshProxies()
{
	if (mObjectManager)
	{
		g_signal_handlers_disconnect_by_data(mObjectManager, this);
		g_object_unref(mObjectManager);
		mObjectManager = 0;
	}
	if (mNetworkInterface)
	{
		g_object_unref(mNetworkInterface);
		mNetworkInterface = 0;
	}
	if (mMgmtInterface)
	{
		g_object_unref(mMgmtInterface);
		mMgmtInterface = 0;
	}
	if (mNodeInterface)
	{
		g_object_unref(mNodeInterface);
		mNodeInterface = 0;
	}
}

void Bluez5MeshAdv::getRandomBytes(unsigned char *buf)
{
	uuid_generate(buf);
}

void Bluez5MeshAdv::updateNetworkId()
{
	DEBUG("updateNetworkId mToken: %" PRIu64, mToken);
	mMesh->getMeshObserver()->updateNetworkId(convertAddressToLowerCase(mAdapter->getAddress()),
		mToken);
}

BluetoothError Bluez5MeshAdv::attachToken(const std::string &token)
{
	std::string::size_type sz = 0;
	mToken = std::stoull(token, &sz, 0);
	DEBUG("attachToken mToken: %" PRIu64, mToken);
	attach();
	return BLUETOOTH_ERROR_NONE;
}

void Bluez5MeshAdv::attach()
{
	if (!mNetworkInterface)	// meshd not there: the call went to a NULL proxy and was lost
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "Attach: no mesh network interface");
		return;
	}

	// Its failure is only logged: the SIL API has no event for it (updateNetworkId would store a token of 0).
	// Nothing of this object is used: finished on the source, as the profile may go with its adapter meanwhile.
	auto attachCallback = [](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		gchar *node = 0;
		GVariant *configuration = 0;
		bluez_mesh_network1_call_attach_finish(BLUEZ_MESH_NETWORK1(source), &node, &configuration, result, &error);
		if (error)
		{
			ERROR(MSGID_MESH_PROFILE_ERROR, 0, "Atttach failed: %s", error->message);
			g_error_free(error);
			return;
		}
		DEBUG("attached node: %s", node);
		g_free(node);	// both leaked
		if (configuration)
			g_variant_unref(configuration);
	};
	bluez_mesh_network1_call_attach(mNetworkInterface, BLUEZ_MESH_APP_PATH, mToken, NULL,
									glibAsyncMethodWrapper, new GlibAsyncFunctionWrapper(attachCallback));
}

BluetoothError Bluez5MeshAdv::createNetwork()
{
	if(!mNetworkInterface)
		return BLUETOOTH_ERROR_NOT_ALLOWED;

	getRandomBytes(mUuid);

	GBytes *bytes = g_bytes_new(mUuid, 16);

	GVariant *uuidVar = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, bytes, true);
	g_bytes_unref(bytes);	// the variant holds its own reference: this one leaked

	// Its failure is only logged: the SIL API has no event for it, and the client waits for updateNetworkId.
	auto createNetworkCallback = [](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		bluez_mesh_network1_call_create_network_finish(BLUEZ_MESH_NETWORK1(source), result, &error);
		if (error)
		{
			ERROR(MSGID_MESH_PROFILE_ERROR, 0, "CreateNetwork failed: %s", error->message);
			g_error_free(error);
			return;
		}
		DEBUG("Mesh CreateNetwork success\n");
	};

	bluez_mesh_network1_call_create_network(mNetworkInterface, BLUEZ_MESH_APP_PATH, uuidVar, NULL, glibAsyncMethodWrapper,
											new GlibAsyncFunctionWrapper(createNetworkCallback));
	return BLUETOOTH_ERROR_NONE;
}

void Bluez5MeshAdv::getMeshInfo(BleMeshInfoCallback callback)
{
	// Answered: it was an empty stub, and its caller waited forever.
	BleMeshInfo meshInfo;
	callback(BLUETOOTH_ERROR_UNSUPPORTED, meshInfo);
}

void Bluez5MeshAdv::handleBluezMeshServiceStarted(GDBusConnection *conn, const gchar */*name*/,
												  const gchar */*nameOwner*/, gpointer userData)
{
	GError *error = 0;
	Bluez5MeshAdv *meshAdv = static_cast<Bluez5MeshAdv *>(userData);
	// Kept, to unexport its objects when meshd or this object goes: they stayed exported, their handlers
	// pointing at freed objects, and a new one could not export at the same path.
	meshAdv->releaseObjectManagerServer();
	GDBusObjectManagerServer * objectManagerServer = g_dbus_object_manager_server_new(BLUEZ_MESH_APP_PATH);
	meshAdv->mObjectManagerServer = objectManagerServer;
	GDBusObjectSkeleton *meshAppSkeleton = g_dbus_object_skeleton_new(BLUEZ_MESH_APP_PATH);

	meshAdv->mMeshApplication->registerApplicationInterface(objectManagerServer, meshAppSkeleton, meshAdv);
	meshAdv->mMeshAdvProv->registerProvisionerInterface(objectManagerServer, meshAppSkeleton);
	meshAdv->mMeshAdvProvAgent->registerProvAgentInterface(objectManagerServer);

	g_dbus_object_manager_server_export(objectManagerServer, meshAppSkeleton);
	g_object_unref(meshAppSkeleton);	// the server holds its own reference: this one leaked

	for (auto element = meshAdv->mElements.begin(); element != meshAdv->mElements.end(); element++)
	{
		element->registerElementInterface(objectManagerServer);
	}

	g_dbus_object_manager_server_set_connection(objectManagerServer, conn);

	meshAdv->releaseMeshProxies();	// a restart of meshd overwrote (and leaked) them
	meshAdv->mObjectManager = g_dbus_object_manager_client_new_sync(
		conn, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
		BLUEZ_MESH_NAME, "/", NULL, NULL, NULL, NULL, &error);

	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0,
			"Failed to create object manager: %s", error->message);
		g_error_free(error);
		return;
	}

	g_signal_connect(meshAdv->mObjectManager, "object-added", G_CALLBACK(handleObjectAdded), meshAdv);
	g_signal_connect(meshAdv->mObjectManager, "object-removed", G_CALLBACK(handleObjectRemoved), meshAdv);

	// Each object and interface released on every path: an error returned from inside the loop leaked the list,
	// the objects left in it and the interface.
	GList* objects = g_dbus_object_manager_get_objects(meshAdv->mObjectManager);
	DEBUG("Mesh object length: %d", g_list_length(objects));
	for (GList *l = objects; l; l = l->next)
	{
		auto object = static_cast<GDBusObject*>(l->data);
		std::string objectPath = g_dbus_object_get_object_path(object);
		DEBUG("Object path: %s", objectPath.c_str());

		auto networkInterface = g_dbus_object_get_interface(object, "org.bluez.mesh.Network1");
		if (networkInterface && !meshAdv->mNetworkInterface)
		{
			DEBUG("org.bluez.mesh.Network1 interface added");
			GError *proxyError = 0;
			meshAdv->mNetworkInterface = bluez_mesh_network1_proxy_new_for_bus_sync(
				G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
				BLUEZ_MESH_NAME, objectPath.c_str(), NULL, &proxyError);
			if (proxyError)
			{
				ERROR(MSGID_MESH_PROFILE_ERROR, 0, "Not able to get Mesh Network interface: %s", proxyError->message);
				g_error_free(proxyError);
			}
		}
		if (networkInterface)
			g_object_unref(networkInterface);
	}
	g_list_free_full(objects, g_object_unref);
}

// It did nothing: the next start made a second object manager and exported a second time next to the first.
void Bluez5MeshAdv::handleBluezMeshServiceStopped(GDBusConnection */*conn*/, const gchar */*name*/,
												  gpointer userData)
{
	Bluez5MeshAdv *meshAdv = static_cast<Bluez5MeshAdv *>(userData);

	meshAdv->releaseObjectManagerServer();
	meshAdv->releaseMeshProxies();
}

namespace {

// A proxy for an interface meshd added, or NULL (logged) when it could not be made.
template <typename T, typename F>
T *meshProxy(F create, const std::string &objectPath, const char *name)
{
	GError *error = 0;
	T *proxy = create(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, BLUEZ_MESH_NAME, objectPath.c_str(), NULL, &error);
	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "Not able to get %s: %s", name, error->message);
		g_error_free(error);
	}
	return proxy;
}

bool proxyIsFor(gpointer proxy, const std::string &objectPath)
{
	return proxy && objectPath == g_dbus_proxy_get_object_path(G_DBUS_PROXY(proxy));
}

} // namespace

// The interfaces looked up are new references, released here (all three leaked), and a proxy replaced is
// released first (overwritten before). One GError was reused for three calls and never checked.
void Bluez5MeshAdv::handleObjectAdded(GDBusObjectManager */*objectManager*/,
										  GDBusObject *object, void *userData)
{
	DEBUG("Bluez5MeshAdv::handleObjectAdded");
	Bluez5MeshAdv *meshAdv = static_cast<Bluez5MeshAdv *>(userData);
	std::string objectPath = g_dbus_object_get_object_path(object);

	//TODO: Compare the application paths
	auto networkInterface = g_dbus_object_get_interface(object, "org.bluez.mesh.Network1");
	if (networkInterface)
	{
		DEBUG("org.bluez.mesh.Network1 interface added");
		if (!meshAdv->mNetworkInterface)
			meshAdv->mNetworkInterface = meshProxy<BluezMeshNetwork1>(bluez_mesh_network1_proxy_new_for_bus_sync,
			                                                          objectPath, "Network1");
		g_object_unref(networkInterface);
	}

	auto meshManagementInterface = g_dbus_object_get_interface(object, "org.bluez.mesh.Management1");
	if (meshManagementInterface)
	{
		DEBUG("org.bluez.mesh.Management1 interface added");
		if (meshAdv->mMgmtInterface)
			g_object_unref(meshAdv->mMgmtInterface);
		meshAdv->mMgmtInterface = meshProxy<BluezMeshManagement1>(bluez_mesh_management1_proxy_new_for_bus_sync,
		                                                          objectPath, "Management1");
		g_object_unref(meshManagementInterface);
	}

	auto meshNodeInterface = g_dbus_object_get_interface(object, "org.bluez.mesh.Node1");
	if (meshNodeInterface)
	{
		DEBUG("org.bluez.mesh.Node1 interface added");
		if (meshAdv->mNodeInterface)
			g_object_unref(meshAdv->mNodeInterface);
		meshAdv->mNodeInterface = meshProxy<BluezMeshNode1>(bluez_mesh_node1_proxy_new_for_bus_sync,
		                                                     objectPath, "Node1");
		g_object_unref(meshNodeInterface);
	}
}

// The node object going (the node left, or was released) takes its proxies with it: calls on them were lost and
// reported as done.
void Bluez5MeshAdv::handleObjectRemoved(GDBusObjectManager */*objectManager*/,
											GDBusObject *object, void *userData)
{
	Bluez5MeshAdv *meshAdv = static_cast<Bluez5MeshAdv *>(userData);
	std::string objectPath = g_dbus_object_get_object_path(object);

	if (proxyIsFor(meshAdv->mMgmtInterface, objectPath))
	{
		g_object_unref(meshAdv->mMgmtInterface);
		meshAdv->mMgmtInterface = 0;
	}
	if (proxyIsFor(meshAdv->mNodeInterface, objectPath))
	{
		g_object_unref(meshAdv->mNodeInterface);
		meshAdv->mNodeInterface = 0;
	}
}

// Calls on meshd's proxies need them: before meshd runs, or before attach or createNetwork completed, they are
// NULL, and the generated calls then return without an error, so the SIL reported success for what was never sent.
BluetoothError Bluez5MeshAdv::scanUnprovisionedDevices(const uint16_t scanTimeout)
{
	if (!mMgmtInterface)
		return BLUETOOTH_ERROR_NOT_ALLOWED;

	GVariantBuilder *builder = 0;
	GVariant *params = 0;
	builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (builder, "{sv}","Seconds" ,g_variant_new_uint16(scanTimeout));
	params = g_variant_builder_end(builder);
	g_variant_builder_unref(builder);

	GError *error = 0;
	bluez_mesh_management1_call_unprovisioned_scan_sync(mMgmtInterface, params, NULL, &error);
	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "UnProvisionedScan failed: %s", error->message);
		g_error_free(error);
		return BLUETOOTH_ERROR_FAIL;
	}
	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5MeshAdv::unprovisionedScanCancel()
{
	if (!mMgmtInterface)
		return BLUETOOTH_ERROR_NOT_ALLOWED;

	GError *error = 0;
	bluez_mesh_management1_call_unprovisioned_scan_cancel_sync(mMgmtInterface, NULL, &error);
	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "UnprovisionedScanCancel failed: %s", error->message);
		g_error_free(error);
		return BLUETOOTH_ERROR_FAIL;
	}
	return BLUETOOTH_ERROR_NONE;
}


BluetoothError Bluez5MeshAdv::provision(const std::string &uuid, const uint16_t /*timeout*/)
{
	// 32 hex digits, from the app: a shorter string was read past its end.
	if (uuid.size() != 32 || uuid.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
		return BLUETOOTH_ERROR_PARAM_INVALID;

	if (!mMgmtInterface)
		return BLUETOOTH_ERROR_NOT_ALLOWED;

	const char *uuidStr = uuid.c_str();
	uint8_t uuidByte[16] = {0};
	int j = 0;

	// meshd answers a rejected AddNode with an error only, no AddNodeFailed: the client waited for its
	// provisionResult forever.
	auto provisionCallback = [this, alive = mLifetime.token(), uuid = convertToLowerCase(uuid)](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		bluez_mesh_management1_call_add_node_finish(BLUEZ_MESH_MANAGEMENT1(source), result, &error);
		if (!error)
		{
			DEBUG("provision started\n");
			return;
		}
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "provision failed: %s", error->message);
		g_error_free(error);
		if (alive.expired())	// the profile went with its adapter
			return;
		mMesh->getMeshObserver()->provisionResult(BLUETOOTH_ERROR_FAIL, convertAddressToLowerCase(mAdapter->getAddress()),
		                                          "endProvision", "", 0, "", "", 0, 0, uuid);
	};

	for (int i = 0; i < 16; ++i)
	{
		sscanf(&uuidStr[i * 2], "%02hhx", &uuidByte[i]);
		DEBUG("%2.2x - %d", uuidByte[i], j);
		++j;
	}

	GBytes *bytes = g_bytes_new(uuidByte, 16);
	GVariant *uuidVar = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, bytes, true);
	g_bytes_unref(bytes);	// the variant holds its own reference: this one leaked

	bluez_mesh_management1_call_add_node(mMgmtInterface, uuidVar, createEmptyStringArrayVariant(), NULL, glibAsyncMethodWrapper,
										new GlibAsyncFunctionWrapper(provisionCallback));

	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5MeshAdv::supplyNumeric(uint32_t number)
{
	return mMeshAdvProvAgent->supplyNumeric(number);
}

BluetoothError Bluez5MeshAdv::supplyStatic(const std::string &oobData)
{
	return mMeshAdvProvAgent->supplyStatic(oobData);
}

BluetoothError Bluez5MeshAdv::createAppKey(uint16_t netKeyIndex, uint16_t appKeyIndex)
{
	if (!mMgmtInterface)
		return BLUETOOTH_ERROR_NOT_ALLOWED;

	GError *error = 0;
	bluez_mesh_management1_call_create_app_key_sync(mMgmtInterface,
													netKeyIndex, appKeyIndex, NULL, &error);
	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "createAppKey failed: %s", error->message);
		g_error_free(error);
		return BLUETOOTH_ERROR_FAIL;
	}
	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5MeshAdv::modelSend(uint16_t /*srcAddress*/, uint16_t destAddress,
										uint16_t appKeyIndex,
										const std::string &command,
										BleMeshPayload &payload)
{
	if (command.compare("onOff") == 0)
	{
		DEBUG("modelSend:: onOff");
		BleMeshPayloadOnOff payloadOnOff = payload.getPayloadOnOff();
		return setOnOff(destAddress, appKeyIndex, payloadOnOff.value);
	}
	else if(command.compare("passThrough") == 0)
	{
		BleMeshPayloadPassthrough payloadPassThr = payload.getPayloadPassthrough();
		return sendPassThrough(destAddress, appKeyIndex, payloadPassThr.value);
	}
	return BLUETOOTH_ERROR_UNSUPPORTED;
}

BluetoothError Bluez5MeshAdv::sendPassThrough(uint16_t destAddress, uint16_t appIndex, const std::vector<uint8_t> value)
{
	if (!mNodeInterface)
		return BLUETOOTH_ERROR_NOT_ALLOWED;

	GError *error = 0;
	GVariantBuilder *builder = 0;
	GVariant *params = 0;

	builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (builder, "{sv}","ForceSegmented" ,g_variant_new_boolean (false));
	params = g_variant_builder_end(builder);
	g_variant_builder_unref(builder);

	GVariantBuilder *dataBuilder = g_variant_builder_new (G_VARIANT_TYPE ("ay"));
	for (auto &dataIt : value)
	{
		DEBUG("data : %u ", dataIt);
		g_variant_builder_add(dataBuilder, "y", dataIt);
	}

	GVariant *dataToSend = g_variant_builder_end(dataBuilder);
	g_variant_builder_unref(dataBuilder);

	bluez_mesh_node1_call_send_sync(mNodeInterface, BLUEZ_MESH_ELEMENT_PATH,
								destAddress, appIndex,
								params, dataToSend, NULL, &error);
	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "Model sendPassThrough failed: %s", error->message);
		g_error_free(error);
		return BLUETOOTH_ERROR_FAIL;
	}

	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5MeshAdv::setOnOff(uint16_t destAddress, uint16_t appIndex, bool onoff, bool ack)
{
	if (!mNodeInterface || mElements.empty())
		return BLUETOOTH_ERROR_NOT_ALLOWED;

	if (ack)
	{
		startTimer("setOnOff");
		mConfiguration.setOnOffState(onoff);
		mOnOffAckPending = true;	// for this node's OnOff Status only, see takeOnOffAck
		mOnOffAckDest = destAddress;
	}
	BluetoothError error = mElements[0].setOnOff(destAddress, appIndex, onoff);
	if (error != BLUETOOTH_ERROR_NONE)
		mOnOffAckPending = false;	// send() stopped the timer
	return error;
}

bool Bluez5MeshAdv::takeOnOffAck(uint32_t opcode, uint16_t src)
{
	if (!mOnOffAckPending || !mReqExpTimerId || !isOnOffStatusFor(opcode, src, mOnOffAckDest))
		return false;
	mOnOffAckPending = false;
	stopReqTimer();
	return true;
}

BluetoothError Bluez5MeshAdv::configGet(uint16_t destAddress,
										const std::string &config,
										uint16_t netKeyIndex)
{
	if (!mNodeInterface || mElements.empty())
		return BLUETOOTH_ERROR_NOT_ALLOWED;

	return mElements[0].configGet(destAddress, config, netKeyIndex);

}

BluetoothError Bluez5MeshAdv::configSet(
		uint16_t destAddress, const std::string &config,
		uint8_t gattProxyState, uint16_t netKeyIndex, uint16_t appKeyIndex,
		uint32_t modelId, uint8_t ttl, BleMeshRelayStatus *relayStatus)
{
	if (!mNodeInterface || mElements.empty())
	{
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}

	return mElements[0].configSet(destAddress, config, gattProxyState, netKeyIndex,
									appKeyIndex, modelId, ttl, relayStatus);
}

BluetoothError Bluez5MeshAdv::deleteNode(uint16_t destAddress, uint8_t count)
{
	if (!mNodeInterface || mElements.empty())
	{
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}

	return mElements[0].deleteNode(destAddress, count);
}

GVariant* Bluez5MeshAdv::createEmptyStringArrayVariant()
{
	GVariantBuilder *builder = 0;
	GVariant *params = 0;
	builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	params = g_variant_builder_end(builder);
	g_variant_builder_unref(builder);
	return params;
}

GVariant* Bluez5MeshAdv::prepareSendDevKeyData(uint8_t *msg, uint16_t n)
{
	GBytes *bytes = g_bytes_new(msg, n);
	GVariant *dataToSend = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, bytes, true);
	g_bytes_unref (bytes);
	return dataToSend;
}

BluetoothError Bluez5MeshAdv::send(uint16_t destAddress, uint16_t appKeyIndex,
									uint8_t *msg, uint16_t msgLen)
{
	if (!mNodeInterface)
	{
		stopReqTimer();
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}

	BluetoothError silError = BLUETOOTH_ERROR_FAIL;

	GError *error = 0;
	GVariantBuilder *builder = 0;
	GVariant *params = 0;

	builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (builder, "{sv}","ForceSegmented" ,g_variant_new_boolean (false));
	params = g_variant_builder_end(builder);
	g_variant_builder_unref(builder);

	GBytes *bytes = g_bytes_new(msg, msgLen);
	GVariant *dataToSend = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, bytes, true);
	g_bytes_unref (bytes);

	bluez_mesh_node1_call_send_sync(mNodeInterface, BLUEZ_MESH_ELEMENT_PATH,
								destAddress, appKeyIndex,
								params, dataToSend, NULL, &error);
	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "Send failed: %s", error->message);
		if (strstr(error->message, "Object not found"))
		{
			silError = BLUETOOTH_ERROR_MESH_APP_KEY_INDEX_DOES_NOT_EXIST;
		}
		g_error_free(error);
		stopReqTimer();
		return silError;
	}

	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5MeshAdv::devKeySend(uint16_t destAddress, uint16_t netKeyIndex, uint8_t *msg, uint16_t n)
{
	if (!mNodeInterface)
		return BLUETOOTH_ERROR_NOT_ALLOWED;

	GError *error = 0;
	BluetoothError silError = BLUETOOTH_ERROR_FAIL;

	bluez_mesh_node1_call_dev_key_send_sync(mNodeInterface, BLUEZ_MESH_ELEMENT_PATH,
											destAddress, true, netKeyIndex,
											createEmptyStringArrayVariant(), prepareSendDevKeyData(msg, n), NULL, &error);

	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "devKeySend failed: %s", error->message);
		silError = BLUETOOTH_ERROR_FAIL;

		g_error_free(error);
		error = NULL;
		stopReqTimer();
		return silError;
	}

	return BLUETOOTH_ERROR_NONE;
}

BluetoothError Bluez5MeshAdv::registerElement(uint8_t index,
												std::vector<uint32_t> &sigModelIds,
												std::vector<uint32_t> &vendorModelIds)
{
	Bluez5MeshElement element(index, mAdapter, mMesh, this);

	for (auto sigModelId = sigModelIds.begin(); sigModelId != sigModelIds.end();
		 sigModelId++)
	{
		element.addModel(*sigModelId);
	}

	for (auto vendorModelId = vendorModelIds.begin(); vendorModelId != vendorModelIds.end();
		 vendorModelId++)
	{
		element.addModel(*vendorModelId);
	}

	mElements.push_back(element);

	return BLUETOOTH_ERROR_NONE;
}

gboolean Bluez5MeshAdv::handleReqTimerExpired(gpointer userdata)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	Bluez5MeshAdv *adv = (Bluez5MeshAdv *)userdata;
	// This source ends with the return below: the id was kept, and the next stopReqTimer() removed a source that
	// was gone (a GLib critical), or by then another one with a reused id.
	adv->mReqExpTimerId = 0;
	if (adv->mConfiguration.getConfig() == "setOnOff")
	{
		// No ack in time: reported as done with the state that was set, as the nRF devices this was written for
		// do not ack. An acknowledged set is effectively fire-and-forget; takeOnOffAck() ends it early when the
		// addressed node does answer.
		adv->mOnOffAckPending = false;
		adv->mMesh->getMeshObserver()->modelSetOnOffResult(convertAddressToLowerCase(adv->mAdapter->getAddress()), adv->mConfiguration.getOnOffState(), BLUETOOTH_ERROR_NONE);
	}
	else
	{
		adv->mMesh->getMeshObserver()->modelConfigResult(convertAddressToLowerCase(adv->mAdapter->getAddress()), adv->mConfiguration, BLUETOOTH_ERROR_MESH_NO_RESPONSE_FROM_NODE);
	}
	return G_SOURCE_REMOVE;
}

void Bluez5MeshAdv::startTimer(const std::string config)
{
	stopReqTimer();
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	mConfiguration.setConfig(std::move(config));
	mReqExpTimerId = g_timeout_add( RESPOND_WAIT_DURATION * ONE_SECOND, handleReqTimerExpired, this);
	DEBUG("Request timer started [%d]",mReqExpTimerId);
}

void Bluez5MeshAdv::stopReqTimer()
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	if (mReqExpTimerId)
	{
		g_source_remove(mReqExpTimerId);
		DEBUG("Request timer stopped [%d]",mReqExpTimerId);
		mReqExpTimerId = 0;
	}
	return;
}

BluetoothError Bluez5MeshAdv::getCompositionData(uint16_t destAddress)
{
	if (!mNodeInterface || mElements.empty())
		return BLUETOOTH_ERROR_NOT_ALLOWED;

	return mElements[0].getCompositionData(destAddress);
}

BluetoothError Bluez5MeshAdv::updateNodeInfo(std::vector<uint16_t> &unicastAddresses)
{
	return mMeshAdvProv->updateNodeInfo(unicastAddresses);
}

BluetoothError Bluez5MeshAdv::deleteRemoteNodeFromLocalKeyDatabase(uint16_t primaryAddress, uint8_t count)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);
	if (!mMgmtInterface)
	{
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}

	GError *error = 0;

	bluez_mesh_management1_call_delete_remote_node_sync(mMgmtInterface,
													primaryAddress, count, NULL, &error);
	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "deleteRemoteNode failed: %s :%d", error->message, primaryAddress);
		g_error_free(error);
		error = NULL;
		return BLUETOOTH_ERROR_FAIL;
	}
	return BLUETOOTH_ERROR_NONE;
}

void Bluez5MeshAdv::distributeKeys(bool refreshAppKeys, std::vector<uint16_t> appKeyIndexesToRefresh,
	std::vector<BleMeshNode> &nodes, uint16_t netKeyIndex, int32_t waitTime)
{
	BluetoothError btError = BLUETOOTH_ERROR_NONE;
	std::string status = "active";
	GError *error = 0;
	for (auto node = nodes.begin(); node != nodes.end();)
	{
		DEBUG("updating netkeyindex to : %d", node->getPrimaryElementAddress());
		btError = mElements[0].configSet(node->getPrimaryElementAddress(),
						"NETKEY_UPDATE", 0, netKeyIndex, 0, 0, 0, NULL,
						waitTime, node->getNumberOfElements());
		if (BLUETOOTH_ERROR_NONE != btError)
		{
			/* Adding update net key failed, delete the node,
			* Call observer API with appropriate info for subscription
			* response for keyRefresh
			*/
			ERROR(MSGID_MESH_PROFILE_ERROR, 0,
					"netKey update to:%d failed", node->getPrimaryElementAddress());
			deleteRemoteNodeFromLocalKeyDatabase(node->getPrimaryElementAddress(),
											node->getNumberOfElements());
			mMesh->getMeshObserver()->keyRefreshResult(BLUETOOTH_ERROR_MESH_NETKEY_UPDATE_FAILED,
										convertAddressToLowerCase(mAdapter->getAddress()),
										netKeyIndex, status, 1, node->getPrimaryElementAddress());
			node = nodes.erase(node);
		}
		else
		{
			++node;
		}
	}
	DEBUG("Distributing netKeyIndex to remote nodes completed");
	if (refreshAppKeys)
	{
		// Refresh All keys is handled in service. service has the info
		// regarding all the appkeys created. So it sends all the app keys in
		// appKeyIndexesToRefresh parameter
		for (auto appKeyIndex : appKeyIndexesToRefresh)
		{
			DEBUG("appKeyIndex: %d", appKeyIndex);
			bluez_mesh_management1_call_update_app_key_sync(
				mMgmtInterface, appKeyIndex, NULL, &error);
			if (error)
			{
				// Generating new key for appIndex failed
				ERROR(MSGID_MESH_PROFILE_ERROR, 0,
					"Generating new key for appIndex:%d failed", appKeyIndex);
				g_error_free(error);
				error = NULL;
				continue;
			}
			DEBUG("Generating new app key for index : %d completed", appKeyIndex);
			btError = mElements[0].configSet(LOCAL_NODE_ADDRESS,
								"APPKEY_UPDATE", 0, netKeyIndex, appKeyIndex,
								0, 0, NULL, waitTime);
			if (BLUETOOTH_ERROR_NONE != btError)
			{
				mMesh->getMeshObserver()->keyRefreshResult(BLUETOOTH_ERROR_MESH_CANNOT_UPDATE_APPKEY,
						convertAddressToLowerCase(mAdapter->getAddress()),
						netKeyIndex, status, 1,
						LOCAL_NODE_ADDRESS, appKeyIndex);
			}
			DEBUG("Updated appkeyIndex : %d in local node", appKeyIndex);
			for (auto node : nodes)
			{
				std::vector<uint16_t> appKeyIndexes = node.getAppKeyIndexes();
				if (appKeyIndexes.end() != std::find(appKeyIndexes.begin(),
						appKeyIndexes.end(), appKeyIndex))
				{
					DEBUG("Distributing appKeyIndex: %d to remote node: %d", appKeyIndex,
									node.getPrimaryElementAddress());
					btError = mElements[0].configSet(node.getPrimaryElementAddress(),
								"APPKEY_UPDATE", 0, netKeyIndex, appKeyIndex,
								0, 0, NULL, waitTime);
					if (BLUETOOTH_ERROR_NONE != btError)
					{
						mMesh->getMeshObserver()->keyRefreshResult(BLUETOOTH_ERROR_MESH_CANNOT_UPDATE_APPKEY,
								convertAddressToLowerCase(mAdapter->getAddress()),
								netKeyIndex, status, 1,
								node.getPrimaryElementAddress(), appKeyIndex);
					}
				}
			}
		}
	}
}

// False when meshd is gone: the call went to a NULL proxy and was reported as done.
bool Bluez5MeshAdv::setKeyRefreshPhase(uint16_t netKeyIndex, uint16_t phase, std::vector<BleMeshNode> nodes)
{
	GError *error = nullptr;
	std::string	status = "active";
	BluetoothError btError = BLUETOOTH_ERROR_NONE;

	if (!mMgmtInterface || !mNodeInterface || mElements.empty())
		return false;

	bluez_mesh_management1_call_set_key_phase_sync(mMgmtInterface, netKeyIndex, phase, NULL, &error);
	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "Set key phase failed: %s", error->message);
		g_error_free(error);
		error = NULL;
		// The phase it is still in.
		mMesh->getMeshObserver()->keyRefreshResult(BLUETOOTH_ERROR_FAIL,
										convertAddressToLowerCase(mAdapter->getAddress()),
										netKeyIndex, status, phase - 1);
	}
	else
	{
		mMesh->getMeshObserver()->keyRefreshResult(BLUETOOTH_ERROR_NONE,
										convertAddressToLowerCase(mAdapter->getAddress()),
										netKeyIndex, status, phase);
	}
	/* Set key phase in local node */
	mElements[0].configSet(LOCAL_NODE_ADDRESS,
								"KR_PHASE_SET", 0, netKeyIndex, 0,
								0, 0, NULL, 0, 0, phase);
	/* Set key phase in remote nodes */
	for (auto node : nodes)
	{
		btError = mElements[0].configSet(node.getPrimaryElementAddress(),
								"KR_PHASE_SET", 0, netKeyIndex, 0,
								0, 0, NULL, 0, 0, phase);
		if (BLUETOOTH_ERROR_NONE != btError)
		{
			ERROR(MSGID_MESH_PROFILE_ERROR, 0, "Set key phase failed for node: %d",
					node.getPrimaryElementAddress());
		}
	}
	return true;
}

// Phase 1 here, on the main loop like everything else; phases 2 and 3 and the completion follow from
// handleKeyRefreshStep, each after the client's wait.
void Bluez5MeshAdv::keyRefresh(BluetoothResultCallback callback,
	bool refreshAppKeys, std::vector<uint16_t> appKeyIndexesToRefresh,
	std::vector<uint16_t> blackListedNodes,
	std::vector<BleMeshNode> nodes, uint16_t netKeyIndex, int32_t waitTime)
{
	DEBUG("%s::%s",__FILE__,__FUNCTION__);

	if (!mMgmtInterface || !mNodeInterface || mElements.empty())
	{
		callback(BLUETOOTH_ERROR_NOT_ALLOWED);
		return;
	}

	// From the LS2 client unchecked: a negative wait slept for about 136 years.
	waitTime = clampWaitSeconds(waitTime);

	std::string status = "idle";

	GError *error = 0;

	/* Start of phase 1 */
	/* Update netKey in the provisioner */
	bluez_mesh_management1_call_update_subnet_sync(mMgmtInterface, netKeyIndex,
	NULL, &error);
	if (error)
	{
		ERROR(MSGID_MESH_PROFILE_ERROR, 0, "UpdateSubnet failed: %s", error->message);
		if (strstr(error->message, "Does not exist")) // Wrong net key
		{
			callback(BLUETOOTH_ERROR_MESH_NET_KEY_INDEX_DOES_NOT_EXIST);
		}
		else
			callback(BLUETOOTH_ERROR_FAIL);
		g_error_free(error);
		error = NULL;
		return;
	}
	DEBUG("Updating netKeyIndex in provisioner completed");
	callback(BLUETOOTH_ERROR_NONE);
	mMesh->getMeshObserver()->keyRefreshResult(BLUETOOTH_ERROR_NONE,
									convertAddressToLowerCase(mAdapter->getAddress()),
									netKeyIndex, status, 0);

	status = "active";
	mMesh->getMeshObserver()->keyRefreshResult(BLUETOOTH_ERROR_NONE,
										convertAddressToLowerCase(mAdapter->getAddress()),
										netKeyIndex, status, 1);
	// Update network key in local node: of the subnet being refreshed, it was always subnet and index 0.
	bluez_mesh_node1_call_add_net_key_sync(
		mNodeInterface, BLUEZ_MESH_ELEMENT_PATH, LOCAL_NODE_ADDRESS, netKeyIndex, netKeyIndex, true, NULL, &error);
	if (error)
	{
		// Adding local node to the network failed. this should never happen!!
		mMesh->getMeshObserver()->keyRefreshResult(BLUETOOTH_ERROR_MESH_NETKEY_UPDATE_FAILED,
										convertAddressToLowerCase(mAdapter->getAddress()),
										netKeyIndex, status, 1, LOCAL_NODE_ADDRESS);
		g_error_free(error);
		error = NULL;
	}
	DEBUG("Distributing netKeyIndex to local node completed");

	/* Remove blackilisted node from list of nodes so that keys are distributed to
	* nodes that are not blacklisted
	*/
	for (size_t i = 0 ; i < blackListedNodes.size(); ++i)
	{
		for (auto node = nodes.begin(); node != nodes.end();)
		{
			if (blackListedNodes[i] == node->getPrimaryElementAddress())
			{
				//delete the blacklisted node
				deleteRemoteNodeFromLocalKeyDatabase(node->getPrimaryElementAddress(), node->getNumberOfElements());
				node = nodes.erase(node);
			}
			else
			{
				++node;
			}
		}
	}

	distributeKeys(refreshAppKeys, appKeyIndexesToRefresh,
					nodes, netKeyIndex, waitTime);

	auto job = std::make_unique<KeyRefreshJob>();
	job->adv = this;
	job->netKeyIndex = netKeyIndex;
	job->waitTime = waitTime;
	job->nodes = std::move(nodes);
	/* RESPOND_WAIT_DURATION is added because otherwise it is observed that
	 * timer expired and fail response
	 * was sent to application after entering phase 2.
	 */
	job->timer = g_timeout_add_seconds((guint)(waitTime + RESPOND_WAIT_DURATION), handleKeyRefreshStep, job.get());
	mKeyRefreshJobs.push_back(std::move(job));
}

gboolean Bluez5MeshAdv::handleKeyRefreshStep(gpointer userData)
{
	auto *job = static_cast<KeyRefreshJob *>(userData);
	Bluez5MeshAdv *adv = job->adv;
	job->timer = 0;	// this source ends with the return below

	if (job->nextPhase <= 3)
	{
		/* Start of Phase 2, then 3 */
		if (!adv->setKeyRefreshPhase(job->netKeyIndex, job->nextPhase, job->nodes))
		{
			// meshd went away meanwhile: the refresh cannot go on.
			std::string status = "active";
			adv->mMesh->getMeshObserver()->keyRefreshResult(BLUETOOTH_ERROR_FAIL,
										convertAddressToLowerCase(adv->mAdapter->getAddress()),
										job->netKeyIndex, status, job->nextPhase - 1);
			adv->endKeyRefreshJob(job);
			return G_SOURCE_REMOVE;
		}
		job->nextPhase++;
		job->timer = g_timeout_add_seconds((guint)job->waitTime, handleKeyRefreshStep, job);
		return G_SOURCE_REMOVE;
	}

	/* Normal Operation */
	std::string status = "completed";
	adv->mMesh->getMeshObserver()->keyRefreshResult(BLUETOOTH_ERROR_NONE,
										convertAddressToLowerCase(adv->mAdapter->getAddress()),
										job->netKeyIndex, status, 0);
	adv->endKeyRefreshJob(job);
	return G_SOURCE_REMOVE;
}

void Bluez5MeshAdv::endKeyRefreshJob(KeyRefreshJob *job)
{
	for (auto it = mKeyRefreshJobs.begin(); it != mKeyRefreshJobs.end(); ++it)
	{
		if (it->get() == job)
		{
			if (job->timer)
				g_source_remove(job->timer);
			mKeyRefreshJobs.erase(it);
			return;
		}
	}
}
