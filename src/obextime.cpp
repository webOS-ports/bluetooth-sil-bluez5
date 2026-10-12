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

#include "obextime.h"

#include <cstdio>
#include <cstring>

// Every field was shifted down by one, not just the month, moving times by 1h1m1s; UTC was read as local time;
// and a string that was not a time came out as year 1900.
time_t decodeObexTime(const std::string &str)
{
	struct tm t;
	memset(&t, 0, sizeof(t));

	// Exactly the shape: sscanf alone takes spaces, signs and short fields.
	bool utc = str.size() == 16 && str[15] == 'Z';
	if (str.size() != 15 && !utc)
		return 0;
	for (size_t i = 0; i < 15; i++)
		if (i == 8 ? str[i] != 'T' : (str[i] < '0' || str[i] > '9'))
			return 0;

	if (sscanf(str.c_str(), "%4d%2d%2dT%2d%2d%2d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min,
	           &t.tm_sec) != 6 ||
	    t.tm_mon < 1 || t.tm_mon > 12 || t.tm_mday < 1 || t.tm_mday > 31 ||
	    t.tm_hour > 23 || t.tm_min > 59 || t.tm_sec > 60)
		return 0;

	t.tm_year -= 1900;	// years since 1900
	t.tm_mon -= 1;		// 0-11
	if (utc)
		return timegm(&t);
	t.tm_isdst = -1;
	return mktime(&t);
}
