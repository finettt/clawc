"""Python unittest coverage for packed text and Unicode JSON decoding.

Only the C ABI probe is compiled; all comparisons, concurrency and iteration
are controlled by Python. No third-party testing framework is required.
"""
import concurrent.futures
import ctypes
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parent.parent
HOSTCC = shlex.split(os.environ.get('HOSTCC', 'cc'))


def compile_shared(output, *sources):
    subprocess.run(HOSTCC + ['-std=c99', '-O2', '-fPIC', '-shared', '-pthread',
                            '-I', str(ROOT / 'src'), *map(str, sources), '-o', str(output)],
                   cwd=ROOT, check=True)


class JVal(ctypes.Structure):
    # The fields read here are the fixed prefix of struct JVal in src/json.h.
    _fields_ = [('type', ctypes.c_int), ('boolean', ctypes.c_int),
                ('num', ctypes.c_double), ('str', ctypes.c_void_p)]


class JsonUnicodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='clawc-py-json-')
        path = Path(cls.tmp.name) / 'json.so'
        compile_shared(path, ROOT / 'src/json.c', ROOT / 'src/util.c')
        cls.lib = ctypes.CDLL(str(path))
        cls.lib.json_parse.argtypes = [ctypes.c_char_p]
        cls.lib.json_parse.restype = ctypes.POINTER(JVal)
        cls.lib.json_free.argtypes = [ctypes.POINTER(JVal)]
        cls.lib.json_free.restype = None

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check_escape(self, encoded, expected):
        v = self.lib.json_parse(encoded)
        self.assertTrue(bool(v), repr(encoded))
        try:
            self.assertEqual(v.contents.type, 3, repr(encoded))
            self.assertEqual(ctypes.string_at(v.contents.str), expected, repr(encoded))
        finally:
            self.lib.json_free(v)

    def test_all_nonzero_bmp_code_units(self):
        for cp in range(1, 0x10000):
            self.check_escape(('"\\u%04x"' % cp).encode(),
                              chr(cp).encode('utf-8', 'surrogatepass'))

    def test_all_surrogate_pairs(self):
        for cp in range(0x10000, 0x110000):
            value = cp - 0x10000
            src = ('"\\u%04x\\u%04x"' % (0xd800 + (value >> 10),
                                           0xdc00 + (value & 1023))).encode()
            self.check_escape(src, chr(cp).encode('utf-8'))

    def test_embedded_nul_is_rejected(self):
        v = self.lib.json_parse(b'"\\u0000"')
        if v:
            self.lib.json_free(v)
            self.fail('embedded NUL must be rejected')


class PackedTextTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='clawc-py-text-')
        tmp = Path(cls.tmp.name)
        generator = tmp / 'textpack'
        subprocess.run(HOSTCC + ['-std=c99', '-O2', str(ROOT / 'tools/textpack.c'),
                                '-o', str(generator)], cwd=ROOT, check=True)
        cls.generated = subprocess.run([str(generator)], cwd=ROOT, stdout=subprocess.PIPE,
                                       check=True).stdout
        # Probe exposes source literals and their full lengths, including NULs.
        fixture = tmp / 'probe.c'
        fixture.write_text('''#include "packed_text.h"\n#include <stddef.h>\n'''
            '''#define TEXT(id,text) static const char raw_##id[] = text;\n'''
            '''#include "packed_text.def"\n#undef TEXT\n'''
            '''struct entry { const char *p; size_t n; };\n'''
            '''static const struct entry entries[] = {\n'''
            '''#define TEXT(id,text) {raw_##id,sizeof raw_##id},\n'''
            '''#include "packed_text.def"\n#undef TEXT\n};\n'''
            '''size_t item_count(void) { return sizeof entries / sizeof *entries; }\n'''
            '''size_t packed_count(void) { return PACKED_TEXT_COUNT; }\n'''
            '''const char *item_data(size_t i) { return entries[i].p; }\n'''
            '''size_t item_size(size_t i) { return entries[i].n; }\n''', encoding='utf-8')
        libpath = tmp / 'packed.so'
        compile_shared(libpath, ROOT / 'src/packed_text.c', fixture)
        cls.lib = ctypes.CDLL(str(libpath))
        cls.lib.item_count.restype = ctypes.c_size_t
        cls.lib.packed_count.restype = ctypes.c_size_t
        cls.lib.item_size.argtypes = [ctypes.c_size_t]
        cls.lib.item_size.restype = ctypes.c_size_t
        cls.lib.item_data.argtypes = [ctypes.c_size_t]
        cls.lib.item_data.restype = ctypes.c_void_p
        cls.lib.packed_text.argtypes = [ctypes.c_int]
        cls.lib.packed_text.restype = ctypes.c_void_p
        count = cls.lib.item_count()
        if count != cls.lib.packed_count():
            raise AssertionError('packed entry count differs from source literals')
        cls.expected = [(ctypes.string_at(cls.lib.item_data(i), cls.lib.item_size(i)),
                         cls.lib.item_size(i)) for i in range(count)]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_generated_header_is_current(self):
        self.assertEqual(self.generated, (ROOT / 'src/packed_text_data.h').read_bytes())

    def test_all_bytes_across_concurrent_first_use(self):
        gate = threading.Barrier(12)
        def worker():
            gate.wait(timeout=10)
            for _ in range(100):
                for i, (original, n) in enumerate(self.expected):
                    result = ctypes.string_at(self.lib.packed_text(i), n)
                    if result != original:
                        return i
            return None
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            self.assertEqual(list(pool.map(lambda _: worker(), range(12))), [None] * 12)


if __name__ == '__main__':
    unittest.main()
