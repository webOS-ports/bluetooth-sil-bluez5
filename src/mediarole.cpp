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

#include "mediarole.h"

#include <cstring>

#include "asyncutils.h"
#include "logging.h"

void selectMediaRole(BluezMedia1 *media, const std::string &role, BluetoothResultCallback done)
{
	// Was synchronous: the service's only main loop stood still until bluetoothd answered (25 s when it hung).
	auto selectRoleCallback = [role, done](GObject *source, GAsyncResult *result) {
		GError *error = 0;
		gboolean ret = bluez_media1_call_select_role_finish(BLUEZ_MEDIA1(source), result, &error);
		BluetoothError answer = BLUETOOTH_ERROR_NONE;
		if (error)
		{
			// The D-Bus error name, not the whole message, decides: the role this adapter already has.
			gchar *remote = g_dbus_error_get_remote_error(error);
			if (!remote || strcmp(remote, "org.bluez.Error.AlreadyExists") != 0)
			{
				ERROR("A2DP_ENABLE_ROLE", 0, "Role enable %s failed error %s", role.c_str(), error->message);
				answer = BLUETOOTH_ERROR_FAIL;
			}
			g_free(remote);
			g_error_free(error);
		}
		else if (!ret)
			answer = BLUETOOTH_ERROR_FAIL;
		if (done)
			done(answer);
	};
	bluez_media1_call_select_role(media, role.c_str(), NULL, glibAsyncMethodWrapper,
								  new GlibAsyncFunctionWrapper(selectRoleCallback));
}
