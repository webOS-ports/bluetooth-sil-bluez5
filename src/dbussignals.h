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

// Reading the bus signals that tell an object or a peer is gone, kept apart so that they can be tested.

#ifndef DBUSSIGNALS_H
#define DBUSSIGNALS_H

#include <string>
#include <vector>

#include <glib.h>

namespace DBusUtils
{
	// The interfaces an ObjectManager.InterfacesRemoved signal, parameters (oas), removes from path: none when it
	// is about another object or not of that type.
	std::vector<std::string> interfacesRemovedAt(GVariant *parameters, const std::string &path);

	// Whether a NameOwnerChanged signal, parameters (sss), tells that name has no owner any more. For a unique
	// name that is the peer leaving the bus.
	bool nameLost(GVariant *parameters, const std::string &name);
}

#endif
