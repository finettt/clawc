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




class PublicFunctionTests(unittest.TestCase):
    """ABI tests for xstrdup0 (util.c) and json_append_quoted (json.c)."""

    class Buf(ctypes.Structure):
        _fields_ = [("s", ctypes.c_char_p),
                    ("len", ctypes.c_size_t),
                    ("cap", ctypes.c_size_t)]

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='clawc-py-pub-')
        path = Path(cls.tmp.name) / 'pub.so'
        compile_shared(path, ROOT / 'src/json.c', ROOT / 'src/util.c')
        cls.lib = ctypes.CDLL(str(path))
        cls.lib.json_parse.argtypes = [ctypes.c_char_p]
        cls.lib.json_parse.restype = ctypes.POINTER(ctypes.c_void_p)
        cls.lib.json_free.restype = None
        cls.lib.json_get_str.restype = ctypes.c_char_p
        cls.lib.xstrdup0.argtypes = [ctypes.c_char_p]
        cls.lib.xstrdup0.restype = ctypes.c_char_p
        cls.lib.buf_init.argtypes = [ctypes.c_void_p]
        cls.lib.buf_init.restype = None
        cls.lib.buf_puts.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        cls.lib.buf_puts.restype = None
        cls.lib.buf_free.argtypes = [ctypes.c_void_p]
        cls.lib.buf_free.restype = None
        cls.lib.json_append_quoted.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        cls.lib.json_append_quoted.restype = None

    def test_xstrdup0_null_becomes_empty(self):
        # xstrdup0(NULL) must return a non-NULL pointer to an empty string.
        p = self.lib.xstrdup0(None)
        self.assertNotEqual(ctypes.cast(p, ctypes.c_void_p).value, 0)
        self.assertEqual(ctypes.string_at(p), b'')

    def test_xstrdup0_copies_string(self):
        src = b'hello'
        p = self.lib.xstrdup0(src)
        self.assertEqual(ctypes.string_at(p), src)

    def test_json_append_quoted_roundtrip(self):
        # Build a JSON object through json_append_quoted (key verbatim + quoted
        # value) and verify it parses back with the expected values.
        buf = self.Buf()
        ctypes.memset(ctypes.byref(buf), 0, ctypes.sizeof(buf))
        self.lib.buf_init(ctypes.byref(buf))
        self.lib.json_append_quoted(ctypes.byref(buf), b'{"from":', b'alice')
        self.lib.json_append_quoted(ctypes.byref(buf), b',"to":', b'bob')
        self.lib.buf_puts(ctypes.byref(buf), b'}')
        v = self.lib.json_parse(buf.s)
        self.assertTrue(bool(v))
        self.assertEqual(self.lib.json_get_str(v, b'from'), b'alice')
        self.assertEqual(self.lib.json_get_str(v, b'to'), b'bob')
        self.lib.json_free(v)
        self.lib.buf_free(ctypes.byref(buf))


class HistWriteTests(unittest.TestCase):
    """hist_write must persist every message and round-trip through hist_load."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='clawc-py-hist-')
        gen = Path(cls.tmp.name) / 'hist_roundtrip'
        harness = Path(cls.tmp.name) / 'hist_roundtrip.c'
        harness.write_text(HIST_ROUNDTRIP_HARNESS)
        subprocess.run(HOSTCC + ['-std=c99', '-O2', '-I', str(ROOT / 'src'),
                                 str(harness), str(ROOT / 'src/history.c'),
                                 str(ROOT / 'src/json.c'), str(ROOT / 'src/util.c'),
                                 '-o', str(gen)], cwd=ROOT, check=True)
        cls.prog = str(gen)

    def test_hist_write_roundtrip(self):
        r = subprocess.run([self.prog], cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('OK', r.stdout)


HIST_ROUNDTRIP_HARNESS = r"""
#include "history.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

static Msg *mk(const char *role, const char *content) {
    Msg *m = calloc(1, sizeof *m);
    m->role = xstrdup(role);
    m->content = xstrdup(content ? content : "");
    return m;
}

int main(void) {
    const char *p = "hist_wt_roundtrip.jsonl";
    remove(p);

    Msg msgs[3];
    msgs[0] = *mk("user", "hello");
    msgs[1] = *mk("assistant", "hi there");
    msgs[2] = *mk("user", "how are you");

    if (hist_write(p, msgs, 3, 0) != 0) { printf("FAIL: hist_write error\n"); return 1; }

    Msg *out = NULL; int n = 0;
    if (hist_load(p, &out, &n) != 0 || n != 3) {
        printf("FAIL: hist_load n=%d\n", n);
        return 1;
    }
    const char *want[3] = {"hello", "hi there", "how are you"};
    for (int i = 0; i < 3; i++) {
        if (!out[i].content || strcmp(out[i].content, want[i]) != 0) {
            printf("FAIL: mismatch at %d: %s\n", i, out[i].content ? out[i].content : "(null)");
            return 1;
        }
    }
    free(out);
    remove(p);
    printf("OK\n");
    return 0;
}
"""


if __name__ == '__main__':
    unittest.main()
