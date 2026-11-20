#!/usr/bin/env python3
"""coop's SysEx channel from a command line (docs/sysex.md, docs/test-loop.md tier 1): what tools/coop_sysex.html
and the librarian do in the browser, for an agent or a script.

    python  tools/coop_usb.py ports                         # Windows: the MIDI ports (enumeration only)
    python  tools/coop_usb.py hello                         # the OP-1 on USB (Windows, winmm)
    python  tools/coop_usb.py inventory
    python  tools/coop_usb.py stats -o stats.txt            # /yaffs2/user/coop_stats.txt, parsed to JSON on stdout
    python  tools/coop_usb.py get /yaffs2/user/coop_run.txt -o run.txt
    python3 tools/coop_usb.py --transport usbip hello       # WSL: a running op1emu (its USB/IP server, port 3240)
    python3 tools/coop_usb.py --transport mock inventory    # the built-in fake device (tests)
    python  tools/coop_usb.py --write put build/plugins/saw.coop      # writes need --write

Reads (HELLO, INVENTORY, FILE_GET, SLOT2 get, RUN status) are the default. Everything that changes the device (PUT,
DELETE, PLUGIN_SET, RUN start / abort) is refused without --write. Every command prints JSON; exit 0 ok, 1 the
device refused or did not answer, 2 usage.

Transports:
  winmm   Windows' own MIDI API through ctypes: no package to install. The port whose name matches /op-?1/i and not
          /field/i, as the librarian picks it. [I: written from the API, not yet run against the OP-1]
  usbip   op1emu serves the emulated OP-1's USB port over USB/IP on TCP 3240; this speaks that protocol itself
          (import, the descriptors, bulk transfers on the MIDI streaming endpoints), so no kernel module or ALSA.
  mock    MockCoop below: HELLO, INVENTORY, FILE_GET, PUT, DELETE, PLUGIN_SET, RUN status, after the firmware's
          contract (docs/handover-coop-platform.md 1.1-1.3) and web/test/fake-coop.ts.
Stdlib only, Python 3.8.
"""
import argparse
import json
import os
import re
import socket
import struct
import sys
import time
import zlib

HEADER = bytes([0xF0, 0x00, 0x20, 0x76, 0x7D])
MAX_MESSAGE = 96                        # the OP-1's USB-MIDI input stops on longer SysEx
CHUNK = 64
FILE_GET, HELLO, PUT_BEGIN, PUT_DATA, PUT_END, DELETE, SLOT2, RUN, INVENTORY, PLUGIN_SET = (
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29)
READ_ONLY = {HELLO, FILE_GET, INVENTORY}                 # SLOT2 and RUN: by their op byte (0 get / status)
STATUS = {0: 'ok', 1: 'format', 2: 'state', 3: 'path', 4: 'size', 5: 'offset', 6: 'crc', 7: 'io'}
CAPS = ['file_read', 'file_write', 'slot2', 'run', 'inventory', 'plugin_set']
RUN_STATES = ['idle', 'armed', 'running', 'done', 'stopped', 'error']
PATH_OK = re.compile(r'^/yaffs2/(plugins|user)/[^/]+$')


class CoopError(Exception):
    pass


# ---- framing (web/src/core/usbmidi/framing.ts) ----

def pack7(raw):
    out = bytearray()
    for s in range(0, len(raw), 7):
        grp = raw[s:s + 7]
        high = 0
        for i, b in enumerate(grp):
            high |= (b >> 7) << i
        out.append(high)
        out += bytes(b & 0x7F for b in grp)
    return bytes(out)


def unpack7(packed):
    out = bytearray()
    i = 0
    while i < len(packed):
        high = packed[i]
        i += 1
        for k in range(7):
            if i >= len(packed):
                break
            out.append(packed[i] | (((high >> k) & 1) << 7))
            i += 1
    return bytes(out)


def build_message(cmd, seq, payload=b''):
    msg = HEADER + bytes([cmd & 0x7F, seq & 0x7F, (seq >> 7) & 0x7F]) + pack7(payload) + b'\xF7'
    if len(msg) > MAX_MESSAGE:
        raise CoopError('a %d-byte message is longer than the OP-1 takes (%d)' % (len(msg), MAX_MESSAGE))
    return msg


