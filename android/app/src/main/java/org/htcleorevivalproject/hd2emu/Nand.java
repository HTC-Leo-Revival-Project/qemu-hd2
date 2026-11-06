package org.htcleorevivalproject.hd2emu;

import java.nio.charset.Charset;


import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * A raw HTC HD2 NAND image: 4096 blocks of 64 pages, four 528-byte codewords
 * per page, the two user bytes the bad-block marker displaces moved into the
 * bbf slots.  This is the same layout tools/hd2_mknand.py writes, ported so
 * the phone can build its own flash inside the app.
 */
class Nand {

/** ASCII-flavoured: byte values have to survive the round trip. */
static final Charset LATIN1 = Charset.forName("ISO-8859-1");
static final Charset UTF8 = Charset.forName("UTF-8");

    static final int PAGE = 2048;
    static final int OOB = 16;
    static final int CW = 528;
    static final int RAW_PAGE = 2112;
    static final int PPB = 64;
    static final int NBLOCKS = 4096;
    static final int ROM_BLOCK = 0x212;
    static final int BLK = PAGE * PPB;

    private final RandomAccessFile f;

    /** The SPL (stock or HSPL) lives in 0:APPSBL, from here (HTCLEO_SPL_BLOCK). */
    static final int SPL_BLOCK = 0x1f1;
    /** The radio's board record + CID (0:HTC) and boot mode (0:FOTA), which OSBL reads. */
    static final int HTC_BLOCK = 0xe6;
    static final int FOTA_BLOCK = 0x1d7;

    /** Opens an existing image for in-place edits (no erase): used to swap the SPL. */
    static Nand edit(File path) throws IOException {
        if (path.length() < (long) NBLOCKS * PPB * RAW_PAGE) {
            throw new IOException(path.getName() + " is not a whole NAND image");
        }
        return new Nand(path, false);
    }

    Nand(File path) throws IOException {
        this(path, true);
    }

    private Nand(File path, boolean wipe) throws IOException {
        f = new RandomAccessFile(path, "rw");
        if (!wipe) return;
        /* erased flash is all 0xFF, written 16 blocks (2 MiB) at a time */
        f.setLength(0);
        byte[] ff = new byte[RAW_PAGE * PPB * 16];
        Arrays.fill(ff, (byte) 0xff);
        for (int i = 0; i < NBLOCKS; i += 16) f.write(ff);
    }

    static byte[] encode(byte[] data, byte[] oob) {
        byte[] raw = new byte[RAW_PAGE];
        Arrays.fill(raw, (byte) 0xff);
        for (int cw = 0; cw < 4; cw++) {
            int o = cw * CW;
            for (int i = 0; i < 512; i++) {
                int src = cw * 512 + i;
                raw[o + i] = src < data.length ? data[src] : (byte) 0xff;
            }
            /* controller ECC parity: zeros, as hd2_mknand.py writes them */
            Arrays.fill(raw, o + 512, o + 522, (byte) 0);
            if (oob != null) {
                for (int i = 0; i < 4; i++) {
                    int src = cw * 4 + i;
                    if (src < oob.length) raw[o + 522 + i] = oob[src];
                }
            }
        }
        /* the x16 bad block marker word stays 0xFFFF and takes two bytes with it */
        int o = 3 * CW;
        raw[o + 526] = raw[o + 464];
        raw[o + 527] = raw[o + 465];
        raw[o + 464] = (byte) 0xff;
        raw[o + 465] = (byte) 0xff;
        return raw;
    }

    void writePage(long page, byte[] data) throws IOException {
        writePage(page, data, null);
    }

    void writePage(long page, byte[] data, byte[] oob) throws IOException {
        f.seek(page * RAW_PAGE);
        f.write(encode(data, oob));
    }

    /** yaffs2 tags: sequence number, object id, chunk id, n_bytes */
    void writePage(long page, byte[] data, int seq, int objId, int chunkId, int nBytes)
            throws IOException {
        byte[] tags = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(seq).putInt(objId).putInt(chunkId).putInt(nBytes).array();
        writePage(page, data, tags);
    }

    static byte[] fill(int n, byte b) {
        byte[] out = new byte[n];
        Arrays.fill(out, b);
        return out;
    }

    void close() throws IOException {
        f.close();
    }

