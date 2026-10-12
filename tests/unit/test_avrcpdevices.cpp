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

#include "avrcpdevices.h"

static const std::string A = "00:11:22:33:44:55";
static const std::string B = "66:77:88:99:AA:BB";
static const std::string PA1 = "/org/bluez/hci0/dev_00_11_22_33_44_55/player0";
static const std::string PA2 = "/org/bluez/hci0/dev_00_11_22_33_44_55/player1";
static const std::string PB1 = "/org/bluez/hci0/dev_66_77_88_99_AA_BB/player0";

using Change = AvrcpDevices::Change;

TEST(AvrcpDevices, OneDeviceBothRoles)
{
	AvrcpDevices d;
	EXPECT_FALSE(d.isConnected(A));
	EXPECT_EQ(d.setRole(A, true, true), Change::CONNECTED);
	EXPECT_EQ(d.setRole(A, false, true), Change::NONE);	// its second role: no second "connected"
	EXPECT_TRUE(d.isController(A));
	EXPECT_TRUE(d.isTarget(A));
	EXPECT_EQ(d.setRole(A, true, false), Change::NONE);	// one role left
	EXPECT_TRUE(d.isConnected(A));
	EXPECT_FALSE(d.isController(A));
	EXPECT_EQ(d.setRole(A, false, false), Change::DISCONNECTED);
	EXPECT_FALSE(d.isConnected(A));
	EXPECT_FALSE(d.anyController());
	EXPECT_FALSE(d.anyTarget());
}

TEST(AvrcpDevices, RepeatedDisconnectReportsOnce)
{
	AvrcpDevices d;
	EXPECT_EQ(d.setRole(A, true, false), Change::NONE);	// never connected
	EXPECT_FALSE(d.isConnected(A));
	EXPECT_EQ(d.setRole(A, true, true), Change::CONNECTED);
	EXPECT_EQ(d.setRole(A, true, true), Change::NONE);
	EXPECT_EQ(d.setRole(A, true, false), Change::DISCONNECTED);
	EXPECT_EQ(d.setRole(A, true, false), Change::NONE);
}

// A as target and B as controller: with one flag pair for all devices, A's disconnect was hidden by B.
TEST(AvrcpDevices, RolesOfTwoDevicesDoNotMix)
{
	AvrcpDevices d;
	EXPECT_EQ(d.setRole(A, false, true), Change::CONNECTED);
	EXPECT_EQ(d.setRole(B, true, true), Change::CONNECTED);
	EXPECT_TRUE(d.isTarget(A));
	EXPECT_FALSE(d.isController(A));
	EXPECT_TRUE(d.isController(B));
	EXPECT_FALSE(d.isTarget(B));
	EXPECT_TRUE(d.anyController());
	EXPECT_TRUE(d.anyTarget());

	EXPECT_EQ(d.setRole(A, false, false), Change::DISCONNECTED);
	EXPECT_FALSE(d.isConnected(A));
	EXPECT_TRUE(d.isConnected(B));
	EXPECT_FALSE(d.anyTarget());
	EXPECT_TRUE(d.anyController());
}

TEST(AvrcpDevices, AddressCaseDoesNotMatter)
{
	AvrcpDevices d;
	d.setRole("66:77:88:99:aa:bb", true, true);
	EXPECT_TRUE(d.isConnected(B));
	EXPECT_TRUE(d.isController("66:77:88:99:aa:bb"));
	d.setAddressedPlayer("66:77:88:99:aa:bb", PB1);
	EXPECT_EQ(d.addressedPlayer(B), PB1);
	EXPECT_EQ(d.currentDevice(), B);
	EXPECT_EQ(d.setRole(B, true, false), Change::DISCONNECTED);
}

TEST(AvrcpDevices, EmptyAddressIsNoDevice)
{
	AvrcpDevices d;
	EXPECT_EQ(d.setRole("", true, true), Change::NONE);
	EXPECT_FALSE(d.isConnected(""));
	EXPECT_FALSE(d.anyController());
	d.setAddressedPlayer("", PA1);
	EXPECT_EQ(d.currentDevice(), "");
}

// One device: the player added last is the addressed one, as with the single pointer before.
TEST(AvrcpDevices, OneDeviceAddressedPlayer)
{
	AvrcpDevices d;
	EXPECT_EQ(d.addressedPlayer(A), "");
	EXPECT_EQ(d.currentDevice(), "");
	d.setAddressedPlayer(A, PA1);
	d.setAddressedPlayer(A, PA2);
	EXPECT_EQ(d.addressedPlayer(A), PA2);
	EXPECT_EQ(d.currentDevice(), A);
	d.removePlayer(PA1);	// not the addressed one
	EXPECT_EQ(d.addressedPlayer(A), PA2);
	d.removePlayer(PA2);	// the addressed one: none, no fallback to another player
	EXPECT_EQ(d.addressedPlayer(A), "");
	EXPECT_EQ(d.currentDevice(), "");
}

// B's new player was taken as the addressed player of A as well, and A's was un-addressed.
TEST(AvrcpDevices, AddressedPlayerPerDevice)
{
	AvrcpDevices d;
	d.setAddressedPlayer(A, PA1);
	d.setAddressedPlayer(B, PB1);
	EXPECT_EQ(d.addressedPlayer(A), PA1);
	EXPECT_EQ(d.addressedPlayer(B), PB1);
	EXPECT_EQ(d.currentDevice(), B);

	d.setAddressedPlayer(A, PA2);	// A addressed again: A is current
	EXPECT_EQ(d.currentDevice(), A);
	EXPECT_EQ(d.addressedPlayer(B), PB1);

	d.removePlayer(PA2);	// back to the device addressed before
	EXPECT_EQ(d.currentDevice(), B);
	EXPECT_EQ(d.addressedPlayer(A), "");
	d.removePlayer(PB1);
	EXPECT_EQ(d.currentDevice(), "");
}

TEST(AvrcpDevices, RolesAndPlayersAreIndependent)
{
	AvrcpDevices d;
	// BlueZ can show a player before the profile's connection is reported, and keep it after.
	d.setAddressedPlayer(A, PA1);
	EXPECT_FALSE(d.isConnected(A));
	d.setRole(A, true, true);
	d.setRole(A, true, false);
	EXPECT_EQ(d.addressedPlayer(A), PA1);
	EXPECT_EQ(d.currentDevice(), A);
}
