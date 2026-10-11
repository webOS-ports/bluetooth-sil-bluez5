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

// Mesh configuration replies and requests without D-Bus: what nodes send back is parsed, matched and turned into
// errors here, so that it can be tested. Every byte of a reply comes from the node.

#ifndef MESHCONFIG_H
#define MESHCONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>

#include <bluetooth-sil-api.h>

// A Configuration status code (Mesh Profile 4.3.5, mesh/mesh-defs.h MESH_STATUS_*) as the SIL's error.
BluetoothError meshStatusToError(uint8_t status);

// Config AppKey Status body after the opcode: status, then the NetKey and AppKey indexes packed in 3 bytes.
struct MeshAppKeyStatus
{
	uint8_t status = 0;
	uint16_t netKeyIndex = 0;
	uint16_t appKeyIndex = 0;
};
bool parseAppKeyStatus(const uint8_t *data, size_t len, MeshAppKeyStatus &out);

// Config NetKey Status body after the opcode: status, then the NetKey index (12 bits of 2 bytes).
struct MeshNetKeyStatus
{
	uint8_t status = 0;
	uint16_t netKeyIndex = 0;
};
bool parseNetKeyStatus(const uint8_t *data, size_t len, MeshNetKeyStatus &out);

// Config Model App Status body after the opcode: status, element address, AppKey index, then a 2-byte SIG or a
// 4-byte vendor model id. modelId is in the stored form (see putModelId).
struct MeshModelAppStatus
{
	uint8_t status = 0;
	uint16_t elementAddress = 0;
	uint16_t appKeyIndex = 0;
	uint32_t modelId = 0;
};
bool parseModelAppStatus(const uint8_t *data, size_t len, MeshModelAppStatus &out);

// Whether a model id is a vendor model: SIG models are kept as the 16-bit id, or as 0xffff0000 | id (BlueZ's
// VENDOR_ID_MASK form); vendor models as company id << 16 | model id.
bool isVendorModelId(uint32_t modelId);

// A model id as a message carries it: 2 bytes for a SIG model, 4 (company id, then model id) for a vendor model.
// buf must hold 4 bytes. Returns the number written. A vendor id was cut to its low 2 bytes, so a bind or unbind
// went to a SIG model of that number.
size_t putModelId(uint8_t *buf, uint32_t modelId);

// The request fields a reply echoes, to tell apart requests to the same node that wait for the same status
// opcode (AppKey add, delete and update; bind and unbind). Unset fields match anything.
struct MeshRequestKey
{
	bool hasNetKey = false;
	uint16_t netKeyIndex = 0;
	bool hasAppKey = false;
	uint16_t appKeyIndex = 0;
	bool hasModel = false;
	uint32_t modelId = 0;
};
bool meshKeysMatch(const MeshRequestKey &request, const MeshRequestKey &reply);

// Whether a received message is the Generic OnOff Status answering a set sent to dest: the exact opcode (not with
// bit 8 masked off, which let 0x8304 through), and, for a unicast dest, from that node. A group or virtual dest may
// be answered by any of its members.
bool isOnOffStatusFor(uint32_t opcode, uint16_t src, uint16_t dest);

// A wait from the LS2 client (keyRefresh waitTimeout), in seconds, clamped to [0, 3600]. Negative values slept for
// about 136 years, large ones overflowed when made milliseconds.
int32_t clampWaitSeconds(int32_t seconds);

// An Attach meshd answered with an error, by the D-Bus error name (NULL when the call failed without a reply from
// meshd), as the SIL's error. Only org.bluez.mesh.Error.NotFound says that meshd knows no node for the token (meshd
// 5.86 mesh/mesh.c attach_call, mesh/node.c node_attach): that is BLUETOOTH_ERROR_MESH_NODE_NOT_AVAILABLE, on which
// the service gives the stored network up. Everything else (meshd not there, no reply, busy, the node in use, a
// failed application) leaves the network as it is: BLUETOOTH_ERROR_BUSY for Busy, else BLUETOOTH_ERROR_FAIL.
BluetoothError meshAttachErrorToError(const char *dbusErrorName);

// A mesh network token as the service stores it (std::to_string of the 64-bit token meshd gave): decimal digits
// only, within 64 bits. std::stoull threw on anything else, which took the service down, and took "-1" and " 1".
bool parseMeshToken(const std::string &token, uint64_t &out);

// The timer for a wait in seconds, in milliseconds; defaultMs when the wait is 0 or less.
unsigned int waitMilliseconds(int32_t seconds, unsigned int defaultMs);

#endif
