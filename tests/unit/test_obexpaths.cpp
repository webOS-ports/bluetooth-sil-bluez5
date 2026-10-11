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

// Names from obexd and from the remote device.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "obexpaths.h"

#include <unistd.h>

TEST(ObexPaths, SessionId)
{
	// The whole element: "session1" was reported for session12 (MAP notifications).
	EXPECT_EQ(obexSessionId("/org/bluez/obex/client/session12/message123"), "session12");
	EXPECT_EQ(obexSessionId("/org/bluez/obex/client/session1"), "session1");
	EXPECT_EQ(obexSessionId("/org/bluez/obex/server/session3/transfer0"), "session3");
	EXPECT_EQ(obexSessionId("/org/bluez/obex/client/sessions/x"), "");
	EXPECT_EQ(obexSessionId("/org/bluez/obex/client/session"), "");
	EXPECT_EQ(obexSessionId(""), "");
}

TEST(ObexPaths, MessageHandle)
{
	EXPECT_EQ(obexMessageHandle("/org/bluez/obex/client/session12/message123"), "message123");
	EXPECT_EQ(obexMessageHandle("/org/bluez/obex/client/session12"), "");
	EXPECT_EQ(obexMessageHandle("/org/bluez/obex/client/session12/messageX"), "");
}

TEST(ObexPaths, ObjectPathElement)
{
	EXPECT_TRUE(isObjectPathElement("message123"));
	EXPECT_TRUE(isObjectPathElement("a_B9"));
	EXPECT_FALSE(isObjectPathElement(""));
	EXPECT_FALSE(isObjectPathElement("message-1"));
	EXPECT_FALSE(isObjectPathElement("message.1"));
	EXPECT_FALSE(isObjectPathElement("../x"));
	EXPECT_FALSE(isObjectPathElement("a/b"));
}

TEST(ObexPaths, SessionKey)
{
	EXPECT_EQ(normaliseSessionKey("AA:BB:CC:DD:EE:FF_MAP-0"), "aa:bb:cc:dd:ee:ff_MAP-0");
	// No '_': this threw std::out_of_range.
	EXPECT_EQ(normaliseSessionKey("AA:BB:CC:DD:EE:FF"), "aa:bb:cc:dd:ee:ff");
	EXPECT_EQ(normaliseSessionKey(""), "");
}

TEST(ObexPaths, AcceptableReceivedName)
{
	EXPECT_TRUE(isAcceptableReceivedName("photo.jpg"));
	EXPECT_TRUE(isAcceptableReceivedName("my file (2).tar.gz"));
	EXPECT_FALSE(isAcceptableReceivedName(""));
	EXPECT_FALSE(isAcceptableReceivedName(".bashrc"));
	EXPECT_FALSE(isAcceptableReceivedName("."));
	EXPECT_FALSE(isAcceptableReceivedName(".."));
	EXPECT_FALSE(isAcceptableReceivedName("a/b"));
	EXPECT_FALSE(isAcceptableReceivedName(std::string("a\nb")));
	EXPECT_FALSE(isAcceptableReceivedName(std::string("a\x7f")));
}

TEST(ObexPaths, UniqueReceivedName)
{
	std::set<std::string> taken;
	auto exists = [&taken](const std::string &n) { return taken.count(n) > 0; };

	EXPECT_EQ(uniqueReceivedName("photo.jpg", exists), "photo.jpg");
	// An existing file was replaced: the next free "(n)" is used.
	taken = {"photo.jpg"};
	EXPECT_EQ(uniqueReceivedName("photo.jpg", exists), "photo (1).jpg");
	taken = {"photo.jpg", "photo (1).jpg", "photo (2).jpg"};
	EXPECT_EQ(uniqueReceivedName("photo.jpg", exists), "photo (3).jpg");
	taken = {"README"};
	EXPECT_EQ(uniqueReceivedName("README", exists), "README (1)");
	taken = {"a.tar.gz"};
	EXPECT_EQ(uniqueReceivedName("a.tar.gz", exists), "a.tar (1).gz");

	EXPECT_EQ(uniqueReceivedName(".hidden", exists), "");
	EXPECT_EQ(uniqueReceivedName("x", [](const std::string &) { return true; }), "");
}

TEST(ObexPaths, PartOfFailedPush)
{
	char dir[] = "/tmp/obexpaths-XXXXXX";
	ASSERT_NE(mkdtemp(dir), nullptr);
	std::string file = std::string(dir) + "/part.bin", link = std::string(dir) + "/link.bin";
	FILE *f = fopen(file.c_str(), "w");
	ASSERT_NE(f, nullptr);
	fputs("12345", f);
	fclose(f);

	EXPECT_TRUE(isPartOfFailedPush(file, 6));		// shorter than announced
	EXPECT_FALSE(isPartOfFailedPush(file, 5));		// complete
	EXPECT_FALSE(isPartOfFailedPush(file, 4));		// longer: not ours to judge
	EXPECT_FALSE(isPartOfFailedPush(file, 0));		// no size given
	EXPECT_FALSE(isPartOfFailedPush(std::string(dir) + "/none", 6));
	EXPECT_FALSE(isPartOfFailedPush(dir, UINT64_MAX));	// a folder, whatever its size
	ASSERT_EQ(symlink(file.c_str(), link.c_str()), 0);
	EXPECT_FALSE(isPartOfFailedPush(link, UINT64_MAX));	// a link, even to a short file

	unlink(link.c_str());
	unlink(file.c_str());
	rmdir(dir);
}
