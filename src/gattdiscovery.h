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
// What GATT service discovery of a remote device answers, kept apart from BlueZ so that it can be tested. BlueZ
// discovers a device's services itself once it is connected, exports them as objects and then sets its
// ServicesResolved. Asked right after a connect, the SIL used to answer from the services it already knew: none
// yet, so "discovery failed", while BlueZ finished a moment later.

#ifndef GATTDISCOVERY_H
#define GATTDISCOVERY_H

#include <cstdint>

enum class GattDiscoveryStep
{
	Done,	// answer success
	Wait,	// ask again a little later
	Fail	// answer failure
};

// known: the device exists. connected and servicesResolved: its Device1 properties. hasServices: the SIL knows at
// least one of its services. Done once BlueZ has resolved them; while connected and not yet resolved, Wait until
// elapsedMs reaches limitMs. Not connected, or out of time: Done with services (BlueZ keeps a bonded device's),
// else Fail.
GattDiscoveryStep gattDiscoveryStep(bool known, bool connected, bool servicesResolved, bool hasServices,
                                    int64_t elapsedMs, int64_t limitMs);

#endif