def parse_reply(msg):
    """(cmd, seq, status, payload) of a reply, or None when the bytes are not one of ours."""
    if len(msg) < len(HEADER) + 4 or msg[-1] != 0xF7 or msg[:5] != HEADER or not msg[5] & 0x40:
        return None
    body = unpack7(msg[8:-1])
    if not body:
        return None
    return msg[5] & 0x3F, msg[6] | (msg[7] << 7), body[0], body[1:]


class Assembler:
    """SysEx messages out of the byte runs a MIDI input hands over; realtime bytes inside a message are dropped."""

    def __init__(self):
        self.buf = None

    def feed(self, data):
        out = []
        for b in data:
            if b == 0xF0:
                self.buf = bytearray([b])
            elif self.buf is None or b >= 0xF8:
                continue
            elif b == 0xF7:
                self.buf.append(b)
                out.append(bytes(self.buf))
                self.buf = None
            elif b & 0x80:
                self.buf = None                              # a status byte ends a message without its F7
            else:
                self.buf.append(b)
        return out


def midi_to_usb(msg, cable=0):
    """A SysEx message as USB-MIDI event packets (4 bytes each; CIN 4 continue, 5 / 6 / 7 end with 1 / 2 / 3)."""
    out = bytearray()
    i = 0
    while len(msg) - i > 3:
        out += bytes([cable << 4 | 0x4]) + msg[i:i + 3]
        i += 3
    rest = msg[i:]
    out += bytes([cable << 4 | (0x4 + len(rest))]) + rest + b'\0' * (3 - len(rest))
    return bytes(out)


def usb_to_midi(data):
    out = bytearray()
    for i in range(0, len(data) - 3, 4):
        cin = data[i] & 0x0F
        n = {0x4: 3, 0x5: 1, 0x6: 2, 0x7: 3, 0x2: 2, 0x3: 3, 0xF: 1}.get(cin, 3 if cin >= 0x8 else 0)
        out += data[i + 1:i + 1 + n]
    return bytes(out)


# ---- the session and the commands ----

class Session:
    """One message in flight; a reply is matched by command and seq, a lost one is asked for again."""

    def __init__(self, transport, allow_write=False, timeout=3.0, tries=5):
        self.t, self.allow_write, self.timeout, self.tries = transport, allow_write, timeout, tries
        self.seq = 0
        self.asm = Assembler()
        self.sent = 0

    def ask(self, cmd, payload=b''):
        writes = not (cmd in READ_ONLY or (cmd in (SLOT2, RUN) and payload[:1] == b'\0'))
        if writes and not self.allow_write:
            raise CoopError('command 0x%02x changes the device: pass --write' % cmd)
        self.seq = (self.seq + 1) & 0x3FFF
        msg = build_message(cmd, self.seq, payload)
        for _ in range(self.tries):
            self.t.send(msg)
            self.sent += 1
            end = time.time() + self.timeout
            while time.time() < end:
                for m in self.asm.feed(self.t.recv(min(0.25, max(0.0, end - time.time())))):
                    r = parse_reply(m)
                    if r and r[0] == cmd and r[1] == self.seq:
                        return r[2], r[3]
        raise CoopError('no answer to command 0x%02x after %d tries' % (cmd, self.tries))

    def ok(self, cmd, payload, what):
        st, data = self.ask(cmd, payload)
        if st != 0:
            raise CoopError('the OP-1 refused %s: status %d (%s)' % (what, st, STATUS.get(st, '?')))
        return data


def hello(s):
    st, p = s.ask(HELLO)
    if st != 0 or len(p) < 7 or p[:4] != b'coop':
        raise CoopError('not a coop firmware (status %d, %r)' % (st, p[:8]))
    caps = p[6]
    return {'firmware': 'coop', 'protocol': p[4] | p[5] << 8, 'caps': caps,
            'caps_names': [n for i, n in enumerate(CAPS) if caps >> i & 1], 'build': p[7:].decode('latin-1')}


def paged(s, cmd, prefix, what):
    out, total = bytearray(), None
    while total is None or len(out) < total:
        p = s.ok(cmd, struct.pack('<I', len(out)) + prefix, what)
        if len(p) < 4:
            raise CoopError('%s: a short reply' % what)
        total = struct.unpack_from('<I', p)[0]
        if len(p) == 4 and len(out) < total:
            raise CoopError('%s: no data at offset %d of %d' % (what, len(out), total))
        out += p[4:]
    return bytes(out[:total])


