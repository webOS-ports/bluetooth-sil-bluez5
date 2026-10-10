#!/usr/bin/env python3
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

# An unprovisioned mesh node for mesh-scenarios.py: a bluetooth-meshd application with one element holding a
# Generic OnOff Server, that asks its meshd to Join() and then waits to be provisioned. Its meshd runs on a
# controller of its own and on a D-Bus of its own (one bus takes one org.bluez.mesh), given here through
# DBUS_SYSTEM_BUS_ADDRESS, which both libdbus and meshd's ell honour.
#
#   DBUS_SYSTEM_BUS_ADDRESS=unix:path=... mesh-node.py <uuid hex32> <state file>
#
# The state file (JSON) is rewritten on every change: joined, token, node path, OnOff state, messages seen, AppKey
# bindings meshd reported. Join() again after the node was removed is the caller's business (restart this).

import json
import os
import struct
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

MESH = 'org.bluez.mesh'
NETWORK_IFACE = 'org.bluez.mesh.Network1'
NODE_IFACE = 'org.bluez.mesh.Node1'
APP_IFACE = 'org.bluez.mesh.Application1'
ELEMENT_IFACE = 'org.bluez.mesh.Element1'
AGENT_IFACE = 'org.bluez.mesh.ProvisionAgent1'
OM_IFACE = 'org.freedesktop.DBus.ObjectManager'

APP_PATH = '/r4mesh/node'
AGENT_PATH = APP_PATH + '/agent'
ELEMENT_PATH = APP_PATH + '/ele00'

ONOFF_SERVER = 0x1000
OP_ONOFF_GET = 0x8201
OP_ONOFF_SET = 0x8202
OP_ONOFF_SET_UNACK = 0x8203
OP_ONOFF_STATUS = 0x8204

state = {'joined': False, 'token': None, 'node': None, 'onoff': 0, 'messages': [], 'bindings': [], 'errors': []}
state_file = None
bus = None
node = None


def save():
    with open(state_file + '.new', 'w') as f:
        json.dump(state, f)
    os.replace(state_file + '.new', state_file)


def error(where):
    def handler(e):
        state['errors'].append('%s: %s' % (where, e))
        save()
    return handler


class Application(dbus.service.Object):
    def __init__(self):
        dbus.service.Object.__init__(self, bus, APP_PATH)

    @dbus.service.method(OM_IFACE, out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        return {
            APP_PATH: {APP_IFACE: {'CompanyID': dbus.UInt16(0x05f1), 'ProductID': dbus.UInt16(0x0001),
                                   'VersionID': dbus.UInt16(0x0001), 'CRPL': dbus.UInt16(10)}},
            AGENT_PATH: {AGENT_IFACE: {'Capabilities': dbus.Array([], signature='s')}},
            ELEMENT_PATH: {ELEMENT_IFACE: {
                'Index': dbus.Byte(0),
                'Models': dbus.Array([(dbus.UInt16(ONOFF_SERVER), dbus.Dictionary({}, signature='sv'))],
                                     signature='(qa{sv})'),
                'VendorModels': dbus.Array([], signature='(qqa{sv})'),
            }},
        }

    @dbus.service.method(APP_IFACE, in_signature='t')
    def JoinComplete(self, token):
        state['joined'] = True
        state['token'] = '%016x' % token
        save()
        GLib.idle_add(attach, int(token))

    @dbus.service.method(APP_IFACE, in_signature='s')
    def JoinFailed(self, reason):
        state['errors'].append('JoinFailed: %s' % reason)
        save()


class Agent(dbus.service.Object):
    """No OOB: meshd asks nothing of it."""

    def __init__(self):
        dbus.service.Object.__init__(self, bus, AGENT_PATH)

    @dbus.service.method(AGENT_IFACE)
    def Cancel(self):
        state['errors'].append('provisioning cancelled')
        save()


class Element(dbus.service.Object):
    def __init__(self):
        dbus.service.Object.__init__(self, bus, ELEMENT_PATH)

    @dbus.service.method(ELEMENT_IFACE, in_signature='qqvay')
    def MessageReceived(self, source, key_index, destination, data):
        data = bytes(data)
        state['messages'].append({'src': int(source), 'key': int(key_index), 'data': data.hex()})
        if len(data) >= 2:
            opcode = struct.unpack('>H', data[:2])[0]
            if opcode in (OP_ONOFF_SET, OP_ONOFF_SET_UNACK) and len(data) >= 4:
                state['onoff'] = data[2]
            if opcode in (OP_ONOFF_GET, OP_ONOFF_SET):
                reply = struct.pack('>HB', OP_ONOFF_STATUS, state['onoff'])
                node.Send(ELEMENT_PATH, dbus.UInt16(source), dbus.UInt16(key_index),
                          dbus.Dictionary({}, signature='sv'), dbus.Array(reply, signature='y'),
                          reply_handler=lambda: None, error_handler=error('Send'))
        save()

    @dbus.service.method(ELEMENT_IFACE, in_signature='qbqay')
    def DevKeyMessageReceived(self, source, remote, net_index, data):
        state['messages'].append({'src': int(source), 'devkey': True, 'data': bytes(data).hex()})
        save()

    @dbus.service.method(ELEMENT_IFACE, in_signature='qa{sv}')
    def UpdateModelConfiguration(self, model_id, config):
        if 'Bindings' in config:
            state['bindings'] = [int(b) for b in config['Bindings']]
        save()


def attach(token):
    def attached(node_path, configuration):
        global node
        node = dbus.Interface(bus.get_object(MESH, node_path), NODE_IFACE)
        state['node'] = str(node_path)
        for element in configuration:
            for model in element[1]:
                if int(model[0]) == ONOFF_SERVER and 'Bindings' in model[1]:
                    state['bindings'] = [int(b) for b in model[1]['Bindings']]
        save()

    network = dbus.Interface(bus.get_object(MESH, '/org/bluez/mesh'), NETWORK_IFACE)
    network.Attach(APP_PATH, dbus.UInt64(token), reply_handler=attached, error_handler=error('Attach'))
    return False


def main():
    global state_file, bus
    uuid = bytes.fromhex(sys.argv[1])
    state_file = sys.argv[2]
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    Application()
    Agent()
    Element()
    save()
    network = dbus.Interface(bus.get_object(MESH, '/org/bluez/mesh'), NETWORK_IFACE)
    network.Join(APP_PATH, dbus.Array(uuid, signature='y'), reply_handler=lambda: None, error_handler=error('Join'))
    GLib.MainLoop().run()


if __name__ == '__main__':
    main()
