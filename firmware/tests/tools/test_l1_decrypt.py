"""Tests for tools/l1_decrypt.py: the XTEA round-trip, the chunk model, and key recovery.

No real firmware or key is used. A synthetic key encrypts synthetic plaintext; the test then builds
the same (cipher, known-plaintext) unit structure the real analysis produces and checks that only the
true key arrangement passes check_units. Python 3.8 compatible, stdlib only.
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', '..', 'tools')
sys.path.insert(0, TOOLS)

import l1_decrypt as L  # noqa: E402


class TestXtea(unittest.TestCase):
    def test_round_trip(self):
        key = [0x01234567, 0x89abcdef, 0x0f1e2d3c, 0x4b5a6978]
        for words in ('<', '>'):
            for block in (b'\x00' * 8, b'coop-fw!', bytes(range(8)), b'\xff' * 8):
                c = L.encrypt_unit(block, key, words)
                self.assertEqual(L.decrypt_unit(c, key, words), block)
                self.assertNotEqual(c, block)  # it actually enciphers

    def test_block_cipher_diffuses(self):
        # Changing one ciphertext byte changes (almost) every plaintext byte: the reason the key
        # check cannot assume unknown bytes stay equal between old and new.
        key = [1, 2, 3, 4]
        c0 = L.encrypt_unit(b'\x10' * 8, key)
        c1 = bytearray(c0)
        c1[0] ^= 0xff
        p0 = L.decrypt_unit(bytes(c0), key)
        p1 = L.decrypt_unit(bytes(c1), key)
        differing = sum(1 for a, b in zip(p0, p1) if a != b)
        self.assertGreaterEqual(differing, 6)

    def test_payload_chunking(self):
        # Only the first 8 of every 24 bytes are enciphered; the rest pass through.
        key = [5, 6, 7, 8]
        payload = bytes(range(48))  # two full 24-byte chunks
        enc = L.encrypt_payload(payload, key)
        self.assertEqual(enc[8:24], payload[8:24])
        self.assertEqual(enc[32:48], payload[32:48])
        self.assertNotEqual(enc[0:8], payload[0:8])
        self.assertNotEqual(enc[24:32], payload[24:32])
        self.assertEqual(L.decrypt_payload(enc, key), payload)


class TestKeyRecovery(unittest.TestCase):
    def _units_for(self, key, words, reveal):
        """Build units like pairs_from_patches would: for a few synthetic plaintext pairs, encipher
        under the true key and reveal `reveal` plaintext positions in both old and new."""
        plains = [
            (b'\x11\x22\x33\x44\x55\x66\x77\x88', b'\x11\x22\x33\x44\x99\xaa\xbb\xcc'),
            (b'\xde\xad\xbe\xef\x01\x02\x03\x04', b'\xde\xad\xbe\xef\x05\x06\x07\x08'),
            (b'\x00\x10\x20\x30\x40\x50\x60\x70', b'\x00\x10\x20\x30\x4f\x5f\x6f\x7f'),
        ]
        units = []
        for i, (p_old, p_new) in enumerate(plains):
            c_old = L.encrypt_unit(p_old, key, words)
            c_new = L.encrypt_unit(p_new, key, words)
            k_old = {j: p_old[j] for j in reveal}
            k_new = {j: p_new[j] for j in reveal}
            units.append((i * 8, 0xffa00000 + i * 8, c_old, c_new, k_old, k_new))
        return units

    def test_true_key_passes_wrong_key_fails(self):
        key = [0xa1b2c3d4, 0x11223344, 0x55667788, 0x99aabbcc]
        units = self._units_for(key, '<', reveal=(4, 5, 6, 7))
        self.assertTrue(L.check_units(units, key, '<'))
        wrong = [key[0] ^ 1] + key[1:]
        self.assertFalse(L.check_units(units, wrong, '<'))
        # a permutation of the key words is also wrong
        self.assertFalse(L.check_units(units, [key[1], key[0], key[2], key[3]], '<'))

    def test_only_the_true_arrangement_survives(self):
        # check-key tries 2 byte orders x 24 word perms x 2 unit-word orders; with enough revealed
        # bytes only the real arrangement should pass.
        key = [0x01020304, 0x05060708, 0x090a0b0c, 0x0d0e0f10]
        words = '<'
        units = self._units_for(key, words, reveal=(4, 5, 6, 7))
        keyhex = struct.pack('<4I', *key).hex()
        hits = []
        for swap, perm, arranged in L.key_arrangements(keyhex):
            for wo in ('<', '>'):
                if L.check_units(units, arranged, wo):
                    hits.append((swap, perm, wo))
        # The identity arrangement (le, 0123, <) must be among the hits.
        self.assertIn(('le', (0, 1, 2, 3), '<'), hits)
        # And it should be essentially unique (a stray collision across 96 tries is astronomically
        # unlikely with 12 revealed bytes per side).
        self.assertLessEqual(len(hits), 2)


if __name__ == '__main__':
    unittest.main()