def file_get(s, path):
    return paged(s, FILE_GET, path.encode(), 'reading %s' % path)


def inventory(s):
    text = paged(s, INVENTORY, b'', 'the inventory').decode('utf-8', 'replace')
    lines = text.split('\n')
    head = lines[0].split('\t')
    rows = []
    for line in lines[1:]:
        f = line.split('\t')
        if len(f) >= 8:
            f += [''] * (9 - len(f))
            row = {'file': f[0], 'kind': f[1], 'name': f[2], 'boot': f[3], 'next': f[4], 'version': f[5],
                   'author': f[6], 'bytes': int(f[7]) if f[7].isdigit() else None, 'reason': f[8]}
            if len(f) >= 11 and f[9].isdigit() and f[10].isdigit():      # e12: the file's own memory, its position buffer
                row['need'], row['scratch'] = int(f[9]), int(f[10])
            rows.append(row)
    out = {'header': head, 'build': next((h[6:] for h in head if h.startswith('build ')), None), 'plugins': rows}
    memory = {}                                                          # e12: budget, used, fxbuf, fxbufs, clip
    for h in head[3:]:
        k, _, v = h.partition(' ')
        if v.isdigit():
            memory[k] = int(v)
    if memory:
        out['memory'] = memory
    return out


def run_status(s):
    st, p = s.ask(RUN, b'\0')
    if st != 0 or len(p) < 12:
        raise CoopError('RUN status: status %d, %d bytes' % (st, len(p)))
    state, block, ev, evs, secs, err, written = struct.unpack_from('<BIHHBBB', p)
    return {'state': RUN_STATES[state] if state < len(RUN_STATES) else state, 'block': block, 'event': ev,
            'events': evs, 'sections': secs, 'error': err, 'run_file_written': bool(written)}


def put(s, path, data):
    if not PATH_OK.match(path) or '..' in path:
        raise CoopError('the firmware only takes /yaffs2/plugins/NAME and /yaffs2/user/NAME: %s' % path)
    s.ok(PUT_BEGIN, path.encode(), 'PUT_BEGIN %s' % path)
    off = 0
    while off < len(data):
        st, p = s.ask(PUT_DATA, struct.pack('<I', off) + data[off:off + CHUNK])
        if st == 5 and len(p) >= 4:                          # a resent chunk the OP-1 had already written
            off = struct.unpack_from('<I', p)[0]
            continue
        if st != 0:
            raise CoopError('PUT_DATA at %d: status %d (%s)' % (off, st, STATUS.get(st, '?')))
        off = struct.unpack_from('<I', p)[0]
    s.ok(PUT_END, struct.pack('<II', len(data), zlib.crc32(data) & 0xFFFFFFFF), 'PUT_END')
    return {'path': path, 'bytes': len(data), 'crc32': '%08x' % (zlib.crc32(data) & 0xFFFFFFFF)}


# ---- transports ----

