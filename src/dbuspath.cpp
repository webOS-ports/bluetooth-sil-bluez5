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

#include "dbuspath.h"

// A plain prefix (or, in one place, a substring anywhere) matched hci1 for hci10's objects.
bool isObjectPathUnder(const std::string &path, const std::string &parent)
{
	if (parent.empty() || path.compare(0, parent.size(), parent) != 0)
		return false;
	return path.size() == parent.size() || path[parent.size()] == '/' || parent == "/";
}