    /**
     * Strips the R000FF container HTC wraps bootloaders in (tools/hd2_mknand.py
     * unwrap_r000ff): a 0x20 header, then 64 KiB blocks with a 0x89-byte
     * signature seam between them.  A flat image (a RUU's section 0x200, the
     * bundled SPLs) comes back as it is.
     */
    static byte[] unwrapR000ff(byte[] d) {
        final byte[] mark = {0, 1, 0, (byte) 0x80, 0, 0, 0, 1};
        final int hdr = 0x20, blk = 0x10000, sig = 0x80, seam = sig + 1 + 8;
        if (d.length < hdr || !Arrays.equals(Arrays.copyOfRange(d, 0x18, 0x20), mark)) return d;
        int pos = hdr, plain = 0;
        while (pos < d.length) {
            plain += Math.min(blk, d.length - pos);
            if (pos + blk + seam >= d.length
                    || !Arrays.equals(Arrays.copyOfRange(d, pos + blk + sig + 1, pos + blk + sig + 9), mark)) {
                break;
            }
            pos += seam;
        }
        byte[] out = new byte[plain];
        int dst = 0;
        while (dst < plain) {
            int n = Math.min(blk, plain - dst);
            System.arraycopy(d, pos, out, dst, n);
            pos += n;
            dst += n;
        }
        return out;
    }

    /**
     * Flashes an SPL (stock or HSPL) where the boot ROM and the emulator's spl=nand
     * look for it: 0:APPSBL, one 2048-byte sector per page.  Pages past the new
     * image's end are erased, so a shorter SPL leaves nothing of the old one behind.
     */
    void flashSpl(byte[] image) throws IOException {
        byte[] spl = unwrapR000ff(image);
        int pages = (spl.length + PAGE - 1) / PAGE;
        for (int i = 0; i < pages; i++) {
            writePage((long) SPL_BLOCK * PPB + i, Arrays.copyOfRange(spl, i * PAGE, Math.min(spl.length, (i + 1) * PAGE)));
        }
        /* the old image may be longer: blank the rest of the 4 blocks the HSPL occupies */
        for (int i = pages; i < 5 * PPB; i++) {
            writePage((long) SPL_BLOCK * PPB + i, fill(PAGE, (byte) 0xff));
        }
    }

    /** True if the OS area (from block 0x212) holds anything: an image that was flashed with an OS. */
    static boolean hasOs(File image) {
        try {
            RandomAccessFile r = new RandomAccessFile(image, "r");
            try {
                if (r.length() < (long) NBLOCKS * PPB * RAW_PAGE) return false;
                byte[] raw = new byte[RAW_PAGE];
                r.seek((long) ROM_BLOCK * PPB * RAW_PAGE);
                r.readFully(raw);
                for (int i = 0; i < 512; i++) if (raw[i] != (byte) 0xff) return true;
                return false;
            } finally {
                r.close();
            }
        } catch (IOException e) {
            return false;
        }
    }

    /** The first MB-sized check that the SPL a NAND holds is there at all. */
    boolean hasSpl() throws IOException {
        byte[] raw = new byte[RAW_PAGE];
        f.seek((long) SPL_BLOCK * PPB * RAW_PAGE);
        f.readFully(raw);
        for (int i = 0; i < 512; i++) if (raw[i] != (byte) 0xff) return true;
        return false;
    }

