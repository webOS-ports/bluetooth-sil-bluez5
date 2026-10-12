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

// libFuzzer target for the local GATT server helpers: the offset of a ReadValue/WriteValue comes from the remote
// client. Input: [8 bytes offset] then the value (its second half also as a fragment written at the offset); the
// rest also as an object path.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gattlocal.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 8)
		return 0;
	uint64_t offset;
	memcpy(&offset, data, 8);
	std::vector<uint8_t> value(data + 8, data + size);
	std::vector<uint8_t> out;
	if (gattValueFromOffset(value, (size_t)offset, out))
	{
		if ((size_t)offset > value.size() || out.size() != value.size() - (size_t)offset)
			abort();
	}

	// The value split in two: the stored value and a fragment written at the offset.
	size_t half = value.size() / 2;
	std::vector<uint8_t> stored(value.begin(), value.begin() + (long)half), fragment(value.begin() + (long)half, value.end());
	std::vector<uint8_t> written;
	if (gattValueWriteAt(stored, (size_t)offset, fragment, written))
	{
		size_t at = (size_t)offset;
		if (at > stored.size() || written.size() < at + fragment.size() ||
		    !std::equal(stored.begin(), stored.begin() + (long)at, written.begin()) ||
		    !std::equal(fragment.begin(), fragment.end(), written.begin() + (long)at))
			abort();
	}

	LocalGattIds ids;
	parseLocalGattPath(std::string(reinterpret_cast<const char *>(data + 8), size - 8), ids);
	return 0;
}
