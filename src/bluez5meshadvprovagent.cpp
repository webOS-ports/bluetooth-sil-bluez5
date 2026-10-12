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
#include "bluez5meshadvprovagent.h"
#include "bluez5profilemesh.h"
#include "bluez5adapter.h"
#include "logging.h"
#include "utils.h"
#include <cstdint>
#include <string>

#define BLUEZ_MESH_AGENT_PATH "/agent"

Bluez5MeshAdvProvAgent::Bluez5MeshAdvProvAgent(Bluez5Adapter *adapter, Bluez5ProfileMesh *mesh) :
mAdapter(adapter),
mMesh(mesh),
supplyNumericInvokation(nullptr),
supplyStaticInvokation(nullptr)
{
}

#define BLUEZ_MESH_ERROR_FAILED "org.bluez.mesh.Error.Failed"

// meshd waits for the prompts it sent: refused, not left unanswered.
Bluez5MeshAdvProvAgent::~Bluez5MeshAdvProvAgent()
{
	if (supplyNumericInvokation)
		g_dbus_method_invocation_return_dbus_error(supplyNumericInvokation, BLUEZ_MESH_ERROR_FAILED, "Cancelled");
	if (supplyStaticInvokation)
		g_dbus_method_invocation_return_dbus_error(supplyStaticInvokation, BLUEZ_MESH_ERROR_FAILED, "Cancelled");
}

void Bluez5MeshAdvProvAgent::registerProvAgentInterface(GDBusObjectManagerServer *objectManagerServer)
{
	GDBusObjectSkeleton *agentSkeleton = g_dbus_object_skeleton_new(BLUEZ_MESH_AGENT_PATH);
	BluezMeshProvisionAgent1 *agentInterface = bluez_mesh_provision_agent1_skeleton_new();

	const char *capabilities[] = {"blink", "beep", "vibrate", "out-numeric", "out-alpha", "push",
							"twist", "in-numeric", "in-alpha", "static-oob", "public-oob"};

	GVariantBuilder *builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
	for(size_t i =0 ;i < (sizeof(capabilities)/sizeof(capabilities[0]));i++)
	{
		g_variant_builder_add(builder, "s", capabilities[i]);
	}
	GVariant *capVariant = g_variant_builder_end(builder);
	g_variant_builder_unref(builder);

	bluez_mesh_provision_agent1_set_capabilities(agentInterface, capVariant);

	g_signal_connect(agentInterface, "handle_display_numeric", G_CALLBACK(handleDisplayNumeric), this);
	g_signal_connect(agentInterface, "handle_display_string", G_CALLBACK(handleDisplayString), this);
	g_signal_connect(agentInterface, "handle_prompt_numeric", G_CALLBACK(handlePromptNumeric), this);
	g_signal_connect(agentInterface, "handle_prompt_static", G_CALLBACK(handlePromptStatic), this);

	g_dbus_object_skeleton_add_interface(agentSkeleton, G_DBUS_INTERFACE_SKELETON(agentInterface));
	g_dbus_object_manager_server_export(objectManagerServer, agentSkeleton);
	// Both hold their own references now: these leaked on every meshd start.
	g_object_unref(agentInterface);
	g_object_unref(agentSkeleton);
}

BluetoothError Bluez5MeshAdvProvAgent::supplyNumeric(uint32_t number)
{
	DEBUG("supplyNumeric");
	if (!supplyNumericInvokation)
	{
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}

	GVariant *reply = g_variant_new("(u)", number);
	g_dbus_method_invocation_return_value(supplyNumericInvokation, reply);
	supplyNumericInvokation = nullptr;
	return BLUETOOTH_ERROR_NONE;
}
BluetoothError Bluez5MeshAdvProvAgent::supplyStatic(const std::string &oobData)
{
	DEBUG("supplyStatic");
	if (!supplyStaticInvokation)
	{
		return BLUETOOTH_ERROR_NOT_ALLOWED;
	}
	uint8_t out[16] = {0};
	for (size_t i = 0; i < 16; i++)
	{
		if (i*2 < oobData.length()) sscanf(&oobData[i * 2], "%02hhx", &out[i]);
	}
	GBytes *bytes = g_bytes_new(out, 16);
	GVariant *oobVar = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, bytes, true);
	g_bytes_unref(bytes);	// the variant holds its own reference: this one leaked
	GVariant *tuple = g_variant_new_tuple(&oobVar, 1);
	g_dbus_method_invocation_return_value(supplyStaticInvokation, tuple);
	supplyStaticInvokation = nullptr;
	return BLUETOOTH_ERROR_NONE;
}

