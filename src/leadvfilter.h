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

// Matching an LE scan filter, given by an app, against what a device advertises. Kept apart from the adapter so
// that it can be tested and fuzzed: the advertised data comes from any device in range, and the filter, data and
// mask lengths are all independent.

#ifndef LEADVFILTER_H
#define LEADVFILTER_H

#include <cstdint>
#include <string>
#include <vector>

// A service UUID: equal (case-insensitive; a 16- or 32-bit one in its 128-bit form), or with a mask, equal at every
// position where the mask has '1'.
bool matchServiceUuid(const std::string &filterUuid, const std::string &filterMask,
                      const std::vector<std::string> &deviceUuids);

// Service data for the filter's UUID: equal, or with a mask, equal at every byte where the mask is 1.
bool matchServiceData(const std::string &filterUuid, const std::vector<uint8_t> &filterData,
                      const std::vector<uint8_t> &filterMask,
                      const std::string &deviceDataUuid, const std::vector<uint8_t> &deviceData);

// Manufacturer data as the device advertises it: two id bytes (high byte first), then the payload, which must
// start with the filter's data (where the mask, if any, is 1).
bool matchManufacturerData(uint16_t filterId, const std::vector<uint8_t> &filterData,
                           const std::vector<uint8_t> &filterMask, const std::vector<uint8_t> &deviceData);

#endif
