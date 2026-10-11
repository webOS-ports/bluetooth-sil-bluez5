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

#include "obexpaths.h"

#include <algorithm>
#include <cctype>

#include <sys/stat.h>

namespace {

// The element of path that starts with prefix followed by at least one digit, up to the next '/'.
std::string elementWithPrefix(const std::string &path, const std::string &prefix)
{
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		std::string element = path.substr(start, end - start);
		if (element.size() > prefix.size() && element.compare(0, prefix.size(), prefix) == 0 &&
		    std::all_of(element.begin() + (long)prefix.size(), element.end(), [](unsigned char c) { return std::isdigit(c); }))
			return element;
		start = end + 1;
	}
	return "";
}

} // namespace

std::string obexSessionId(const std::string &objectPath)
{
	return elementWithPrefix(objectPath, "session");
}

std::string obexMessageHandle(const std::string &objectPath)
{
	return elementWithPrefix(objectPath, "message");
}

bool isObjectPathElement(const std::string &element)
{
	return !element.empty() && std::all_of(element.begin(), element.end(), [](unsigned char c) {
		return std::isalnum(c) || c == '_';
	});
}

std::string normaliseSessionKey(const std::string &sessionKey)
{
	size_t keyPos = sessionKey.find('_');
	size_t addressEnd = keyPos == std::string::npos ? sessionKey.size() : keyPos;
	std::string result = sessionKey;
	std::transform(result.begin(), result.begin() + (long)addressEnd, result.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	return result;
}

bool isAcceptableReceivedName(const std::string &name)
{
	if (name.empty() || name[0] == '.' || name.find('/') != std::string::npos)
		return false;
	return std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::string uniqueReceivedName(const std::string &name, const std::function<bool(const std::string &)> &exists)
{
	if (!isAcceptableReceivedName(name))
		return "";
	if (!exists(name))
		return name;

	// The extension starts at the last '.', which is never the first character here.
	size_t dot = name.rfind('.');
	std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
	std::string ext = dot == std::string::npos ? "" : name.substr(dot);
	for (int n = 1; n <= 999; n++)
	{
		std::string candidate = stem + " (" + std::to_string(n) + ")" + ext;
		if (!exists(candidate))
			return candidate;
	}
	return "";
}

bool isPartOfFailedPush(const std::string &path, uint64_t announcedSize)
{
	struct stat st;
	// With no size given (0) no file is shorter.
	if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
		return false;
	return (uint64_t) st.st_size < announcedSize;
}