// meshd sends (su): with "i" in the introspection GDBus refused the call as InvalidArgs, and this never ran.
// Both display calls are answered: meshd waits for the reply, and got a NoReply error after the bus timeout.
gboolean Bluez5MeshAdvProvAgent::handleDisplayNumeric(BluezMeshProvisionAgent1 *object,
													  GDBusMethodInvocation *invocation,
													  const gchar */*argType*/,
													  guint argNumber,
													  gpointer userData)
{
	DEBUG("handleDisplayNumeric");
	bluez_mesh_provision_agent1_complete_display_numeric(object, invocation);
	Bluez5MeshAdvProvAgent *provAgent = (Bluez5MeshAdvProvAgent *)userData;
	provAgent->mMesh->getMeshObserver()->provisionResult(BLUETOOTH_ERROR_NONE,
													convertAddressToLowerCase(
														provAgent->mAdapter->getAddress()),
														"displayNumeric", "", argNumber, "", "", 0, 0, "");
	return true;
}

gboolean Bluez5MeshAdvProvAgent::handleDisplayString(BluezMeshProvisionAgent1 *object,
													 GDBusMethodInvocation *invocation,
													 const gchar *argValue,
													 gpointer userData)
{
	DEBUG("handleDisplayString");
	bluez_mesh_provision_agent1_complete_display_string(object, invocation);
	Bluez5MeshAdvProvAgent *provAgent = (Bluez5MeshAdvProvAgent *)userData;
	provAgent->mMesh->getMeshObserver()->provisionResult(BLUETOOTH_ERROR_NONE,
													convertAddressToLowerCase(
														provAgent->mAdapter->getAddress()),
														"displayString", argValue, 0, "", "", 0, 0, "");
	return true;
}

gboolean Bluez5MeshAdvProvAgent::handlePromptNumeric(BluezMeshProvisionAgent1 */*object*/,
													 GDBusMethodInvocation *invocation,
													 const gchar *argType,
													 gpointer userData)
{
	DEBUG("handlePromptNumeric");
	Bluez5MeshAdvProvAgent *provAgent = (Bluez5MeshAdvProvAgent *)userData;
	DEBUG("handlePromptNumeric: %s", argType);
	// A prompt still waiting was never answered when the next one replaced it.
	if (provAgent->supplyNumericInvokation)
		g_dbus_method_invocation_return_dbus_error(provAgent->supplyNumericInvokation, BLUEZ_MESH_ERROR_FAILED,
		                                           "Superseded");
	provAgent->supplyNumericInvokation = invocation;
	provAgent->mMesh->getMeshObserver()->provisionResult(BLUETOOTH_ERROR_NONE,
													convertAddressToLowerCase(
														provAgent->mAdapter->getAddress()),
														"promptNumeric", "", 0, argType, "", 0, 0, "");
	return true;
}

gboolean Bluez5MeshAdvProvAgent::handlePromptStatic(BluezMeshProvisionAgent1 */*object*/,
													GDBusMethodInvocation *invocation,
													const gchar *argType,
													gpointer userData)
{
	Bluez5MeshAdvProvAgent *provAgent = (Bluez5MeshAdvProvAgent *)userData;
	DEBUG("handlePromptStatic: %s", argType);
	if (provAgent->supplyStaticInvokation)	// as for PromptNumeric
		g_dbus_method_invocation_return_dbus_error(provAgent->supplyStaticInvokation, BLUEZ_MESH_ERROR_FAILED,
		                                           "Superseded");
	provAgent->supplyStaticInvokation = invocation;
	provAgent->mMesh->getMeshObserver()->provisionResult(BLUETOOTH_ERROR_NONE,
													convertAddressToLowerCase(
														provAgent->mAdapter->getAddress()),
														"promptStatic", "", 0, "", "", 0, 0, "");
	return true;
}