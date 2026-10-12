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

// Composition Data page 0 as a mesh node sends it. Every byte comes from the node; under AddressSanitizer these
// also catch reads past the end of the message.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "meshcomposition.h"

namespace {

using Bytes = std::vector<uint8_t>;

// Page 0, CID 0x05f1, PID 0x0102, VID 0x0304, CRPL 0x0020, features relay+friend.
const Bytes header = {0x00, 0xf1, 0x05, 0x02, 0x01, 0x04, 0x03, 0x20, 0x00, 0x05, 0x00};

bool parse(const Bytes &data, BleMeshCompositionData &out)
{
	// A copy of exactly the right size, so that ASan sees any read past it.
	auto *copy = new uint8_t[data.size() ? data.size() : 1];
	std::copy(data.begin(), data.end(), copy);
	bool ok = parseCompositionData(copy, data.size(), out);
	delete[] copy;
	return ok;
}

Bytes cat(Bytes a, const Bytes &b)
{
	a.insert(a.end(), b.begin(), b.end());
	return a;
}

} // namespace

TEST(MeshComposition, Header)
{
	BleMeshCompositionData c;
	ASSERT_TRUE(parse(header, c));
	EXPECT_EQ(c.getCompanyId(), 0x05f1);
	EXPECT_EQ(c.getProductId(), 0x0102);
	EXPECT_EQ(c.getVersionId(), 0x0304);
	EXPECT_EQ(c.getNumRplEnteries(), 0x0020);
	EXPECT_TRUE(c.getFeatures().getRelaySupport());
	EXPECT_FALSE(c.getFeatures().getProxySupport());
	EXPECT_TRUE(c.getFeatures().getFriendSupport());
	EXPECT_FALSE(c.getFeatures().getLowPowerSupport());
	EXPECT_TRUE(c.getElements().empty());
}

TEST(MeshComposition, ShorterThanHeaderFails)
{
	for (size_t len = 0; len < header.size(); len++)
	{
		BleMeshCompositionData c;
		EXPECT_FALSE(parse(Bytes(header.begin(), header.begin() + (long)len), c)) << len;
	}
	BleMeshCompositionData c;
	EXPECT_FALSE(parseCompositionData(nullptr, 20, c));
}

TEST(MeshComposition, Elements)
{
	// Element 0: loc 0x0100, 2 SIG models (0x0000, 0x1001), 1 vendor model (CID 0x05f1, model 0x0042).
	// Element 1: loc 0x0200, 1 SIG model 0x1000, no vendor models.
	Bytes data = cat(header, {0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x01, 0x10, 0xf1, 0x05, 0x42, 0x00,
	                          0x00, 0x02, 0x01, 0x00, 0x00, 0x10});
	BleMeshCompositionData c;
	ASSERT_TRUE(parse(data, c));
	auto elements = c.getElements();
	ASSERT_EQ(elements.size(), 2u);

	EXPECT_EQ(elements[0].getLoc(), 0x0100);
	EXPECT_EQ(elements[0].getNumS(), 2);
	EXPECT_EQ(elements[0].getNumV(), 1);
	EXPECT_EQ(elements[0].getSigModelIds(), (std::vector<uint32_t>{0x0000, 0x1001}));
	// Company id in the high half, model id in the low half. Both halves were the company id.
	EXPECT_EQ(elements[0].getVendorModelIds(), (std::vector<uint32_t>{0x05f10042}));

	EXPECT_EQ(elements[1].getLoc(), 0x0200);
	EXPECT_EQ(elements[1].getSigModelIds(), (std::vector<uint32_t>{0x1000}));
	EXPECT_TRUE(elements[1].getVendorModelIds().empty());
}

TEST(MeshComposition, TruncatedElementHeaderIgnored)
{
	// 1 to 3 trailing bytes: not an element. With an odd count the unsigned length wrapped and the loop ran on.
	for (size_t extra = 1; extra < 4; extra++)
	{
		Bytes data = header;
		data.insert(data.end(), extra, 0x01);
		BleMeshCompositionData c;
		ASSERT_TRUE(parse(data, c)) << extra;
		EXPECT_TRUE(c.getElements().empty()) << extra;
	}
}

TEST(MeshComposition, ModelsCutOff)
{
	// Claims 3 SIG and 2 vendor models, carries 1 SIG model and half a vendor model.
	Bytes data = cat(header, {0x00, 0x01, 0x03, 0x02, 0x00, 0x10, 0xf1});
	BleMeshCompositionData c;
	ASSERT_TRUE(parse(data, c));
	auto elements = c.getElements();
	ASSERT_EQ(elements.size(), 1u);
	EXPECT_EQ(elements[0].getNumS(), 3);
	EXPECT_EQ(elements[0].getSigModelIds(), (std::vector<uint32_t>{0x1000}));
	EXPECT_TRUE(elements[0].getVendorModelIds().empty());
}

TEST(MeshComposition, AllLengths)
{
	// Every prefix of a well-formed message parses without reading past it.
	Bytes data = cat(header, {0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x01, 0x10, 0xf1, 0x05, 0x42, 0x00,
	                          0x00, 0x02, 0xff, 0xff, 0x00, 0x10});
	for (size_t len = header.size(); len <= data.size(); len++)
	{
		BleMeshCompositionData c;
		EXPECT_TRUE(parse(Bytes(data.begin(), data.begin() + (long)len), c)) << len;
	}
}
