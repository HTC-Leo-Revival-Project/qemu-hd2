package org.htcleorevivalproject.hd2emu;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The radio half of the NAND, and the signed NBH files it comes in: the Java
 * twin of tools/hd2_radio.py (and the NBH section reader of hd2_wmnand.py).
 *
 * A stock Windows Mobile RUU is a couple of hundred MiB, more than an app's
 * heap, so a signed NBH is read where it lies: {@link Nbh} maps the joined
 * data of its signed blocks onto the file instead of copying it out.
 */
final class Radio {
    /** The HTCIMAGE header in front of an NBH's payload. */
    static final int HEADER = 0x200;
    /** The MIBIB partition table's magic: 0x55EE73AA, 0xE35EBDDB, little endian. */
    private static final byte[] PART_MAGIC = {
            (byte) 0xaa, 0x73, (byte) 0xee, 0x55, (byte) 0xdb, (byte) 0xbd, 0x5e, (byte) 0xe3};
    private static final byte[] ELF_ARM = {0x7f, 'E', 'L', 'F', 1, 1, 1};

    private Radio() {
    }

    /* ------------------------------------------------------------- signed NBH */

    /**
     * A signed NBH ("R000FF\n"): from offset 0x17, blocks of a 9-byte header
     * (u32 length, u32 signature length, u8 flag), the data and its
     * signature.  Reads address the joined data, as unsign() returns it.
     */
    static final class Nbh implements Closeable {
        private final RandomAccessFile f;
        private final long[] at;            /* where each block's data starts in the file */
        private final long[] pos;           /* and where it starts in the joined data */
        private final long size;

        Nbh(File file) throws IOException {
            f = new RandomAccessFile(file, "r");
            byte[] magic = new byte[7];
            if (f.length() < 7) {
                f.close();
                throw new IOException(file.getName() + ": not a signed NBH");
            }
            f.readFully(magic);
            if (!"R000FF\n".equals(new String(magic, Nand.LATIN1))) {
                f.close();
                throw new IOException(file.getName() + ": not a signed NBH");
            }
            List<long[]> blocks = new ArrayList<long[]>();
            long off = 0x17, total = 0, length = f.length();
            byte[] h = new byte[9];
            while (off + 9 <= length) {
                f.seek(off);
                f.readFully(h);
                ByteBuffer b = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN);
                long ln = b.getInt() & 0xffffffffL, sl = b.getInt() & 0xffffffffL;
                if (ln == 0 || off + 9 + ln + sl > length) break;
                blocks.add(new long[]{off + 9, total});
                total += ln;
                off += 9 + ln + sl;
            }
            at = new long[blocks.size()];
            pos = new long[blocks.size()];
            for (int i = 0; i < at.length; i++) {
                at[i] = blocks.get(i)[0];
                pos[i] = blocks.get(i)[1];
            }
            size = total;
        }

        long size() {
            return size;
        }

        /** {@code n} bytes of the joined data from {@code from}, short at the end. */
        byte[] read(long from, int n) throws IOException {
            if (from >= size || n <= 0) return new byte[0];
            n = (int) Math.min(n, size - from);
            byte[] out = new byte[n];
            int k = Arrays.binarySearch(pos, from);
            if (k < 0) k = -k - 2;
            int done = 0;
            while (done < n && k < at.length) {
                long end = k + 1 < pos.length ? pos[k + 1] : size;
                long inBlock = from + done - pos[k];
                int take = (int) Math.min(n - done, end - (from + done));
                f.seek(at[k] + inBlock);
                f.readFully(out, done, take);
                done += take;
                k++;
            }
            return out;
        }

        /** hd2_wmnand.py nbh_sections(): type -> {offset, length} in the joined data. */
        Map<Integer, long[]> sections() throws IOException {
            ByteBuffer h = ByteBuffer.wrap(read(0, 0x1c0)).order(ByteOrder.LITTLE_ENDIAN);
            Map<Integer, long[]> out = new HashMap<Integer, long[]>();
            if (h.capacity() < 0x1c0) return out;
            for (int i = 0; i < 32; i++) {
                int type = h.getInt(0x40 + 4 * i);
                if (type == 0) continue;
                out.put(type, new long[]{h.getInt(0xc0 + 4 * i) & 0xffffffffL,
                        h.getInt(0x140 + 4 * i) & 0xffffffffL});
            }
            return out;
        }

