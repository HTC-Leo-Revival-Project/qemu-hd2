package org.htcleorevivalproject.hd2emu;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * SD-card editor engine: reads and edits a FAT12/16/32 disk image (a raw
 * microSD image, with or without an MBR) without mounting it, so no root and
 * no loop device.  Supports long file names, listing, reading, writing,
 * mkdir, delete (recursive), rename and move, and creating a fresh image.
 *
 * Paths are "/dir/sub/file".  The whole FAT is kept in memory and written back
 * (to every copy) after each modifying call.
 */
public class FatImage implements Closeable {
    public static final class Entry {
        public String name;
        public boolean dir;
        public long size;
        int cluster;
        /* where its directory slots live: file offsets of the LFN slots + the short slot */
        long[] slots;
        byte[] shortRaw;
    }

    private final RandomAccessFile f;
    private final long base;               // partition start in the image
    private int bps, spc, reserved, nfats, rootEntries, fatSectors;
    private long totalSectors;
    private int type;                      // 12, 16 or 32
    private int rootCluster;
    private long fatStart, rootStart, dataStart;
    private int clusters;
    private byte[] fat;
    private boolean fatDirty;

    private FatImage(RandomAccessFile f, long base) throws IOException {
        this.f = f;
        this.base = base;
        byte[] b = new byte[512];
        f.seek(base);
        f.readFully(b);
        bps = u16(b, 11);
        spc = b[13] & 0xff;
        reserved = u16(b, 14);
        nfats = b[16] & 0xff;
        rootEntries = u16(b, 17);
        totalSectors = u16(b, 19) != 0 ? u16(b, 19) : u32(b, 32);
        fatSectors = u16(b, 22) != 0 ? u16(b, 22) : (int) u32(b, 36);
        if (bps == 0 || spc == 0 || nfats == 0 || (b[510] & 0xff) != 0x55 || (b[511] & 0xff) != 0xAA) {
            throw new IOException("not a FAT volume");
        }
        int rootSectors = (rootEntries * 32 + bps - 1) / bps;
        fatStart = base + (long) reserved * bps;
        rootStart = fatStart + (long) nfats * fatSectors * bps;
        dataStart = rootStart + (long) rootSectors * bps;
        long dataSectors = totalSectors - reserved - (long) nfats * fatSectors - rootSectors;
        clusters = (int) (dataSectors / spc);
        type = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
        if (type == 32) rootCluster = (int) u32(b, 44);
        fat = new byte[fatSectors * bps];
        f.seek(fatStart);
        f.readFully(fat);
    }

    /** Opens an image: a bare FAT volume or one behind an MBR (first FAT partition). */
    public static FatImage open(File file) throws IOException {
        RandomAccessFile f = new RandomAccessFile(file, "rw");
        try {
            byte[] b = new byte[512];
            f.readFully(b);
            if (b[0] == (byte) 0xEB || b[0] == (byte) 0xE9) return new FatImage(f, 0);
            for (int i = 0; i < 4; i++) {
                int t = b[446 + i * 16 + 4] & 0xff;
                if (t == 1 || t == 4 || t == 6 || t == 0xB || t == 0xC || t == 0xE) {
                    return new FatImage(f, u32(b, 446 + i * 16 + 8) * 512L);
                }
            }
            throw new IOException("no FAT partition found");
        } catch (IOException e) {
            f.close();
            throw e;
        }
    }

