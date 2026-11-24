"""Test tools/even_bufs.py against a synthetic ELF32 symbol table (no real firmware)."""
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))
import even_bufs as E  # noqa: E402


def build_elf(syms):
    """syms: (name, value, size, type, shndx). Sections: null, .bss (1), .symtab (2), .strtab (3)."""
    strtab = b'\0'
    symtab = b'\0' * 16
    for name, value, size, typ, shndx in syms:
        symtab += struct.pack('<IIIBBH', len(strtab), value, size, typ, 0, shndx)
        strtab += name.encode() + b'\0'
    body = symtab + strtab
    shoff = 0x34 + len(body)
    hdr = b'\x7fELF' + bytes([1, 1, 1]) + b'\0' * 9
    hdr += struct.pack('<HHIIIIIHHHHHH', 2, 106, 1, 0, 0, shoff, 0, 0x34, 0, 0, 40, 4, 0)
    sh = b'\0' * 40
    sh += struct.pack('<IIIIIIIIII', 0, 8, 3, 0x1000, 0x34, 0, 0, 0, 4, 0)             # .bss
    sh += struct.pack('<IIIIIIIIII', 0, 2, 0, 0, 0x34, len(symtab), 3, 1, 4, 16)      # .symtab -> .strtab
    sh += struct.pack('<IIIIIIIIII', 0, 3, 0, 0, 0x34 + len(symtab), len(strtab), 0, 0, 1, 0)
    return hdr + body + sh


class EvenBufs(unittest.TestCase):
    def run_on(self, syms):
        with tempfile.NamedTemporaryFile(suffix='.elf', delete=False) as f:
            f.write(build_elf(syms))
        try:
            return E.main(['even_bufs.py', f.name])
        finally:
            os.unlink(f.name)

    def test_even_image_passes(self):
        self.assertEqual(self.run_on([('_buf', 0x2000, 64, 1, 1), ('_flag', 0x2041, 1, 1, 1)]), 0)

    def test_odd_buffer_fails(self):
        self.assertEqual(self.run_on([('_buf', 0x2001, 32, 1, 1)]), 1)

    def test_functions_absolute_and_undefined_are_ignored(self):
        self.assertEqual(self.run_on([('_f', 0x2001, 8, 2, 1), ('_abs', 0x3, 4, 1, 0xFFF1), ('_u', 0x5, 4, 1, 0)]), 0)

    def test_objects(self):
        objs = E.objects(build_elf([('_a', 0x10, 4, 1, 1), ('_b', 0x13, 2, 1, 1)]))
        self.assertEqual(objs, [('_a', 0x10, 4), ('_b', 0x13, 2)])
        self.assertEqual(E.odd(objs), [('_b', 0x13, 2)])


if __name__ == '__main__':
    unittest.main()
