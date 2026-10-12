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
#include <gtest/gtest.h>

#include "gattdiscovery.h"

TEST(GattDiscovery, Steps)
{
	const int64_t limit = 15000;
	// Unknown device.
	EXPECT_EQ(gattDiscoveryStep(false, true, true, true, 0, limit), GattDiscoveryStep::Fail);
	// Resolved, with services: done at once, connected or not.
	EXPECT_EQ(gattDiscoveryStep(true, true, true, true, 0, limit), GattDiscoveryStep::Done);
	EXPECT_EQ(gattDiscoveryStep(true, false, true, true, 0, limit), GattDiscoveryStep::Done);
	// Right after a connect: nothing yet, or some services but not resolved: wait.
	EXPECT_EQ(gattDiscoveryStep(true, true, false, false, 0, limit), GattDiscoveryStep::Wait);
	EXPECT_EQ(gattDiscoveryStep(true, true, false, true, 100, limit), GattDiscoveryStep::Wait);
	// Resolved but none known yet (their objects still on the way): wait.
	EXPECT_EQ(gattDiscoveryStep(true, true, true, false, 100, limit), GattDiscoveryStep::Wait);
	// Out of time: what is known.
	EXPECT_EQ(gattDiscoveryStep(true, true, false, false, limit, limit), GattDiscoveryStep::Fail);
	EXPECT_EQ(gattDiscoveryStep(true, true, false, true, limit, limit), GattDiscoveryStep::Done);
	// Not connected: the cached services of a bonded device, or nothing.
	EXPECT_EQ(gattDiscoveryStep(true, false, false, true, 0, limit), GattDiscoveryStep::Done);
	EXPECT_EQ(gattDiscoveryStep(true, false, false, false, 0, limit), GattDiscoveryStep::Fail);
}
