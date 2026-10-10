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

#include "gattlocal.h"

namespace {

// "<prefix><digits>": the number, 1 to 65535.
bool parseId(const std::string &segment, const std::string &prefix, uint16_t &id)
{
	if (segment.size() <= prefix.size() || segment.compare(0, prefix.size(), prefix) != 0)
		return false;
	unsigned long value = 0;
	for (size_t i = prefix.size(); i < segment.size(); i++)
	{
		char c = segment[i];
		if (c < '0' || c > '9')
			return false;
		value = value * 10 + (unsigned long)(c - '0');
		if (value > 0xffff)
			return false;
	}
	if (value == 0)
		return false;
	id = (uint16_t)value;
	return true;
}

} // namespace

// The ids were taken from the last character of each path component: App12/Service13 was app 2, service 3.
bool parseLocalGattPath(const std::string &path, LocalGattIds &ids)
{
	std::vector<std::string> segments;
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		segments.push_back(path.substr(start, end - start));
		start = end + 1;
	}

	// From the end: [Desc] [Char] Service App, the manager's path before them.
	LocalGattIds parsed;
	size_t i = segments.size();
	if (i && parseId(segments[i - 1], "Desc", parsed.descId))
		i--;
	if (i && parseId(segments[i - 1], "Char", parsed.charId))
		i--;
	else if (parsed.descId)
		return false;	// a descriptor belongs to a characteristic
	if (i < 2 || !parseId(segments[i - 1], "Service", parsed.serviceId) || !parseId(segments[i - 2], "App", parsed.appId))
		return false;

	ids = parsed;
	return true;
}

bool gattValueFromOffset(const std::vector<uint8_t> &value, size_t offset, std::vector<uint8_t> &out)
{
	if (offset > value.size())
		return false;
	out.assign(value.begin() + (std::ptrdiff_t)offset, value.end());
	return true;
}

bool gattValueWriteAt(const std::vector<uint8_t> &value, size_t offset, const std::vector<uint8_t> &data,
                      std::vector<uint8_t> &out)
{
	if (offset > value.size())
		return false;
	out.assign(value.begin(), value.begin() + (std::ptrdiff_t)offset);
	out.insert(out.end(), data.begin(), data.end());
	return true;
}