        /** One section, read whole (a radio is a few MiB). */
        byte[] section(int type) throws IOException {
            long[] s = sections().get(type);
            if (s == null) return null;
            if (s[1] > Integer.MAX_VALUE) throw new IOException("section " + Integer.toHexString(type) + " is too big");
            byte[] b = read(s[0], (int) s[1]);
            return b.length == s[1] ? b : null;
        }

        /** hd2_radio.py main(): the payload after the HTCIMAGE header, its length at 0x140. */
        byte[] payload() throws IOException {
            byte[] h = read(0x140, 4);
            if (h.length < 4) throw new IOException("this NBH has no payload");
            long plen = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN).getInt() & 0xffffffffL;
            if (plen > Integer.MAX_VALUE) throw new IOException("the radio payload is too big");
            return read(HEADER, (int) plen);
        }

        @Override
        public void close() throws IOException {
            f.close();
        }
    }

    /* ------------------------------------------------------------- the payload */

    /** One MIBIB partition: name, first block, length in blocks, attributes. */
    static final class Part {
        final String name;
        final long block, blocks, attr;

        Part(String name, long block, long blocks, long attr) {
            this.name = name;
            this.block = block;
            this.blocks = blocks;
            this.attr = attr;
        }
    }

    private static long u32(byte[] b, int at) {
        return ByteBuffer.wrap(b, at, 4).order(ByteOrder.LITTLE_ENDIAN).getInt() & 0xffffffffL;
    }

    private static int u16(byte[] b, int at) {
        return ByteBuffer.wrap(b, at, 2).order(ByteOrder.LITTLE_ENDIAN).getShort() & 0xffff;
    }

    /** The MIBIB system partition table (the magic also occurs in code). */
    static List<Part> partitions(byte[] p) {
        List<Part> out = new ArrayList<Part>();
        int i = -1;
        long n;
        while (true) {
            i = Nand.indexOf(p, PART_MAGIC, i + 1);
            if (i < 0 || i + 18 > p.length) return out;
            n = u32(p, i + 12);
            if (n > 0 && n < 32 && p[i + 16] == '0' && p[i + 17] == ':') break;
        }
        int o = i + 16;
        for (int k = 0; k < n && o + 28 <= p.length; k++) {
            int z = o;
            while (z < o + 16 && p[z] != 0) z++;
            String name = new String(p, o, z - o, Nand.LATIN1);
            out.add(new Part(name, u32(p, o + 16), u32(p, o + 20), u32(p, o + 24)));
            o += 28;
        }
        return out;
    }

    /** An ELF's size: the end of its program header table or of its last segment. */
    static long elfSize(byte[] p, int e) {
        long phoff = u32(p, e + 28);
        int phes = u16(p, e + 42), phn = u16(p, e + 44);
        long end = phoff + (long) phes * phn;       /* the DSP ELF has them at its end */
        for (int k = 0; k < phn; k++) {
            long ph = e + phoff + 32L * k;
            if (ph + 32 > p.length) break;
            end = Math.max(end, u32(p, (int) ph + 4) + u32(p, (int) ph + 16));
        }
        return end;
    }

    /** The first ARM executable ELF whose program headers all fit: {offset, length}. */
    static long[] findAmss(byte[] p) {
        int i = 0;
        while (true) {
            i = Nand.indexOf(p, ELF_ARM, i);
            if (i < 0 || i + 52 > p.length) return null;
            long len = p.length - i;
            int etype = u16(p, i + 16), mach = u16(p, i + 18);
            long phoff = u32(p, i + 28);
            int phes = u16(p, i + 42), phn = u16(p, i + 44);
            if (etype == 2 && mach == 40 && phes == 32 && phn > 0 && phn < 64
                    && phoff + 32L * phn <= len) {
                long end = 0;
                boolean ok = true;
                for (int k = 0; k < phn; k++) {
                    int ph = (int) (i + phoff + 32L * k);
                    long off = u32(p, ph + 4), fsz = u32(p, ph + 16);
                    end = Math.max(end, off + fsz);
                    ok &= off + fsz <= len;
                }
                if (ok) return new long[]{i, end};
            }
            i += 4;
        }
    }

    public interface Log {
        void line(String text);
    }

    private static byte[] slice(byte[] p, long from, long to) {
        int a = (int) Math.min(Math.max(from, 0), p.length);
        int b = (int) Math.min(Math.max(to, a), p.length);
        return Arrays.copyOfRange(p, a, b);
    }

    private static Part find(List<Part> parts, String name) throws IOException {
        for (Part part : parts) if (part.name.equals(name)) return part;
        throw new IOException("the radio's partition table has no " + name);
    }

    /**
     * hd2_radio.py write_nand(): the boot area at block 0, FSBL, OSBL (twice),
     * AMSS and, if one follows it, the DSP ELF in DSP1 - each with its trailing
     * erased bytes left off, page by page.
     */
    static void writeNand(Nand nand, byte[] p, long amssAt, long amssLen, List<Part> parts, Log log)
            throws IOException {
        long dspAt = amssAt + amssLen;
        long dspLen = 0;
        if (dspAt + 52 <= p.length && p[(int) dspAt] == 0x7f && p[(int) dspAt + 1] == 'E'
                && p[(int) dspAt + 2] == 'L' && p[(int) dspAt + 3] == 'F') {
            dspLen = elfSize(p, (int) dspAt);
        }
        List<Object[]> images = new ArrayList<Object[]>();
        images.add(new Object[]{"boot area", 0L, slice(p, 0x20, 0x80020)});
        images.add(new Object[]{"FSBL", find(parts, "0:FSBL").block, slice(p, 0x80020, 0xC0020)});
        images.add(new Object[]{"OSBL", find(parts, "0:OSBL").block, slice(p, 0xC0020, 0x140020)});
        images.add(new Object[]{"OSBL2", find(parts, "0:OSBL2").block, slice(p, 0xC0020, 0x140020)});
        images.add(new Object[]{"AMSS", find(parts, "0:AMSS").block, slice(p, amssAt, amssAt + amssLen)});
        if (dspLen > 0) {
            images.add(new Object[]{"DSP1", find(parts, "0:DSP1").block, slice(p, dspAt, dspAt + dspLen)});
        }
        for (Object[] img : images) {
            String name = (String) img[0];
            long start = (Long) img[1];
            byte[] data = (byte[]) img[2];
            int n = data.length;
            while (n > 0 && data[n - 1] == (byte) 0xff) n--;           /* rstrip(b'\xff') */
            for (int i = 0; i < n; i += Nand.PAGE) {
                byte[] page = Arrays.copyOfRange(data, i, Math.min(i + Nand.PAGE, n));
                nand.writePage(start * Nand.PPB + i / Nand.PAGE, page);
            }
            log.line(String.format(java.util.Locale.ROOT, "%-9s block %3d  %d bytes", name, start, n));
        }
    }

    /* ------------------------------------------------------- the firmware ELFs */

    /** The two ELFs the emulator runs with baseband=lle and qdsp6=lle. */
    static final class Firmware {
        byte[] amss;                /* the ARM9 modem's AMSS */
        byte[] dsp;                 /* the QDSP6's DSP1, or null */

        /** Into the images folder as amss.elf and dsp1.elf, each replaced whole. */
        void save(File imagesDir) throws IOException {
            write(new File(imagesDir, "amss.elf"), amss);
            if (dsp != null) write(new File(imagesDir, "dsp1.elf"), dsp);
        }

        static void write(File out, byte[] data) throws IOException {
            File tmp = new File(out.getPath() + ".part");
            java.io.FileOutputStream o = new java.io.FileOutputStream(tmp);
            try {
                o.write(data);
            } finally {
                o.close();
            }
            if (out.exists() && !out.delete()) throw new IOException("cannot replace " + out.getName());
            if (!tmp.renameTo(out)) throw new IOException("cannot write " + out.getName());
        }
    }

    private static boolean isElf(byte[] p, int at) {
        return at >= 0 && at + 52 <= p.length && p[at] == 0x7f && p[at + 1] == 'E'
                && p[at + 2] == 'L' && p[at + 3] == 'F';
    }

    /** hd2_radio.py main(), -o and --dsp: the AMSS ELF and the DSP ELF after it. */
    static Firmware fromNbh(File nbhFile) throws IOException {
        byte[] p;
        Nbh nbh = new Nbh(nbhFile);
        try {
            p = nbh.payload();
        } finally {
            nbh.close();
        }
        long[] amss = findAmss(p);
        if (amss == null) throw new IOException(nbhFile.getName() + ": no AMSS ELF in the payload");
        Firmware fw = new Firmware();
        fw.amss = slice(p, amss[0], amss[0] + amss[1]);
        int dspAt = (int) (amss[0] + amss[1]);
        if (isElf(p, dspAt)) fw.dsp = slice(p, dspAt, dspAt + elfSize(p, dspAt));
        return fw;
    }

    /**
     * The same two ELFs out of a NAND image that has a radio in it - one a
     * stock RUU was flashed to (the Windows Mobile kind), or MAGLDR with a
     * radio: the MIBIB table in the boot area says where AMSS and DSP1 are.
     */
    static Firmware fromNand(File image) throws IOException {
        RandomAccessFile f = new RandomAccessFile(image, "r");
        try {
            if (f.length() < (long) Nand.NBLOCKS * Nand.PPB * Nand.RAW_PAGE) {
                throw new IOException(image.getName() + " is not a whole 512 MiB NAND image");
            }
            List<Part> parts = partitions(readData(f, 0, 0x80000));
            Part amssPart = null, dspPart = null;
            for (Part part : parts) {
                if ("0:AMSS".equals(part.name)) amssPart = part;
                if ("0:DSP1".equals(part.name)) dspPart = part;
            }
            if (amssPart == null) {
                throw new IOException(image.getName() + " has no radio: there is no partition table "
                        + "in its first blocks. Build it with a radio, or extract from a radio NBH.");
            }
            Firmware fw = new Firmware();
            fw.amss = readElf(f, amssPart, false);
            if (fw.amss == null) throw new IOException(image.getName() + ": no AMSS ELF in its 0:AMSS partition");
            if (dspPart != null) fw.dsp = readElf(f, dspPart, true);
            return fw;
        } finally {
            f.close();
        }
    }

    /**
     * An ELF that starts at a partition's first block, as long as its headers
     * say: for AMSS the end of its last segment (what findAmss() returns), for
     * the DSP elfSize(), whose program headers come after its segments.
     */
    private static byte[] readElf(RandomAccessFile f, Part part, boolean dsp) throws IOException {
        /* the rest of the flash: --write-nand does not hold an ELF to its partition's length */
        long room = (Nand.NBLOCKS - part.block) * (long) Nand.BLK;
        if (room <= 0) return null;
        byte[] h = readData(f, part.block, 52);
        if (!isElf(h, 0)) return null;
        long phoff = u32(h, 28);
        int phes = u16(h, 42), phn = u16(h, 44);
        if (dsp ? phes != 32 : (u16(h, 16) != 2 || u16(h, 18) != 40 || phes != 32)) return null;
        long head = phoff + 32L * phn;
        if (head > room) return null;
        byte[] withHeaders = readData(f, part.block, (int) Math.max(52, head));
        long size;
        if (dsp) {
            size = elfSize(withHeaders, 0);
        } else {
            size = 0;
            for (int k = 0; k < phn; k++) {
                int ph = (int) (phoff + 32L * k);
                size = Math.max(size, u32(withHeaders, ph + 4) + u32(withHeaders, ph + 16));
            }
        }
        if (size <= 0 || size > room || size > Integer.MAX_VALUE) return null;
        return readData(f, part.block, (int) size);
    }

    /**
     * {@code n} bytes of page data from a block on, out of the raw image: four
     * 512-byte codewords per page, with the two user bytes the bad-block
     * marker displaced put back (Nand.encode() in reverse).
     */
    static byte[] readData(RandomAccessFile f, long block, int n) throws IOException {
        byte[] out = new byte[n];
        byte[] raw = new byte[Nand.RAW_PAGE];
        long page = block * Nand.PPB;
        for (int done = 0; done < n; page++) {
            f.seek(page * Nand.RAW_PAGE);
            f.readFully(raw);
            byte[] data = new byte[Nand.PAGE];
            for (int cw = 0; cw < 4; cw++) System.arraycopy(raw, cw * Nand.CW, data, cw * 512, 512);
            data[3 * 512 + 464] = raw[3 * Nand.CW + 526];
            data[3 * 512 + 465] = raw[3 * Nand.CW + 527];
            int take = Math.min(Nand.PAGE, n - done);
            System.arraycopy(data, 0, out, done, take);
            done += take;
        }
        return out;
    }

    /** A radio payload into the NAND, the way hd2_radio.py --write-nand does it. */
    static void install(Nand nand, byte[] p, Log log) throws IOException {
        long[] amss = findAmss(p);
        if (amss == null) throw new IOException("no AMSS ELF in the radio");
        writeNand(nand, p, amss[0], amss[1], partitions(p), log);
    }
}
