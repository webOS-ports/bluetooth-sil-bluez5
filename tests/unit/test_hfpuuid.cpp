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

#include <gtest/gtest.h>

#include "hfpuuid.h"

using hfpuuid::AG;
using hfpuuid::HF;

namespace {

const std::string A2DP_SINK = "0000110b-0000-1000-8000-00805f9b34fb";
const std::string A2DP_SOURCE = "0000110a-0000-1000-8000-00805f9b34fb";
const std::string HSP_HS = "00001108-0000-1000-8000-00805f9b34fb";
const std::string AVRCP_TARGET = "0000110c-0000-1000-8000-00805f9b34fb";

// A LuneOS phone: oFono's hfp_ag_bluez5 added the audio gateway record, and A2DP source is there.
const std::vector<std::string> PHONE = { A2DP_SOURCE, AG, AVRCP_TARGET };
// A device in OSE's HF role: oFono's hfp_hf_bluez5, and A2DP sink.
const std::vector<std::string> CARKIT = { A2DP_SINK, HF };
// A headset.
const std::vector<std::string> HEADSET = { A2DP_SINK, HSP_HS, HF };
const std::vector<std::string> HFP_ONLY_HEADSET = { HF };
// A remote phone.
const std::vector<std::string> REMOTE_PHONE = { A2DP_SOURCE, AG, AVRCP_TARGET };

}

TEST(HfpUuid, PhoneConnectsHeadsetsHandsFreeUnit)
{
	EXPECT_EQ(hfpuuid::connectUuid(PHONE, HEADSET), HF);
	EXPECT_EQ(hfpuuid::connectUuid(PHONE, HFP_ONLY_HEADSET), HF);
}

TEST(HfpUuid, HandsFreeConnectsPhonesAudioGateway)
{
	EXPECT_EQ(hfpuuid::connectUuid(CARKIT, REMOTE_PHONE), AG);
}

TEST(HfpUuid, NoAudioGatewayHereKeepsAudioGateway)
{
	// oFono registers its AG only with a voice-capable modem: without it, as before.
	EXPECT_EQ(hfpuuid::connectUuid({ A2DP_SOURCE }, HEADSET), AG);
	EXPECT_EQ(hfpuuid::connectUuid({}, HEADSET), AG);
	// The SIL itself adds 0x111e to the adapter's list when it creates the profile: that is not an AG.
	EXPECT_EQ(hfpuuid::connectUuid({ HF }, HEADSET), AG);
}

TEST(HfpUuid, RemoteWithoutHandsFreeKeepsAudioGateway)
{
	EXPECT_EQ(hfpuuid::connectUuid(PHONE, REMOTE_PHONE), AG);
	EXPECT_EQ(hfpuuid::connectUuid(PHONE, { A2DP_SINK }), AG);
	EXPECT_EQ(hfpuuid::connectUuid(PHONE, {}), AG);
}

TEST(HfpUuid, CaseDoesNotMatter)
{
	EXPECT_EQ(hfpuuid::connectUuid({ "0000111F-0000-1000-8000-00805F9B34FB" }, { "0000111E-0000-1000-8000-00805F9B34FB" }), HF);
	// A prefix is not the UUID.
	EXPECT_EQ(hfpuuid::connectUuid({ AG }, { std::string(HF).substr(0, 8) }), AG);
	EXPECT_EQ(hfpuuid::connectUuid({ std::string(AG).substr(0, 8) }, { HF }), AG);
}

TEST(HfpUuid, DisconnectsTheEndThatConnected)
{
	// The headset connected itself, or BlueZ did: disconnect that, whatever the lists say.
	EXPECT_EQ(hfpuuid::disconnectUuid(HF, {}, {}), HF);
	EXPECT_EQ(hfpuuid::disconnectUuid(AG, PHONE, HEADSET), AG);
	EXPECT_EQ(hfpuuid::disconnectUuid("0000111E-0000-1000-8000-00805F9B34FB", {}, {}), HF);
	// Nothing known connected, or something else: as connectUuid() picks.
	EXPECT_EQ(hfpuuid::disconnectUuid("", PHONE, HEADSET), HF);
	EXPECT_EQ(hfpuuid::disconnectUuid(A2DP_SINK, PHONE, HEADSET), HF);
	EXPECT_EQ(hfpuuid::disconnectUuid("", CARKIT, REMOTE_PHONE), AG);
}
