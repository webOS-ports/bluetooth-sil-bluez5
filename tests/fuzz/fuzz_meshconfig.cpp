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

// libFuzzer target for the mesh config status parsers: every byte comes from the node.

#include <cstddef>
#include <cstdint>

#include "meshconfig.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	MeshAppKeyStatus appKey;
	MeshNetKeyStatus netKey;
	MeshModelAppStatus modelApp;
	parseAppKeyStatus(data, size, appKey);
	parseNetKeyStatus(data, size, netKey);
	parseModelAppStatus(data, size, modelApp);
	if (size)
		meshStatusToError(data[0]);
	return 0;
}
