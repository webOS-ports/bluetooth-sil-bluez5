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

#include "hfpuuid.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace hfpuuid {

namespace {

bool same(std::string_view a, std::string_view b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
		return std::tolower(x) == std::tolower(y);
	});
}

bool has(const std::vector<std::string> &uuids, std::string_view uuid)
{
	return std::ranges::any_of(uuids, [uuid](const std::string &u) { return same(u, uuid); });
}

}

std::string connectUuid(const std::vector<std::string> &localUuids, const std::vector<std::string> &remoteUuids)
{
	if (has(remoteUuids, HF) && has(localUuids, AG))
		return std::string(HF);
	return std::string(AG);
}

std::string disconnectUuid(const std::string &connectedUuid, const std::vector<std::string> &localUuids,
                           const std::vector<std::string> &remoteUuids)
{
	if (same(connectedUuid, HF))
		return std::string(HF);
	if (same(connectedUuid, AG))
		return std::string(AG);
	return connectUuid(localUuids, remoteUuids);
}

}
