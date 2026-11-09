#!/usr/bin/env python3
"""ELF minimizer preserves every program header and file-backed segment."""
import os
import shlex
import struct
import subprocess
import tempfile

with tempfile.TemporaryDirectory() as d:
    tool = os.path.join(d, 'ministrip')
    subprocess.run(shlex.split(os.environ.get('HOSTCC', 'cc')) +
                   ['-std=c99', '-O2', 'tools/ministrip.c', '-o', tool], check=True)
    for endian in ('<', '>'):
        path = os.path.join(d, 'sample')
        ident = b'\x7fELF\x02' + (b'\x01' if endian == '<' else b'\x02') + b'\x01' + b'\0'*9
        # One loadable segment and an independent note following it.
        h = struct.pack(endian+'16sHHIQQQIHHHHHH', ident, 3, 62, 1, 0, 64, 600, 0, 64, 56, 2, 64, 2, 1)
        load = struct.pack(endian+'IIQQQQQQ', 1, 5, 0, 0, 0, 256, 256, 4096)
        note = struct.pack(endian+'IIQQQQQQ', 4, 4, 512, 0, 0, 4, 4, 4)
        data = bytearray(800); data[:64] = h; data[64:120] = load; data[120:176] = note
        data[512:516] = b'NOTE'
        with open(path,'wb') as f: f.write(data)
        subprocess.run([tool,path], check=True)
        with open(path,'rb') as f: out=f.read()
        expected=bytearray(data[:516]); expected[40:48]=b'\0'*8; expected[60:64]=b'\0'*4
        assert out == expected, 'segment or program-header changed'
        # An out-of-bounds segment is refused without changing the input.
        bad=bytearray(data); bad[120:176]=struct.pack(endian+'IIQQQQQQ',4,4,799,0,0,4,4,4)
        with open(path,'wb') as f: f.write(bad)
        assert subprocess.run([tool,path],capture_output=True).returncode != 0
        with open(path,'rb') as f: assert f.read() == bad
        # Extended program counts rely on section headers and must be retained.
        ext=bytearray(data); ext[56:58]=struct.pack(endian+'H',0xffff)
        with open(path,'wb') as f: f.write(ext)
        p=subprocess.run([tool,path],capture_output=True)
        assert p.returncode == 0 and b'preserving' in p.stderr
        with open(path,'rb') as f: assert f.read() == ext
print('ok: ELF minimizer preserves segments, rejects malformed inputs, handles both byte orders')