    /** Creates a new image of the given size: an MBR with one FAT16 (under 64 MiB) or FAT32 partition. */
    public static void create(File file, int mb, String label) throws IOException {
        if (mb < 16) throw new IOException("at least 16 MB");
        long total = mb * 2048L;
        long start = 2048;
        long psec = total - start;
        boolean f32 = mb >= 64;
        int spc = f32 ? (mb < 260 ? 1 : mb < 8192 ? 8 : 16) : (mb < 32 ? 4 : 8);
        if (f32 && mb >= 260 && mb < 8192) spc = mb < 1024 ? 8 : 16;
        int reserved = f32 ? 32 : 4;
        int rootEntries = f32 ? 0 : 512;
        int rootSectors = rootEntries * 32 / 512;
        long clus = (psec - reserved - rootSectors) / spc;
        int entBytes = f32 ? 4 : 2;
        int fatSec = (int) (((clus + 2) * entBytes + 511) / 512);
        clus = (psec - reserved - rootSectors - 2L * fatSec) / spc;
        RandomAccessFile r = new RandomAccessFile(file, "rw");
        try {
            r.setLength(total * 512);
            byte[] mbr = new byte[512];
            mbr[446 + 4] = (byte) (f32 ? 0x0C : 0x0E);
            put32(mbr, 446 + 8, start);
            put32(mbr, 446 + 12, psec);
            mbr[446 + 1] = 0x01; mbr[446 + 2] = 0x01;
            mbr[446 + 5] = (byte) 0xFE; mbr[446 + 6] = (byte) 0xFF; mbr[446 + 7] = (byte) 0xFF;
            mbr[510] = 0x55; mbr[511] = (byte) 0xAA;
            r.seek(0);
            r.write(mbr);
            byte[] b = new byte[512];
            b[0] = (byte) 0xEB; b[1] = 0x58; b[2] = (byte) 0x90;
            System.arraycopy("MSDOS5.0".getBytes("ISO-8859-1"), 0, b, 3, 8);
            put16(b, 11, 512);
            b[13] = (byte) spc;
            put16(b, 14, reserved);
            b[16] = 2;
            put16(b, 17, rootEntries);
            if (!f32 && psec < 65536) put16(b, 19, (int) psec);
            b[21] = (byte) 0xF8;
            if (!f32) put16(b, 22, fatSec);
            put16(b, 24, 63); put16(b, 26, 255);
            put32(b, 28, start);
            if (!f32 && psec >= 65536) put32(b, 32, psec);
            if (f32) {
                put32(b, 32, psec);
                put32(b, 36, fatSec);
                put32(b, 44, 2);
                put16(b, 48, 1);
                put16(b, 50, 6);
                b[64] = (byte) 0x80; b[66] = 0x29;
                put32(b, 67, 0x48443200L);
                writeLabel(b, 71, label);
                System.arraycopy("FAT32   ".getBytes("ISO-8859-1"), 0, b, 82, 8);
            } else {
                b[36] = (byte) 0x80; b[38] = 0x29;
                put32(b, 39, 0x48443200L);
                writeLabel(b, 43, label);
                System.arraycopy("FAT16   ".getBytes("ISO-8859-1"), 0, b, 54, 8);
            }
            b[510] = 0x55; b[511] = (byte) 0xAA;
            r.seek(start * 512);
            r.write(b);
            if (f32) {                         // FSInfo + backup boot sector
                byte[] fs = new byte[512];
                put32(fs, 0, 0x41615252L); put32(fs, 484, 0x61417272L);
                put32(fs, 488, 0xFFFFFFFFL); put32(fs, 492, 0xFFFFFFFFL);
                fs[510] = 0x55; fs[511] = (byte) 0xAA;
                r.seek((start + 1) * 512);
                r.write(fs);
                r.seek((start + 6) * 512);
                r.write(b);
            }
            byte[] fatInit = new byte[fatSec * 512];
            if (f32) {
                put32(fatInit, 0, 0x0FFFFFF8L); put32(fatInit, 4, 0x0FFFFFFFL); put32(fatInit, 8, 0x0FFFFFFFL);
            } else {
                put16(fatInit, 0, 0xFFF8); put16(fatInit, 2, 0xFFFF);
            }
            for (int i = 0; i < 2; i++) {
                r.seek((start + reserved + (long) i * fatSec) * 512);
                r.write(fatInit);
            }
        } finally {
            r.close();
        }
    }

    private static void writeLabel(byte[] b, int off, String label) {
        /* no volume-label entry is written, so the boot sector says "no name" like mkfs does */
        String l = "NO NAME";
        for (int i = 0; i < 11; i++) b[off + i] = (byte) (i < l.length() ? l.charAt(i) : ' ');
    }

    /* ------------------------------------------------------------ helpers */

