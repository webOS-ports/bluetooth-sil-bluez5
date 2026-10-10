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

#include "avrcpdevices.h"

#include <algorithm>
#include <cctype>

std::string AvrcpDevices::key(const std::string &address)
{
	std::string upper = address;
	std::transform(upper.begin(), upper.end(), upper.begin(),
		[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	return upper;
}

AvrcpDevices::Change AvrcpDevices::setRole(const std::string &address, bool controller, bool connected)
{
	if (address.empty())
		return Change::NONE;

	const std::string k = key(address);
	Roles &roles = mRoles[k];
	const bool wasConnected = roles.controller || roles.target;
	if (controller)
		roles.controller = connected;
	else
		roles.target = connected;
	const bool isNowConnected = roles.controller || roles.target;
	if (!isNowConnected)
		mRoles.erase(k);

	if (isNowConnected && !wasConnected)
		return Change::CONNECTED;
	if (!isNowConnected && wasConnected)
		return Change::DISCONNECTED;
	return Change::NONE;
}

bool AvrcpDevices::isConnected(const std::string &address) const
{
	return !address.empty() && mRoles.count(key(address));
}

bool AvrcpDevices::isController(const std::string &address) const
{
	auto it = mRoles.find(key(address));
	return it != mRoles.end() && it->second.controller;
}

bool AvrcpDevices::isTarget(const std::string &address) const
{
	auto it = mRoles.find(key(address));
	return it != mRoles.end() && it->second.target;
}

bool AvrcpDevices::anyController() const
{
	return std::any_of(mRoles.begin(), mRoles.end(), [](const auto &entry) { return entry.second.controller; });
}

bool AvrcpDevices::anyTarget() const
{
	return std::any_of(mRoles.begin(), mRoles.end(), [](const auto &entry) { return entry.second.target; });
}

void AvrcpDevices::setAddressedPlayer(const std::string &address, const std::string &playerPath)
{
	if (address.empty() || playerPath.empty())
		return;

	const std::string k = key(address);
	mAddressedPlayers[k] = playerPath;
	mAddressedOrder.remove(k);
	mAddressedOrder.push_back(k);
}

std::string AvrcpDevices::addressedPlayer(const std::string &address) const
{
	auto it = mAddressedPlayers.find(key(address));
	return it != mAddressedPlayers.end() ? it->second : std::string();
}

void AvrcpDevices::removePlayer(const std::string &playerPath)
{
	for (auto it = mAddressedPlayers.begin(); it != mAddressedPlayers.end();)
	{
		if (it->second == playerPath)
		{
			mAddressedOrder.remove(it->first);
			it = mAddressedPlayers.erase(it);
		}
		else
			++it;
	}
}

std::string AvrcpDevices::currentDevice() const
{
	return mAddressedOrder.empty() ? std::string() : mAddressedOrder.back();
}
