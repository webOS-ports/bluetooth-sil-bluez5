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

#include "dbussignals.h"

namespace DBusUtils
{

std::vector<std::string> interfacesRemovedAt(GVariant *parameters, const std::string &path)
{
	std::vector<std::string> removed;
	if (!parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(oas)")))
		return removed;

	const gchar *objectPath = nullptr;
	GVariantIter *interfaces = nullptr;
	g_variant_get(parameters, "(&oas)", &objectPath, &interfaces);
	if (objectPath && path == objectPath)
	{
		const gchar *name = nullptr;
		while (g_variant_iter_next(interfaces, "&s", &name))
			removed.push_back(name);
	}
	g_variant_iter_free(interfaces);
	return removed;
}

bool nameLost(GVariant *parameters, const std::string &name)
{
	if (!parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)")))
		return false;

	const gchar *changed = nullptr;
	const gchar *newOwner = nullptr;
	g_variant_get(parameters, "(&s&s&s)", &changed, nullptr, &newOwner);
	return changed && newOwner && name == changed && newOwner[0] == '\0';
}

} // namespace DBusUtils
