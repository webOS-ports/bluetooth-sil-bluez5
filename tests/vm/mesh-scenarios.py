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

# Drives the service's mesh API (/mesh/...) on the real bluez5 SIL against bluetooth-meshd, with a second meshd as
# the node it provisions and configures. Three virtual controllers (btvirt -l3):
#
#   hci0  bluetoothd: the adapter the service and the SIL use
#   hci1  the provisioner's meshd, on the system bus (the one the SIL talks to), HCI user channel
#   hci2  the node's meshd, on a D-Bus of its own (one bus takes one org.bluez.mesh), with mesh-node.py as its
#         application: one element with a Generic OnOff Server
#
# The image's bluetooth-mesh.service runs meshd with --use_raw on hci0 next to bluetoothd. That is not the default
# here: meshd 5.86 with the layer's patches refuses a controller bluetoothd already powered ("failed to start generic
# IO : already powered on"). MESH_PROVISIONER_IO picks how the provisioner's meshd gets its controller:
#
#   user  (default) hci1 of its own, HCI user channel, as above
#   raw   hci0, shared with bluetoothd: --use_raw --io generic:hci0, as the image's service runs it (needs a meshd
#         that takes a powered controller for the raw channel); the node is then on hci1
#   auto  hci0, shared with bluetoothd through the kernel's mesh MGMT interface (meshd's default --io auto; the
#         kernel needs its experimental mesh feature, 6.0 or later); the node is then on hci1
#
# With a shared controller, scenarios that need hci1 back in bluetoothd's hands or a fourth controller are skipped,
# and the provisioner meshd starts after bluetoothd and the service powered hci0.
#
# The provisioner sends with an AppKey only once the local node has it too (meshd: "Key not found"): createAppKey
# only makes the key, so the scenarios also add it to the local node (config/set APPKEY_ADD to address 1).
#
# Meant for a SIL and service built with AddressSanitizer/UBSan; their reports are listed at the end (see
# sil-scenarios.py, whose helpers this uses). Run as root on the target, next to sil-scenarios.py and mesh-node.py:
#
#   mesh-scenarios.py [scenario-name...]
#
# It forgets the mesh network: the service's mesh records in DB8 and the provisioner meshd's storage
# (/var/lib/bluetooth/mesh), before every scenario and at the end. Afterwards btvirt runs two controllers again,
# as sil-scenarios.py expects, and no meshd runs.
#
# Not covered: PB-GATT (the SIL has only PB-ADV), OOB provisioning (the node offers none; the SIL's agent prompts
# are not driven), the Node Reset Status (meshd removes a reset node before its status is sent, so removeNode
# always ends in the SIL's timeout; the outcome is the same).

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('sil_scenarios', os.path.join(HERE, 'sil-scenarios.py'))
sil = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sil)

call, client, check, settle, Failure, Pty = sil.call, sil.client, sil.check, sil.settle, sil.Failure, sil.Pty
SERVICE = sil.SERVICE

MESHD = '/usr/libexec/bluetooth/bluetooth-meshd'
WORK = os.environ.get('MESH_WORK_DIR', '/var/lib/bt-mesh-scenarios')
BUS_CONF = os.path.join(WORK, 'node-bus.conf')
BUS_SOCKET = os.path.join(WORK, 'node-bus.sock')
NODE_STORAGE = os.path.join(WORK, 'node-storage')
NODE_STATE = os.path.join(WORK, 'node-state.json')
PROV_STORAGE = '/var/lib/bluetooth/mesh'
NODE_UUID = 'c0ffee00000040008000000000000001'	# a valid version 4 UUID: meshd checks
ONOFF_SERVER = 0x1000
ONOFF_CLIENT = 0x1001	# a model the node does not have
LOCAL_NODE = 1
UNITS = ('mesh-scn-node-app', 'mesh-scn-node', 'mesh-scn-prov', 'mesh-scn-bus')
PROV_IO = os.environ.get('MESH_PROVISIONER_IO', 'user')
SHARED = PROV_IO != 'user'	# the provisioner's meshd on bluetoothd's hci0
NODE_HCI = 'hci1' if SHARED else 'hci2'
CONTROLLERS_USED = 2 if SHARED else 3
PROV_ARGS = {'user': ['--io', 'generic:hci1'], 'raw': ['--use_raw', '--io', 'generic:hci0'], 'auto': []}[PROV_IO]

