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

// The local GATT server's object paths and values, kept apart from the profile so that they can be tested: the
// paths come back from BlueZ with every remote write, and the offsets from the remote client.

#ifndef GATTLOCAL_H
#define GATTLOCAL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct LocalGattIds
{
	uint16_t appId = 0;
	uint16_t serviceId = 0;
	uint16_t charId = 0;
	uint16_t descId = 0;
};

// The ids in <manager path>/App<a>/Service<s>[/Char<c>[/Desc<d>]]. False for anything else, and for ids that are
// 0 or do not fit 16 bits.
bool parseLocalGattPath(const std::string &path, LocalGattIds &ids);

// A value read from an offset (Read Blob): false when the offset is past its end.
bool gattValueFromOffset(const std::vector<uint8_t> &value, size_t offset, std::vector<uint8_t> &out);

// A value written at an offset (the fragments of a long write): what comes before the offset is kept, the rest
// replaced. False when the offset is past the end of the current value.
bool gattValueWriteAt(const std::vector<uint8_t> &value, size_t offset, const std::vector<uint8_t> &data,
                      std::vector<uint8_t> &out);

#endif
