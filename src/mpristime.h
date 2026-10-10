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

// Times handed to BlueZ's MPRIS player (AVRCP target). The SIL API has milliseconds, as BlueZ's MediaPlayer1 does;
// MPRIS Position and mpris:length are microseconds, which BlueZ divides by 1000 again (profiles/audio/media.c,
// set_position and the Duration metadata). Passed through unchanged, a 4-minute track showed as 0:00.

#ifndef MPRISTIME_H
#define MPRISTIME_H

#include <cstdint>
#include <limits>

inline int64_t msToMprisMicroseconds(uint64_t milliseconds)
{
	const uint64_t limit = (uint64_t)std::numeric_limits<int64_t>::max() / 1000;
	return milliseconds > limit ? std::numeric_limits<int64_t>::max() : (int64_t)(milliseconds * 1000);
}

#endif
