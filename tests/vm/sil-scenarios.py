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

# Drives the service on the real bluez5 SIL against two virtual controllers (btvirt -l2, which needs a kernel with
# CONFIG_BT_HCIVHCI): discovery, pairing both ways, bluetoothd going away, OBEX pushes between them, and service
# restarts. Meant for a SIL and service built with AddressSanitizer/UBSan; their reports that point at either are
# listed at the end.
#
# Run as root on the target, with the service's environment file selecting the bluez5 SIL:
#
#   sil-scenarios.py [scenario-name...]
#
# luna-send and obexctl only flush their output to a terminal, so they run on pseudo-terminals.
#
# Needs: a kernel with CONFIG_BT_HCIVHCI, btvirt (bluez5-noinst-tools; started here when not running), obexctl.
# Not testable with btvirt: numeric comparison (its controllers pair as NoInputNoOutput whatever the agents say,
# bluetoothctl's too), and a one-sided unpair followed by a new pairing (the initiator never gets its Auth
# Complete). btvirt may itself crash (SIGSEGV) when both sides unpair; it is restarted.

import json
import os
import pty
import re
import select
import signal
import subprocess
import sys
import threading
import time

SERVICE = 'webos-bluetooth-service'
URI = 'luna://com.webos.service.bluetooth2/'
APP_ID = 'com.palm.app.settings'
SANITIZER_LOGS = os.environ.get('SANITIZER_LOG_DIR', '/var/log/bt2-asan')
OUR_MODULES = ('/usr/sbin/webos-bluetooth-service', '/bluetooth-sils/bluez5.so')
OPP_ROOT = '/media/internal'


