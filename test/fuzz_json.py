#!/usr/bin/env python3
"""Deterministic mutational fuzzer for the C JSON parser, with optional ASan.

Run `python3 test/fuzz_json.py [iterations] [--sanitize]`. The sanitizer mode
requires a compiler with libasan and libubsan. Corpus, RNG and mutations mirror
the former native fuzzer; Python owns all generation and test orchestration.
"""
import argparse
import ctypes
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SEEDS = [b'{}', b'[]', b'{"a":1}', b'{"a":[1,2,{"b":null}]}', b'[[[[[]]]]]',
         b'{"text":"hi","chat":{"id":123}}',
         b'{"choices":[{"delta":{"content":"x"},"finish_reason":"stop"}]}',
         b'{"content":[{"type":"text","text":"y"}],"stop_reason":"end_turn"}',
         b'"\\u00e4\\ud83d\\ude00"', b'1.5e-9', b'-0', b'true', b'false', b'null',
         b'1e9999999999999999999999999', b'1e-9999999999999999999999999',
         b'{"a":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}',
         b'{"nested":{"nested":{"nested":{"nested":[1,2,3,4,5]}}}}',
         b'{"id":"j1","task":"x","at":1700000000,"every":60,"enabled":true}',
         b'', b' ', b'\t\n', b'{', b'}', b'[', b']', b'{"a":', b':1', b',,', b'"\\', b'0123456789']
ALPHABET = b'{}[]",\\:0123456789abcdefu tn'

class Random:
    def __init__(self): self.state = 12345
    def next(self):
        self.state = (self.state * 1103515245 + 12345) & 0xffffffff
        return (self.state >> 16) & 0x7fff

def fuzz(path, iterations):
    lib = ctypes.CDLL(str(path))
    lib.json_parse.argtypes = [ctypes.c_char_p]; lib.json_parse.restype = ctypes.c_void_p
    lib.json_obj_get.argtypes = [ctypes.c_void_p, ctypes.c_char_p]; lib.json_obj_get.restype = ctypes.c_void_p
    lib.json_get_str.argtypes = [ctypes.c_void_p, ctypes.c_char_p]; lib.json_get_str.restype = ctypes.c_void_p
    lib.json_get_num.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]; lib.json_get_num.restype = ctypes.c_double
    lib.json_free.argtypes = [ctypes.c_void_p]; lib.json_free.restype = None
    rng = Random()
    for i in range(iterations):
        if i % 4 == 0:
            data = bytearray(SEEDS[rng.next() % len(SEEDS)])
            if data and rng.next() % 2:
                for _ in range(1 + rng.next() % 4):
                    pos = rng.next() % len(data)
                    method = rng.next() % 3
                    if method == 0: data[pos] = rng.next() % 256
                    elif method == 1: data[pos] = ALPHABET[rng.next() % 28]
                    else: data.insert(pos, data[pos])
        else:
            length = rng.next() % 64
            data = bytearray(rng.next() % 256 for _ in range(length))
        value = lib.json_parse(bytes(data))
        if value:
            lib.json_obj_get(value, b'a'); lib.json_get_str(value, b'b')
            lib.json_get_num(value, b'c', 0.0); lib.json_free(value)
        if i % 20000 == 0:
            print('fuzz_json: %d ok' % i, flush=True)
    print('fuzz_json: done, %d iterations, no crashes' % iterations, flush=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iterations', nargs='?', type=int, default=100000)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--library', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.iterations < 0: parser.error('iterations must be nonnegative')
    if args.library:
        fuzz(args.library, args.iterations)
        return 0
    cc = shlex.split(os.environ.get('CC', 'cc'))
    with tempfile.TemporaryDirectory(prefix='clawc-fuzz-') as tmp:
        so = Path(tmp) / 'json.so'
        command = cc + ['-std=c99', '-g', '-fPIC', '-shared', '-pthread']
        if args.sanitize: command += ['-O1', '-fsanitize=address,undefined']
        else: command += ['-O2']
        subprocess.run(command + [str(ROOT / 'src/json.c'), str(ROOT / 'src/util.c'), '-o', str(so)], check=True)
        if args.sanitize:
            libasan = subprocess.check_output(cc + ['-print-file-name=libasan.so'], text=True).strip()
            if not Path(libasan).is_file():
                raise RuntimeError('compiler did not locate libasan.so; cannot run sanitized Python fuzzer')
            env = os.environ.copy()
            env['LD_PRELOAD'] = libasan + (':' + env['LD_PRELOAD'] if env.get('LD_PRELOAD') else '')
            env['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
            env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
            return subprocess.run([sys.executable, __file__, str(args.iterations), '--library', str(so)], env=env).returncode
        fuzz(so, args.iterations)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