    /**
     * hd2_mknand.py write_htc_config(): what OSBL reads from the radio's config
     * partitions.  The CID is always SuperCID (11111111), so the stock SPL a RUU puts
     * back flashes any region's ROM.  Without these records the real boot chain
     * (SPL LLE) stalls in OSBL's serial console.
     */
    void writeHtcConfig() throws IOException {
        java.nio.ByteBuffer le = java.nio.ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN);
        byte[] hdr = fill(PAGE, (byte) 0xff);
        le.putInt(0xa5b61234).putInt(1).putInt(1);
        System.arraycopy(le.array(), 0, hdr, 0, 12);
        byte[] rec = fill(PAGE, (byte) 0xff);
        Arrays.fill(rec, 0, 48, (byte) 0);
        System.arraycopy("11111111".getBytes(LATIN1), 0, rec, 0, 8);
        System.arraycopy("62".getBytes(LATIN1), 0, rec, 8, 2);
        System.arraycopy("35000000000000".getBytes(LATIN1), 0, rec, 16, 14);
        byte[] fota = fill(PAGE, (byte) 0xff);
        java.nio.ByteBuffer fb = java.nio.ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN);
        fb.putInt(0x82508650);
        System.arraycopy(fb.array(), 0, fota, 0, 24);
        writePage((long) HTC_BLOCK * PPB, hdr);
        writePage((long) HTC_BLOCK * PPB + 1, rec);
        writePage((long) FOTA_BLOCK * PPB, fota);
    }

    /** The MAC addresses the kernel reads out of block 505. */
    static final int MAC_BLOCK = 505;

    void writeFactoryMacs(byte[] wifi, byte[] bt) throws IOException {
        byte[] p0 = fill(PAGE, (byte) 0xff);
        System.arraycopy(wifi, 0, p0, 40, 6);
        byte[] p6 = fill(PAGE, (byte) 0xff);
        for (int i = 0; i < 6; i++) p6[i] = bt[5 - i];          // byte reversed
        writePage((long) MAC_BLOCK * PPB, p0);
        writePage((long) MAC_BLOCK * PPB + 6, p6);
    }

    static byte[] mac(String text) {
        String[] parts = text.split(":");
        if (parts.length != 6) throw new IllegalArgumentException("bad MAC address " + text);
        byte[] out = new byte[6];
        for (int i = 0; i < 6; i++) out[i] = (byte) Integer.parseInt(parts[i], 16);
        return out;
    }

    /* ---------------------------------------------------------- NBH / cLK 1.4 */

    /** The data of a signed NBH: blocks of a 9-byte header plus data+signature. */
    static byte[] nbhUnsign(byte[] d) {
        byte[] out = new byte[d.length];
        int n = 0, off = 0x17;
        while (off + 9 <= d.length) {
            ByteBuffer b = ByteBuffer.wrap(d, off, 9).order(ByteOrder.LITTLE_ENDIAN);
            int len = b.getInt(), sig = b.getInt();
            if (len == 0 || off + 9 + len + sig > d.length) break;
            System.arraycopy(d, off + 9, out, n, len);
            n += len;
            off += 9 + len + sig;
        }
        return Arrays.copyOf(out, n);
    }

    public static class Os {
        public List<byte[]> sectors = new ArrayList<byte[]>();
        public int start, count;
    }

    /** The OS section of an NBH, as the 2048-byte sectors HSPL writes to NAND. */
    static Os extractNbhOs(byte[] d) throws IOException {
        if (d.length < 6 || new String(d, 0, 6, LATIN1).indexOf("R000FF") != 0) {
            throw new IOException("not an NBH");
        }
        d = nbhUnsign(d);
        int base = indexOf(d, new byte[]{(byte) 0xe9, (byte) 0xfd, (byte) 0xff});
        if (base < 0) throw new IOException("no OS image (WinCE MBR) in this NBH");
        if (d[base + 510] != 0x55 || d[base + 511] != (byte) 0xaa || d[base + 0x1be + 4] != 0x23) {
            throw new IOException("bad OS image MBR in this NBH");
        }
        ByteBuffer mbr = ByteBuffer.wrap(d, base + 0x1be + 8, 8).order(ByteOrder.LITTLE_ENDIAN);
        int start = mbr.getInt(), count = mbr.getInt();
        Os os = new Os();
        os.start = start;
        os.count = count;
        for (int i = 0; i < start + count; i++) {
            byte[] s = new byte[PAGE];
            int at = base + i * 0x808;
            if (at + PAGE <= d.length) System.arraycopy(d, at, s, 0, PAGE);
            os.sectors.add(s);
        }
        if (os.sectors.size() < 2 || indexOf(os.sectors.get(1), "MSFLSH50".getBytes()) != 0) {
            throw new IOException("MSFLSH50 tag missing in this NBH");
        }
        return os;
    }

    /** cLK 1.4's compiled partition list, if this LK has one. */
    static List<String[]> clk14Ptable(byte[] lk, List<Integer> sizes) {
        byte[] tag = "PTABLE-MB\0".getBytes(LATIN1);
        int i = lastIndexOf(lk, tag);
        if (i < 0 || i % 32 != 0) return null;
        List<String[]> parts = new ArrayList<String[]>();
        for (int k = 1; k < 16; k++) {
            int o = i + 32 * k;
            if (o + 32 > lk.length) break;
            int z = o;
            while (z < o + 16 && lk[z] != 0) z++;
            String name = new String(lk, o, z - o, LATIN1);
            if (name.isEmpty()) break;
            ByteBuffer e = ByteBuffer.wrap(lk, o + 16, 8).order(ByteOrder.LITTLE_ENDIAN);
            parts.add(new String[]{name, String.valueOf(e.getInt()), String.valueOf(e.getInt()),
                    String.valueOf(o + 20)});
        }
        return parts;
    }

    /** target/htcleo/init.c target_init() for cLK 1.4: 8 blocks per MiB. */
    static List<String[]> clk14Layout(List<String[]> parts) {
        final int bpm = 8;
        int blk = 0x219;
        List<String[]> out = new ArrayList<String[]>();
        for (int idx = 0; idx < parts.size(); idx++) {
            int length = Integer.parseInt(parts.get(idx)[2]);
            int size;
            if (length != 0) {
                size = length * bpm;
            } else {
                int rest = 0;
                for (int k = idx + 1; k < parts.size(); k++) rest += Integer.parseInt(parts.get(k)[2]) * bpm;
                size = (NBLOCKS - 1 - 186 - 4) - (blk + rest);
            }
            out.add(new String[]{parts.get(idx)[0], String.valueOf(blk), String.valueOf(size)});
            blk += size;
        }
        return out;
    }

    /** The OS area for a raw cLK binary, the way the NBH tools make one. */
    static Os osSectorsFromLk(byte[] lk) {
        int count = (lk.length + PAGE - 1) / PAGE;
        byte[] mbr = new byte[512];
        mbr[0] = (byte) 0xe9;
        mbr[1] = (byte) 0xfd;
        mbr[2] = (byte) 0xff;
        ByteBuffer.wrap(mbr).order(ByteOrder.LITTLE_ENDIAN);
        mbr[0x1be] = 0;
        mbr[0x1bf] = 2;
        mbr[0x1c0] = 1;
        mbr[0x1c1] = 0;
        mbr[0x1c2] = 0;
        mbr[0x1c3] = 0x23;
        mbr[0x1c4] = 0x3f;
        mbr[0x1c5] = 1;
        mbr[0x1c6] = 3;
        ByteBuffer.wrap(mbr, 0x1c6, 8).order(ByteOrder.LITTLE_ENDIAN).putInt(2).putInt(count);
        mbr[510] = 0x55;
        mbr[511] = (byte) 0xaa;

        Os os = new Os();
        os.start = 2;
        os.count = count;
        byte[] first = new byte[PAGE];
        System.arraycopy(mbr, 0, first, 0, 512);
        os.sectors.add(first);
        byte[] tag = new byte[PAGE];
        System.arraycopy("MSFLSH50".getBytes(), 0, tag, 0, 8);
        os.sectors.add(tag);
        for (int i = 0; i < count; i++) {
            byte[] s = new byte[PAGE];
            System.arraycopy(lk, i * PAGE, s, 0, Math.min(PAGE, lk.length - i * PAGE));
            os.sectors.add(s);
        }
        return os;
    }

    /** struct vpartitions, as lib/vptable writes it. */
    static byte[] vptable(List<String[]> parts, boolean extrom) {
        byte[] b = new byte[442];
        byte[] tag = "VPTABLE".getBytes(LATIN1);
        System.arraycopy(tag, 0, b, 0, 7);
        for (int i = 0; i < parts.size(); i++) {
            int o = 8 + i * 36;
            byte[] name = parts.get(i)[0].getBytes(LATIN1);
            System.arraycopy(name, 0, b, o, Math.min(name.length, 32));
            short blocks = (short) Integer.parseInt(parts.get(i)[1]);
            boolean asize = "1".equals(parts.get(i)[2]);
            ByteBuffer.wrap(b, o + 32, 3).order(ByteOrder.LITTLE_ENDIAN)
                    .putShort(blocks).put((byte) (asize ? 1 : 0));
        }
        ByteBuffer.wrap(b, 440, 2).order(ByteOrder.LITTLE_ENDIAN)
                .putShort((short) (extrom ? 1 : 0));
        return b;
    }

    /* --------------------------------------------------------------- helpers */

    static int indexOf(byte[] haystack, byte[] needle) {
        return indexOf(haystack, needle, 0);
    }

    static int indexOf(byte[] haystack, byte[] needle, int from) {
        outer:
        for (int i = from; i <= haystack.length - needle.length; i++) {
            for (int j = 0; j < needle.length; j++) {
                if (haystack[i + j] != needle[j]) continue outer;
            }
            return i;
        }
        return -1;
    }

    static int lastIndexOf(byte[] haystack, byte[] needle) {
        outer:
        for (int i = haystack.length - needle.length; i >= 0; i--) {
            for (int j = 0; j < needle.length; j++) {
                if (haystack[i + j] != needle[j]) continue outer;
            }
            return i;
        }
        return -1;
    }

    static byte[] read(File f) throws IOException {
        RandomAccessFile raf = new RandomAccessFile(f, "r");
        try {
            long len = raf.length();
            if (len > Integer.MAX_VALUE) throw new IOException(f + " is too big");
            byte[] out = new byte[(int) len];
            raf.readFully(out);
            return out;
        } finally {
            raf.close();
        }
    }
}
