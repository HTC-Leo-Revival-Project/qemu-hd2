#!/usr/bin/env python3
"""Build a raw HTC HD2 NAND image with cLK and a NAND Android ROM installed.

The result is what the 512 MiB NAND of a real HD2 contains after:

  1. flashing cLK with HSPL (LEOIMG.nbh -> OS area at block 0x212),
  2. cLK creating its partition table (VPTABLE block),
  3. installing the ROM zip from recovery (boot.img + yaffs2 system/data).

Raw layout (what the emulated controller stores): 4096 blocks x 64 pages x
2112 bytes, every page four 528-byte codewords

    [0..511] data  [512..521] ECC  [522..525] spare (yaffs tags)  [526..527]

with the user bytes at offsets 464-465 of the last codeword relocated to raw
bytes 526-527 (the x16 factory bad-block marker word, CFG1
BAD_BLOCK_BYTE_NUM=465).
This is exactly what the msm NAND controller produces in ECC mode for cLK's
and Linux's writes, so both read the image back unchanged.

usage:
  hd2_mknand.py --nbh LEOIMG.nbh --rom NexusHD2-ICS-CM9-HWA_V2.8.zip \
                [--recovery recovery.img] -o nand.img
"""
import argparse
import gzip
import hashlib
import io
import os
import re
import stat
import struct
import sys
import time
import zipfile

PAGE = 2048
OOB = 16
CW = 528
RAW_PAGE = 2112
PPB = 64                    # pages per block
NBLOCKS = 4096
ROM_BLOCK = 0x212           # HTCLEO_ROM_OFFSET
BLK = PAGE * PPB            # 128 KiB


# ---------------------------------------------------------------- raw NAND
class Nand:
    def __init__(self, path):
        self.f = open(path, 'w+b')
        # erased flash: all 0xFF (written block by block to keep it sparse-ish)
        ff = b'\xff' * (RAW_PAGE * PPB)
        for _ in range(NBLOCKS):
            self.f.write(ff)

    @staticmethod
    def encode(data, oob=None):
        data = data.ljust(PAGE, b'\xff')
        oob = (oob or b'').ljust(OOB, b'\xff')
        raw = bytearray(b'\xff' * RAW_PAGE)
        for cw in range(4):
            o = cw * CW
            raw[o:o + 512] = data[cw * 512:(cw + 1) * 512]
            raw[o + 512:o + 522] = bytes(10)         # controller ECC parity
            raw[o + 522:o + 526] = oob[cw * 4:(cw + 1) * 4]
        # last codeword: the x16 bad-block marker word (bytes 464-465) stays
        # 0xFFFF and the two user bytes it displaces go to the bbf slots
        o = 3 * CW
        raw[o + 526:o + 528] = raw[o + 464:o + 466]
        raw[o + 464:o + 466] = b'\xff\xff'
        return bytes(raw)

    def write_page(self, page, data, oob=None):
        self.f.seek(page * RAW_PAGE)
        self.f.write(self.encode(data, oob))

    def write_raw_sector(self, page, data):
        self.write_page(page, data)

    def close(self):
        self.f.close()


# ---------------------------------------------------------------- factory data
MAC_BLOCK = 505             # HTC config block the kernel reads MACs from


def parse_mac(text):
    b = bytes(int(x, 16) for x in text.split(':'))
    if len(b) != 6:
        raise ValueError('bad MAC address ' + text)
    return b


def write_factory_macs(nand, wifi, bt):
    """htcleo_nand.c scanmac(): WiFi MAC at bytes 40-45 of block 505 page
    0, Bluetooth MAC (byte-reversed) at bytes 0-5 of page 6."""
    p0 = bytearray(b'\xff' * PAGE)
    p0[40:46] = wifi
    p6 = bytearray(b'\xff' * PAGE)
    p6[0:6] = bt[::-1]
    nand.write_page(MAC_BLOCK * PPB, bytes(p0))
    nand.write_page(MAC_BLOCK * PPB + 6, bytes(p6))


