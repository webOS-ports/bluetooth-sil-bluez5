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

// Byte and string arrays from BlueZ properties: the expected type converts; anything else, or NULL, is empty and
// raises no GLib critical (an uninitialised iterator was read after g_variant_get refused the type).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "utils.h"

namespace {

int criticals = 0;

void countCriticals(const gchar *, GLogLevelFlags level, const gchar *, gpointer)
{
	if (level & (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING))
		criticals++;
}

struct CountCriticals
{
	CountCriticals() { criticals = 0; g_log_set_default_handler(countCriticals, nullptr); }
	~CountCriticals() { g_log_set_default_handler(g_log_default_handler, nullptr); }
};

} // namespace

TEST(ConvertArray, Bytes)
{
	CountCriticals guard;
	const guint8 data[] = {1, 2, 255};
	GVariant *v = g_variant_ref_sink(g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, 3, 1));
	EXPECT_EQ(convertArrayByteGVariantToVector(v), (std::vector<unsigned char>{1, 2, 255}));
	g_variant_unref(v);

	GVariant *wrong = g_variant_ref_sink(g_variant_new_string("not bytes"));
	EXPECT_TRUE(convertArrayByteGVariantToVector(wrong).empty());
	g_variant_unref(wrong);
	EXPECT_TRUE(convertArrayByteGVariantToVector(nullptr).empty());
	EXPECT_EQ(criticals, 0);
}

TEST(ConvertArray, Strings)
{
	CountCriticals guard;
	const gchar *strv[] = {"a", "bc", nullptr};
	GVariant *v = g_variant_ref_sink(g_variant_new_strv(strv, -1));
	EXPECT_EQ(convertArrayStringGVariantToVector(v), (std::vector<std::string>{"a", "bc"}));
	g_variant_unref(v);

	GVariant *wrong = g_variant_ref_sink(g_variant_new_uint32(7));
	EXPECT_TRUE(convertArrayStringGVariantToVector(wrong).empty());
	g_variant_unref(wrong);
	EXPECT_TRUE(convertArrayStringGVariantToVector(nullptr).empty());
	EXPECT_EQ(criticals, 0);
}
