#!/usr/bin/env python3
"""Check security-relevant ELF program headers and dynamic binding flags."""
import struct
import sys

path = sys.argv[1]
data = open(path, 'rb').read()
if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] not in (1, 2):
    raise SystemExit('expected an ELF64 binary; check hardening manually for other formats')
endian = '<' if data[5] == 1 else '>'
u16 = lambda off: struct.unpack_from(endian+'H', data, off)[0]
u64 = lambda off: struct.unpack_from(endian+'Q', data, off)[0]
assert u16(16) == 3, 'not PIE/ET_DYN'
off, entsize, count = u64(32), u16(54), u16(56)
assert entsize >= 56 and count and count != 0xffff
ph = [struct.unpack_from(endian+'IIQQQQQQ', data, off+i*entsize) for i in range(count)]
loads = [p for p in ph if p[0] == 1]
assert loads and all((p[1] & 3) != 3 for p in loads), 'writable executable load segment'
stack = [p for p in ph if p[0] == 0x6474e551]
assert len(stack) == 1 and not (stack[0][1] & 1), 'missing or executable stack'
relro = [p for p in ph if p[0] == 0x6474e552]
assert len(relro) == 1 and relro[0][6] > 0, 'missing GNU_RELRO'
r = relro[0]
assert any(p[3] <= r[3] and r[3]+r[6] <= p[3]+p[6] for p in loads), 'RELRO outside mapped loads'
dyn = [p for p in ph if p[0] == 2]
assert len(dyn) == 1, 'missing dynamic section'
now = False
for i in range(dyn[0][2], dyn[0][2]+dyn[0][5], 16):
    tag, value = struct.unpack_from(endian+'qQ', data, i)
    if tag == 0: break
    if tag == 24 or (tag == 30 and value & 8) or (tag == 0x6ffffffb and value & 1): now = True
assert now, 'missing BIND_NOW'
print('ok: PIE, NX stack, non-WX loads, RELRO and BIND_NOW')
