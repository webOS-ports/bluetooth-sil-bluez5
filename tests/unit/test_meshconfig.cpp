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

// Configuration replies from mesh nodes, request matching, OnOff acks and client wait times. Reply bytes come
// from the node; under AddressSanitizer these also catch reads past them.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "meshconfig.h"

namespace {

using Bytes = std::vector<uint8_t>;

// A copy of exactly the right size, so that ASan sees any read past it.
template <typename Out, typename Parse>
bool parse(const Bytes &data, Out &out, Parse parser)
{
	auto *copy = new uint8_t[data.size() ? data.size() : 1];
	std::copy(data.begin(), data.end(), copy);
	bool ok = parser(copy, data.size(), out);
	delete[] copy;
	return ok;
}

} // namespace

TEST(MeshConfig, StatusCodes)
{
	EXPECT_EQ(meshStatusToError(0x00), BLUETOOTH_ERROR_NONE);
	EXPECT_EQ(meshStatusToError(0x03), BLUETOOTH_ERROR_MESH_APP_KEY_INDEX_DOES_NOT_EXIST);
	EXPECT_EQ(meshStatusToError(0x04), BLUETOOTH_ERROR_MESH_NET_KEY_INDEX_DOES_NOT_EXIST);
	EXPECT_EQ(meshStatusToError(0x06), BLUETOOTH_ERROR_MESH_APP_KEY_INDEX_ALREADY_EXISTS);
	EXPECT_EQ(meshStatusToError(0x0b), BLUETOOTH_ERROR_MESH_CANNOT_UPDATE_APPKEY);
	// Everything else that is not success is a failure, unknown codes included.
	for (int status = 1; status < 256; status++)
		EXPECT_NE(meshStatusToError((uint8_t)status), BLUETOOTH_ERROR_NONE) << status;
}

TEST(MeshConfig, AppKeyStatus)
{
	// Status 0x04, NetKey 0x123, AppKey 0x456: 23 61 45.
	MeshAppKeyStatus s;
	ASSERT_TRUE(parse(Bytes{0x04, 0x23, 0x61, 0x45}, s, parseAppKeyStatus));
	EXPECT_EQ(s.status, 0x04);
	EXPECT_EQ(s.netKeyIndex, 0x123);
	EXPECT_EQ(s.appKeyIndex, 0x456);
	for (size_t len : {0u, 1u, 3u, 5u})
		EXPECT_FALSE(parse(Bytes(len, 0), s, parseAppKeyStatus)) << len;
}

TEST(MeshConfig, NetKeyStatus)
{
	MeshNetKeyStatus s;
	ASSERT_TRUE(parse(Bytes{0x00, 0x34, 0xf2}, s, parseNetKeyStatus));
	EXPECT_EQ(s.status, 0x00);
	EXPECT_EQ(s.netKeyIndex, 0x234);	// 12 bits
	EXPECT_FALSE(parse(Bytes{0x00, 0x34}, s, parseNetKeyStatus));
	EXPECT_FALSE(parse(Bytes{0x00, 0x34, 0x02, 0x00}, s, parseNetKeyStatus));
}

TEST(MeshConfig, ModelAppStatusSigAndVendor)
{
	MeshModelAppStatus s;
	// Status 0x02, element 0x0102, AppKey 0x001, SIG model 0x1000.
	ASSERT_TRUE(parse(Bytes{0x02, 0x02, 0x01, 0x01, 0x00, 0x00, 0x10}, s, parseModelAppStatus));
	EXPECT_EQ(s.status, 0x02);
	EXPECT_EQ(s.elementAddress, 0x0102);
	EXPECT_EQ(s.appKeyIndex, 0x001);
	EXPECT_EQ(s.modelId, 0x1000u);
	// Vendor: company 0x05f1, model 0x0042.
	ASSERT_TRUE(parse(Bytes{0x00, 0x02, 0x01, 0x01, 0x00, 0xf1, 0x05, 0x42, 0x00}, s, parseModelAppStatus));
	EXPECT_EQ(s.modelId, 0x05f10042u);
	for (size_t len : {0u, 6u, 8u, 10u})
		EXPECT_FALSE(parse(Bytes(len, 0), s, parseModelAppStatus)) << len;
}

TEST(MeshConfig, PutModelId)
{
	uint8_t buf[4] = {0xee, 0xee, 0xee, 0xee};
	EXPECT_EQ(putModelId(buf, 0x1000), 2u);
	EXPECT_EQ(buf[0], 0x00);
	EXPECT_EQ(buf[1], 0x10);
	EXPECT_EQ(buf[2], 0xee);	// nothing past the 2 bytes

	// BlueZ's SIG form, 0xffff0000 | id, is still a SIG model.
	EXPECT_EQ(putModelId(buf, 0xffff1001), 2u);
	EXPECT_EQ(buf[0], 0x01);
	EXPECT_EQ(buf[1], 0x10);

	// A vendor model takes 4 bytes, company first: it was cut to 2 and named a SIG model.
	EXPECT_EQ(putModelId(buf, 0x05f10042), 4u);
	EXPECT_EQ(buf[0], 0xf1);
	EXPECT_EQ(buf[1], 0x05);
	EXPECT_EQ(buf[2], 0x42);
	EXPECT_EQ(buf[3], 0x00);

	EXPECT_FALSE(isVendorModelId(0x1000));
	EXPECT_FALSE(isVendorModelId(0xffff1000));
	EXPECT_TRUE(isVendorModelId(0x05f10042));
}