class MockCoop:
    """The device side, in process. `boot` maps a plugin file name to (kind, name, state, version, author, reason)."""

    def __init__(self, build='e10', caps=0x3F, files=None, boot=None, disabled=(), split=5, lose_first=0):
        self.build, self.caps = build, caps
        self.files = dict(files or {})                       # full path -> bytes
        self.boot = dict(boot or {})
        self.disabled = set(disabled)
        self.split, self.lose = split, lose_first
        self.out = bytearray()
        self.requests = []
        self.put = None
        self.inv = b''

    def send(self, msg):
        if len(msg) > MAX_MESSAGE or msg[:5] != HEADER or msg[-1] != 0xF7:
            raise CoopError('mock: not a coop message of at most %d bytes' % MAX_MESSAGE)
        cmd, seq, p = msg[5], msg[6] | msg[7] << 7, unpack7(msg[8:-1])
        self.requests.append((cmd, p))
        body = bytes(self.answer(cmd, p))
        if len(body) > 73:
            raise CoopError('mock: a %d-byte reply' % len(body))
        if self.lose:
            self.lose -= 1
            return
        self.out += b'\xFE' + HEADER + bytes([cmd | 0x40, seq & 0x7F, seq >> 7]) + pack7(body) + b'\xF7'

    def recv(self, timeout):
        n = min(self.split, len(self.out))
        d, self.out = bytes(self.out[:n]), self.out[n:]
        return d

    def close(self):
        pass

    def inventory_text(self):
        rows = []
        for path in sorted(self.files):
            if not path.startswith('/yaffs2/plugins/'):
                continue
            f = path.rsplit('/', 1)[1]
            b = self.boot.get(f, ('?', '', 'new', '', '', ''))
            rows.append('\t'.join([f, b[0], b[1], b[2], 'off' if f in self.disabled else 'on', b[3], b[4],
                                   str(len(self.files[path])), b[5]]) + '\n')
        return ('coop-inventory 1\tbuild %s\tprotocol 1\n%s' % (self.build, ''.join(rows))).encode()

    def answer(self, cmd, p):
        le = lambda v: struct.pack('<I', v)                  # noqa: E731
        if cmd == HELLO:
            return b'\0coop' + bytes([1, 0, self.caps]) + self.build.encode()
        if cmd == FILE_GET:
            if len(p) < 5:
                return b'\x01'
            data = self.files.get(p[4:].decode('latin-1'))
            if data is None:
                return b'\x03'
            off = struct.unpack_from('<I', p)[0]
            return b'\0' + le(len(data)) + data[off:off + CHUNK]
        if cmd == INVENTORY:
            if len(p) < 4:
                return b'\x01'
            off = struct.unpack_from('<I', p)[0]
            if off == 0:
                self.inv = self.inventory_text()
            return b'\0' + le(len(self.inv)) + self.inv[off:off + CHUNK]
        if cmd in (PUT_BEGIN, DELETE):
            path = p.decode('latin-1')
            if not PATH_OK.match(path) or '..' in path:
                return b'\x03'
            if cmd == DELETE:
                return b'\0' if self.files.pop(path, None) is not None else b'\x03'
            self.put = [path, bytearray()]
            return b'\0'
        if cmd == PUT_DATA:
            if self.put is None:
                return b'\x02'
            if len(p) < 4 or struct.unpack_from('<I', p)[0] != len(self.put[1]):
                return b'\x05' + le(len(self.put[1]))
            self.put[1] += p[4:]
            return b'\0' + le(len(self.put[1]))
        if cmd == PUT_END:
            if self.put is None:
                return b'\x02'
            path, data = self.put
            self.put = None
            total, crc = struct.unpack_from('<II', p) if len(p) >= 8 else (-1, 0)
            if total != len(data):
                return b'\x04'
            if crc != zlib.crc32(bytes(data)) & 0xFFFFFFFF:
                return b'\x06'
            self.files[path] = bytes(data)
            return b'\0'
        if cmd == PLUGIN_SET:
            name = p[1:].decode('latin-1')
            if len(p) < 2 or '/' in name:
                return b'\x03'
            (self.disabled.discard if p[0] else self.disabled.add)(name)
            return b'\0' + p[:1]
        if cmd == RUN and p[:1] == b'\0':
            return b'\0' + struct.pack('<BIHHBBB', 0, 0, 0, 0, 0, 0, 0)
        return b'\x01'


