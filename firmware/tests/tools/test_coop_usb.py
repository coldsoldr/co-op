"""Tests for tools/coop_usb.py (the SysEx client) against its built-in fake device: the framing, the paged reads,
a file write, a lost reply, the write guard, USB-MIDI packets and the configuration parser. Nothing is sent to a
MIDI port. Python 3.8 compatible, stdlib only.
"""
import os
import sys
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))

import coop_usb as C  # noqa: E402


class Framing(unittest.TestCase):
    def test_pack7_round_trip(self):
        for n in (0, 1, 6, 7, 8, 13, 14, 64, 72):
            raw = bytes((i * 37 + 200) & 0xFF for i in range(n))
            packed = C.pack7(raw)
            self.assertTrue(all(b < 0x80 for b in packed))
            self.assertEqual(len(packed), n + (n + 6) // 7)
            self.assertEqual(C.unpack7(packed), raw)

    def test_pack7_layout(self):                             # as web/src/core/usbmidi/framing.ts: bit k = byte k's top bit
        self.assertEqual(C.pack7(bytes([0x80, 0x01, 0xFF])), bytes([0b101, 0x00, 0x01, 0x7F]))

    def test_message(self):
        m = C.build_message(C.HELLO, 0x1234)
        self.assertEqual(m, bytes([0xF0, 0x00, 0x20, 0x76, 0x7D, 0x21, 0x34, 0x24, 0xF7]))
        with self.assertRaises(C.CoopError):
            C.build_message(C.PUT_DATA, 1, bytes(80))
        self.assertLessEqual(len(C.build_message(C.PUT_DATA, 1, bytes(4 + C.CHUNK))), C.MAX_MESSAGE)

    def test_reply_and_assembler(self):
        reply = C.HEADER + bytes([0x61, 0x34, 0x24]) + C.pack7(b'\0coop') + b'\xF7'
        a = C.Assembler()
        got = []
        for i in range(0, len(reply), 4):
            got += a.feed(b'\xFE' + reply[i:i + 4])          # active sensing between the pieces
        self.assertEqual(got, [reply])
        self.assertEqual(C.parse_reply(got[0]), (0x21, 0x1234, 0, b'coop'))
        self.assertIsNone(C.parse_reply(C.build_message(C.HELLO, 1)))    # a request is not a reply

    def test_usb_midi_packets(self):
        for n in (9, 10, 11, 96):
            msg = b'\xF0' + bytes(i & 0x7F for i in range(n - 2)) + b'\xF7'
            pk = C.midi_to_usb(msg)
            self.assertEqual(len(pk) % 4, 0)
            self.assertIn(pk[-4] & 0x0F, (5, 6, 7))
            self.assertEqual(C.usb_to_midi(pk), msg)


class Device(unittest.TestCase):
    def session(self, write=False, lose=0):
        self.dev = C.demo_mock()
        self.dev.lose = lose
        return C.Session(self.dev, allow_write=write, timeout=0.2, tries=3)

    def test_hello(self):
        h = C.hello(self.session())
        self.assertEqual((h['build'], h['protocol'], h['caps']), ('e10', 1, 0x3F))
        self.assertIn('inventory', h['caps_names'])

    def test_inventory_pages(self):
        inv = C.inventory(self.session())
        self.assertEqual(inv['build'], 'e10')
        self.assertEqual([(p['file'], p['boot']) for p in inv['plugins']], [('big.coop', 'failed'), ('saw.coop', 'loaded')])
        self.assertEqual(inv['plugins'][0]['reason'], 'over the plugin memory budget')
        self.assertGreater(len([r for r in self.dev.requests if r[0] == C.INVENTORY]), 1)

    def test_file_get(self):
        s = self.session()
        data = C.file_get(s, '/yaffs2/user/coop_stats.txt')
        self.assertEqual(data, self.dev.files['/yaffs2/user/coop_stats.txt'])
        with self.assertRaises(C.CoopError):
            C.file_get(s, '/yaffs2/user/none.txt')

    def test_lost_reply_is_asked_again(self):
        s = self.session(lose=1)
        self.assertEqual(C.hello(s)['build'], 'e10')
        self.assertEqual(s.sent, 2)

    def test_writes_need_the_flag(self):
        s = self.session()
        with self.assertRaises(C.CoopError):
            C.put(s, '/yaffs2/plugins/x.coop', b'COOP')
        with self.assertRaises(C.CoopError):
            s.ask(C.DELETE, b'/yaffs2/plugins/saw.coop')
        with self.assertRaises(C.CoopError):
            s.ask(C.RUN, b'\x01/yaffs2/user/coop_test.crun')
        self.assertEqual(self.dev.requests, [])                # nothing reached the device
        self.assertEqual(C.run_status(s)['state'], 'idle')       # the status op is a read

    def test_put(self):
        s = self.session(write=True)
        data = bytes((i * 7) & 0xFF for i in range(300))
        r = C.put(s, '/yaffs2/plugins/new.coop', data)
        self.assertEqual(self.dev.files['/yaffs2/plugins/new.coop'], data)
        self.assertEqual(r['crc32'], '%08x' % (zlib.crc32(data) & 0xFFFFFFFF))
        self.assertEqual([p['boot'] for p in C.inventory(s)['plugins'] if p['file'] == 'new.coop'], ['new'])
        with self.assertRaises(C.CoopError):
            C.put(s, '/yaffs2/user/audio/x.aif', data)        # the firmware's one-level whitelist


class Usb(unittest.TestCase):
    def test_config_parser(self):
        cfg = bytes([9, 2, 0, 0, 2, 1, 0, 0x80, 50,
                     9, 4, 1, 1, 1, 1, 2, 0, 0, 7, 5, 0x86, 0x05, 0x14, 0x01, 1,
                     9, 4, 3, 0, 2, 1, 3, 0, 0, 9, 5, 0x05, 2, 64, 0, 0, 0, 0, 9, 5, 0x84, 2, 64, 0, 0, 0, 0])
        ifs, out, inp = C.UsbipTransport.parse_config(cfg)
        self.assertEqual((out, inp), (5, 4))
        self.assertEqual(ifs[0]['endpoints'][0], {'address': '86', 'type': 'iso', 'max_packet': 276})

    def test_port_choice(self):
        ports = [('in', 0, 'UR44C'), ('in', 1, 'OP-1 field'), ('in', 2, 'OP-1 Midi Device'), ('out', 0, 'GS Wavetable'),
                 ('out', 3, 'OP-1 Midi Device')]
        self.assertEqual(C.pick_op1(ports), (2, 3))
        self.assertEqual(C.pick_op1(ports[:2]), (None, None))


if __name__ == '__main__':
    unittest.main()