TEST(MeshConfig, KeysTellRequestsApart)
{
	MeshRequestKey add;
	add.hasNetKey = add.hasAppKey = true;
	add.netKeyIndex = 0;
	add.appKeyIndex = 1;
	MeshRequestKey del = add;
	del.appKeyIndex = 2;

	MeshRequestKey reply;
	reply.hasNetKey = reply.hasAppKey = true;
	reply.netKeyIndex = 0;
	reply.appKeyIndex = 2;
	EXPECT_FALSE(meshKeysMatch(add, reply));
	EXPECT_TRUE(meshKeysMatch(del, reply));
	reply.netKeyIndex = 1;
	EXPECT_FALSE(meshKeysMatch(del, reply));	// another subnet

	// Unknown on either side: matches, as before.
	EXPECT_TRUE(meshKeysMatch(MeshRequestKey(), reply));
	EXPECT_TRUE(meshKeysMatch(add, MeshRequestKey()));

	// A Model App Status echoes the AppKey and model, not the NetKey; SIG ids in either form.
	MeshRequestKey bind = add;
	bind.hasModel = true;
	bind.modelId = 0x1000;
	MeshRequestKey bindReply;
	bindReply.hasAppKey = bindReply.hasModel = true;
	bindReply.appKeyIndex = 1;
	bindReply.modelId = 0xffff1000;
	EXPECT_TRUE(meshKeysMatch(bind, bindReply));
	bindReply.modelId = 0x1001;
	EXPECT_FALSE(meshKeysMatch(bind, bindReply));
	bindReply.modelId = 0x1000;
	bindReply.appKeyIndex = 3;
	EXPECT_FALSE(meshKeysMatch(bind, bindReply));
}

TEST(MeshConfig, OnOffStatusMatching)
{
	EXPECT_TRUE(isOnOffStatusFor(0x8204, 0x0005, 0x0005));
	EXPECT_FALSE(isOnOffStatusFor(0x8204, 0x0006, 0x0005));	// another node
	EXPECT_FALSE(isOnOffStatusFor(0x8304, 0x0005, 0x0005));	// 0x8304 & ~0x0100 == 0x8204: was taken
	EXPECT_FALSE(isOnOffStatusFor(0x8202, 0x0005, 0x0005));	// a set, not a status
	EXPECT_TRUE(isOnOffStatusFor(0x8204, 0x0006, 0xc000));	// group: any member
	EXPECT_TRUE(isOnOffStatusFor(0x8204, 0x0006, 0x8001));	// virtual
}

TEST(MeshConfig, WaitTimes)
{
	EXPECT_EQ(clampWaitSeconds(-1), 0);
	EXPECT_EQ(clampWaitSeconds(INT32_MIN), 0);
	EXPECT_EQ(clampWaitSeconds(10), 10);
	EXPECT_EQ(clampWaitSeconds(INT32_MAX), 3600);
	EXPECT_EQ(waitMilliseconds(0, 2000), 2000u);
	EXPECT_EQ(waitMilliseconds(-5, 2000), 2000u);
	EXPECT_EQ(waitMilliseconds(3, 2000), 3000u);
	EXPECT_EQ(waitMilliseconds(3000000, 2000), 3600000u);	// was 3e9 in an int32
}

TEST(MeshConfig, NetworkToken)
{
	uint64_t token = 7;
	EXPECT_TRUE(parseMeshToken("0", token));
	EXPECT_EQ(token, 0u);
	EXPECT_TRUE(parseMeshToken("15004514395959625402", token));	// as the service stored one
	EXPECT_EQ(token, 15004514395959625402u);
	EXPECT_TRUE(parseMeshToken("18446744073709551615", token));
	EXPECT_EQ(token, UINT64_MAX);
	token = 7;
	EXPECT_FALSE(parseMeshToken("18446744073709551616", token));	// one past: stoull threw
	EXPECT_FALSE(parseMeshToken("99999999999999999999", token));
	EXPECT_FALSE(parseMeshToken("", token));
	EXPECT_FALSE(parseMeshToken("abc", token));	// stoull threw
	EXPECT_FALSE(parseMeshToken("-1", token));	// stoull took it as UINT64_MAX
	EXPECT_FALSE(parseMeshToken(" 1", token));
	EXPECT_FALSE(parseMeshToken("12x", token));	// stoull stopped at the x
	EXPECT_FALSE(parseMeshToken("000000000000000000001", token));	// 21 digits
	EXPECT_EQ(token, 7u);	// left alone on failure
}
