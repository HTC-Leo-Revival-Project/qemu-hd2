#!/usr/bin/env python3
"""Create a microSD card image that iBoot's fsboot path can actually mount.

usage: mkibootcard.py [options] <output.img>

What iBoot expects, read from the source rather than assumed
------------------------------------------------------------
apps/iBoot/boot.c:565 mount_bootfs() does:

    bootdevice = env_get("boot-device");
    snprintf(subdevice, 128, "%s%c", bootdevice,
             (char)env_get_uint("boot-partition", 0) + 'a');
    return mount_bootfs_from_device(subdevice);

and mount_bootfs_from_device() then does lookup_blockdev(device) and hands the
result to each filesystem until one mounts it. So the device name it looks for is
literally "<boot-device><partition-letter>", where the letter is 'a' plus
boot-partition. With boot-device="sd0" and the default boot-partition=0 that is
"sd0a" -- so the boot filesystem has to be the *first* partition, published by
lib/partition under the letter 'a'.

lib/partition/partition.c:113 partition_scan_and_publish_subdevices() reads an
MBR at MBR_ADDRESS (0x1be), four 16-byte entries, and mbr_scan() accepts any
entry with a nonzero sysid, naming them sd0a, sd0b, ... in table order. It tries
LwVM first and falls back to the MBR, so a plain MBR is what we want.

lib/fs/hfs/hfs.c:177 HFSInitPartition() reads kBlockSize (0x200) bytes from
kMDBBaseOffset (2 * kBlockSize = 1024) within the partition and requires:

    signature == 0x482B ('H+')  or  0x4858 ('HX')
    version  == 4
    blockSize: power of two, 0x200 .. 0x10000
    totalBlocks != 0
    catalogFile.logicalSize >= 512
    extentsFile.logicalSize  >= 512

which is what a stock HFS+ or HFSX volume already provides. HFSX is the default
here because iBoot accepts either and HFSX is what Apple ships.

Layout produced
---------------
    sector 0            protective MBR, partition type 0x0C (FAT32 LBA) is
                        deliberately NOT used -- see MBR_TYPE below
    partition 1         HFS+, starts at PART_START_LBA

MBR_TYPE is 0xAF (Apple HFS/HFS+) rather than 0x0C because mbr_scan() only tests
for nonzero and the letter, not the type -- but a type of 0xAF is what a Mac
formatted this card, so an image produced here is also mountable on a real host.
"""
import argparse
import os
import struct
import shutil
import subprocess
import sys
import tempfile

SECTOR = 512

# Where the boot filesystem starts, and how big it is.
#
# PART_START_LBA is 2048, the same offset mksdcard.sh uses for its FAT32
# partition, because it is the conventional first-partition start and it leaves
# the MBR and its slack well clear.
PART_START_LBA = 2048

# Apple HFS/HFS+. mbr_scan() accepts any nonzero sysid, but this is what a Mac
# writes, so the image is a real HFS disk rather than a partition that only
# happens to parse.
MBR_TYPE = 0xAF

# The one partition iBoot will find. Only partition 1 is published: letter 'a'.
#
# Two partitions would be published as sd0a and sd0b, and fsboot asks for sd0a.
# Keeping it to one means there is no ambiguity about which one the boot
# filesystem is.
NUM_PARTITIONS = 1


def find_mkfs_hfsplus():
    """Locate mkfs.hfsplus.

    Normally on PATH from hfsprogs. Also looks next to this script and in
    tools/hfsprogs/, because it is often extracted rather than installed --
    hfsprogs is not in a default Ubuntu install and pulling in root to add it is
    more friction than unpacking the .deb.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.environ.get("MKFS_HFSPLUS"),
        "mkfs.hfsplus",
        os.path.join(here, "hfsprogs", "usr", "sbin", "mkfs.hfsplus"),
        os.path.join(here, "..", "tools", "hfsprogs", "usr", "sbin", "mkfs.hfsplus"),
        os.path.join(here, "..", "..", "tools", "hfsprogs", "usr", "sbin", "mkfs.hfsplus"),
    ]
    for c in candidates:
        if not c:
            continue
        if os.path.sep in c:
            if os.path.isfile(c) and os.access(c, os.X_OK):
                return os.path.abspath(c)
        else:
            found = shutil_which(c)
            if found:
                return found
    return None


def shutil_which(name):
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, name)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return None


def write_mbr(path, total_lbas, part_start_lba, part_sectors):
    """Write a protective MBR with one HFS partition.

    Written by hand rather than via sfdisk because the partition entry has to be
    byte-exact for lib/partition's struct mbr_partition, which is __packed with
    8 single-byte fields followed by two uint32_t LE (partition_private.h:26).
    CHS fields are left zero: the disk is larger than CHS can address and the
    LBA fields are what actually get used.
    """
    entry = struct.pack(
        "<BBBBBBBB II",
        0x00,              # bootid
        0x00, 0x00, 0x00,  # starthead/sect/cyl
        MBR_TYPE,          # sysid
        0x00, 0x00, 0x00,  # endhead/sect/cyl
        part_start_lba,
        part_sectors,
    )
    assert len(entry) == 16, len(entry)

    # Disk signature, non-zero: some tools refuse an all-zero one.
    mbr = bytearray(SECTOR)
    struct.pack_into("<I", mbr, 0x1B8, 0x48563221)

    for i in range(4):
        off = 0x1BE + i * 16
        if i == 0:
            mbr[off:off + 16] = entry
        # remaining entries stay zero, which mbr_scan() skips on sysid == 0

    mbr[0x1FE] = 0x55
    mbr[0x1FF] = 0xAA

    with open(path, "r+b") as f:
        f.seek(0)
        f.write(bytes(mbr))


def main():
    ap = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Build an HFS+ microSD image for iBoot's fsboot path.",
        epilog="""mkfs.hfsplus comes from hfsprogs, which is not in a default Ubuntu image.
