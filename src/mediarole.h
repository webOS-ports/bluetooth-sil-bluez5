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

// Media1.SelectRole, which switches the adapter between A2DP source and sink. It is a webOS extension of BlueZ
// (upstream BlueZ has no such method) and only used in WEBOS_AUTO builds. Kept apart so that it can be tested.

#ifndef MEDIAROLE_H
#define MEDIAROLE_H

#include <string>

#include <bluetooth-sil-api.h>

extern "C" {
#include "bluez-interface.h"
}

// Asks bluetoothd for the role ("source" or "sink") without waiting for it: done is called once, from the main
// loop, with BLUETOOTH_ERROR_NONE when the role was selected or already was (org.bluez.Error.AlreadyExists), else
// BLUETOOTH_ERROR_FAIL. The call holds its own reference to media.
void selectMediaRole(BluezMedia1 *media, const std::string &role, BluetoothResultCallback done);

#endif // MEDIAROLE_H
