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

// Times in an OBEX folder listing, as a remote FTP server sends them.

#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <string>

#include "obextime.h"

namespace {

// A zone one hour east of UTC, without daylight saving, for the test's lifetime: local and UTC times differ
// whatever zone the test runs in.
struct ScopedTz
{
	ScopedTz()
	{
		const char *old = getenv("TZ");
		hadOld = old != nullptr;
		saved = old ? old : "";
		setenv("TZ", "CET-1", 1);
		tzset();
	}
	~ScopedTz()
	{
		if (hadOld)
			setenv("TZ", saved.c_str(), 1);
		else
			unsetenv("TZ");
		tzset();
	}
	bool hadOld;
	std::string saved;
};

} // namespace

TEST(ObexTime, Utc)
{
	ScopedTz tz;
	// 2026-10-10 12:34:56 UTC
	EXPECT_EQ(decodeObexTime("20261010T123456Z"), (time_t)1791635696);
	// Midnight on new year: every field at its lowest, which the old code left unshifted while shifting others.
	EXPECT_EQ(decodeObexTime("20260101T000000Z"), (time_t)1767225600);
	EXPECT_EQ(decodeObexTime("20261010T000001Z") - decodeObexTime("20261010T000000Z"), 1);
}

TEST(ObexTime, LocalWithoutZone)
{
	ScopedTz tz;
	EXPECT_EQ(decodeObexTime("20261010T123456"), (time_t)(1791635696 - 3600));
}

TEST(ObexTime, NotATime)
{
	for (const char *s : {"", "garbage", "2026", "20261010", "20261010T1234", "20261010T123456X", "20261010T123456Zjunk",
	                      "20261310T123456Z", "20261000T123456Z", "20261010T243456Z", "20261010T126056Z",
	                      " 2026101T123456Z"})
		EXPECT_EQ(decodeObexTime(s), (time_t)0) << '"' << s << '"';
}