# ---------------------------------------------------------------- cLK / ROM header
def nbh_unsign(d):
    """Join the data of a signed NBH ("R000FF"): from offset 0x17, blocks of
    a 9-byte header (u32 length, u32 signature length, u8 flag), the data
    and its signature."""
    out, off = bytearray(), 0x17
    while off + 9 <= len(d):
        ln, sl, flag = struct.unpack_from('<IIB', d, off)
        if not ln or off + 9 + ln + sl > len(d):
            break
        out += d[off + 9:off + 9 + ln]
        off += 9 + ln + sl
    return bytes(out)


def extract_nbh_os(path):
    """Return the OS section of an HTC NBH as a list of 2048-byte sectors.

    HSPL writes the OS section to NAND from block 0x212, one 2048-byte sector
    per page (each followed by 8 bytes of WinCE sector info in the file).
    The first sector is a WinCE MBR (E9 FD FF ... 55 AA, partition type 0x23)
    whose first partition gives the image's start/length in sectors; sector
    1 carries the MSFLSH50 tag, the cLK image follows.
    """
    d = open(path, 'rb').read()
    if not d.startswith(b'R000FF'):
        raise SystemExit('not an NBH: %s' % path)
    d = nbh_unsign(d)
    base = d.find(b'\xe9\xfd\xff')
    if base < 0:
        raise SystemExit('no OS image (WinCE MBR) in %s' % path)
    mbr = d[base:base + 512]
    if mbr[510:512] != b'\x55\xaa' or mbr[0x1be + 4] != 0x23:
        raise SystemExit('bad OS image MBR in %s' % path)
    start, count = struct.unpack_from('<II', mbr, 0x1be + 8)
    n = start + count
    sectors = [d[base + i * 0x808: base + i * 0x808 + 2048] for i in range(n)]
    if not sectors[1].startswith(b'MSFLSH50'):
        raise SystemExit('MSFLSH50 tag missing in %s' % path)
    return sectors, start, count


def clk14_ptable(lk):
    """Locate cLK 1.4's compiled board_part_list ("PTABLE-MB" + entries).

    struct ptentry { char name[16]; u32 start, length, flags; u8 type, perm; }
    lengths are in MiB; partitions are laid out from block 0x219
    (HTCLEO_FLASH_OFFSET), the last zero-length one taking the rest.
    """
    i = lk.rfind(b'PTABLE-MB\x00')
    if i < 0 or i % 32:
        return None, None
    parts = []
    for k in range(1, 16):
        e = lk[i + 32 * k:i + 32 * k + 32]
        name = e[:16].split(b'\0')[0].decode()
        if not name:
            break
        start, length = struct.unpack_from('<II', e, 16)
        parts.append([name, start, length, i + 32 * k + 20])
    return i, parts


def clk14_layout(parts):
    """Replicate target/htcleo/init.c target_init() for cLK 1.4."""
    bpm = 8
    blk = 0x219
    out = []
    fixed_after = 0
    for idx, (name, start, length, _) in enumerate(parts):
        if length:
            size = length * bpm
        else:
            rest = sum(p[2] * bpm for p in parts[idx + 1:])
            size = (NBLOCKS - 1 - 186 - 4) - (blk + rest)
        out.append((name, blk, size))
        blk += size
    return out


def os_sectors_from_lk(lk):
    """Build the OS area for a raw cLK binary (what the NBH tools produce)."""
    count = (len(lk) + PAGE - 1) // PAGE
    mbr = bytearray(512)
    mbr[0:3] = b'\xe9\xfd\xff'
    struct.pack_into('<BBBBBBBBII', mbr, 0x1be, 0, 2, 1, 0, 0x23, 0x3f, 1, 3,
                     2, count)
    mbr[510:512] = b'\x55\xaa'
    secs = [bytes(mbr).ljust(PAGE, b'\x00'),
            b'MSFLSH50'.ljust(PAGE, b'\x00')]
    for i in range(count):
        secs.append(lk[i * PAGE:(i + 1) * PAGE].ljust(PAGE, b'\x00'))
    return secs, 2, count


