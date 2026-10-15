package org.htcleorevivalproject.hd2emu;

import java.nio.charset.Charset;


import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Builds a NAND image inside the app, in one of three kinds:
 *
 * ANDROID, the Java twin of tools/hd2_mknand.py: cLK (LEOIMG.nbh or a raw
 * lk.bin), a NAND Android ROM zip and optionally a recovery image, laid out
 * the way HSPL, the cLK partition table and a recovery install would: the OS
 * area at block 0x212, then a VPTABLE (or cLK 1.4's compiled table), then
 * boot.img, then yaffs2 /system and /userdata.
 *
 * MAGLDR, tools/hd2_mkmagldr.py: magldr.nbh's OS section in the OS area and
 * the factory MACs - plus, if given, a radio NBH's partitions the way
 * tools/hd2_radio.py --write-nand adds them afterwards.
 *
 * WINMO, tools/hd2_wmnand.py: a stock Windows Mobile 6.5 RUU - its radio
 * (or another radio NBH's) in the first blocks, its OS section page by page,
 * WinCE sector info and all, from block 0x212.
 */
public class NandBuilder {

/** ASCII-flavoured: byte values have to survive the round trip. */
static final Charset LATIN1 = Charset.forName("ISO-8859-1");
static final Charset UTF8 = Charset.forName("UTF-8");

    public interface Progress {
        void onStep(String what);

        void onDone(File image);

        void onError(String why);
    }

    public static final int ANDROID = 0;
    public static final int MAGLDR = 1;
    public static final int WINMO = 2;

    public static class Options {
        public int kind = ANDROID;
        public File magldr;                   // MAGLDR: magldr.nbh
        public File ruu;                      // WINMO: the stock RUU_signed.nbh
        public File radio;                    // MAGLDR, WINMO: a radio RUU_signed.nbh, optional
        public File nbh;                      // cLK LEOIMG.nbh
        public File lk;                       // or a raw lk.bin
        public File rom;                      // a NAND ROM zip
        public File recovery;                 // optional
        public int systemMb = 220;
        public int cacheMb = 30;
        public boolean softkeys = true;
        public boolean gpu = false;
        public String wifiMac = "00:23:76:48:44:32";
        public String btMac = "00:23:76:48:44:33";
        public File output;
    }

    private final Options o;
    private final Progress p;

    public NandBuilder(Options o, Progress p) {
        this.o = o;
        this.p = p;
    }

    public void run() {
        try {
            build();
        } catch (Throwable e) {
            p.onError(e.getMessage() == null ? e.toString() : e.getMessage());
        }
    }

    private void step(String what) {
        p.onStep(what);
    }

    private void build() throws Exception {
        if (o.kind == MAGLDR) {
            buildMagldr();
            return;
        }
        if (o.kind == WINMO) {
            buildWinMo();
            return;
        }
        Nand.Os os;
        List<String[]> clk14 = null;

        if (o.nbh != null) {
            step("Reading the bootloader");
            os = Nand.extractNbhOs(Nand.read(o.nbh));
        } else if (o.lk != null) {
            step("Reading the bootloader");
            byte[] lk = Nand.read(o.lk);
            List<String[]> parts = Nand.clk14Ptable(lk, null);
            if (parts != null && !parts.isEmpty()) {
                /* cLK 1.4: a fixed table compiled into the image; resize it */
                step("cLK 1.4 partition table");
                for (String[] part : parts) {
                    int want = "boot".equals(part[0]) ? 5 : "system".equals(part[0]) ? o.systemMb
                            : "cache".equals(part[0]) ? o.cacheMb : -1;
                    if (want >= 0 && Integer.parseInt(part[2]) != want) {
                        java.nio.ByteBuffer.wrap(lk, Integer.parseInt(part[3]), 4)
                                .order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(want);
                        part[2] = String.valueOf(want);
                    }
                }
                clk14 = Nand.clk14Layout(parts);
            }
            os = Nand.osSectorsFromLk(lk);
        } else {
            throw new IOException("need a bootloader (LEOIMG.nbh) or a raw lk.bin");
        }
        if (o.rom == null) throw new IOException("need a NAND ROM zip");

        step("Erasing the NAND");
        Nand nand = new Nand(o.output);
        try {
            nand.writeFactoryMacs(Nand.mac(o.wifiMac), Nand.mac(o.btMac));

            step("Writing the OS area");
            for (int i = 0; i < os.sectors.size(); i++) {
                nand.writePage((long) Nand.ROM_BLOCK * Nand.PPB + i, os.sectors.get(i));
            }

            java.util.Map<String, int[]> layout = new java.util.LinkedHashMap<String, int[]>();
            if (clk14 != null) {
                step("cLK 1.4 partitions");
                for (String[] part : clk14) {
                    layout.put(part[0], new int[]{Integer.parseInt(part[1]), Integer.parseInt(part[2])});
                }
            } else {
                int startBlk = Nand.ROM_BLOCK + (os.start + os.count) / Nand.PPB;
                int vptBlk = startBlk;
                int partBlk = startBlk + 1;
                int total = Nand.NBLOCKS - partBlk;
                step("cLK 1.5 partition table");
                int bpm = (1024 * 1024) / Nand.BLK;                 // 8 blocks per MiB
                String[] fixed = {"misc", "recovery", "boot", "system", "cache"};
                int[] mb = {1, 5, 5, o.systemMb, o.cacheMb};
                List<String[]> parts = new ArrayList<String[]>();
                int used = 0;
                for (int i = 0; i < fixed.length; i++) {
                    parts.add(new String[]{fixed[i], String.valueOf(mb[i] * bpm), "0"});
                    used += mb[i] * bpm;
                }
                parts.add(new String[]{"userdata", String.valueOf(total - used), "1"});
                nand.writePage((long) vptBlk * Nand.PPB, Nand.vptable(parts, true));
                int blk = partBlk;
                for (String[] part : parts) {
                    layout.put(part[0], new int[]{blk, Integer.parseInt(part[1])});
                    blk += Integer.parseInt(part[1]);
                }
            }

            step("Reading the ROM");
            ZipFile z = new ZipFile(o.rom);
            try {
                step("Building boot.img");
                byte[] boot = bootImage(z);
                int[] b = layout.get("boot");
                if (b == null) throw new IOException("this ROM has no boot partition");
                if (boot.length > b[1] * Nand.BLK) throw new IOException("boot.img is too large");
                for (int i = 0; i < boot.length; i += Nand.PAGE) {
                    byte[] p = Nand.fill(Nand.PAGE, (byte) 0xff);
                    System.arraycopy(boot, i, p, 0, Math.min(Nand.PAGE, boot.length - i));
                    nand.writePage((long) b[0] * Nand.PPB + i / Nand.PAGE, p);
                }

                if (o.recovery != null) {
                    step("Writing recovery");
                    int[] r = layout.get("recovery");
                    byte[] rec = Nand.read(o.recovery);
                    if (r != null && rec.length <= r[1] * Nand.BLK) {
                        for (int i = 0; i < rec.length; i += Nand.PAGE) {
                            byte[] p = Nand.fill(Nand.PAGE, (byte) 0xff);
                            System.arraycopy(rec, i, p, 0, Math.min(Nand.PAGE, rec.length - i));
                            nand.writePage((long) r[0] * Nand.PPB + i / Nand.PAGE, p);
                        }
                    }
                }

                step("Installing the ROM into /system");
                Yaffs2.Tree system = new Yaffs2.Tree();
                Yaffs2.Tree data = new Yaffs2.Tree();
                installRom(z, system, data);

                writeYaffs(nand, layout, "system", system);
                writeYaffs(nand, layout, "userdata", data);
            } finally {
                z.close();
            }
        } finally {
            nand.close();
        }
        p.onDone(o.output);
    }

    /* -------------------------------------------------------------- MAGLDR */

    /** hd2_mkmagldr.py, then (with a radio) hd2_radio.py --write-nand. */
    private void buildMagldr() throws Exception {
        if (o.magldr == null) throw new IOException("need magldr.nbh");
        step("Reading MAGLDR");
        Nand.Os os = Nand.extractNbhOs(Nand.read(o.magldr));
        byte[] radio = null;
        if (o.radio != null) {
            step("Reading the radio");
            Radio.Nbh nbh = new Radio.Nbh(o.radio);
            try {
                radio = nbh.payload();
            } finally {
                nbh.close();
            }
        }

        step("Erasing the NAND");
        Nand nand = new Nand(o.output);
        try {
            nand.writeFactoryMacs(Nand.mac(o.wifiMac), Nand.mac(o.btMac));
            step("Writing the OS area");
            for (int i = 0; i < os.sectors.size(); i++) {
                nand.writePage((long) Nand.ROM_BLOCK * Nand.PPB + i, os.sectors.get(i));
            }
            step(os.sectors.size() + " OS-area pages from " + o.magldr.getName());
            if (radio != null) {
                step("Writing the radio partitions");
                Radio.install(nand, radio, log());
            }
        } finally {
            nand.close();
        }
        p.onDone(o.output);
    }

    /* ------------------------------------------------------- Windows Mobile */

    /** Each RUU page in the file: 2048 data bytes and 8 of WinCE SectorInfo. */
    private static final int NBH_PAGE = Nand.PAGE + 8;

    /** hd2_wmnand.py: the radio partitions, then the OS section from block 0x212. */
    private void buildWinMo() throws Exception {
        if (o.ruu == null) throw new IOException("need the stock ROM's RUU_signed.nbh");
        step("Reading the ROM");
        Radio.Nbh ruu = new Radio.Nbh(o.ruu);
        try {
            long[] osSec = ruu.sections().get(0x400);
            if (osSec == null || osSec[1] == 0 || osSec[1] % NBH_PAGE != 0) {
                throw new IOException(o.ruu.getName() + ": no OS section");
            }

            step("Reading the radio");
            byte[] radio;
            File radioFile = o.radio != null ? o.radio : o.ruu;
            if (o.radio != null) {
                Radio.Nbh r = new Radio.Nbh(o.radio);
                try {
                    radio = r.section(0x301);
                } finally {
                    r.close();
                }
            } else {
                radio = ruu.section(0x301);
            }
            if (radio == null || radio.length == 0) {
                throw new IOException(radioFile.getName() + ": no radio section");
            }

            step("Erasing the NAND");
            Nand nand = new Nand(o.output);
            try {
                step("Writing the radio partitions");
                Radio.install(nand, radio, log());

                long n = osSec[1] / NBH_PAGE;
                if ((long) Nand.ROM_BLOCK * Nand.PPB + n > (long) Nand.NBLOCKS * Nand.PPB) {
                    throw new IOException("the OS image does not fit");
                }
                step("Writing the OS area");
                final int batch = 256;                       /* pages per read */
                for (long i = 0; i < n; i += batch) {
                    int count = (int) Math.min(batch, n - i);
                    byte[] buf = ruu.read(osSec[0] + i * NBH_PAGE, count * NBH_PAGE);
                    if (buf.length < count * NBH_PAGE) throw new IOException(o.ruu.getName() + " is cut short");
                    for (int k = 0; k < count; k++) {
                        int at = k * NBH_PAGE;
                        if (erased(buf, at, NBH_PAGE)) continue;
                        byte[] data = java.util.Arrays.copyOfRange(buf, at, at + Nand.PAGE);
                        byte[] info = java.util.Arrays.copyOfRange(buf, at + Nand.PAGE, at + NBH_PAGE);
                        nand.writePage((long) Nand.ROM_BLOCK * Nand.PPB + i + k, data, info);
                    }
                }
                step(String.format(java.util.Locale.ROOT, "OS        block %d  %d pages", Nand.ROM_BLOCK, n));
            } finally {
                nand.close();
            }
        } finally {
            ruu.close();
        }
        p.onDone(o.output);
    }

    private static boolean erased(byte[] b, int at, int n) {
        for (int i = at; i < at + n; i++) if (b[i] != (byte) 0xff) return false;
        return true;
    }

    private Radio.Log log() {
        return new Radio.Log() {
            @Override
            public void line(String text) {
                step(text);
            }
        };
    }

    private void writeYaffs(Nand nand, java.util.Map<String, int[]> layout, String name,
                            Yaffs2.Tree tree) throws IOException {
        int[] where = layout.get("system".equals(name) ? "system" : "userdata");
        if (where == null) return;
        step("Writing " + name + " (yaffs2)");
        Yaffs2.Writer w = new Yaffs2.Writer(nand, where[0], where[1]);
        w.writeTree(tree);
    }

    /* ------------------------------------------------------------- boot.img */

    private static byte[] pad(byte[] b, int pagesize) {
        int n = (pagesize - b.length % pagesize) % pagesize;
        byte[] out = new byte[b.length + n];
        System.arraycopy(b, 0, out, 0, b.length);
        return out;
    }

    private byte[] bootImage(ZipFile z) throws Exception {
        byte[] kernel = read(z, "kernel/zImage");
        byte[] ramdisk = read(z, "kernel/boot_NAND/initrd.gz");
        String cmdline = "no_console_suspend=1 console=null";
        String script = readText(z, "kernel/mkbootimg.sh");
        Matcher m = Pattern.compile("--cmdline '([^']*)'").matcher(script);
        if (m.find()) cmdline = m.group(1);

        final int base = 0x11800000;
        final int pagesize = Nand.PAGE;

        MessageDigest sha = MessageDigest.getInstance("SHA-1");
        sha.update(kernel);
        sha.update(intLe(kernel.length));
        sha.update(ramdisk);
        sha.update(intLe(ramdisk.length));
        sha.update(new byte[0]);
        sha.update(intLe(0));
        byte[] digest = sha.digest();

        java.nio.ByteBuffer hdr = java.nio.ByteBuffer.allocate(608).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        hdr.put("ANDROID!".getBytes(LATIN1), 0, 8);
        hdr.putInt(8, kernel.length);
        hdr.putInt(12, base + 0x8000);
        hdr.putInt(16, ramdisk.length);
        hdr.putInt(20, base + 0x1000000);
        hdr.putInt(24, 0);
        hdr.putInt(28, base + 0xF00000);
        hdr.putInt(32, base + 0x100);
        hdr.putInt(36, pagesize);
        hdr.putInt(40, 0);
        hdr.putInt(44, 0);
        byte[] cmd = cmdline.getBytes(LATIN1);
        System.arraycopy(cmd, 0, hdr.array(), 48, Math.min(cmd.length, 512));
        System.arraycopy(digest, 0, hdr.array(), 560, 20);

        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(pad(hdr.array(), pagesize));
        out.write(pad(kernel, pagesize));
        out.write(pad(ramdisk, pagesize));
        return out.toByteArray();
    }

    private static byte[] intLe(int v) {
        return new byte[]{(byte) v, (byte) (v >> 8), (byte) (v >> 16), (byte) (v >> 24)};
    }

    /* ------------------------------------------------------------ ROM install */

    private void installRom(ZipFile z, Yaffs2.Tree system, Yaffs2.Tree data) throws Exception {
        step("Copying /system");
        zipTree(z, "system/", system, "");
        step("Copying /data");
        zipTree(z, "data/", data, "");
        step("Applying the cLK PPP patch");
        zipTree(z, "mods/clk-ppp-patch/", system, "");

        String script = readText(z, "META-INF/com/google/android/updater-script");
        int tail = script.indexOf("run_program(\"/tmp/check_data_apps.sh\")");
        String tailScript = tail < 0 ? "" : script.substring(tail);

        for (String[] call : edify(tailScript, "symlink", "set_perm_recursive",
                "set_perm", "delete")) {
            String fn = call[0];
            List<String> args = new ArrayList<String>();
            for (int i = 1; i < call.length; i++) args.add(call[i]);
            if ("symlink".equals(fn) && args.size() >= 2) {
                String target = args.get(0);
                for (int i = 1; i < args.size(); i++) {
                    String p = args.get(i);
                    if (p.startsWith("/system/")) system.symlink(target, p.substring(7));
                }
            } else if ("set_perm".equals(fn) && args.size() >= 4) {
                Yaffs2.Tree tree = args.get(3).startsWith("/system") ? system
                        : args.get(3).startsWith("/data") ? data : null;
                String rel = args.get(3).startsWith("/system") ? args.get(3).substring(7)
                        : args.get(3).startsWith("/data") ? args.get(3).substring(5) : null;
                if (tree != null) {
                    Yaffs2.Node n = rel == null || rel.isEmpty() ? tree.root : tree.lookup(rel);
                    if (n != null) {
                        Yaffs2.setPerm(n, Integer.parseInt(args.get(0)),
                                Integer.parseInt(args.get(1)), Integer.parseInt(args.get(2), 8));
                    }
                }
            } else if ("set_perm_recursive".equals(fn) && args.size() >= 5) {
                String path = args.get(4);
                int uid = Integer.parseInt(args.get(0)), gid = Integer.parseInt(args.get(1));
                int dm = Integer.parseInt(args.get(2), 8), fm = Integer.parseInt(args.get(3), 8);
                if (path.startsWith("/system")) {
                    Yaffs2.setPermRecursive(system, uid, gid, dm, fm,
                            path.length() > 7 ? path.substring(7) : "/");
                } else if (path.startsWith("/data")) {
                    Yaffs2.setPermRecursive(data, uid, gid, dm, fm,
                            path.length() > 5 ? path.substring(5) : "/");
                }
            }
        }

        /*
         * cLK cannot use RMNET, so the installer applies the PPP patch, and
         * without --gpu libhwui has to be told this is not a GPU emulator -
         * qemu.hw.mainkeys=0 gives the phone its soft buttons.
         */
        step("Patching build.prop");
        StringBuilder props = new StringBuilder("\n# emulator\n");
        if (!o.gpu) props.append("ro.kernel.qemu=0\nro.kernel.qemu.gles=0\ndebug.egl.hw=0\n");
        if (o.softkeys) props.append("qemu.hw.mainkeys=0\n");
        Yaffs2.Node bp = system.lookup("build.prop");
        if (bp != null) {
            String text = new String(bp.data, LATIN1).replaceAll("\n+$", "")
                    + props;
            bp.data = text.getBytes(LATIN1);
        }
    }

    private static void zipTree(ZipFile z, String prefix, Yaffs2.Tree tree, String dest)
            throws IOException {
        java.util.Enumeration<? extends ZipEntry> en = z.entries();
        while (en.hasMoreElements()) {
            ZipEntry info = en.nextElement();
            if (!info.getName().startsWith(prefix)) continue;
            String rel = info.getName().substring(prefix.length());
            if (rel.isEmpty()) continue;
            String path = dest + "/" + rel;
            int mtime = (int) (info.getTime() / 1000);
            if (info.isDirectory()) {
                tree.addDir(path, 0755, mtime);
            } else {
                tree.addFile(path, read(z, info.getName()), 0644, mtime);
            }
        }
    }

    /**
     * The calls to the given edify functions in a script, in order: the name
     * and then its arguments, strings unquoted, numbers as written (so octal
     * permissions keep working).
     */
    static List<String[]> edify(String script, String... names) {
        Pattern p = Pattern.compile("\\b(" + String.join("|", names) + ")\\s*\\(");
        Matcher m = p.matcher(script);
        List<String[]> out = new ArrayList<String[]>();
        int pos = 0;
        while (m.find(pos)) {
            int depth = 1;
            int i = m.end();
            while (depth > 0 && i < script.length()) {
                char c = script.charAt(i);
                if (c == '(') depth++;
                else if (c == ')') depth--;
                else if (c == '"') {
                    int end = script.indexOf('"', i + 1);
                    if (end < 0) break;
                    i = end;
                }
                i++;
            }
            if (depth != 0) break;
            String body = script.substring(m.end(), i - 1);
            Matcher a = Pattern.compile("\"([^\"]*)\"|(\\b0?[0-7]+\\b|\\b\\d+\\b)")
                    .matcher(body);
            List<String> args = new ArrayList<String>();
            args.add(m.group(1));
            while (a.find()) args.add(a.group(1) != null ? a.group(1) : a.group(2));
            out.add(args.toArray(new String[0]));
            pos = i;
        }
        return out;
    }

    /* ----------------------------------------------------------------- zip io */

    static byte[] read(ZipFile z, String name) throws IOException {
        ZipEntry e = z.getEntry(name);
        if (e == null) throw new IOException("the ROM has no " + name);
        InputStream in = z.getInputStream(e);
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toByteArray();
        } finally {
            in.close();
        }
    }

    private static String readText(ZipFile z, String name) throws IOException {
        ZipEntry e = z.getEntry(name);
        if (e == null) return "";
        return new String(read(z, name), UTF8);
    }
}
