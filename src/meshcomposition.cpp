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

#include "meshcomposition.h"

#include <vector>

namespace {

const uint16_t FEATURE_RELAY = 1;
const uint16_t FEATURE_PROXY = 2;
const uint16_t FEATURE_FRIEND = 4;
const uint16_t FEATURE_LPN = 8;

uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

} // namespace

bool parseCompositionData(const uint8_t *data, size_t len, BleMeshCompositionData &compositionData)
{
	// Page number, CID, PID, VID, CRPL and features. A shorter body made the unsigned length wrap, and the element
	// loop read up to 64 KiB past the message.
	if (!data || len < 11)
		return false;

	/* skip page -- We only support Page Zero */
	data++;
	len--;

	compositionData.setCompanyId(le16(&data[0]));
	compositionData.setProductId(le16(&data[2]));
	compositionData.setVersionId(le16(&data[4]));
	compositionData.setNumRplEnteries(le16(&data[6]));

	uint16_t features = le16(&data[8]);
	BleMeshFeature meshFeatures(features & FEATURE_RELAY, features & FEATURE_PROXY,
								features & FEATURE_FRIEND, features & FEATURE_LPN);
	compositionData.setFeatures(meshFeatures);
	data += 10;
	len -= 10;

	std::vector<BleMeshElement> elements;
	while (len >= 4)	// location, and the SIG and vendor model counts
	{
		BleMeshElement element;
		element.setLoc(le16(data));
		uint8_t m = data[2];
		uint8_t v = data[3];
		data += 4;
		len -= 4;
		element.setNumS(m);
		element.setNumV(v);

		std::vector<uint32_t> sigModId;
		while (len >= 2 && m--)
		{
			sigModId.push_back(le16(data));
			data += 2;
			len -= 2;
		}
		element.setSigModelIds(sigModId);

		std::vector<uint32_t> vendorModId;
		while (len >= 4 && v--)
		{
			// Company id, then the vendor's model id. The model id was read from the company id's bytes again.
			vendorModId.push_back((uint32_t)le16(data) << 16 | le16(data + 2));
			data += 4;
			len -= 4;
		}
		element.setVendorModelIds(vendorModId);
		elements.push_back(element);
	}
	compositionData.setElements(elements);
	return true;
}