class UsbipTransport:
    """op1emu's USB/IP server (its README, "USB"): attach as its one client, read the descriptors the firmware
    gives, set the configuration, then bulk transfers on the MIDI streaming interface's endpoints."""

    def __init__(self, host='127.0.0.1', port=3240, busid='1-1', wait=0.0):
        self.seqnum = 0
        self.pending_in = None
        self.dirs = {}                                       # seqnum -> direction of the URBs in flight
        self.stash = b''
        end = time.time() + wait
        while True:
            try:
                self._import(host, port, busid)
                break
            except (OSError, CoopError) as e:
                if time.time() >= end:
                    raise CoopError('usbip %s:%d: %s' % (host, port, e))
                time.sleep(2)
        dev = self.control(0x80, 6, 0x0100, 0, 18)
        head = self.control(0x80, 6, 0x0200, 0, 9)
        cfg = self.control(0x80, 6, 0x0200, 0, head[2] | head[3] << 8)
        self.device = {'vid': '%04x' % (dev[8] | dev[9] << 8), 'pid': '%04x' % (dev[10] | dev[11] << 8)}
        self.interfaces, self.ep_out, self.ep_in = self.parse_config(cfg)
        if self.ep_out is None or self.ep_in is None:
            raise CoopError('usbip: no MIDI streaming endpoints in the configuration (%s)' % self.interfaces)
        self.control(0x00, 9, cfg[5], 0, 0)                  # SET_CONFIGURATION

    @staticmethod
    def parse_config(cfg):
        """([interface dicts], MIDI bulk OUT endpoint, MIDI bulk IN endpoint) from a configuration descriptor."""
        ifs, cur, ep_out, ep_in = [], None, None, None
        i = 0
        while i + 2 <= len(cfg) and cfg[i]:
            n, t = cfg[i], cfg[i + 1]
            if t == 4 and n >= 9:
                cur = {'number': cfg[i + 2], 'alt': cfg[i + 3], 'class': cfg[i + 5], 'subclass': cfg[i + 6], 'endpoints': []}
                ifs.append(cur)
            elif t == 5 and n >= 7 and cur is not None:
                addr, attr = cfg[i + 2], cfg[i + 3]
                cur['endpoints'].append({'address': '%02x' % addr, 'type': ['control', 'iso', 'bulk', 'interrupt'][attr & 3],
                                         'max_packet': cfg[i + 4] | cfg[i + 5] << 8})
                if cur['class'] == 1 and cur['subclass'] == 3 and attr & 3 in (2, 3):
                    if addr & 0x80:
                        ep_in = addr & 0x0F
                    else:
                        ep_out = addr & 0x0F
            i += n
        return ifs, ep_out, ep_in

    def _import(self, host, port, busid):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.sendall(struct.pack('>HHI', 0x0111, 0x8003, 0) + busid.encode().ljust(32, b'\0'))
        ver, cmd, status = struct.unpack('>HHI', self._read(8))
        if status != 0:
            self.sock.close()
            raise CoopError('attach refused (status %d): the firmware is not on the bus yet, or a client is attached' % status)
        udev = self._read(312)
        busnum, devnum = struct.unpack_from('>II', udev, 288)
        self.devid = busnum << 16 | devnum

    def _read(self, n, timeout=5.0):
        self.sock.settimeout(timeout)
        buf = b''
        while len(buf) < n:
            d = self.sock.recv(n - len(buf))
            if not d:
                raise CoopError('usbip: the emulator closed the connection')
            buf += d
        return buf

    def _submit(self, direction, ep, length, setup=b'\0' * 8, data=b''):
        self.seqnum += 1
        self.dirs[self.seqnum] = direction
        self.sock.sendall(struct.pack('>IIIII', 1, self.seqnum, self.devid, direction, ep)
                          + struct.pack('>IIIII', 0, length, 0, 0, 0) + setup + data)
        return self.seqnum

    def _ret(self, timeout):
        """One RET_SUBMIT: (seqnum, status, data)."""
        h = self._read(48, timeout)
        cmd, seq, _, direction, _, status, actual = struct.unpack_from('>IIIIIiI', h)
        data = b''
        if cmd == 3 and self.dirs.pop(seq, 0) == 1 and actual:
            data = self._read(actual)
        return seq, status, data

    def control(self, bm, req, value, index, length, data=b''):
        setup = struct.pack('<BBHHH', bm, req, value, index, length)
        seq = self._submit(1 if bm & 0x80 else 0, 0, length, setup, data)
        while True:
            s, status, d = self._ret(5.0)
            if s == seq:
                if status != 0:
                    raise CoopError('usbip: control request %02x/%02x failed (%d)' % (bm, req, status))
                return d

    def send(self, msg):
        pk = midi_to_usb(msg)
        seq = self._submit(0, self.ep_out, len(pk), data=pk)
        end = time.time() + 5
        while time.time() < end:
            try:
                s, status, d = self._ret(0.5)
            except socket.timeout:
                continue
            if s == self.pending_in:
                self.pending_in = None
                self.stash += usb_to_midi(d)
            elif s == seq:
                if status != 0:
                    raise CoopError('usbip: the bulk OUT failed (%d)' % status)
                return
        raise CoopError('usbip: the bulk OUT was not acknowledged')

    def recv(self, timeout):
        stash, self.stash = self.stash, b''
        if stash:
            return stash
        if self.pending_in is None:
            self.pending_in = self._submit(1, self.ep_in, 64)
        try:
            s, status, d = self._ret(max(timeout, 0.02))
        except socket.timeout:
            return b''
        if s == self.pending_in:
            self.pending_in = None
            if not d:
                time.sleep(0.01)                             # an empty answer: do not spin
            return usb_to_midi(d)
        return b''

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def winmm_ports():
    """[(direction, index, name)] from winmm; enumeration only, nothing is opened."""
    import ctypes
    from ctypes import wintypes
    mm = ctypes.WinDLL('winmm')

    class OutCaps(ctypes.Structure):
        _fields_ = [('wMid', wintypes.WORD), ('wPid', wintypes.WORD), ('vDriverVersion', wintypes.UINT),
                    ('szPname', wintypes.WCHAR * 32), ('wTechnology', wintypes.WORD), ('wVoices', wintypes.WORD),
                    ('wNotes', wintypes.WORD), ('wChannelMask', wintypes.WORD), ('dwSupport', wintypes.DWORD)]

    class InCaps(ctypes.Structure):
        _fields_ = [('wMid', wintypes.WORD), ('wPid', wintypes.WORD), ('vDriverVersion', wintypes.UINT),
                    ('szPname', wintypes.WCHAR * 32), ('dwSupport', wintypes.DWORD)]
    out = []
    for i in range(mm.midiInGetNumDevs()):
        c = InCaps()
        if mm.midiInGetDevCapsW(ctypes.c_size_t(i), ctypes.byref(c), ctypes.sizeof(c)) == 0:
            out.append(('in', i, c.szPname))
    for i in range(mm.midiOutGetNumDevs()):
        c = OutCaps()
        if mm.midiOutGetDevCapsW(ctypes.c_size_t(i), ctypes.byref(c), ctypes.sizeof(c)) == 0:
            out.append(('out', i, c.szPname))
    return out


