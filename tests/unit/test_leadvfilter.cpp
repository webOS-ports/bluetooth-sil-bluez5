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

// LE scan filters from an app against what a device in range advertises. The lengths of the filter, its mask and
// the device's data are independent of each other; under AddressSanitizer these also catch reads past any of them.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "leadvfilter.h"

namespace {

using Bytes = std::vector<uint8_t>;
const std::string heartRate = "0000180d-0000-1000-8000-00805f9b34fb";
const std::string battery = "0000180f-0000-1000-8000-00805f9b34fb";

} // namespace

TEST(LeAdvFilter, ServiceUuidExact)
{
	EXPECT_TRUE(matchServiceUuid(heartRate, "", {battery, heartRate}));
	EXPECT_TRUE(matchServiceUuid("0000180D-0000-1000-8000-00805F9B34FB", "", {heartRate}));
	EXPECT_FALSE(matchServiceUuid(heartRate, "", {battery}));
	EXPECT_FALSE(matchServiceUuid(heartRate, "", {}));
}

TEST(LeAdvFilter, ServiceUuidShortForm)
{
	EXPECT_TRUE(matchServiceUuid("180d", "", {battery, heartRate}));
	EXPECT_TRUE(matchServiceUuid("180D", "", {heartRate}));
	EXPECT_TRUE(matchServiceUuid("0000180d", "", {heartRate}));
	EXPECT_FALSE(matchServiceUuid("180f", "", {heartRate}));
	EXPECT_FALSE(matchServiceUuid("18g0", "", {heartRate}));
}

TEST(LeAdvFilter, ServiceUuidMasked)
{
	// Only the first 8 characters count.
	std::string mask(8, '1');
	mask.append(heartRate.size() - 8, '0');
	EXPECT_TRUE(matchServiceUuid(heartRate, mask, {"0000180D-ffff-ffff-ffff-ffffffffffff"}));
	EXPECT_FALSE(matchServiceUuid(heartRate, mask, {battery}));

	// A mismatch at the last masked position: this used to match.
	std::string all(heartRate.size(), '1');
	std::string lastDiffers = heartRate.substr(0, heartRate.size() - 1) + "c";
	EXPECT_FALSE(matchServiceUuid(heartRate, all, {lastDiffers}));
	EXPECT_TRUE(matchServiceUuid(heartRate, all, {heartRate}));
}

TEST(LeAdvFilter, ServiceUuidShortSides)
{
	std::string all(heartRate.size(), '1');
	// A device UUID or a mask shorter than the filter's UUID never matches.
	EXPECT_FALSE(matchServiceUuid(heartRate, all, {"180d"}));
	EXPECT_FALSE(matchServiceUuid(heartRate, "11", {heartRate}));
	EXPECT_TRUE(matchServiceUuid("0000", all, {heartRate}));
}

TEST(LeAdvFilter, ServiceData)
{
	EXPECT_TRUE(matchServiceData(heartRate, {1, 2, 3}, {}, heartRate, {1, 2, 3}));
	EXPECT_FALSE(matchServiceData(heartRate, {1, 2, 3}, {}, heartRate, {1, 2, 3, 4}));
	EXPECT_FALSE(matchServiceData(heartRate, {1, 2, 3}, {}, battery, {1, 2, 3}));

	// Masked: equal where the mask is 1, longer device data allowed.
	EXPECT_TRUE(matchServiceData(heartRate, {1, 9, 3}, {1, 0, 1}, heartRate, {1, 2, 3, 4}));
	EXPECT_FALSE(matchServiceData(heartRate, {1, 9, 3}, {1, 1, 1}, heartRate, {1, 2, 3}));
	// Device data or mask shorter than the filter's data.
	EXPECT_FALSE(matchServiceData(heartRate, {1, 2, 3}, {1, 1, 1}, heartRate, {1, 2}));
	EXPECT_FALSE(matchServiceData(heartRate, {1, 2, 3}, {1}, heartRate, {1, 2, 3}));
}

TEST(LeAdvFilter, ManufacturerData)
{
	// Id 0x004c, high byte first, then the payload.
	EXPECT_TRUE(matchManufacturerData(0x004c, {2, 0x15}, {}, {0x00, 0x4c, 2, 0x15, 7}));
	EXPECT_FALSE(matchManufacturerData(0x004c, {2, 0x16}, {}, {0x00, 0x4c, 2, 0x15}));
	EXPECT_FALSE(matchManufacturerData(0x0075, {}, {}, {0x00, 0x4c, 2}));
	EXPECT_TRUE(matchManufacturerData(0x004c, {}, {}, {0x00, 0x4c, 2}));

	// Masked.
	EXPECT_TRUE(matchManufacturerData(0x004c, {2, 0xff}, {1, 0}, {0x00, 0x4c, 2, 0x15}));
	EXPECT_FALSE(matchManufacturerData(0x004c, {3, 0xff}, {1, 0}, {0x00, 0x4c, 2, 0x15}));
}

TEST(LeAdvFilter, ManufacturerDataShortSides)
{
	// Id only, or less: no payload to match.
	EXPECT_FALSE(matchManufacturerData(0x004c, {}, {}, {0x00, 0x4c}));
	EXPECT_FALSE(matchManufacturerData(0x004c, {}, {}, {0x4c}));
	EXPECT_FALSE(matchManufacturerData(0x004c, {}, {}, {}));
	// Payload or mask shorter than the filter's data.
	EXPECT_FALSE(matchManufacturerData(0x004c, {2, 0x15, 7}, {}, {0x00, 0x4c, 2, 0x15}));
	EXPECT_FALSE(matchManufacturerData(0x004c, {2, 0x15}, {1}, {0x00, 0x4c, 2, 0x15}));
}