class Pty:
    """A command on a pseudo-terminal; lines are collected, JSON lines also parsed."""

    def __init__(self, args, env=None):
        master, slave = pty.openpty()
        self.proc = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave, close_fds=True, env=env)
        os.close(slave)
        self.master = master
        self.lines = []
        self.replies = []
        self.buffer = b''
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        while True:
            try:
                ready, _, _ = select.select([self.master], [], [], 0.2)
                if not ready:
                    if self.proc.poll() is not None:
                        break
                    continue
                data = os.read(self.master, 65536)
            except OSError:
                break
            if not data:
                break
            self.buffer += data
            while b'\n' in self.buffer:
                line, self.buffer = self.buffer.split(b'\n', 1)
                text = re.sub(r'\x1b\[[0-9;]*[A-Za-z]|\r', '', line.decode(errors='replace')).strip()
                with self.lock:
                    self.lines.append(text)
                    if text.startswith('{'):
                        try:
                            self.replies.append(json.loads(text))
                        except ValueError:
                            pass

    def write(self, text):
        os.write(self.master, text.encode())

    def wait(self, count=1, timeout=10.0, until=None):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if until is not None and any(until(r) for r in self.replies):
                    return list(self.replies)
                if until is None and len(self.replies) >= count:
                    return list(self.replies)
            time.sleep(0.1)
        with self.lock:
            return list(self.replies)

    def wait_line(self, pattern, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for line in self.lines:
                    if re.search(pattern, line):
                        return line
            time.sleep(0.1)
        return None

    def all(self):
        with self.lock:
            return list(self.replies)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()
        self.thread.join(1)
        try:
            os.close(self.master)
        except OSError:
            pass


def client(method, payload, interactive=True):
    return Pty(['luna-send', '-a', APP_ID] + (['-i'] if interactive else ['-n', '1']) +
               [URI + method, json.dumps(payload)])


def call(method, payload, timeout=10.0):
    c = client(method, payload, interactive=False)
    replies = c.wait(1, timeout)
    c.stop()
    return replies[0] if replies else None


def main_pid():
    out = subprocess.run(['systemctl', 'show', '-p', 'MainPID', '--value', SERVICE], capture_output=True, text=True)
    return out.stdout.strip()


def settle(seconds=0.5):
    time.sleep(seconds)


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def adapters():
    reply = call('adapter/getStatus', {})
    check(reply and reply.get('returnValue'), 'getStatus failed: %r' % reply)
    return {a['interfaceName']: a for a in reply.get('adapters', [])}


def wait_adapters(count=2, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        found = adapters()
        if len(found) >= count:
            return found
        settle(1.0)
    raise Failure('expected %d adapters, have %r' % (count, list(adapters())))


def set_state(address, **state):
    payload = dict(state, adapterAddress=address)
    reply = call('adapter/setState', payload)
    check(reply and reply.get('returnValue'), 'setState %r failed: %r' % (payload, reply))


def pair_a_with_b():
    """Returns the two adapters' addresses, A powered and B powered and discoverable."""
    found = wait_adapters()
    a, b = found['hci0']['adapterAddress'], found['hci1']['adapterAddress']
    set_state(a, powered=True)
    set_state(b, powered=True, discoverable=True, discoverableTimeout=0)
    return a, b


def devices(adapter):
    reply = call('device/getStatus', {'adapterAddress': adapter})
    check(reply and reply.get('returnValue'), 'device/getStatus failed: %r' % reply)
    return reply.get('devices', [])


def device(adapter, address):
    return next((d for d in devices(adapter) if d.get('address') == address), None)


def discover(a, b, timeout=20.0):
    reply = call('adapter/startDiscovery', {'adapterAddress': a})
    check(reply and reply.get('returnValue'), 'startDiscovery failed: %r' % reply)
    end = time.time() + timeout
    while time.time() < end and not device(a, b):
        settle(1.0)
    call('adapter/cancelDiscovery', {'adapterAddress': a})
    check(device(a, b), '%s did not find %s' % (a, b))


def unpair_all():
    for adapter in adapters().values():
        for d in devices(adapter['adapterAddress']):
            if d.get('paired'):
                call('adapter/unpair', {'adapterAddress': adapter['adapterAddress'], 'address': d['address']})


def pair(a, b, accept_on_b=True, accept_on_a=True, timeout=30.0):
    """A pairs with B, answering confirmations if any come. Returns (pair replies, B's request replies)."""
    incoming = client('adapter/awaitPairingRequests', {'adapterAddress': b, 'subscribe': True})
    check(incoming.wait(1), 'awaitPairingRequests did not answer')
    outgoing = client('adapter/pair', {'adapterAddress': a, 'address': b, 'subscribe': True})
    answered_a = answered_b = False
    end = time.time() + timeout
    while time.time() < end:
        for r in outgoing.all():
            if r.get('request') == 'confirmPasskey' and not answered_a:
                call('adapter/supplyPasskeyConfirmation', {'adapterAddress': a, 'address': b, 'accept': accept_on_a})
                answered_a = True
        for r in incoming.all():
            if r.get('request') == 'confirmPasskey' and not answered_b:
                call('adapter/supplyPasskeyConfirmation', {'adapterAddress': b, 'address': a, 'accept': accept_on_b})
                answered_b = True
        done = [r for r in outgoing.all() if 'returnValue' in r and (r.get('subscribed') is False or
                                                                     not r.get('returnValue'))]
        if done:
            break
        settle(0.3)
    result = (outgoing.all(), incoming.all())
    outgoing.stop()
    incoming.stop()
    return result


ENV_FILE = '/var/systemd/system/env/webos-bluetooth-service.env'
CAPABILITY_VAR = 'WEBOS_BLUETOOTH_PAIRING_IO_CAPABILITY'
VIRTUAL_PREFIX = '00:AA:01:'	# btvirt's controller addresses


def set_capability(capability):
    """The service's pairing IO capability (its build default when None), through its environment file."""
    lines = [l for l in read_lines(ENV_FILE) if not l.startswith(CAPABILITY_VAR + '=')]
    if capability:
        lines.append('%s=%s\n' % (CAPABILITY_VAR, capability))
    with open(ENV_FILE, 'w') as f:
        f.writelines(lines)


def forget_virtual_bonds():
    """Bonds between the virtual controllers, from earlier runs, would pair without asking."""
    base = '/var/lib/bluetooth'
    for adapter in os.listdir(base) if os.path.isdir(base) else []:
        if not adapter.startswith(VIRTUAL_PREFIX):
            continue
        for peer in os.listdir(os.path.join(base, adapter)):
            if peer.startswith(VIRTUAL_PREFIX):
                subprocess.run(['rm', '-rf', os.path.join(base, adapter, peer)], check=False)


def ensure_btvirt():
    """Two virtual controllers from btvirt, (re)started as a transient unit when it is not running."""
    if subprocess.run(['systemctl', 'is-active', '--quiet', 'btvirt']).returncode != 0:
        subprocess.run(['systemctl', 'reset-failed', 'btvirt'], check=False)
        subprocess.run(['systemd-run', '--unit=btvirt', '/usr/bin/btvirt', '-l2'], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        settle(2.0)


def restart_bluetooth_stack(capability=None):
    ensure_btvirt()
    # connman soft-blocks controllers that appear while its Bluetooth technology is disabled.
    subprocess.run(['rfkill', 'unblock', 'bluetooth'], check=False)
    subprocess.run(['systemctl', 'stop', SERVICE], check=False)
    subprocess.run(['systemctl', 'stop', 'bluetooth'], check=False)
    forget_virtual_bonds()
    set_capability(capability)
    subprocess.run(['systemctl', 'start', 'bluetooth'], check=False)
    # obexd is D-Bus activated; obexctl waits for it without activating it.
    subprocess.run(['systemctl', 'start', 'obex'], check=False)
    subprocess.run(['systemctl', 'start', SERVICE], check=False)
    wait_adapters()
    settle(2.0)


# ---------------------------------------------------------------- scenarios

def power_cycles():
    """Both adapters powered off and on a few times; the SIL reports each state."""
    found = wait_adapters()
    for _ in range(3):
        for name in ('hci0', 'hci1'):
            address = found[name]['adapterAddress']
            set_state(address, powered=False)
            settle(0.5)
            check(not adapters()[name]['powered'], '%s still powered' % name)
            set_state(address, powered=True)
            settle(0.5)
            check(adapters()[name]['powered'], '%s not powered' % name)


def discovery_finds_the_other_adapter():
    """A finds B over the virtual air, and B's name comes with it."""
    a, b = pair_a_with_b()
    discover(a, b)
    found = device(a, b)
    check(found.get('name'), 'device found without a name: %r' % found)


def pairing_just_works():
    """With the default capability (NoInputNoOutput) A pairs with B without asking either side."""
    a, b = pair_a_with_b()
    discover(a, b)
    out, inc = pair(a, b)
    check(not any(r.get('request') == 'confirmPasskey' for r in out + inc), 'asked although NoInputNoOutput')
    settle(2.0)
    check((device(a, b) or {}).get('paired'), 'A does not list B as paired: %r' % out)
    check((device(b, a) or {}).get('paired'), 'B does not list A as paired')
    reply = call('adapter/unpair', {'adapterAddress': a, 'address': b})
    check(reply and reply.get('returnValue'), 'unpair failed: %r' % reply)
    settle(1.0)
    check(not (device(a, b) or {}).get('paired'), 'A still lists B as paired')


def pairing_again_after_unpair():
    """Found, paired and unpaired on both sides, three times: each pairing completes and each unpair takes it back."""
    a, b = pair_a_with_b()
    for round in range(3):
        discover(a, b)	# unpair removes the device in BlueZ: found again first
        out, inc = pair(a, b)
        ends = [r for r in out if r.get('request') == 'endPairing' or r.get('subscribed') is False]
        check(ends, 'round %d: pairing did not end: %r' % (round, out))
        settle(1.5)
        check((device(a, b) or {}).get('paired'), 'round %d: not paired: %r, B got %r' % (round, out, inc))
        # Both sides: with B still bonded, btvirt never sends A its Auth Complete for the next pairing (A then
        # times out), while plain bluetoothd shows the same. A one-sided unpair cannot be tested here.
        for x, y in ((a, b), (b, a)):
            reply = call('adapter/unpair', {'adapterAddress': x, 'address': y})
            check(reply and reply.get('returnValue'), 'round %d: unpair on %s failed: %r' % (round, x, reply))
        settle(1.5)
        check(not (device(a, b) or {}).get('paired'), 'round %d: still paired' % round)


def pairing_with_nobody_there():
    """A pairs with an address no controller has: the pair fails, and a real pairing works afterwards."""
    a, b = pair_a_with_b()
    nobody = '00:aa:01:02:00:02'
    out = client('adapter/pair', {'adapterAddress': a, 'address': nobody, 'subscribe': True})
    replies = out.wait(timeout=60, until=lambda r: 'returnValue' in r and (not r['returnValue'] or
                                                                         r.get('subscribed') is False))
    out.stop()
    check(any('returnValue' in r and (not r['returnValue'] or r.get('request') != 'endPairing')
              for r in replies[1:] or replies), 'pairing with nobody there did not fail: %r' % replies)
    discover(a, b)
    out, _ = pair(a, b)
    settle(1.5)
    check((device(a, b) or {}).get('paired'), 'pairing after the failed one did not pair: %r' % out)


def bluetoothd_killed_and_back():
    """bluetoothd killed (SIGKILL) twice with devices known: the service keeps running and gets both adapters back."""
    a, b = pair_a_with_b()
    discover(a, b)
    for _ in range(2):
        subprocess.run(['systemctl', 'kill', '-s', 'KILL', 'bluetooth'], check=False)
        settle(2.0)
        subprocess.run(['systemctl', 'start', 'bluetooth'], check=False)
        wait_adapters(timeout=30)
        settle(2.0)
    a, b = pair_a_with_b()
    discover(a, b)


def controllers_vanish_mid_pairing():
    """The controllers disappear (btvirt killed) at a random point of a pairing, six times: the service keeps
    running and gets both adapters back. Before the adapter and profile replies checked their object, this was a
    heap-use-after-free in most runs (Bluez5Adapter::getAdapterProperties after Bluez5SIL::removeAdapter)."""
    import random
    for round in range(6):
        a, b = pair_a_with_b()
        discover(a, b)
        pairing = client('adapter/pair', {'adapterAddress': a, 'address': b, 'subscribe': True})
        if round % 2:
            settle(1.5)
            call('adapter/unpair', {'adapterAddress': b, 'address': a})
        time.sleep(random.uniform(0, 1.0))
        subprocess.run(['systemctl', 'kill', '-s', 'KILL', 'btvirt'], check=False)
        settle(3.0)
        pairing.stop()
        ensure_btvirt()
        subprocess.run(['rfkill', 'unblock', 'bluetooth'], check=False)
        wait_adapters(timeout=30)
        settle(2.0)


def obexctl_push(destination, path, timeout=60.0):
    """Pushes a file with obexctl (obexd is on the system bus here) and returns its output lines."""
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS='unix:path=/var/run/dbus/system_bus_socket')
    ctl = Pty(['obexctl'], env=env)
    try:
        check(ctl.wait_line(r'Client /org/bluez/obex', 10) or True, '')
        # The service record lookup fails now and then: retried.
        for attempt in range(4):
            seen = len(ctl.lines)
            ctl.write('connect %s 00001105-0000-1000-8000-00805f9b34fb\n' % destination)
            end = time.time() + 20
            result = None
            while time.time() < end and result is None:
                for line in ctl.lines[seen:]:
                    if 'Connection successful' in line or 'Failed to connect' in line:
                        result = line
                settle(0.2)
            check(result, 'obexctl connect gave no answer')
            if 'Connection successful' in result:
                break
            settle(3.0)
        else:
            return ctl.lines
        # "Connection successful" comes before the session's ObjectPush interface; send is refused until then.
        found = ctl.wait_line(r'\[NEW\] ObjectPush /org/bluez/obex/client/session\d+', 10)
        check(found, 'no ObjectPush interface on the session')
        # Both ends are in this machine: the receiving side's server session shows up too and becomes obexctl's
        # default, which cannot send. Select the client session.
        session = re.search(r'/org/bluez/obex/client/session\d+', found).group(0)
        settle(1.0)
        ctl.write('select %s\n' % session)
        settle(0.5)
        ctl.write('send %s\n' % path)
        ctl.wait_line(r'Status: (complete|error)|Transfer .* (complete|error)|Failed', timeout)
        settle(1.0)
        return list(ctl.lines)
    finally:
        ctl.stop()


def opp_push(answer, a, b, name, delay=0.0, wait=60.0):
    """B pushes a file to A, which answers ('accept', 'reject' or None). Returns (A's request replies, answer
    reply, obexctl lines, whether the file landed)."""
    path = '/tmp/' + name
    with open(path, 'wb') as f:
        f.write(os.urandom(64 * 1024))
    target = os.path.join(OPP_ROOT, name)
    if os.path.exists(target):
        os.remove(target)
    waiting = client('opp/awaitTransferRequest', {'adapterAddress': a, 'subscribe': True})
    check(waiting.wait(1), 'awaitTransferRequest did not answer')
    result = {}

    def answer_it():
        requests = waiting.wait(timeout=30, until=lambda r: 'request' in r)
        request = next((r['request'] for r in requests if 'request' in r), None)
        result['request'] = request
        if request and answer:
            settle(delay)
            method = 'opp/acceptTransferRequest' if answer == 'accept' else 'opp/rejectTransferRequest'
            result['answer'] = call(method, {'adapterAddress': a, 'requestId': request['requestId']})

    answerer = threading.Thread(target=answer_it)
    answerer.start()
    lines = obexctl_push(a, path, wait)
    answerer.join(max(delay, 0) + 35)
    waiting.stop()
    landed = os.path.exists(target)
    if landed:
        os.remove(target)
    os.remove(path)
    return result.get('request'), result.get('answer'), lines, landed


def opp_accept_reject_and_unanswered():
    """B pushes to A: accepted lands; rejected and unanswered (after the SIL's 50 s) do not, and a late accept fails."""
    a, b = pair_a_with_b()
    set_state(a, discoverable=True, discoverableTimeout=0)
    discover(b, a)
    request, reply, lines, landed = opp_push('accept', a, b, 'sil-opp-accept.bin')
    check(request, 'no transfer request reached the service: %r' % lines[-5:])
    check(reply and reply.get('returnValue'), 'accept failed: %r' % reply)
    check(landed, 'accepted file did not land: %r' % lines[-5:])
    request, reply, lines, landed = opp_push('reject', a, b, 'sil-opp-reject.bin')
    check(request and reply and reply.get('returnValue'), 'reject failed: %r %r' % (request, reply))
    check(not landed, 'rejected file landed')
    request, reply, lines, landed = opp_push('accept', a, b, 'sil-opp-late.bin', delay=55.0, wait=80.0)
    check(request, 'no transfer request for the unanswered push')
    check(not landed, 'unanswered file landed')
    check(reply and not reply.get('returnValue'), 'a late accept succeeded: %r' % reply)


def push_accepted(a, path, timeout=60.0):
    """B pushes path to A, which accepts. Returns (request, answer reply)."""
    waiting = client('opp/awaitTransferRequest', {'adapterAddress': a, 'subscribe': True})
    check(waiting.wait(1), 'awaitTransferRequest did not answer')
    result = {}

    def answer_it():
        requests = waiting.wait(timeout=30, until=lambda r: 'request' in r)
        request = next((r['request'] for r in requests if 'request' in r), None)
        result['request'] = request
        if request:
            result['answer'] = call('opp/acceptTransferRequest', {'adapterAddress': a, 'requestId': request['requestId']})

    answerer = threading.Thread(target=answer_it)
    answerer.start()
    obexctl_push(a, path, timeout)
    answerer.join(35)
    waiting.stop()
    return result.get('request'), result.get('answer')


def opp_same_name_kept_and_dotfile_refused():
    """A second push of the same name lands under a free name, leaving the first; a dot-file is refused."""
    a, b = pair_a_with_b()
    set_state(a, discoverable=True, discoverableTimeout=0)
    discover(b, a)
    name = 'sil-opp-dup.bin'
    first, second = os.path.join(OPP_ROOT, name), os.path.join(OPP_ROOT, 'sil-opp-dup (1).bin')
    for p in (first, second):
        if os.path.exists(p):
            os.remove(p)
    contents = []
    try:
        for _ in range(2):
            data = os.urandom(32 * 1024)
            contents.append(data)
            with open('/tmp/' + name, 'wb') as f:
                f.write(data)
            request, reply = push_accepted(a, '/tmp/' + name)
            check(request and reply and reply.get('returnValue'), 'push not accepted: %r %r' % (request, reply))
            settle(2.0)
        check(os.path.exists(first) and open(first, 'rb').read() == contents[0], 'the first file was replaced')
        check(os.path.exists(second) and open(second, 'rb').read() == contents[1],
              'the second push did not land under a free name')
    finally:
        for p in (first, second, '/tmp/' + name):
            if os.path.exists(p):
                os.remove(p)

    dot = '.sil-opp-hidden'
    with open('/tmp/' + dot, 'wb') as f:
        f.write(os.urandom(1024))
    try:
        waiting = client('opp/awaitTransferRequest', {'adapterAddress': a, 'subscribe': True})
        check(waiting.wait(1), 'awaitTransferRequest did not answer')
        lines = obexctl_push(a, '/tmp/' + dot, 30)
        requests = [r for r in waiting.wait(timeout=3, until=lambda r: 'request' in r) if 'request' in r]
        waiting.stop()
        check(not os.path.exists(os.path.join(OPP_ROOT, dot)), 'a dot-file landed: %r' % lines[-3:])
        check(not requests, 'a dot-file was offered to the user: %r' % requests)
    finally:
        os.remove('/tmp/' + dot)
        if os.path.exists(os.path.join(OPP_ROOT, dot)):
            os.remove(os.path.join(OPP_ROOT, dot))


def spp_server_data_both_ways():
    """Paired A and B: B serves an SPP channel through the SIL, A connects with rfcomm(1) (one bluetoothd takes a
    UUID only once, so A cannot be an SPP client of the same UUID through the SIL as well), and data goes both
    ways; A closing is seen as a disconnect."""
    import base64
    a, b = pair_a_with_b()
    discover(a, b)
    out, _ = pair(a, b)
    settle(1.5)
    check((device(a, b) or {}).get('paired'), 'pairing failed: %r' % out)
    uuid = '7b6c8a2e-1d8e-4c1a-9a49-5b2f6c1d0e01'
    server = client('spp/createChannel', {'name': 'sil-test', 'uuid': uuid, 'adapterAddress': b, 'subscribe': True})
    check(server.wait(1) and server.all()[0].get('returnValue'), 'createChannel failed: %r' % server.all())
    settle(1.0)
    # rfcomm(1) makes the RFCOMM link a tty (Python here has no Bluetooth sockets). The SIL's RFCOMM channel is
    # not reported: read from B's SDP record for the UUID.
    sdp = subprocess.run(['sdptool', 'browse', b], capture_output=True, text=True).stdout
    record = next((r for r in re.split(r'\n\s*\n', sdp) if uuid in r.lower()), '')
    found = re.search(r'Channel: (\d+)', record)
    check(found, 'no SDP record with an RFCOMM channel for the UUID: %r' % record[-300:])
    link = None
    for channel in [int(found.group(1))]:
        subprocess.run(['rfcomm', 'release', '0'], capture_output=True)
        link = subprocess.Popen(['rfcomm', '-i', a, 'connect', '/dev/rfcomm0', b, str(channel)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        end = time.time() + 8
        while time.time() < end and link.poll() is None and not os.path.exists('/dev/rfcomm0'):
            settle(0.2)
        settle(1.0)
        if link.poll() is None and os.path.exists('/dev/rfcomm0'):
            break
        link.kill()
        link.wait()
        link = None
    check(link, 'no RFCOMM channel of B accepted a connection')
    tty = os.open('/dev/rfcomm0', os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        subprocess.run(['stty', '-F', '/dev/rfcomm0', 'raw', '-echo'], capture_output=True)
        served = server.wait(timeout=10, until=lambda r: r.get('connected') is True)
        up = next((r for r in served if r.get('connected') is True), None)
        check(up and up.get('channelId'), 'the SIL reported no connection: %r' % served)
        channel_b = up['channelId']

        reader = client('spp/readData', {'channelId': channel_b, 'adapterAddress': b, 'subscribe': True})
        settle(1.0)	# no first reply: it answers with data
        payload = b'hello from A ' + os.urandom(16).hex().encode()
        os.write(tty, payload)
        got = b''
        end = time.time() + 10
        while time.time() < end and len(got) < len(payload):
            got = b''.join(base64.b64decode(r['data']) for r in reader.all() if r.get('data'))
            settle(0.2)
        reader.stop()
        check(got == payload, 'B received %r' % got[:40])

        payload = b'hello from B ' + os.urandom(16).hex().encode()
        reply = call('spp/writeData', {'channelId': channel_b, 'adapterAddress': b,
                                       'data': base64.b64encode(payload).decode()})
        check(reply and reply.get('returnValue'), 'writeData failed: %r' % reply)
        got = b''
        end = time.time() + 10
        while time.time() < end and len(got) < len(payload):
            try:
                got += os.read(tty, 4096)
            except BlockingIOError:
                settle(0.1)
        check(got == payload, 'A received %r' % got[:40])
    finally:
        os.close(tty)
        link.kill()
        link.wait()
        subprocess.run(['rfcomm', 'release', '0'], capture_output=True)
    down = server.wait(timeout=10, until=lambda r: r.get('connected') is False)
    server.stop()
    check(any(r.get('connected') is False for r in down), 'the SIL saw no disconnect: %r' % down)


def service_restarts():
    """The service stopped and started five times on the real SIL; each time both adapters come back."""
    for _ in range(5):
        subprocess.run(['systemctl', 'restart', SERVICE], check=False)
        wait_adapters()


SCENARIOS = [
    power_cycles,
    discovery_finds_the_other_adapter,
    pairing_just_works,
    pairing_again_after_unpair,
    pairing_with_nobody_there,
    bluetoothd_killed_and_back,
    controllers_vanish_mid_pairing,
    opp_accept_reject_and_unanswered,
    opp_same_name_kept_and_dotfile_refused,
    spp_server_data_both_ways,
    service_restarts,
]


# btvirt's controllers pair with NoInputNoOutput whatever the agent says (bluetoothctl's own DisplayYesNo agent
# too), so numeric comparison, and rejecting or cancelling it, cannot be driven here. Every scenario runs with the
# service's default capability.
CAPABILITIES = {}


def read_lines(path):
    try:
        with open(path, errors='replace') as f:
            return f.readlines()
    except OSError:
        return []


def sanitizer_findings():
    """Every sanitizer error, and every leak with one of its frames (above the allocator) in the service or the
    SIL. luna-service2's own transport data, with only glib and luna-service2 frames, is left out."""
    findings = []
    if not os.path.isdir(SANITIZER_LOGS):
        return findings
    for name in sorted(os.listdir(SANITIZER_LOGS)):
        record = None
        for line in read_lines(os.path.join(SANITIZER_LOGS, name)):
            if 'runtime error' in line or 'ERROR: AddressSanitizer' in line:
                findings.append('%s: %s' % (name, line.strip()))
            if 'leak of' in line:
                record = [line.strip(), False]
                continue
            if record is not None:
                frame = line.strip()
                if not frame:
                    if record[1]:
                        findings.append('%s: %s' % (name, record[0]))
                    record = None
                elif frame.startswith('#') and not frame.startswith('#0 ') and any(m in frame for m in OUR_MODULES):
                    record[1] = True
    return findings


def main(argv):
    wanted = set(argv[1:])
    failures = 0
    ran = 0
    if os.path.isdir(SANITIZER_LOGS):
        for name in os.listdir(SANITIZER_LOGS):
            os.remove(os.path.join(SANITIZER_LOGS, name))
    for scenario in SCENARIOS:
        if wanted and scenario.__name__ not in wanted:
            continue
        ran += 1
        restart_bluetooth_stack(CAPABILITIES.get(scenario.__name__))
        pid = main_pid()
        try:
            scenario()
            if scenario is not service_restarts:
                check(main_pid() == pid, 'the service restarted (pid %s -> %s)' % (pid, main_pid()))
            print('PASS  %s' % scenario.__name__)
        except (Failure, RuntimeError) as e:
            failures += 1
            print('FAIL  %s: %s' % (scenario.__name__, e))
        sys.stdout.flush()
    subprocess.run(['systemctl', 'stop', SERVICE], check=False)
    set_capability(None)
    settle(2.0)
    print('%d of %d scenarios passed' % (ran - failures, ran))
    findings = sanitizer_findings()
    for finding in findings:
        print('SANITIZER  %s' % finding)
    return 1 if failures or findings else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