Rather than needing root to install it, unpack it next to this script:

    apt-get download hfsprogs
    dpkg-deb -x hfsprogs_*.deb tools/hfsprogs/

which puts the binary where this script already looks. Set MKFS_HFSPLUS to
override the search.
""")
    ap.add_argument("image", help="output image file")
    ap.add_argument("--size-mb", type=int, default=512,
                    help="total card size in MiB (default 512; a power of two "
                         "keeps the generated CSD exact)")
    ap.add_argument("--volume-name", default="iBootSD",
                    help="HFS+ volume name (default iBootSD)")
    ap.add_argument("--add", action="append", default=[], metavar="SRC[:DST]",
                    help="copy a file into the volume; repeat for more")
    args = ap.parse_args()

    size_mb = args.size_mb
    if size_mb & (size_mb - 1):
        sys.exit("error: --size-mb must be a power of two")

    mkfs = find_mkfs_hfsplus()
    if not mkfs:
        sys.exit("error: mkfs.hfsplus not found.\n"
                 "       Install hfsprogs, or unpack it and point MKFS_HFSPLUS "
                 "at the binary:\n"
                 "         apt-get download hfsprogs && "
                 "dpkg-deb -x hfsprogs_*.deb hfsprogs/")

    total_bytes = size_mb * 1024 * 1024
    total_lbas = total_bytes // SECTOR
    part_sectors = total_lbas - PART_START_LBA

    # Stage the HFS+ volume in a file of its own, then splice it in after the
    # MBR. mkfs.hfsplus wants a device it can seek in, and writing the partition
    # directly into the card image would leave it operating on a file whose
    # start is not its own offset.
    with tempfile.TemporaryDirectory() as td:
        part = os.path.join(td, "part.img")
        with open(part, "wb") as f:
            f.truncate(part_sectors * SECTOR)

        # -c c=<blocks> sizes the catalog clump, which is what mkfs.hfsplus
        # takes; "c" alone is the attributes file and is not what is wanted
        # here. No -J: iBoot never mounts this journaled, and an inode-backed
        # journal would just be dead weight in the image.
        cmd = [mkfs, "-v", args.volume_name, "-c", "c=2500", part]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("error: mkfs.hfsplus failed:\n" + r.stdout + r.stderr)

        if args.add:
            add_files(part, args.add, mkfs)

        # Assemble: the MBR sector, then enough padding to reach LBA
        # PART_START_LBA, then the volume. The padding is PART_START_LBA - 1
        # sectors, not one: getting that wrong puts the filesystem at LBA 1
        # while the MBR entry claims 2048, and the volume header then lands
        # where iBoot is not looking for it.
        pad = b"\0" * (1 << 20)          # 1 MiB chunk, to keep memory flat
        with open(args.image, "wb") as out:
            out.write(b"\0" * SECTOR)                       # sector 0, rewritten below
            remaining = (PART_START_LBA - 1) * SECTOR
            while remaining:
                n = min(remaining, len(pad))
                out.write(pad[:n])
                remaining -= n
            with open(part, "rb") as src:
                shutil.copyfileobj(src, out, 1 << 20)

        write_mbr(args.image, total_lbas, PART_START_LBA, part_sectors)

    got = os.path.getsize(args.image)
    if got != total_bytes:
        sys.exit("error: image is %d bytes, expected %d" % (got, total_bytes))

    print("wrote %s" % args.image)
    print("  %d MiB, MBR partition 1 = HFS+ at LBA %d (%d MiB)"
          % (size_mb, PART_START_LBA, part_sectors * SECTOR // (1024 * 1024)))
    print("  iBoot will look for blockdev \"sd0a\" (boot-device=sd0, "
          "boot-partition=0)")


def add_files(part_path, specs, mkfs):
    """Copy files into the staged volume using hfsprogs' own hcopy.

    Not present in a stock hfsprogs build, so this is best-effort: an image
    without extra files still mounts, it just does not contain a kernelcache.
    """
    hcopy = os.path.join(os.path.dirname(mkfs), "hcopy")
    if not os.path.isfile(hcopy):
        return

    for spec in specs:
        src, _, dst = spec.partition(":")
        dst = dst or os.path.basename(src)
        r = subprocess.run([hcopy, "-r", src, os.path.join(part_path, "..", dst)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print("note: could not add %s: %s" % (src, r.stderr.strip()))


if __name__ == "__main__":
    main()