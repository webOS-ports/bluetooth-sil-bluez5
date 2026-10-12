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

// The local GATT server's object paths and value offsets. The ids were parsed from the last character of each path
// component, and offsets were ignored.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gattlocal.h"

namespace {

using Bytes = std::vector<uint8_t>;
const std::string base = "/org/bluez/hci0/server";

} // namespace

TEST(GattLocalPath, ServiceCharacteristicDescriptor)
{
	LocalGattIds ids;
	ASSERT_TRUE(parseLocalGattPath(base + "/App1/Service2", ids));
	EXPECT_EQ(ids.appId, 1);
	EXPECT_EQ(ids.serviceId, 2);
	EXPECT_EQ(ids.charId, 0);
	EXPECT_EQ(ids.descId, 0);

	ASSERT_TRUE(parseLocalGattPath(base + "/App1/Service2/Char3", ids));
	EXPECT_EQ(ids.charId, 3);

	ASSERT_TRUE(parseLocalGattPath(base + "/App1/Service2/Char3/Desc4", ids));
	EXPECT_EQ(ids.descId, 4);
}

TEST(GattLocalPath, IdsOfMoreThanOneDigit)
{
	// App12/Service13 was app 2, service 3.
	LocalGattIds ids;
	ASSERT_TRUE(parseLocalGattPath(base + "/App12/Service13/Char140/Desc65535", ids));
	EXPECT_EQ(ids.appId, 12);
	EXPECT_EQ(ids.serviceId, 13);
	EXPECT_EQ(ids.charId, 140);
	EXPECT_EQ(ids.descId, 65535);
}

TEST(GattLocalPath, Rejected)
{
	LocalGattIds ids;
	ids.appId = 77;
	for (const char *path : {"", "/", "/App1", "/Service2", "/App1/Char3", "/App/Service2", "/App1/Service",
	                         "/App1/Service0", "/App1/Service65536", "/App1/Service2x", "/App1/Service-2",
	                         "/App1/Service2/Desc4", "/App1/Service2/Char3/Desc4/Extra"})
	{
		EXPECT_FALSE(parseLocalGattPath(base + path, ids)) << path;
		EXPECT_EQ(ids.appId, 77) << path;	// left alone
	}
}

TEST(GattLocalValue, ReadFromOffset)
{
	Bytes out;
	ASSERT_TRUE(gattValueFromOffset({1, 2, 3, 4}, 0, out));
	EXPECT_EQ(out, (Bytes{1, 2, 3, 4}));
	ASSERT_TRUE(gattValueFromOffset({1, 2, 3, 4}, 3, out));
	EXPECT_EQ(out, (Bytes{4}));
	ASSERT_TRUE(gattValueFromOffset({1, 2, 3, 4}, 4, out));
	EXPECT_TRUE(out.empty());
	EXPECT_FALSE(gattValueFromOffset({1, 2, 3, 4}, 5, out));
	EXPECT_FALSE(gattValueFromOffset({}, 1, out));
}

TEST(GattLocalValue, LongWriteInFragments)
{
	// A long write: each fragment lands at its offset. Every fragment used to replace the whole value.
	Bytes value = {9, 9, 9, 9, 9, 9, 9};
	ASSERT_TRUE(gattValueWriteAt(value, 0, {1, 2, 3}, value));
	ASSERT_TRUE(gattValueWriteAt(value, 3, {4, 5, 6}, value));
	ASSERT_TRUE(gattValueWriteAt(value, 6, {7}, value));
	EXPECT_EQ(value, (Bytes{1, 2, 3, 4, 5, 6, 7}));

	Bytes out;
	EXPECT_FALSE(gattValueWriteAt({1, 2}, 3, {4}, out));
	ASSERT_TRUE(gattValueWriteAt({1, 2}, 2, {}, out));
	EXPECT_EQ(out, (Bytes{1, 2}));
}
