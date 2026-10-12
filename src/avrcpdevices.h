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

// AVRCP state per remote device, kept apart so that it can be tested: which roles each device has connected and
// which of its players is the addressed one. The profile kept one pair of role flags, one connected address and one
// addressed player for all devices: with device A as target and B as controller, A's disconnect was never
// reported, and B's players and folder were reported under A's address.

#ifndef AVRCPDEVICES_H
#define AVRCPDEVICES_H

#include <list>
#include <map>
#include <string>

class AvrcpDevices
{
public:
	enum class Change { NONE, CONNECTED, DISCONNECTED };

	// One role of a device connected or went: controller is the local controller role (the remote's target
	// connected), otherwise the local target role. CONNECTED when the first of the device's roles came,
	// DISCONNECTED when its last one went, NONE otherwise.
	Change setRole(const std::string &address, bool controller, bool connected);

	bool isConnected(const std::string &address) const;
	bool isController(const std::string &address) const;
	bool isTarget(const std::string &address) const;
	bool anyController() const;
	bool anyTarget() const;

	// BlueZ addresses one player per device. A player made addressed replaces only its own device's.
	void setAddressedPlayer(const std::string &address, const std::string &playerPath);
	std::string addressedPlayer(const std::string &address) const;
	// A player went: it is no longer addressed, wherever it was.
	void removePlayer(const std::string &playerPath);

	// The device whose player was addressed last and still is: the one that the calls naming no device (browsing,
	// player application settings) go to. Empty if none.
	std::string currentDevice() const;

	// Addresses are compared without regard to case.
	static std::string key(const std::string &address);

private:
	struct Roles
	{
		bool controller = false;
		bool target = false;
	};
	std::map<std::string, Roles> mRoles;
	std::map<std::string, std::string> mAddressedPlayers;
	std::list<std::string> mAddressedOrder;	// devices, the one addressed last at the back
};

#endif
