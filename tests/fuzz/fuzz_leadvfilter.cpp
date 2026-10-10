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

// libFuzzer target for the LE scan filter matchers. Input: [id hi] [id lo] then blocks, each [length] [length
// bytes]: filter data, filter mask, device data, filter UUID, filter UUID mask, device UUID. Missing blocks are
// empty, so every combination of lengths comes up.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "leadvfilter.h"

namespace {

std::vector<uint8_t> block(const uint8_t *&data, size_t &size)
{
	if (!size)
		return {};
	size_t n = *data++;
	size--;
	if (n > size)
		n = size;
	std::vector<uint8_t> out(data, data + n);
	data += n;
	size -= n;
	return out;
}

std::string str(const std::vector<uint8_t> &v)
{
	return std::string(v.begin(), v.end());
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 2)
		return 0;
	uint16_t id = (uint16_t)(data[0] << 8 | data[1]);
	data += 2;
	size -= 2;

	auto filterData = block(data, size);
	auto filterMask = block(data, size);
	auto deviceData = block(data, size);
	auto filterUuid = str(block(data, size));
	auto uuidMask = str(block(data, size));
	auto deviceUuid = str(block(data, size));

	matchManufacturerData(id, filterData, filterMask, deviceData);
	matchServiceData(filterUuid, filterData, filterMask, deviceUuid, deviceData);
	matchServiceUuid(filterUuid, uuidMask, {deviceUuid, filterUuid});
	return 0;
}
