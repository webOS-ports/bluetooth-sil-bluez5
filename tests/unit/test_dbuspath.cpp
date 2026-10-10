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

#include "dbuspath.h"

TEST(DBusPath, Under)
{
	EXPECT_TRUE(isObjectPathUnder("/org/bluez/hci1", "/org/bluez/hci1"));
	EXPECT_TRUE(isObjectPathUnder("/org/bluez/hci1/dev_00_11_22_33_44_55", "/org/bluez/hci1"));
	EXPECT_TRUE(isObjectPathUnder("/org/bluez/hci1/dev_00_11_22_33_44_55/service0001", "/org/bluez/hci1"));
	EXPECT_TRUE(isObjectPathUnder("/org/bluez", "/"));
}

TEST(DBusPath, NotUnder)
{
	// The eleventh adapter's objects are not the second's.
	EXPECT_FALSE(isObjectPathUnder("/org/bluez/hci10", "/org/bluez/hci1"));
	EXPECT_FALSE(isObjectPathUnder("/org/bluez/hci10/dev_00_11_22_33_44_55", "/org/bluez/hci1"));
	// Containing the adapter's path elsewhere is not enough.
	EXPECT_FALSE(isObjectPathUnder("/x/org/bluez/hci1/dev_00", "/org/bluez/hci1"));
	EXPECT_FALSE(isObjectPathUnder("/org/bluez", "/org/bluez/hci1"));
	EXPECT_FALSE(isObjectPathUnder("/org/bluez/hci1", ""));
}
