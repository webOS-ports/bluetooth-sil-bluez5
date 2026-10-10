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

// What the SIL needs to know about obexd's objects without waiting on the main loop: whom to make proxies for
// (ObexPeer), and the properties an incoming push is decided on. Kept apart so that they can be tested.

#ifndef OBEXPUSH_H
#define OBEXPUSH_H

#include <cstdint>
#include <string>

#include <glib.h>
#include <gio/gio.h>

// The obexd an object belongs to: the connection a reply or request came on (borrowed) and obexd's unique name.
// A proxy on a unique name is made without a call: on the well-known name each one asked the bus, synchronously,
// with StartServiceByName and GetNameOwner (GLib gdbusproxy.c, initable_init). And obexd's objects go with the
// instance that made them, which the unique name stands for.
struct ObexPeer
{
	GDBusConnection *connection = nullptr;
	std::string name;

	bool isValid() const { return connection && g_dbus_is_unique_name(name.c_str()); }
};

// The obexd behind a proxy made on its unique name (the session's): its connection and name.
ObexPeer obexPeerOfProxy(GObject *proxy);

// The obexd a well-known-name proxy (the client's) currently talks to: its connection and name owner. Invalid
// when obexd is not on the bus.
ObexPeer obexPeerOfOwner(GObject *proxy);

// What an incoming push is decided on: obexd's Transfer1 and Session1 properties, both answered by obexd itself
// (obexd/src/manager.c) from its nested loop while it waits for the agent.
struct ObexPushRequest
{
	std::string transferPath;
	std::string sessionPath;	// Transfer1.Session
	bool hasName = false;		// a PUT without a Name header has none
	std::string name;			// Transfer1.Name
	uint64_t size = 0;			// Transfer1.Size, 0 when not given
	std::string source;			// Session1.Source: the local adapter's address
	std::string destination;	// Session1.Destination: the sender's address
	gint64 receivedAt = 0;		// g_get_monotonic_time() when obexd asked
};

// From the a{sv} that GetAll("org.bluez.obex.Transfer1") returns. False without a Session of type 'o'; a value of
// another type counts as absent.
bool parseObexTransferProperties(GVariant *properties, ObexPushRequest &request);

// From the a{sv} that GetAll("org.bluez.obex.Session1") returns. False without Source and Destination strings.
bool parseObexSessionProperties(GVariant *properties, ObexPushRequest &request);

// How long, in ms, a push asked about at receivedAt may still wait for the user at now (both monotonic, us) when
// it may wait limitMs in all: 0 once that has passed. The lookups before the user is asked count against it, so
// that the refusal still comes before obexd's own 60 s.
guint obexConfirmationRemainingMs(gint64 receivedAt, gint64 now, guint limitMs);

#endif