BUS_CONFIG = '''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>custom</type>
  <listen>unix:path=%s</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow own="*"/>
    <allow send_destination="*"/>
    <allow receive_sender="*"/>
  </policy>
</busconfig>
''' % BUS_SOCKET


def run(*args):
    subprocess.run(list(args), check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def start_unit(name, args, env=None):
    run('systemctl', 'stop', name)
    run('systemctl', 'reset-failed', name)
    cmd = ['systemd-run', '-q', '--unit=' + name]
    for key, value in (env or {}).items():
        cmd += ['-E', '%s=%s' % (key, value)]
    run(*(cmd + args))


def active(name):
    return subprocess.run(['systemctl', 'is-active', '--quiet', name]).returncode == 0


def unit_pid(name):
    out = subprocess.run(['systemctl', 'show', '-p', 'MainPID', '--value', name], capture_output=True, text=True)
    return int(out.stdout.strip() or 0)


def mesh(method, payload, timeout=15.0):
    return call('mesh/' + method, payload, timeout)


def subscribed(method, payload, until, timeout=30.0):
    """A subscription call; its replies once one satisfies until (or the timeout)."""
    c = client('mesh/' + method, dict(payload, subscribe=True))
    replies = c.wait(timeout=timeout, until=until)
    c.stop()
    return replies


def ended(r):
    return not r.get('returnValue', True) or r.get('subscribed') is False


def start_provisioner():
    """The provisioner meshd on hci1 (or hci0, shared: see MESH_PROVISIONER_IO). bluetoothd takes hci1 whenever
    meshd lets go of it, and meshd's user channel only binds to one that is not powered."""
    run('systemctl', 'stop', 'mesh-scn-prov')
    if not SHARED:
        run('btmgmt', '--index', '1', 'power', 'off')
        run('hciconfig', 'hci1', 'down')
    start_unit('mesh-scn-prov', [MESHD, '--nodetach', '--debug'] + PROV_ARGS)
    end = time.time() + 10
    while time.time() < end and not active('mesh-scn-prov'):
        settle(0.2)
    settle(2.0)
    check(active('mesh-scn-prov'), 'the provisioner meshd did not start')


NODE_ENV = {'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=' + BUS_SOCKET}


def start_node_meshd():
    """The node's meshd on hci2 (hci1 when the provisioner shares hci0), with empty storage. Before bluetoothd: a
    controller bluetoothd had used, powered down and given to meshd, sent nothing meshd queued (no beacon was ever
    seen)."""
    run('systemctl', 'stop', 'mesh-scn-node')
    shutil.rmtree(NODE_STORAGE, ignore_errors=True)
    os.makedirs(NODE_STORAGE)
    run('hciconfig', NODE_HCI, 'down')
    start_unit('mesh-scn-node', [MESHD, '--nodetach', '--debug', '--io', 'generic:' + NODE_HCI,
                                 '--storage', NODE_STORAGE], NODE_ENV)


def start_node():
    """A fresh unprovisioned node: mesh-node.py asking the node's meshd to Join()."""
    if os.path.exists(NODE_STATE):
        os.remove(NODE_STATE)
    start_unit('mesh-scn-node-app', ['/usr/bin/python3', os.path.join(HERE, 'mesh-node.py'), NODE_UUID, NODE_STATE],
               NODE_ENV)
    settle(1.0)
    check(active('mesh-scn-node') and active('mesh-scn-node-app'), 'the node meshd or its application did not start')


def node_state():
    try:
        with open(NODE_STATE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def wait_node(predicate, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        state = node_state()
        if predicate(state):
            return state
        settle(0.2)
    return node_state()


def forget_network():
    """The service's mesh records and the provisioner's storage. DB8 lets only the service delete its kinds, and
    only while the service itself is not running."""
    run('systemctl', 'stop', SERVICE)
    for kind in ('meshtoken', 'meshappkey', 'meshnodeinfo'):
        db = Pty(['luna-send', '-m', 'com.webos.service.bluetooth2', '-n', '1', 'luna://com.webos.service.db/del',
                  json.dumps({'query': {'from': 'com.webos.service.bluetooth2.%s:1' % kind}})])
        db.wait(1, 10)
        db.stop()
    run('systemctl', 'stop', 'mesh-scn-prov')
    shutil.rmtree(PROV_STORAGE, ignore_errors=True)


def controllers():
    try:
        return sorted(n for n in os.listdir('/sys/class/bluetooth') if n.startswith('hci') and ':' not in n)
    except OSError:
        return []


def start_btvirt(count):
    """btvirt with count controllers. Killed, it takes a few seconds to go, and a new unit of the same name could
    not start meanwhile."""
    run('systemctl', 'kill', '-s', 'KILL', 'btvirt')
    end = time.time() + 15
    while time.time() < end and (active('btvirt') or controllers()):
        settle(0.2)
    run('systemctl', 'reset-failed', 'btvirt')
    run('systemd-run', '--unit=btvirt', '/usr/bin/btvirt', '-l%d' % count)
    end = time.time() + 10
    while time.time() < end and len(controllers()) < count:
        settle(0.2)
    check(len(controllers()) == count, 'btvirt gave %r' % controllers())
    settle(1.0)
    # connman soft-blocks controllers that appear while its Bluetooth technology is disabled.
    run('rfkill', 'unblock', 'bluetooth')


def without_meshd():
    """The adapters bluetoothd has while the provisioner meshd is stopped: hci1 comes back to it, a shared hci0 is
    its all along."""
    return 1 if SHARED else 2


def restart_mesh_stack(count=3):
    """Fresh controllers (three, or a fourth for bluetoothd as hci3), the node's bus, the provisioner meshd before
    bluetoothd (so it gets hci1), no network, and the service."""
    # bluetoothd stays down until both meshds have their controllers: D-Bus activation (pulseaudio, connman)
    # started it again behind the stop, it took the new controllers, and a controller bluetoothd had used, powered
    # down and given to meshd's user channel, carried nothing (no beacon seen, no scan result).
    run('systemctl', 'mask', '--runtime', 'bluetooth')
    run('systemctl', 'stop', SERVICE, 'bluetooth', 'obex', 'bluetooth-mesh', *UNITS[:3])
    start_btvirt(count)
    os.makedirs(WORK, exist_ok=True)
    with open(BUS_CONF, 'w') as f:
        f.write(BUS_CONFIG)
    if not active('mesh-scn-bus'):
        start_unit('mesh-scn-bus', ['dbus-daemon', '--nofork', '--config-file=' + BUS_CONF])
        settle(1.0)
    forget_network()
    start_node_meshd()
    if not SHARED:
        start_provisioner()
    run('systemctl', 'unmask', '--runtime', 'bluetooth')
    run('systemctl', 'start', 'bluetooth')
    settle(2.0)
    run('systemctl', 'start', SERVICE)
    found = sil.wait_adapters(count - 2 if not SHARED else 1)
    if SHARED:	# as on the image: bluetoothd has powered hci0 before meshd comes
        sil.set_state(found['hci0']['adapterAddress'], powered=True)
        settle(1.0)
        start_provisioner()
    settle(2.0)


def adapter():
    return sil.wait_adapters(1)['hci0']['adapterAddress']


def create_network(adapter_address=None):
    """createNetwork's reply: the network id, which comes later, or an error. It takes no subscribe, but answers
    the call again once meshd made the network."""
    c = client('mesh/createNetwork', {'adapterAddress': adapter_address} if adapter_address else {})
    replies = c.wait(timeout=30, until=lambda r: 'networkId' in r or not r.get('returnValue', True))
    c.stop()
    return next((r for r in replies if 'networkId' in r or not r.get('returnValue', True)), None)


def provisioned():
    """A network with AppKey 0, the node provisioned into it, the key added to the node and bound to its OnOff
    server, and added to the local node too (meshd sends with an AppKey only when the sending node has it).
    Returns the node's unicast address."""
    start_node()
    reply = create_network()
    check(reply and 'networkId' in reply, 'createNetwork: %r' % reply)
    reply = mesh('createAppKey', {'netKeyIndex': 0, 'appKeyIndex': 0})
    check(reply and reply.get('returnValue'), 'createAppKey: %r' % reply)
    found = subscribed('scanUnprovisionedDevices', {'scanTimeout': 10},
                       lambda r: any(d.get('uuid') == NODE_UUID for d in r.get('devices', [])), 20)
    check(any(d.get('uuid') == NODE_UUID for r in found for d in r.get('devices', [])),
          'the scan did not find the node: %r' % found[-2:])
    replies = subscribed('provision', {'uuid': NODE_UUID},
                         lambda r: r.get('request') == 'endProvision' or not r.get('returnValue', True), 60)
    done = next((r for r in replies if r.get('request') == 'endProvision'), None)
    check(done and done.get('returnValue') and done.get('unicastAddress'), 'provision: %r' % replies)
    unicast = done['unicastAddress']
    check(wait_node(lambda s: s.get('node')).get('node'), 'the node did not attach: %r' % node_state())
    for dest, config, extra in ((unicast, 'APPKEY_ADD', {}), (unicast, 'APPKEY_BIND', {'modelId': ONOFF_SERVER}),
                                (LOCAL_NODE, 'APPKEY_ADD', {})):
        reply = config_set(dest, config, appKeyIndex=0, **extra)
        check(reply and reply.get('returnValue') and reply.get('config') == config, '%s to %d: %r' %
              (config, dest, reply))
    check(wait_node(lambda s: s.get('bindings') == [0]).get('bindings') == [0],
          'the node reports no binding: %r' % node_state())
    return unicast


def config_get(dest, config):
    replies = subscribed('model/config/get', {'destAddress': dest, 'config': config, 'netKeyIndex': 0},
                         lambda r: 'config' in r or ended(r))
    return next((r for r in replies if 'config' in r or ended(r)), None)


def config_set(dest, config, **values):
    replies = subscribed('model/config/set', dict(values, destAddress=dest, config=config, netKeyIndex=0),
                         lambda r: 'config' in r or ended(r))
    return next((r for r in replies if 'config' in r or ended(r)), None)


def set_onoff(dest, state):
    replies = subscribed('model/onOff/set', {'destAddress': dest, 'appKeyIndex': 0, 'state': state},
                         lambda r: 'onOFF' in r or not r.get('returnValue', True))
    return next((r for r in replies if 'onOFF' in r or not r.get('returnValue', True)), None)


# ---------------------------------------------------------------- scenarios

def network_created_and_described():
    """createNetwork answers with the network id; getMeshInfo describes it, with the AppKey made; a second
    createNetwork is refused as existing."""
    reply = mesh('getMeshInfo', {})
    check(reply and not reply.get('returnValue'), 'getMeshInfo before any network: %r' % reply)
    reply = create_network()
    check(reply and reply.get('returnValue') and reply.get('networkId'), 'createNetwork: %r' % reply)
    reply = mesh('createAppKey', {'netKeyIndex': 0, 'appKeyIndex': 0})
    check(reply and reply.get('returnValue'), 'createAppKey: %r' % reply)
    info = (mesh('getMeshInfo', {}) or {}).get('meshInfo', {})
    check([k.get('index') for k in info.get('netKeys', [])] == [0], 'netKeys: %r' % info)
    check([k.get('index') for k in info.get('appKeys', [])] == [0], 'appKeys: %r' % info)
    check(info.get('provisioners'), 'no provisioner: %r' % info)
    reply = mesh('createNetwork', {})
    check(reply and reply.get('errorCode') == 39, 'second createNetwork: %r' % reply)


def create_network_refused_reaches_the_client():
    """meshd goes away before it answered CreateNetwork: the caller gets an error, and a later createNetwork
    works. Before, the refusal was only logged: the caller waited for good, and the service took the network
    for created, refusing every later createNetwork as existing."""
    pid = unit_pid('mesh-scn-prov')
    os.kill(pid, 19)	# SIGSTOP: the call waits in its socket
    c = client('mesh/createNetwork', {})
    settle(1.5)
    run('systemctl', 'kill', '-s', 'KILL', 'mesh-scn-prov')
    replies = c.wait(timeout=10, until=lambda r: 'networkId' in r or not r.get('returnValue', True))
    c.stop()
    check(any(not r.get('returnValue', True) for r in replies), 'no error for the refused network: %r' % replies)
    start_provisioner()
    reply = create_network()
    check(reply and reply.get('networkId'), 'createNetwork after the refused one: %r' % reply)


def lost_network_is_made_again():
    """The service's database names a network the provisioner meshd no longer has: meshd answers the attach with
    NotFound, the service drops the stored network, and the next createNetwork makes a new one that works. Before,
    the token stayed: every createNetwork answered NETWORK_EXISTS and tried the same attach, with no way out."""
    reply = create_network()
    check(reply and reply.get('networkId'), 'createNetwork: %r' % reply)
    lost = reply['networkId']
    run('systemctl', 'stop', SERVICE)
    run('systemctl', 'stop', 'mesh-scn-prov')
    shutil.rmtree(PROV_STORAGE, ignore_errors=True)
    start_provisioner()
    run('systemctl', 'start', SERVICE)
    sil.wait_adapters(1)
    settle(2.0)
    reply = mesh('createNetwork', {})
    check(reply and reply.get('errorCode') == 39, 'createNetwork with a stored token: %r' % reply)
    settle(2.0)
    reply = mesh('createAppKey', {'netKeyIndex': 0, 'appKeyIndex': 0})
    check(reply and reply.get('errorCode') == 327, 'createAppKey on the lost network: %r' % reply)
    reply = create_network()
    check(reply and reply.get('networkId') and reply['networkId'] != lost,
          'createNetwork after the lost one (was %s): %r' % (lost, reply))
    reply = mesh('createAppKey', {'netKeyIndex': 0, 'appKeyIndex': 0})
    check(reply and reply.get('returnValue'), 'createAppKey on the new network: %r' % reply)
    info = (mesh('getMeshInfo', {}) or {}).get('meshInfo', {})
    check([k.get('index') for k in info.get('appKeys', [])] == [0], 'appKeys of the new network: %r' % info)


def unanswered_attach_keeps_the_network():
    """An attach that fails without meshd saying the node is unknown keeps the stored network: meshd not running
    when createNetwork attaches (the SIL attaches by itself once meshd is back), then meshd stopped (SIGSTOP) so the
    attach times out: the network is kept, and a restarted service attaches to it again."""
    reply = create_network()
    check(reply and reply.get('networkId'), 'createNetwork: %r' % reply)
    run('systemctl', 'stop', SERVICE)
    run('systemctl', 'stop', 'mesh-scn-prov')
    run('systemctl', 'start', SERVICE)
    sil.wait_adapters(without_meshd())	# hci1, given back to bluetoothd
    settle(2.0)
    reply = mesh('createNetwork', {})
    check(reply and reply.get('errorCode') == 39, 'createNetwork with meshd not running: %r' % reply)
    start_provisioner()
    settle(2.0)
    reply = mesh('createAppKey', {'netKeyIndex': 0, 'appKeyIndex': 0})
    check(reply and reply.get('returnValue'), 'createAppKey once meshd is back: %r' % reply)

    run('systemctl', 'restart', SERVICE)
    sil.wait_adapters(1)
    settle(2.0)
    os.kill(unit_pid('mesh-scn-prov'), 19)	# SIGSTOP: the attach gets no answer
    reply = mesh('createNetwork', {})
    check(reply and reply.get('errorCode') == 39, 'createNetwork with meshd stopped: %r' % reply)
    settle(28.0)	# GDBus' default 25 s call timeout
    reply = mesh('createAppKey', {'netKeyIndex': 0, 'appKeyIndex': 1})
    check(reply and reply.get('errorCode') == 327, 'createAppKey after the attach timed out: %r' % reply)
    # The record must still be there: a service that starts afresh (the SIL no longer has the token) attaches to it.
    run('systemctl', 'stop', SERVICE)
    run('systemctl', 'kill', '-s', 'KILL', 'mesh-scn-prov')
    settle(1.0)
    start_provisioner()
    settle(1.0)
    run('systemctl', 'reset-failed', SERVICE)
    run('systemctl', 'start', SERVICE)
    sil.wait_adapters(1)
    settle(2.0)
    reply = mesh('createNetwork', {})
    check(reply and reply.get('errorCode') == 39, 'createNetwork after the timed-out attach: %r' % reply)
    settle(2.0)
    reply = mesh('createAppKey', {'netKeyIndex': 0, 'appKeyIndex': 1})
    check(reply and reply.get('returnValue'), 'createAppKey on the kept network: %r' % reply)


def meshd_restart_is_survived():
    """The provisioner meshd restarted twice under a running service (bluetoothd shows hci1 as a second adapter in
    between): mesh calls keep working. Before, the SIL never attached again (every call NOT_ALLOWED until the
    service restarted), and the second adapter's teardown unexported the first one's application at "/"."""
    reply = create_network()
    check(reply and reply.get('networkId'), 'createNetwork: %r' % reply)
    for index in range(2):
        run('systemctl', 'stop', 'mesh-scn-prov')
        sil.wait_adapters(without_meshd())	# hci1, given back to bluetoothd
        settle(1.0)
        start_provisioner()
        settle(2.0)
        reply = mesh('createAppKey', {'netKeyIndex': 0, 'appKeyIndex': index})
        check(reply and reply.get('returnValue'), 'createAppKey after meshd restart %d: %r' % (index, reply))


def network_on_either_of_two_adapters():
    """Two adapters (hci0 and hci3), each with its mesh profile: a new network made through either one reaches its
    caller. Both profiles exported the mesh application at "/" on the one connection of the process: only one
    could, so meshd's JoinComplete for a network made through the other went to the first one's handler, the
    network id was reported for the wrong adapter, and the caller waited for good. (When the adapter that had
    the export went, its teardown also left nothing at "/" for meshd to read.)"""
    for name in ('hci0', 'hci3'):
        forget_network()
        start_provisioner()
        run('systemctl', 'start', SERVICE)
        address = sil.wait_adapters(2)[name]['adapterAddress']
        settle(2.0)
        reply = create_network(address)
        check(reply and reply.get('networkId') and reply.get('adapterAddress') == address,
              'createNetwork on %s: %r' % (name, reply))
        reply = mesh('createAppKey', {'adapterAddress': address, 'netKeyIndex': 0, 'appKeyIndex': 0})
        check(reply and reply.get('returnValue'), 'createAppKey on %s: %r' % (name, reply))


def provision_and_configure():
    """The node is found, provisioned and configured: composition data, the config gets, AppKey add and bind
    (the node's meshd reports the binding), a default TTL set and read back, and a refused bind is an error that
    leaves the next request answered."""
    unicast = provisioned()
    nodes = (mesh('listProvisionedNodes', {}) or {}).get('nodes', [])
    check([(n.get('primaryElementAddress'), n.get('uuid')) for n in nodes] == [(unicast, NODE_UUID)],
          'listProvisionedNodes: %r' % nodes)
    replies = subscribed('model/config/getCompositionData', {'destAddress': unicast},
                         lambda r: 'compositionData' in r or ended(r))
    data = next((r['compositionData'] for r in replies if 'compositionData' in r), None)
    check(data and data.get('companyId') == 0x05f1, 'composition data: %r' % replies)
    check(ONOFF_SERVER in data['elements'][0].get('sigModelIds', []), 'no OnOff server in %r' % data)

    reply = config_get(unicast, 'DEFAULT_TTL')
    check(reply and reply.get('ttl') == 127, 'DEFAULT_TTL: %r' % reply)
    reply = config_get(unicast, 'GATT_PROXY')
    check(reply and 'gattProxyState' in reply, 'GATT_PROXY: %r' % reply)
    reply = config_get(unicast, 'RELAY')
    check(reply and 'relayStatus' in reply, 'RELAY: %r' % reply)
    reply = config_get(unicast, 'APPKEYINDEX')
    check(reply and reply.get('appKeyIndexes') == [0], 'APPKEYINDEX: %r' % reply)

    reply = config_set(unicast, 'DEFAULT_TTL', ttl=10)
    check(reply and reply.get('returnValue'), 'DEFAULT_TTL set: %r' % reply)
    reply = config_get(unicast, 'DEFAULT_TTL')
    check(reply and reply.get('ttl') == 10, 'DEFAULT_TTL after the set: %r' % reply)

    # The node has no OnOff client: its Model App Status refuses the bind (Invalid Model). Twice, the first caller
    # still listening: a refused request's watch stayed, and took the next result for the same node and config,
    # so the second caller was never answered.
    bind = {'destAddress': unicast, 'config': 'APPKEY_BIND', 'netKeyIndex': 0, 'appKeyIndex': 0,
            'modelId': ONOFF_CLIENT, 'subscribe': True}
    first = client('mesh/model/config/set', bind)
    replies = first.wait(timeout=30, until=ended)
    check(any(not r.get('returnValue', True) for r in replies), 'bind to a model the node lacks: %r' % replies)
    reply = config_set(unicast, 'APPKEY_BIND', appKeyIndex=0, modelId=ONOFF_CLIENT)
    first.stop()
    check(reply and not reply.get('returnValue'), 'the second refused bind: %r' % reply)
    reply = config_get(unicast, 'DEFAULT_TTL')
    check(reply and reply.get('ttl') == 10, 'the request after the refused one: %r' % reply)


def onoff_reaches_the_node():
    """Generic OnOff set to the node, acknowledged: its server takes the state and the client gets it."""
    unicast = provisioned()
    for state in (True, False, True):
        reply = set_onoff(unicast, state)
        check(reply and reply.get('returnValue') and reply.get('onOFF') is state, 'onOff %s: %r' % (state, reply))
        got = wait_node(lambda s: s.get('onoff') == int(state))
        check(got.get('onoff') == int(state), 'the node has %r, not %r' % (got.get('onoff'), state))


def key_refresh_keeps_the_node():
    """A key refresh of NetKey 0 and its AppKey runs through its phases to completion, and the node still takes an
    OnOff set afterwards (it has the new keys)."""
    unicast = provisioned()
    # waitTimeout is also how long each key update waits for the node's status: at 2 s, 2 of 6 runs had a
    # status arrive late (segmented messages over btvirt), reported as a failed update.
    replies = subscribed('keyRefresh', {'netKeyIndex': 0, 'refreshAppKeys': True, 'waitTimeout': 10},
                         lambda r: r.get('status') == 'completed' or not r.get('returnValue', True), 120)
    phases = [r.get('keyRefreshPhase') for r in replies if r.get('status') == 'active']
    check(any(r.get('status') == 'completed' for r in replies), 'key refresh did not complete: %r' % replies)
    check(phases == [1, 2, 3], 'phases %r: %r' % (phases, replies))
    reply = set_onoff(unicast, True)
    check(reply and reply.get('onOFF') is True, 'onOff after the key refresh: %r' % reply)
    check(wait_node(lambda s: s.get('onoff') == 1).get('onoff') == 1, 'the node did not take it')


def remove_node():
    """removeNode resets the node (its meshd drops it) and the service forgets it."""
    unicast = provisioned()
    reply = mesh('removeNode', {'primaryElementAddress': unicast})
    check(reply and reply.get('returnValue'), 'removeNode: %r' % reply)
    end = time.time() + 10
    while time.time() < end and os.listdir(NODE_STORAGE):
        settle(0.2)
    check(not os.listdir(NODE_STORAGE), 'the node meshd kept the node: %r' % os.listdir(NODE_STORAGE))
    settle(3.0)	# the Node Reset Status never comes (see the top): the SIL's 2 s timeout
    check((mesh('listProvisionedNodes', {}) or {}).get('nodes') == [], 'the service still lists the node')


def service_restarts_keep_the_network():
    """The service restarted three times: createNetwork attaches to the stored network each time, and the node
    still answers."""
    unicast = provisioned()
    for _ in range(3):
        run('systemctl', 'restart', SERVICE)
        sil.wait_adapters(1)
        settle(2.0)
        reply = mesh('createNetwork', {})
        check(reply and reply.get('errorCode') == 39, 'createNetwork after the restart: %r' % reply)
        settle(1.0)
        reply = config_get(unicast, 'DEFAULT_TTL')
        check(reply and reply.get('ttl') == 127, 'DEFAULT_TTL after the restart: %r' % reply)


SCENARIOS = [
    network_created_and_described,
    create_network_refused_reaches_the_client,
    lost_network_is_made_again,
    unanswered_attach_keeps_the_network,
    meshd_restart_is_survived,
    network_on_either_of_two_adapters,
    provision_and_configure,
    onoff_reaches_the_node,
    key_refresh_keeps_the_node,
    remove_node,
    service_restarts_keep_the_network,
]


RESTART_THE_SERVICE = (lost_network_is_made_again, unanswered_attach_keeps_the_network,
                       network_on_either_of_two_adapters, service_restarts_keep_the_network)
CONTROLLERS = {'network_on_either_of_two_adapters': 4}
NOT_SHARED = (network_on_either_of_two_adapters,)	# needs a fourth controller for bluetoothd


def restore():
    """No meshd and no network left; btvirt with two controllers again."""
    for name in UNITS:
        run('systemctl', 'stop', name)
        run('systemctl', 'reset-failed', name)
    forget_network()
    shutil.rmtree(WORK, ignore_errors=True)
    run('systemctl', 'mask', '--runtime', 'bluetooth')
    run('systemctl', 'stop', 'bluetooth')
    start_btvirt(2)
    run('systemctl', 'unmask', '--runtime', 'bluetooth')
    run('systemctl', 'start', 'bluetooth')


def main(argv):
    wanted = set(argv[1:])
    failures = ran = 0
    if os.path.isdir(sil.SANITIZER_LOGS):
        for name in os.listdir(sil.SANITIZER_LOGS):
            os.remove(os.path.join(sil.SANITIZER_LOGS, name))
    try:
        for scenario in SCENARIOS:
            if wanted and scenario.__name__ not in wanted:
                continue
            if SHARED and scenario in NOT_SHARED:
                print('SKIP  %s (MESH_PROVISIONER_IO=%s)' % (scenario.__name__, PROV_IO))
                continue
            ran += 1
            try:
                restart_mesh_stack(CONTROLLERS.get(scenario.__name__, CONTROLLERS_USED))
                pid = sil.main_pid()
                scenario()
                if scenario not in RESTART_THE_SERVICE:
                    check(sil.main_pid() == pid, 'the service restarted (pid %s -> %s)' % (pid, sil.main_pid()))
                print('PASS  %s' % scenario.__name__)
            except (Failure, RuntimeError) as e:
                failures += 1
                print('FAIL  %s: %s' % (scenario.__name__, e))
            sys.stdout.flush()
    finally:
        restore()
    settle(2.0)
    print('%d of %d scenarios passed' % (ran - failures, ran))
    findings = sil.sanitizer_findings()
    for finding in findings:
        print('SANITIZER  %s' % finding)
    return 1 if failures or findings else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