def pick_op1(ports, pattern=None):
    rx = re.compile(pattern, re.I) if pattern else None
    def want(name):
        return bool(rx.search(name)) if rx else bool(re.search(r'op-?1', name, re.I) and not re.search('field', name, re.I))
    ins = [i for d, i, n in ports if d == 'in' and want(n)]
    outs = [i for d, i, n in ports if d == 'out' and want(n)]
    return (ins[0] if ins else None), (outs[0] if outs else None)


class WinMMTransport:
    """The OP-1's MIDI port through winmm. Input: SysEx buffers handed to the driver and polled for MHDR_DONE (no
    callback thread). Output: midiOutLongMsg. [I: not yet run against the device]"""
    NBUF, BUFLEN, MHDR_DONE = 8, 512, 1

    def __init__(self, pattern=None):
        import ctypes
        from ctypes import wintypes
        self.ct = ctypes
        ports = winmm_ports()
        i_in, i_out = pick_op1(ports, pattern)
        if i_in is None or i_out is None:
            raise CoopError('no OP-1 MIDI port (is it plugged in, switched on and out of disk mode?); ports: %s'
                            % ', '.join('%s %s' % (d, n) for d, _, n in ports))
        ptr = ctypes.c_size_t

        class Hdr(ctypes.Structure):
            pass
        Hdr._fields_ = [('lpData', ctypes.c_void_p), ('dwBufferLength', wintypes.DWORD), ('dwBytesRecorded', wintypes.DWORD),
                        ('dwUser', ptr), ('dwFlags', wintypes.DWORD), ('lpNext', ctypes.c_void_p), ('reserved', ptr),
                        ('dwOffset', wintypes.DWORD), ('dwReserved', ptr * 8)]
        self.Hdr = Hdr
        self.mm = mm = ctypes.WinDLL('winmm')
        for f in ('midiInOpen', 'midiOutOpen'):
            getattr(mm, f).argtypes = [ctypes.c_void_p, wintypes.UINT, ptr, ptr, wintypes.DWORD]
        for f in ('midiInPrepareHeader', 'midiInUnprepareHeader', 'midiInAddBuffer', 'midiOutPrepareHeader',
                  'midiOutUnprepareHeader', 'midiOutLongMsg'):
            getattr(mm, f).argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]
        for f in ('midiInStart', 'midiInReset', 'midiInClose', 'midiOutReset', 'midiOutClose'):
            getattr(mm, f).argtypes = [ctypes.c_void_p]
        self.hin, self.hout = ctypes.c_void_p(), ctypes.c_void_p()
        self._check(mm.midiOutOpen(ctypes.byref(self.hout), i_out, 0, 0, 0), 'midiOutOpen')
        self._check(mm.midiInOpen(ctypes.byref(self.hin), i_in, 0, 0, 0), 'midiInOpen')
        self.bufs = []
        for _ in range(self.NBUF):
            data = ctypes.create_string_buffer(self.BUFLEN)
            h = Hdr()
            h.lpData, h.dwBufferLength = ctypes.cast(data, ctypes.c_void_p), self.BUFLEN
            self._check(mm.midiInPrepareHeader(self.hin, ctypes.byref(h), ctypes.sizeof(h)), 'midiInPrepareHeader')
            self._check(mm.midiInAddBuffer(self.hin, ctypes.byref(h), ctypes.sizeof(h)), 'midiInAddBuffer')
            self.bufs.append((h, data))
        self._check(mm.midiInStart(self.hin), 'midiInStart')

    def _check(self, code, what):
        if code:
            raise CoopError('%s failed (MMSYSERR %d)%s' % (what, code, ': another program holds the port' if code == 4 else ''))

    def send(self, msg):
        ct = self.ct
        data = ct.create_string_buffer(bytes(msg), len(msg))
        h = self.Hdr()
        h.lpData, h.dwBufferLength, h.dwBytesRecorded = ct.cast(data, ct.c_void_p), len(msg), len(msg)
        self._check(self.mm.midiOutPrepareHeader(self.hout, ct.byref(h), ct.sizeof(h)), 'midiOutPrepareHeader')
        try:
            self._check(self.mm.midiOutLongMsg(self.hout, ct.byref(h), ct.sizeof(h)), 'midiOutLongMsg')
            end = time.time() + 2
            while not h.dwFlags & self.MHDR_DONE and time.time() < end:
                time.sleep(0.001)
        finally:
            self.mm.midiOutUnprepareHeader(self.hout, ct.byref(h), ct.sizeof(h))

    def recv(self, timeout):
        ct = self.ct
        end = time.time() + timeout
        while True:
            out = b''
            for h, data in self.bufs:
                if h.dwFlags & self.MHDR_DONE:
                    out += data.raw[:h.dwBytesRecorded]
                    h.dwBytesRecorded = 0
                    self._check(self.mm.midiInAddBuffer(self.hin, ct.byref(h), ct.sizeof(h)), 'midiInAddBuffer')
            if out or time.time() >= end:
                return out
            time.sleep(0.002)

    def close(self):
        ct = self.ct
        self.mm.midiInReset(self.hin)
        for h, _ in self.bufs:
            self.mm.midiInUnprepareHeader(self.hin, ct.byref(h), ct.sizeof(h))
        self.mm.midiInClose(self.hin)
        self.mm.midiOutReset(self.hout)
        self.mm.midiOutClose(self.hout)


