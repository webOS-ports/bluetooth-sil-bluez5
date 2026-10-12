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

// libFuzzer target for the OBEX path and name helpers: object paths come from obexd, received file names from the
// sending device. Checks, besides the sanitizers, that an acceptable name stays a bare name and that the free
// name chosen is free and still acceptable.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>

#include "obexpaths.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	std::string s(reinterpret_cast<const char *>(data), size);

	obexSessionId(s);
	obexMessageHandle(s);
	isObjectPathElement(s);
	normaliseSessionKey(s);

	if (isAcceptableReceivedName(s))
	{
		if (s.find('/') != std::string::npos || s.empty() || s[0] == '.')
			abort();
		// The name and its first few free alternatives are taken.
		std::set<std::string> taken = { s };
		for (int i = 0; i < 3; i++)
		{
			std::string next = uniqueReceivedName(s, [&](const std::string &n) { return taken.count(n) > 0; });
			if (next.empty() || taken.count(next) || next.find('/') != std::string::npos)
				abort();
			taken.insert(next);
		}
	}
	return 0;
}
