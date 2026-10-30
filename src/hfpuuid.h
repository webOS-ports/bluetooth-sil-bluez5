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

// Which end of HFP the HFP profile connects and disconnects, kept apart so that it can be tested.
//
// BlueZ's ConnectProfile and DisconnectProfile take the remote end's UUID: they look for a profile registered with
// that remote_uuid (src/device.c find_connectable_service). The profile always asked for the remote audio gateway
// (0x111f), which is right where this device is the hands-free unit (OSE's com.webos.service.hfp in its HF role,
// oFono's hfp_hf_bluez5) and the remote is a phone. On a LuneOS phone oFono's hfp_ag_bluez5 registers the audio
// gateway: its local record is 0x111f, its remote_uuid the hands-free unit, 0x111e (BlueZ src/profile.c
// HFP_AG_UUID). Asking a headset for 0x111f found nothing and failed at once, so HFP to a headset could not be
// connected.

#ifndef HFPUUID_H
#define HFPUUID_H

#include <string>
#include <vector>

namespace hfpuuid {

// Constants, not std::string objects: other translation units use them in their own static initializers.
constexpr char HF[] = "0000111e-0000-1000-8000-00805f9b34fb";	// hands-free unit
constexpr char AG[] = "0000111f-0000-1000-8000-00805f9b34fb";	// audio gateway

// The remote UUID to connect HFP with: the hands-free unit when the remote offers one and this adapter has the
// audio gateway, the remote's audio gateway otherwise (as before). UUIDs are compared without regard to case.
std::string connectUuid(const std::vector<std::string> &localUuids, const std::vector<std::string> &remoteUuids);

// The remote UUID to disconnect: the one that connected (from ConnectedUUIDS) when it is an HFP one, else what
// connectUuid() would pick.
std::string disconnectUuid(const std::string &connectedUuid, const std::vector<std::string> &localUuids,
                           const std::vector<std::string> &remoteUuids);

}

#endif
