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

// Times handed to BlueZ's MPRIS player: milliseconds in, microseconds out, saturating.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "mpristime.h"

TEST(MprisTime, MillisecondsToMicroseconds)
{
	EXPECT_EQ(msToMprisMicroseconds(0), 0);
	EXPECT_EQ(msToMprisMicroseconds(1), 1000);
	EXPECT_EQ(msToMprisMicroseconds(240000), 240000000);	// four minutes
}

TEST(MprisTime, Saturates)
{
	const int64_t max = std::numeric_limits<int64_t>::max();
	EXPECT_EQ(msToMprisMicroseconds(std::numeric_limits<uint64_t>::max()), max);
	EXPECT_EQ(msToMprisMicroseconds((uint64_t)max / 1000 + 1), max);
	EXPECT_EQ(msToMprisMicroseconds((uint64_t)max / 1000), (int64_t)((uint64_t)max / 1000 * 1000));
}