    private static int u16(byte[] b, int o) { return (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8); }
    private static long u32(byte[] b, int o) { return (u16(b, o) & 0xffffL) | ((long) u16(b, o + 2) << 16); }
    private static void put16(byte[] b, int o, int v) { b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); }
    private static void put32(byte[] b, int o, long v) { put16(b, o, (int) v); put16(b, o + 2, (int) (v >> 16)); }

    private int clusterSize() { return spc * bps; }

    private long clusterOffset(int c) { return dataStart + (long) (c - 2) * spc * bps; }

    private int getFat(int c) {
        switch (type) {
            case 12: {
                int o = c + c / 2;
                int v = u16(fat, o);
                return (c & 1) != 0 ? v >> 4 : v & 0xFFF;
            }
            case 16: return u16(fat, c * 2);
            default: return (int) (u32(fat, c * 4) & 0x0FFFFFFF);
        }
    }

    private void setFat(int c, int v) {
        switch (type) {
            case 12: {
                int o = c + c / 2;
                int old = u16(fat, o);
                int n = (c & 1) != 0 ? (old & 0x000F) | (v << 4) : (old & 0xF000) | (v & 0xFFF);
                put16(fat, o, n);
                break;
            }
            case 16: put16(fat, c * 2, v); break;
            default: {
                long old = u32(fat, c * 4);
                put32(fat, c * 4, (old & 0xF0000000L) | (v & 0x0FFFFFFFL));
            }
        }
        fatDirty = true;
    }

    private boolean isEnd(int v) {
        return type == 12 ? v >= 0xFF8 : type == 16 ? v >= 0xFFF8 : v >= 0x0FFFFFF8;
    }

    private int endMark() { return type == 12 ? 0xFFF : type == 16 ? 0xFFFF : 0x0FFFFFFF; }

    private int allocCluster() throws IOException {
        for (int c = 2; c < clusters + 2; c++) {
            if (getFat(c) == 0) {
                setFat(c, endMark());
                byte[] z = new byte[clusterSize()];
                f.seek(clusterOffset(c));
                f.write(z);
                return c;
            }
        }
        throw new IOException("the card image is full");
    }

    private void freeChain(int c) {
        while (c >= 2 && c < clusters + 2) {
            int n = getFat(c);
            setFat(c, 0);
            if (isEnd(n) || n == 0) break;
            c = n;
        }
    }

    private List<Integer> chain(int c) {
        List<Integer> l = new ArrayList<Integer>();
        while (c >= 2 && c < clusters + 2 && l.size() <= clusters) {
            l.add(c);
            int n = getFat(c);
            if (isEnd(n) || n == 0) break;
            c = n;
        }
        return l;
    }

    private void flush() throws IOException {
        if (!fatDirty) return;
        for (int i = 0; i < nfats; i++) {
            f.seek(fatStart + (long) i * fatSectors * bps);
            f.write(fat);
        }
        fatDirty = false;
    }

    @Override
    public void close() throws IOException {
        flush();
        f.close();
    }

    public long freeBytes() {
        long n = 0;
        for (int c = 2; c < clusters + 2; c++) if (getFat(c) == 0) n++;
        return n * clusterSize();
    }

    /* -------------------------------------------------------- directories */

    /** All 32-byte slots of a directory: their file offsets (dirCluster 0 = fixed FAT12/16 root). */
    private List<Long> dirSlots(int dirCluster) {
        List<Long> out = new ArrayList<Long>();
        if (dirCluster == 0 && type != 32) {
            for (int i = 0; i < rootEntries; i++) out.add(rootStart + i * 32L);
            return out;
        }
        if (dirCluster == 0) dirCluster = rootCluster;
        for (int c : chain(dirCluster)) {
            for (int i = 0; i < clusterSize() / 32; i++) out.add(clusterOffset(c) + i * 32L);
        }
        return out;
    }

    private byte[] readSlot(long off) throws IOException {
        byte[] b = new byte[32];
        f.seek(off);
        f.readFully(b);
        return b;
    }

    private List<Entry> readDir(int dirCluster) throws IOException {
        List<Entry> out = new ArrayList<Entry>();
        List<Long> slots = dirSlots(dirCluster);
        StringBuilder lfn = new StringBuilder();
        List<Long> lfnSlots = new ArrayList<Long>();
        for (long off : slots) {
            byte[] e = readSlot(off);
            int first = e[0] & 0xff;
            if (first == 0) break;
            if (first == 0xE5) { lfn.setLength(0); lfnSlots.clear(); continue; }
            int attr = e[11] & 0xff;
            if (attr == 0x0F) {
                StringBuilder part = new StringBuilder();
                int[] offs = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int o : offs) {
                    int ch = u16(e, o);
                    if (ch == 0 || ch == 0xFFFF) break;
                    part.append((char) ch);
                }
                lfn.insert(0, part);
                lfnSlots.add(off);
                continue;
            }
            if ((attr & 0x08) != 0) { lfn.setLength(0); lfnSlots.clear(); continue; }   // volume label
            Entry en = new Entry();
            String sn = new String(e, 0, 8, "ISO-8859-1").trim();
            String ext = new String(e, 8, 3, "ISO-8859-1").trim();
            if ((e[0] & 0xff) == 0x05) sn = (char) 0xE5 + sn.substring(1);
            en.name = lfn.length() > 0 ? lfn.toString() : (ext.isEmpty() ? sn : sn + "." + ext);
            if (lfn.length() == 0 && (e[12] & 0x08) != 0) en.name = en.name.toLowerCase(Locale.US);
            en.dir = (attr & 0x10) != 0;
            en.size = u32(e, 28);
            en.cluster = u16(e, 26) | (u16(e, 20) << 16);
            en.slots = new long[lfnSlots.size() + 1];
            for (int i = 0; i < lfnSlots.size(); i++) en.slots[i] = lfnSlots.get(lfnSlots.size() - 1 - i);
            en.slots[en.slots.length - 1] = off;
            en.shortRaw = e;
            lfn.setLength(0);
            lfnSlots.clear();
            if (!en.name.equals(".") && !en.name.equals("..")) out.add(en);
        }
        return out;
    }

    /** Resolves a directory path to its first cluster (0 = root). */
    private int dirCluster(String path) throws IOException {
        int c = 0;
        for (String part : split(path)) {
            Entry hit = null;
            for (Entry e : readDir(c)) if (e.name.equalsIgnoreCase(part)) { hit = e; break; }
            if (hit == null || !hit.dir) throw new IOException("no such directory: " + path);
            c = hit.cluster;
        }
        return c;
    }

    private static List<String> split(String path) {
        List<String> l = new ArrayList<String>();
        for (String p : path.split("/")) if (!p.isEmpty()) l.add(p);
        return l;
    }

    private static String parent(String path) {
        List<String> p = split(path);
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < p.size() - 1; i++) sb.append('/').append(p.get(i));
        return sb.toString();
    }

    private static String leaf(String path) {
        List<String> p = split(path);
        return p.isEmpty() ? "" : p.get(p.size() - 1);
    }

    private Entry find(String path) throws IOException {
        int dc = dirCluster(parent(path));
        String n = leaf(path);
        for (Entry e : readDir(dc)) if (e.name.equalsIgnoreCase(n)) return e;
        throw new IOException("not found: " + path);
    }

    /* ---------------------------------------------------------- public API */

    public List<Entry> list(String path) throws IOException {
        return readDir(dirCluster(path));
    }

    public void read(String path, OutputStream out) throws IOException {
        Entry e = find(path);
        if (e.dir) throw new IOException("is a directory");
        long left = e.size;
        byte[] buf = new byte[clusterSize()];
        for (int c : chain(e.cluster)) {
            if (left <= 0) break;
            int n = (int) Math.min(left, buf.length);
            f.seek(clusterOffset(c));
            f.readFully(buf, 0, n);
            out.write(buf, 0, n);
            left -= n;
        }
    }

    /** Writes (or replaces) a file in a directory from a stream of known size. */
    public void write(String dirPath, String name, InputStream in, long size) throws IOException {
        checkName(name);
        int dc = dirCluster(dirPath);
        for (Entry e : readDir(dc)) {
            if (e.name.equalsIgnoreCase(name)) {
                if (e.dir) throw new IOException(name + " is a directory");
                removeEntry(e);
            }
        }
        int first = 0, prev = 0;
        long left = size;
        byte[] buf = new byte[clusterSize()];
        try {
            while (left > 0) {
                int c = allocCluster();
                if (prev == 0) first = c; else setFat(prev, c);
                prev = c;
                int n = (int) Math.min(left, buf.length);
                int got = 0;
                while (got < n) {
                    int r = in.read(buf, got, n - got);
                    if (r < 0) throw new IOException("input ended early");
                    got += r;
                }
                java.util.Arrays.fill(buf, n, buf.length, (byte) 0);
                f.seek(clusterOffset(c));
                f.write(buf);
                left -= n;
            }
        } catch (IOException e) {
            if (first != 0) freeChain(first);
            flush();
            throw e;
        }
        addEntry(dc, name, false, first, size);
        flush();
    }

    public void mkdir(String path) throws IOException {
        String name = leaf(path);
        checkName(name);
        int dc = dirCluster(parent(path));
        for (Entry e : readDir(dc)) if (e.name.equalsIgnoreCase(name)) throw new IOException("exists: " + name);
        int c = allocCluster();
        byte[] d = new byte[clusterSize()];
        dot(d, 0, ".", c);
        dot(d, 32, "..", dc);
        f.seek(clusterOffset(c));
        f.write(d);
        addEntry(dc, name, true, c, 0);
        flush();
    }

    private void dot(byte[] d, int o, String n, int cluster) {
        java.util.Arrays.fill(d, o, o + 11, (byte) ' ');
        for (int i = 0; i < n.length(); i++) d[o + i] = (byte) n.charAt(i);
        d[o + 11] = 0x10;
        put16(d, o + 26, cluster & 0xFFFF);
        put16(d, o + 20, cluster >>> 16);
    }

    public void delete(String path) throws IOException {
        deleteEntry(find(path));
        flush();
    }

    private void deleteEntry(Entry e) throws IOException {
        if (e.dir) {
            for (Entry c : readDir(e.cluster)) deleteEntry(c);
        }
        removeEntry(e);
    }

    private void removeEntry(Entry e) throws IOException {
        for (long off : e.slots) {
            f.seek(off);
            f.write(0xE5);
        }
        if (e.cluster >= 2) freeChain(e.cluster);
    }

    /** Renames within the same directory. */
    public void rename(String path, String newName) throws IOException {
        move(path, parent(path), newName);
    }

    /** Moves (and optionally renames) an entry into another directory. */
    public void move(String path, String toDir, String newName) throws IOException {
        Entry e = find(path);
        checkName(newName);
        int target = dirCluster(toDir);
        if (e.dir) {
            String full = "/" + String.join("/", split(path)) + "/";
            String dest = "/" + String.join("/", split(toDir)) + "/";
            if (dest.toLowerCase(Locale.US).startsWith(full.toLowerCase(Locale.US))) {
                throw new IOException("cannot move a directory into itself");
            }
        }
        for (Entry o : readDir(target)) {
            if (o.name.equalsIgnoreCase(newName) && o.slots[o.slots.length - 1] != e.slots[e.slots.length - 1]) {
                throw new IOException("exists: " + newName);
            }
        }
        for (long off : e.slots) {          // free the old slots, keep the data
            f.seek(off);
            f.write(0xE5);
        }
        addEntry(target, newName, e.dir, e.cluster, e.size);
        if (e.dir) {                         // fix ".." in the moved directory
            byte[] dd = readSlot(clusterOffset(e.cluster) + 32);
            put16(dd, 26, target & 0xFFFF);
            put16(dd, 20, target >>> 16);
            f.seek(clusterOffset(e.cluster) + 32);
            f.write(dd);
        }
        flush();
    }

    private static void checkName(String n) throws IOException {
        if (n.isEmpty() || n.length() > 255 || n.matches(".*[\\\\/:*?\"<>|].*") || n.equals(".") || n.equals("..")) {
            throw new IOException("bad name: " + n);
        }
    }

    /* ------------------------------------------------------ entry creation */

    private static boolean fits83(String n) {
        return n.matches("[A-Z0-9_~!#$%&'()\\-@^`{}]{1,8}(\\.[A-Z0-9_~!#$%&'()\\-@^`{}]{1,3})?");
    }

    private byte[] shortName(String name, int dirCluster) throws IOException {
        byte[] sn = new byte[11];
        java.util.Arrays.fill(sn, (byte) ' ');
        String up = name.toUpperCase(Locale.US);
        if (fits83(up) && up.equals(name)) {
            int dot = up.indexOf('.');
            String b = dot < 0 ? up : up.substring(0, dot), x = dot < 0 ? "" : up.substring(dot + 1);
            for (int i = 0; i < b.length(); i++) sn[i] = (byte) b.charAt(i);
            for (int i = 0; i < x.length(); i++) sn[8 + i] = (byte) x.charAt(i);
            return sn;
        }
        String clean = up.replaceAll("[^A-Z0-9_]", "_");
        int dot = up.lastIndexOf('.');
        String b = (dot > 0 ? up.substring(0, dot) : up).replaceAll("[^A-Z0-9_]", "");
        String x = dot > 0 ? up.substring(dot + 1).replaceAll("[^A-Z0-9_]", "") : "";
        if (b.isEmpty()) b = clean.substring(0, Math.min(6, clean.length()));
        if (x.length() > 3) x = x.substring(0, 3);
        List<Entry> existing = readDir(dirCluster);
        for (int n = 1; n < 1000000; n++) {
            String tail = "~" + n;
            String stem = b.substring(0, Math.min(b.length(), 8 - tail.length())) + tail;
            String full = x.isEmpty() ? stem : stem + "." + x;
            boolean clash = false;
            for (Entry e : existing) if (e.name.equalsIgnoreCase(full) || shortOf(e).equals(full)) clash = true;
            if (!clash) {
                java.util.Arrays.fill(sn, (byte) ' ');
                for (int i = 0; i < stem.length(); i++) sn[i] = (byte) stem.charAt(i);
                for (int i = 0; i < x.length(); i++) sn[8 + i] = (byte) x.charAt(i);
                return sn;
            }
        }
        throw new IOException("no short name free");
    }

    private String shortOf(Entry e) {
        byte[] r = e.shortRaw;
        String s = new String(r, 0, 8, java.nio.charset.Charset.forName("ISO-8859-1")).trim();
        String x = new String(r, 8, 3, java.nio.charset.Charset.forName("ISO-8859-1")).trim();
        return x.isEmpty() ? s : s + "." + x;
    }

    private static int lfnChecksum(byte[] sn) {
        int sum = 0;
        for (int i = 0; i < 11; i++) sum = (((sum & 1) << 7) + (sum >> 1) + (sn[i] & 0xff)) & 0xff;
        return sum;
    }

    private void addEntry(int dirCluster, String name, boolean dir, int cluster, long size) throws IOException {
        byte[] sn = shortName(name, dirCluster);
        boolean needLfn = !(new String(sn, java.nio.charset.Charset.forName("ISO-8859-1")).trim().length() > 0
                && shortOf2(sn).equals(name));
        int nl = needLfn ? (name.length() + 12) / 13 : 0;
        int need = nl + 1;
        long[] found = findFree(dirCluster, need);
        int k = 0;
        if (needLfn) {
            int sum = lfnChecksum(sn);
            for (int i = nl; i >= 1; i--) {
                byte[] e = new byte[32];
                e[0] = (byte) (i == nl ? 0x40 | i : i);
                e[11] = 0x0F;
                e[13] = (byte) sum;
                int[] offs = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int j = 0; j < 13; j++) {
                    int idx = (i - 1) * 13 + j;
                    int ch = idx < name.length() ? name.charAt(idx) : idx == name.length() ? 0 : 0xFFFF;
                    put16(e, offs[j], ch);
                }
                f.seek(found[k++]);
                f.write(e);
            }
        }
        byte[] e = new byte[32];
        System.arraycopy(sn, 0, e, 0, 11);
        e[11] = (byte) (dir ? 0x10 : 0x20);
        long now = System.currentTimeMillis();
        java.util.Calendar cal = java.util.Calendar.getInstance();
        cal.setTimeInMillis(now);
        int date = ((Math.max(cal.get(java.util.Calendar.YEAR), 1980) - 1980) << 9)
                | ((cal.get(java.util.Calendar.MONTH) + 1) << 5) | cal.get(java.util.Calendar.DAY_OF_MONTH);
        int time = (cal.get(java.util.Calendar.HOUR_OF_DAY) << 11) | (cal.get(java.util.Calendar.MINUTE) << 5)
                | (cal.get(java.util.Calendar.SECOND) / 2);
        put16(e, 14, time); put16(e, 16, date); put16(e, 18, date);
        put16(e, 22, time); put16(e, 24, date);
        put16(e, 20, cluster >>> 16);
        put16(e, 26, cluster & 0xFFFF);
        put32(e, 28, dir ? 0 : size);
        f.seek(found[k]);
        f.write(e);
    }

    private static String shortOf2(byte[] sn) {
        java.nio.charset.Charset l = java.nio.charset.Charset.forName("ISO-8859-1");
        String s = new String(sn, 0, 8, l).trim(), x = new String(sn, 8, 3, l).trim();
        return x.isEmpty() ? s : s + "." + x;
    }

    /** Finds (growing the directory when it can) a run of n consecutive free slots. */
    private long[] findFree(int dirCluster, int n) throws IOException {
        for (;;) {
            List<Long> slots = dirSlots(dirCluster);
            int run = 0;
            for (int i = 0; i < slots.size(); i++) {
                int first = readSlot(slots.get(i))[0] & 0xff;
                if (first == 0 || first == 0xE5) {
                    run++;
                    if (run == n) {
                        long[] r = new long[n];
                        for (int j = 0; j < n; j++) r[j] = slots.get(i - n + 1 + j);
                        return r;
                    }
                } else {
                    run = 0;
                }
            }
            if (dirCluster == 0 && type != 32) throw new IOException("the root directory is full");
            int dc = dirCluster == 0 ? rootCluster : dirCluster;
            List<Integer> ch = chain(dc);
            int nc = allocCluster();
            setFat(ch.get(ch.size() - 1), nc);
        }
    }
}