def demo_mock():
    stats = (b'coop_stats 3 build e10\ncb blocks 1000 max 90000 last 999 cycles 40000000\n'
             b'cost avg/max/n stage=6730/7557/4045 fx1_plugin=3068/3570/900\n'
             b'plugins seen 2 loaded 1 failed 1 disabled 0\nplugfile big.coop failed needs 718480: over the plugin memory budget\n')
    return MockCoop(files={'/yaffs2/plugins/saw.coop': b'COOP' + bytes(2316), '/yaffs2/plugins/big.coop': b'COOP' + bytes(700),
                           '/yaffs2/user/coop_stats.txt': stats},
                    boot={'saw.coop': ('synth', 'saw', 'loaded', '0.2', 'coldsoldr', ''),
                          'big.coop': ('fx', 'big', 'failed', '0.1', 'coldsoldr', 'over the plugin memory budget')})


def open_transport(a):
    if a.transport == 'mock':
        return demo_mock()
    if a.transport == 'usbip':
        return UsbipTransport(a.host, a.usbip_port, wait=a.wait)
    if a.transport == 'winmm' or (a.transport == 'auto' and sys.platform == 'win32'):
        return WinMMTransport(a.port)
    raise CoopError('no transport for this platform: on Windows the OP-1 is reached with winmm, in WSL the emulator '
                    'with --transport usbip')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--transport', choices=['auto', 'winmm', 'usbip', 'mock'], default='auto')
    ap.add_argument('--port', help='winmm: a regex for the port name (default: OP-1, not field)')
    ap.add_argument('--host', default='127.0.0.1', help='usbip: where op1emu runs')
    ap.add_argument('--usbip-port', type=int, default=3240)
    ap.add_argument('--wait', type=float, default=0.0, help='usbip: seconds to keep trying until the firmware is on the bus')
    ap.add_argument('--write', action='store_true', help='allow the commands that change the device')
    ap.add_argument('--timeout', type=float, default=3.0)
    ap.add_argument('--tries', type=int, default=5)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('ports', help='list the MIDI ports (Windows)')
    sub.add_parser('hello')
    sub.add_parser('inventory')
    sub.add_parser('run-status')
    sub.add_parser('describe', help='usbip: the emulated device as its descriptors give it')
    p = sub.add_parser('get', help='read a file'); p.add_argument('path'); p.add_argument('-o', '--out')
    p = sub.add_parser('stats', help='read and parse coop_stats.txt'); p.add_argument('-o', '--out')
    p = sub.add_parser('put', help='write a file (--write)'); p.add_argument('file'); p.add_argument('--to')
    p = sub.add_parser('delete', help='remove a file (--write)'); p.add_argument('path')
    p = sub.add_parser('plugin-set', help='switch a plugin file on or off for the next boot (--write)')
    p.add_argument('file'); p.add_argument('state', choices=['on', 'off'])
    a = ap.parse_args(argv)
    t = None
    try:
        if a.cmd == 'ports':
            ports = winmm_ports() if sys.platform == 'win32' else []
            i_in, i_out = pick_op1(ports, a.port)
            res = {'ports': [{'direction': d, 'index': i, 'name': n} for d, i, n in ports],
                   'op1_present': i_in is not None and i_out is not None}
            if sys.platform != 'win32':
                res['note'] = 'winmm is Windows only; run this with the Windows python'
        else:
            t = open_transport(a)
            s = Session(t, allow_write=a.write, timeout=a.timeout, tries=a.tries)
            if a.cmd == 'hello':
                res = hello(s)
            elif a.cmd == 'describe':
                res = {'device': getattr(t, 'device', None), 'interfaces': getattr(t, 'interfaces', None),
                       'midi_out_endpoint': getattr(t, 'ep_out', None), 'midi_in_endpoint': getattr(t, 'ep_in', None)}
            elif a.cmd == 'inventory':
                res = inventory(s)
            elif a.cmd == 'run-status':
                res = run_status(s)
            elif a.cmd in ('get', 'stats'):
                path = a.path if a.cmd == 'get' else '/yaffs2/user/coop_stats.txt'
                t0 = time.time()
                data = file_get(s, path)
                res = {'path': path, 'bytes': len(data), 'seconds': round(time.time() - t0, 2), 'messages': s.sent}
                if a.out:
                    open(a.out, 'wb').write(data)
                    res['saved'] = os.path.abspath(a.out)
                if a.cmd == 'stats':
                    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
                    from emu_gate import parse_stats
                    st = parse_stats(data.decode('latin-1'))
                    res['stats'] = {k: [{kk: vv for kk, vv in e.items() if kk != '_' or vv} for e in v]
                                    for k, v in st.items() if k in ('coop_stats', 'cb', 'cost', 'engines', 'plugins',
                                                                    'plugmem', 'plugfile', 'fxctl')}
                elif not a.out:
                    res['text'] = data.decode('latin-1')
            elif a.cmd == 'put':
                data = open(a.file, 'rb').read()
                res = put(s, a.to or '/yaffs2/plugins/' + os.path.basename(a.file), data)
                res['note'] = 'loaded at the next boot: power-cycle the OP-1'
            elif a.cmd == 'delete':
                s.ok(DELETE, a.path.encode(), 'DELETE %s' % a.path)
                res = {'deleted': a.path}
            elif a.cmd == 'plugin-set':
                s.ok(PLUGIN_SET, bytes([1 if a.state == 'on' else 0]) + a.file.encode(), 'PLUGIN_SET %s' % a.file)
                res = {'file': a.file, 'next_boot': a.state}
        print(json.dumps(res, indent=1))
        return 0
    except CoopError as e:
        print(json.dumps({'error': str(e)}, indent=1))
        return 1
    finally:
        if t is not None:
            t.close()


if __name__ == '__main__':
    sys.exit(main())
