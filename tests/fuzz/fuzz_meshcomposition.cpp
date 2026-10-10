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

// libFuzzer target for Composition Data: every byte comes from the mesh node. Checks, besides the sanitizers,
// that no element carries more models than the message had room for.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "meshcomposition.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	BleMeshCompositionData c;
	if (!parseCompositionData(data, size, c))
	{
		if (size >= 11)
			abort();
		return 0;
	}

	size_t used = 11;
	for (auto &e : c.getElements())
	{
		auto sig = e.getSigModelIds();
		auto vendor = e.getVendorModelIds();
		if (sig.size() > e.getNumS() || vendor.size() > e.getNumV())
			abort();
		used += 4 + 2 * sig.size() + 4 * vendor.size();
	}
	if (used > size)
		abort();
	return 0;
}