# ---------------------------------------------------------------- VPTABLE
def vptable(parts, extrom):
    """struct vpartitions (lib/vptable): tag[7], part_def[12], short extrom.

    part_def = { char name[32]; short size /* blocks */; bool asize; } (36 B)
    """
    b = bytearray(442)
    b[0:7] = b'VPTABLE'
    for i, (name, blocks, asize) in enumerate(parts):
        o = 8 + i * 36
        b[o:o + len(name)] = name.encode()
        struct.pack_into('<hB', b, o + 32, blocks, 1 if asize else 0)
    struct.pack_into('<h', b, 440, 1 if extrom else 0)
    return bytes(b)


# ---------------------------------------------------------------- boot.img
def mkbootimg(kernel, ramdisk, cmdline, base=0x11800000, pagesize=2048):
    def pad(b):
        return b + b'\x00' * ((-len(b)) % pagesize)
    sha = hashlib.sha1()
    sha.update(kernel); sha.update(struct.pack('<I', len(kernel)))
    sha.update(ramdisk); sha.update(struct.pack('<I', len(ramdisk)))
    sha.update(b''); sha.update(struct.pack('<I', 0))
    hdr = struct.pack('<8s10I16s512s32s', b'ANDROID!',
                      len(kernel), base + 0x8000,
                      len(ramdisk), base + 0x1000000,
                      0, base + 0xF00000,
                      base + 0x100, pagesize, 0, 0,
                      b'', cmdline.encode(), sha.digest().ljust(32, b'\0'))
    return pad(hdr) + pad(kernel) + pad(ramdisk)


# ---------------------------------------------------------------- yaffs2
YAFFS_OBJECT_TYPE_FILE = 1
YAFFS_OBJECT_TYPE_SYMLINK = 2
YAFFS_OBJECT_TYPE_DIRECTORY = 3


class Node:
    def __init__(self, name, kind, mode=0o644, uid=0, gid=0, mtime=0,
                 data=b'', target=''):
        self.name, self.kind = name, kind
        self.mode, self.uid, self.gid, self.mtime = mode, uid, gid, mtime
        self.data, self.target = data, target
        self.children = {}


class Tree:
    def __init__(self):
        self.root = Node('', 'dir', 0o771, 1000, 1000)

    def lookup(self, path, create_dirs=False):
        n = self.root
        for p in [x for x in path.strip('/').split('/') if x]:
            if p not in n.children:
                if not create_dirs:
                    return None
                n.children[p] = Node(p, 'dir', 0o755)
            n = n.children[p]
        return n

    def add_file(self, path, data, mode=0o644, mtime=0):
        d, name = os.path.split(path.strip('/'))
        parent = self.lookup(d, True)
        parent.children[name] = Node(name, 'file', mode, 0, 0, mtime, data)

    def add_dir(self, path, mode=0o755, mtime=0):
        n = self.lookup(path, True)
        n.mode, n.mtime = mode, mtime

    def symlink(self, target, path):
        d, name = os.path.split(path.strip('/'))
        parent = self.lookup(d, True)
        parent.children[name] = Node(name, 'link', 0o777, 0, 0,
                                     int(time.time()), target=target)

    def delete(self, path):
        d, name = os.path.split(path.strip('/'))
        parent = self.lookup(d)
        if parent and name in parent.children:
            del parent.children[name]

    def walk(self, n=None, path=''):
        n = n if n is not None else self.root
        yield path or '/', n
        for name in sorted(n.children):
            c = n.children[name]
            if c.kind == 'dir':
                yield from self.walk(c, path + '/' + name)
            else:
                yield path + '/' + name, c


def set_perm(node, uid, gid, mode):
    node.uid, node.gid = uid, gid
    node.mode = (node.mode & ~0o7777) | mode


def set_perm_recursive(tree, uid, gid, dmode, fmode, path):
    top = tree.lookup(path)
    if top is None:
        return
    for _p, n in tree.walk(top, path):
        if n.kind == 'dir':
            set_perm(n, uid, gid, dmode)
        elif n.kind == 'file':
            set_perm(n, uid, gid, fmode)


