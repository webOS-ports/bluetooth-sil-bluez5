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

#include "leadvfilter.h"

#include <algorithm>
#include <cctype>

namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

} // namespace

// Every index is bounded by both sides. The device's data was indexed up to the filter's length, reading past it
// for a shorter advertisement, and the app's mask up to the data's length.

bool matchServiceUuid(const std::string &filterUuid, const std::string &filterMask,
                      const std::vector<std::string> &deviceUuids)
{
	std::string wanted = lower(filterUuid);

	for (auto &uuid : deviceUuids)
	{
		std::string deviceUuid = lower(uuid);
		if (filterMask.empty())
		{
			if (deviceUuid == wanted)
				return true;
			continue;
		}

		if (wanted.size() > filterMask.size() || wanted.size() > deviceUuid.size())
			continue;

		// A mismatch at the last position still returned true.
		bool isMatching = true;
		for (size_t j = 0; j < wanted.size() && isMatching; j++)
			if (filterMask[j] == '1' && wanted[j] != deviceUuid[j])
				isMatching = false;
		if (isMatching)
			return true;
	}
	return false;
}

bool matchServiceData(const std::string &filterUuid, const std::vector<uint8_t> &filterData,
                      const std::vector<uint8_t> &filterMask,
                      const std::string &deviceDataUuid, const std::vector<uint8_t> &deviceData)
{
	if (lower(deviceDataUuid) != lower(filterUuid))
		return false;

	if (filterMask.empty())
		return filterData == deviceData;

	if (deviceData.size() < filterData.size() || filterMask.size() < filterData.size())
		return false;

	for (size_t j = 0; j < filterData.size(); j++)
		if (filterMask[j] == 1 && filterData[j] != deviceData[j])
			return false;
	return true;
}

bool matchManufacturerData(uint16_t filterId, const std::vector<uint8_t> &filterData,
                           const std::vector<uint8_t> &filterMask, const std::vector<uint8_t> &deviceData)
{
	if (deviceData.size() <= 2)
		return false;

	uint16_t deviceId = (uint16_t)((deviceData[0] << 8) | deviceData[1]);
	if (filterId != deviceId)
		return false;

	bool masked = !filterMask.empty();
	if (deviceData.size() - 2 < filterData.size() || (masked && filterMask.size() < filterData.size()))
		return false;

	for (size_t j = 0; j < filterData.size(); j++)
		if ((!masked || filterMask[j] == 1) && filterData[j] != deviceData[j + 2])
			return false;
	return true;
}
