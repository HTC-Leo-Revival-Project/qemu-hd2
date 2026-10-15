#!/usr/bin/env python3
"""ios7_initrd.py TEMPLATE_INITRD MACH_KERNEL OUT: put a new XNU kernel into the iOS 7 SD package's initrd.gz.

Layout (from the shipped ios7/initrd.gz):
  0x00000  UNXF loader (ARM, 64 KiB): +0x0c payload offset, +0x10 file size, +0x14 payload
           offset, +0x18 payload size
  0x10000  LEOXNU1 header (0x80): +0x10 payload size, +0x14 kernel offset, +0x18 kernel size,
           +0x1c DT offset, +0x20 DT size, +0x24 entry pc, +0x28 crc32(kernel), +0x2c crc32(DT),
           +0x30 sha256(kernel), +0x50 sha256(DT)   (offsets relative to the header)
           Mach-O kernel, then the flattened device tree; file padded to 4 KiB.
The loader and the device tree are kept from the template."""
import hashlib, struct, sys, zlib
t = bytearray(open(sys.argv[1], 'rb').read()); k = open(sys.argv[2], 'rb').read()
assert t[4:8] == b'UNXF' and k[:4] == b'\xce\xfa\xed\xfe'
P = struct.unpack_from('<I', t, 0x0c)[0]; h = t[P:P + 0x80]
assert h[:7] == b'LEOXNU1'
koff, ksz, dtoff, dtsz = struct.unpack_from('<4I', h, 0x14)
dt = bytes(t[P + dtoff:P + dtoff + dtsz])
# entry pc from LC_UNIXTHREAD (ARM_THREAD_STATE: r0-r12, sp, lr, pc)
ncmds = struct.unpack_from('<I', k, 16)[0]; off = 28; pc = None
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', k, off)
    if cmd == 5: pc = struct.unpack_from('<I', k, off + 16 + 15 * 4)[0]
    off += size
assert pc, 'no LC_UNIXTHREAD'
ndt = koff + len(k); total = ndt + len(dt)
h = bytearray(h)
struct.pack_into('<I', h, 0x10, total)
struct.pack_into('<6I', h, 0x14, koff, len(k), ndt, len(dt), pc, zlib.crc32(k))
struct.pack_into('<I', h, 0x2c, zlib.crc32(dt))
h[0x30:0x50] = hashlib.sha256(k).digest(); h[0x50:0x70] = hashlib.sha256(dt).digest()
loader = bytearray(t[:P])
struct.pack_into('<I', loader, 0x10, P + total); struct.pack_into('<I', loader, 0x18, total)
out = bytes(loader) + bytes(h) + k + dt
out += b'\0' * (-len(out) % 4096)
open(sys.argv[3], 'wb').write(out)
print('kernel %d bytes entry 0x%x sha256 %s; initrd %d bytes' % (len(k), pc, hashlib.sha256(k).hexdigest(), len(out)))