class Yaffs2Writer:
    """Write a yaffs2 filesystem image the way the kernel would have."""

    def __init__(self, nand, first_block, nblocks):
        self.nand = nand
        self.first = first_block
        self.nblocks = nblocks
        self.block = 0
        self.chunk = 0
        self.seq = 0x1000           # YAFFS_LOWEST_SEQUENCE_NUMBER
        self.next_id = 257          # YAFFS_NOBJECT_BUCKETS + 1

    def _write(self, data, obj_id, chunk_id, n_bytes):
        if self.block >= self.nblocks:
            raise SystemExit('yaffs2: partition full')
        tags = struct.pack('<IIII', self.seq, obj_id, chunk_id, n_bytes)
        page = (self.first + self.block) * PPB + self.chunk
        self.nand.write_page(page, data, tags)
        self.chunk += 1
        if self.chunk == PPB:
            self.chunk = 0
            self.block += 1
            self.seq += 1

    def _header(self, obj_id, parent, node, otype, size=0):
        h = bytearray(PAGE)
        h[512:] = b'\xff' * (PAGE - 512)        # empty xattr area
        name = node.name.encode()[:255]
        alias = node.target.encode()[:159]
        mode = node.mode
        mode |= {YAFFS_OBJECT_TYPE_FILE: stat.S_IFREG,
                 YAFFS_OBJECT_TYPE_SYMLINK: stat.S_IFLNK,
                 YAFFS_OBJECT_TYPE_DIRECTORY: stat.S_IFDIR}[otype]
        struct.pack_into('<Ii', h, 0, otype, parent)
        h[10:10 + len(name)] = name
        struct.pack_into('<IIIIIIii', h, 268, mode, node.uid, node.gid,
                         node.mtime, node.mtime, node.mtime, size, 0)
        h[300:300 + len(alias)] = alias
        # extra header info in the tags, as yaffs_update_oh() writes it
        chunk_id = 0x80000000 | parent
        tag_obj = obj_id | (otype << 28)
        n_bytes = size if otype == YAFFS_OBJECT_TYPE_FILE else 0
        self._write(bytes(h), tag_obj, chunk_id, n_bytes)

    def write_tree(self, tree):
        def emit(node, parent):
            for name in sorted(node.children):
                c = node.children[name]
                oid = self.next_id
                self.next_id += 1
                if c.kind == 'dir':
                    self._header(oid, parent, c, YAFFS_OBJECT_TYPE_DIRECTORY)
                    emit(c, oid)
                elif c.kind == 'link':
                    self._header(oid, parent, c, YAFFS_OBJECT_TYPE_SYMLINK)
                else:
                    self._header(oid, parent, c, YAFFS_OBJECT_TYPE_FILE,
                                 len(c.data))
                    for i in range(0, len(c.data), PAGE):
                        chunk = c.data[i:i + PAGE]
                        self._write(chunk.ljust(PAGE, b'\xff'), oid,
                                    i // PAGE + 1, len(chunk))
        emit(tree.root, 1)
        used = self.block + (1 if self.chunk else 0)
        return used


# ---------------------------------------------------------------- ROM install
def zip_tree(z, prefix, tree, dest=''):
    for info in z.infolist():
        if not info.filename.startswith(prefix):
            continue
        rel = info.filename[len(prefix):]
        if not rel:
            continue
        mtime = int(time.mktime(info.date_time + (0, 0, -1)))
        path = dest + '/' + rel
        if info.is_dir():
            tree.add_dir(path, 0o755, mtime)
        else:
            tree.add_file(path, z.read(info), 0o644, mtime)


def parse_edify_calls(script, names):
    """Yield (func, [args]) for calls to `names`, in script order."""
    pat = re.compile(r'\b(%s)\s*\(' % '|'.join(names))
    pos = 0
    while True:
        m = pat.search(script, pos)
        if not m:
            return
        depth, i = 1, m.end()
        while depth and i < len(script):
            if script[i] == '(':
                depth += 1
            elif script[i] == ')':
                depth -= 1
            elif script[i] == '"':
                i = script.index('"', i + 1)
            i += 1
        body = script[m.end():i - 1]
        args = re.findall(r'"([^"]*)"|(\b0?[0-7]+\b|\b\d+\b)', body)
        yield m.group(1), [a or b for a, b in args]
        pos = i


def install_rom(z, system, data, softkeys=True, gpu=False):
    script = z.read('META-INF/com/google/android/updater-script').decode()
    # NAND install branch (installtype 1), cLK bootloader:
    zip_tree(z, 'system/', system)
    zip_tree(z, 'data/', data)
    # cLK cannot use RMNET: the installer applies the PPP patch to /system
    zip_tree(z, 'mods/clk-ppp-patch/', system)
    # Without --gpu: libhwui treats any ro.kernel.qemu as "an emulator, GPU
    # only if ro.kernel.qemu.gles=1", so hwui stays off; debug.egl.hw=0 keeps
    # libEGL on the PixelFlinger renderer.  The value is 0, not 1:
    # SystemServer skips the Bluetooth service when it is "1".  With --gpu
    # the ROM uses the Adreno 200 libraries as on the phone.
    props = b'\n# emulator\n'
    if not gpu:
        props += b'ro.kernel.qemu=0\nro.kernel.qemu.gles=0\ndebug.egl.hw=0\n'
    if softkeys:
        props += b'qemu.hw.mainkeys=0\n'
    bp = system.lookup('build.prop')
    bp.data = bp.data.rstrip(b'\n') + b'\n' + props

    # unconditional tail of the script: symlinks and permissions
    tail = script[script.index('run_program("/tmp/check_data_apps.sh")'):]
    for fn, args in parse_edify_calls(tail, ['symlink', 'set_perm_recursive',
                                             'set_perm', 'delete']):
        if fn == 'symlink':
            target = args[0]
            for p in args[1:]:
                if p.startswith('/system/'):
                    system.symlink(target, p[len('/system'):])
        elif fn == 'set_perm':
            uid, gid, mode, path = int(args[0]), int(args[1]), \
                int(args[2], 8), args[3]
            tree, rel = (system, path[7:]) if path.startswith('/system') else \
                (data, path[5:]) if path.startswith('/data') else (None, None)
            if tree:
                n = tree.lookup(rel) if rel else tree.root
                if n:
                    set_perm(n, uid, gid, mode)
        elif fn == 'set_perm_recursive':
            uid, gid, dm, fm, path = int(args[0]), int(args[1]), \
                int(args[2], 8), int(args[3], 8), args[4]
            if path.startswith('/system'):
                set_perm_recursive(system, uid, gid, dm, fm, path[7:] or '/')
            elif path.startswith('/data'):
                set_perm_recursive(data, uid, gid, dm, fm, path[5:] or '/')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('--nbh', help='cLK LEOIMG.nbh (official image)')
    ap.add_argument('--lk', help='raw cLK lk.bin instead of an NBH')
    ap.add_argument('--rom', required=True, help='NAND ROM zip')
    ap.add_argument('--kernel',
                    help='replace the ROM kernel with this zImage')
    ap.add_argument('--cmdline',
                    help='replace the ROM boot cmdline, e.g. to put the '
                         'console on the emulator UART')
    ap.add_argument('--recovery', help='recovery boot image (optional)')
    ap.add_argument('--system-mb', type=int, default=220)
    ap.add_argument('--cache-mb', type=int, default=30)
    ap.add_argument('--no-softkeys', action='store_true')
    ap.add_argument('--gpu', action='store_true',
                    help='render with the emulated Adreno 200 (no emulator props)')
    ap.add_argument('--wifi-mac', default='00:23:76:48:44:32')
    ap.add_argument('--bt-mac', default='00:23:76:48:44:33')
    ap.add_argument('-o', '--output', required=True)
    a = ap.parse_args()

    clk14 = None
    if a.nbh:
        secs, start, count = extract_nbh_os(a.nbh)
    elif a.lk:
        lk = bytearray(open(a.lk, 'rb').read())
        off, parts = clk14_ptable(lk)
        if parts:
            # cLK 1.4: fixed compiled partition list; resize to fit the ROM
            want = {'boot': 5, 'system': a.system_mb, 'cache': a.cache_mb}
            for p in parts:
                if p[0] in want and p[2] != want[p[0]]:
                    print('cLK 1.4 ptable: %s %d MiB -> %d MiB'
                          % (p[0], p[2], want[p[0]]))
                    struct.pack_into('<I', lk, p[3], want[p[0]])
                    p[2] = want[p[0]]
            clk14 = clk14_layout(parts)
        secs, start, count = os_sectors_from_lk(bytes(lk))
    else:
        raise SystemExit('need --nbh or --lk')

    nand = Nand(a.output)
    write_factory_macs(nand, parse_mac(a.wifi_mac), parse_mac(a.bt_mac))

    # 1. OS area (what HSPL flashes)
    for i, s in enumerate(secs):
        nand.write_page(ROM_BLOCK * PPB + i, s)
    layout = {}
    if clk14:
        print('cLK 1.4: %d sectors, fixed partitions from 0x219' % count)
        for name, blk, size in clk14:
            layout[name] = (blk, size)
    else:
        start_blk = ROM_BLOCK + (start + count) // PPB
        vpt_blk = start_blk
        part_blk = start_blk + 1
        total = NBLOCKS - part_blk              # flash_size_blk, ExtROM on
        print('cLK 1.5: %d sectors, VPTABLE at block 0x%x, partitions from '
              '0x%x' % (count, vpt_blk, part_blk))
        # 2. partition table (sizes in blocks; 8 blocks per MiB)
        bpm = (1024 * 1024) // BLK
        fixed = [('misc', 1), ('recovery', 5), ('boot', 5),
                 ('system', a.system_mb), ('cache', a.cache_mb)]
        parts = [(n, mb * bpm, False) for n, mb in fixed]
        used = sum(p[1] for p in parts)
        parts.append(('userdata', total - used, True))
        nand.write_page(vpt_blk * PPB, vptable(parts, extrom=True))
        blk = part_blk
        for name, size, _ in parts:
            layout[name] = (blk, size)
            blk += size
    for name, (blk, size) in layout.items():
        print('  %-9s block 0x%04x  %4d blocks  %6.1f MiB'
              % (name, blk, size, size * BLK / 1048576))

    z = zipfile.ZipFile(a.rom)

    # 3. boot.img exactly as the ROM's mkbootimg.sh builds it for cLK
    kernel = z.read('kernel/zImage')
    if a.kernel:
        kernel = open(a.kernel, 'rb').read()
        print('kernel: %s (%d bytes)' % (a.kernel, len(kernel)))
    ramdisk = z.read('kernel/boot_NAND/initrd.gz')
    cmdline = 'no_console_suspend=1 console=null'
    m = re.search(r"--cmdline '([^']*)'", z.read('kernel/mkbootimg.sh').decode())
    if m:
        cmdline = m.group(1)
    # the emulator has a UART where a real HD2 has none, so the console can go
    # there too and nothing is lost when the kernel wedges before the ramcon
    # is up: "console=null" alone swallows everything
    if a.cmdline:
        cmdline = a.cmdline
    boot = mkbootimg(kernel, ramdisk, cmdline)
    b0, bsize = layout['boot']
    if len(boot) > bsize * BLK:
        raise SystemExit('boot.img too large')
    for i in range(0, len(boot), PAGE):
        nand.write_page(b0 * PPB + i // PAGE, boot[i:i + PAGE])
    print('boot.img: %d bytes, cmdline "%s"' % (len(boot), cmdline))

    if a.recovery:
        rec = open(a.recovery, 'rb').read()
        r0, rsize = layout['recovery']
        if len(rec) > rsize * BLK:
            raise SystemExit('recovery too large')
        for i in range(0, len(rec), PAGE):
            nand.write_page(r0 * PPB + i // PAGE, rec[i:i + PAGE])
        print('recovery: %d bytes' % len(rec))

    # 4. yaffs2 system and userdata
    system, data = Tree(), Tree()
    system.root.mode, system.root.uid, system.root.gid = 0o755, 0, 0
    install_rom(z, system, data, softkeys=not a.no_softkeys, gpu=a.gpu)
    for name, tree in (('system', system), ('userdata', data)):
        first, size = layout[name]
        used = Yaffs2Writer(nand, first, size).write_tree(tree)
        print('%s: yaffs2, %d of %d blocks used' % (name, used, size))

    nand.close()
    print('wrote', a.output)


if __name__ == '__main__':
    main()
