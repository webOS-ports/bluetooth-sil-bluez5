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

// Composition Data page 0, as a mesh node sends it in a Config Composition Data Status. Kept apart from the config
// client, which needs meshd, so that it can be tested and fuzzed on its own: every byte comes from the node.

#ifndef MESHCOMPOSITION_H
#define MESHCOMPOSITION_H

#include <cstddef>
#include <cstdint>

#include <bluetooth-sil-api.h>

// The message body after the opcode: page number, then the page. False when it is shorter than the page header;
// elements cut off at the end are left out.
bool parseCompositionData(const uint8_t *data, size_t len, BleMeshCompositionData &compositionData);

#endif
