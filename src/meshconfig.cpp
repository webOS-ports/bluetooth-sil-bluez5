// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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

#include "meshconfig.h"

namespace {

const uint32_t OP_GENERIC_ONOFF_STATUS = 0x8204;
const int32_t MAX_WAIT_SECONDS = 3600;

uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

void putLe16(uint16_t value, uint8_t *p)
{
	p[0] = (uint8_t)(value & 0xff);
	p[1] = (uint8_t)(value >> 8);
}

// Unicast addresses are 0x0001-0x7fff; 0x0000 is unassigned, 0x8000 and up virtual and group addresses.
bool isUnicast(uint16_t address)
{
	return address != 0 && address < 0x8000;
}

uint32_t normalisedModelId(uint32_t modelId)
{
	return isVendorModelId(modelId) ? modelId : (modelId & 0xffff);
}

} // namespace

BluetoothError meshStatusToError(uint8_t status)
{
	switch (status)
	{
	case 0x00:	// success
		return BLUETOOTH_ERROR_NONE;
	case 0x03:	// invalid AppKey index
		return BLUETOOTH_ERROR_MESH_APP_KEY_INDEX_DOES_NOT_EXIST;
	case 0x04:	// invalid NetKey index
		return BLUETOOTH_ERROR_MESH_NET_KEY_INDEX_DOES_NOT_EXIST;
	case 0x05:	// insufficient resources
		return BLUETOOTH_ERROR_MESH_OUT_OF_RESOURCES;
	case 0x06:	// key index already stored (with another key)
		return BLUETOOTH_ERROR_MESH_APP_KEY_INDEX_ALREADY_EXISTS;
	case 0x0b:	// cannot update
		return BLUETOOTH_ERROR_MESH_CANNOT_UPDATE_APPKEY;
	default:	// invalid address or model, cannot bind or remove, storage failure, ...
		return BLUETOOTH_ERROR_FAIL;
	}
}

BluetoothError meshAttachErrorToError(const char *dbusErrorName)
{
	if (!dbusErrorName)
		return BLUETOOTH_ERROR_FAIL;
	const std::string name(dbusErrorName);
	if (name == "org.bluez.mesh.Error.NotFound")
		return BLUETOOTH_ERROR_MESH_NODE_NOT_AVAILABLE;
	if (name == "org.bluez.mesh.Error.Busy")
		return BLUETOOTH_ERROR_BUSY;
	return BLUETOOTH_ERROR_FAIL;
}

bool parseAppKeyStatus(const uint8_t *data, size_t len, MeshAppKeyStatus &out)
{
	if (!data || len != 4)
		return false;
	out.status = data[0];
	out.netKeyIndex = le16(data + 1) & 0xfff;
	out.appKeyIndex = le16(data + 2) >> 4;
	return true;
}

bool parseNetKeyStatus(const uint8_t *data, size_t len, MeshNetKeyStatus &out)
{
	if (!data || len != 3)
		return false;
	out.status = data[0];
	out.netKeyIndex = le16(data + 1) & 0xfff;
	return true;
}

bool parseModelAppStatus(const uint8_t *data, size_t len, MeshModelAppStatus &out)
{
	if (!data || (len != 7 && len != 9))
		return false;
	out.status = data[0];
	out.elementAddress = le16(data + 1);
	out.appKeyIndex = le16(data + 3) & 0xfff;
	if (len == 9)	// vendor model: company id, then the model id
		out.modelId = (uint32_t)le16(data + 5) << 16 | le16(data + 7);
	else
		out.modelId = le16(data + 5);
	return true;
}

bool isVendorModelId(uint32_t modelId)
{
	uint32_t company = modelId >> 16;
	return company != 0 && company != 0xffff;
}

size_t putModelId(uint8_t *buf, uint32_t modelId)
{
	if (isVendorModelId(modelId))
	{
		putLe16((uint16_t)(modelId >> 16), buf);
		putLe16((uint16_t)(modelId & 0xffff), buf + 2);
		return 4;
	}
	putLe16((uint16_t)(modelId & 0xffff), buf);
	return 2;
}

bool meshKeysMatch(const MeshRequestKey &request, const MeshRequestKey &reply)
{
	if (request.hasNetKey && reply.hasNetKey && request.netKeyIndex != reply.netKeyIndex)
		return false;
	if (request.hasAppKey && reply.hasAppKey && request.appKeyIndex != reply.appKeyIndex)
		return false;
	if (request.hasModel && reply.hasModel && normalisedModelId(request.modelId) != normalisedModelId(reply.modelId))
		return false;
	return true;
}

bool isOnOffStatusFor(uint32_t opcode, uint16_t src, uint16_t dest)
{
	if (opcode != OP_GENERIC_ONOFF_STATUS)
		return false;
	return !isUnicast(dest) || src == dest;
}

int32_t clampWaitSeconds(int32_t seconds)
{
	if (seconds < 0)
		return 0;
	if (seconds > MAX_WAIT_SECONDS)
		return MAX_WAIT_SECONDS;
	return seconds;
}

unsigned int waitMilliseconds(int32_t seconds, unsigned int defaultMs)
{
	int32_t clamped = clampWaitSeconds(seconds);
	if (clamped == 0)
		return defaultMs;
	return (unsigned int)clamped * 1000u;
}

bool parseMeshToken(const std::string &token, uint64_t &out)
{
	if (token.empty() || token.size() > 20)	// UINT64_MAX has 20 digits
		return false;
	uint64_t value = 0;
	for (char c : token)
	{
		if (c < '0' || c > '9')
			return false;
		uint64_t digit = static_cast<uint64_t>(c - '0');
		if (value > (UINT64_MAX - digit) / 10)
			return false;
		value = value * 10 + digit;
	}
	out = value;
	return true;
}
