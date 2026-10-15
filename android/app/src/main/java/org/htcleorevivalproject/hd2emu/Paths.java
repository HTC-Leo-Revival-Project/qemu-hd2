package org.htcleorevivalproject.hd2emu;

import android.content.Context;
import android.content.res.AssetManager;
import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Where everything lives inside the app's private storage:
 *
 *   files/emu/bin     qemu-system-arm, qemu-system-hexagon
 *   files/emu/pc-bios the VNC server's keymaps
 *   files/emu/run     the QMP and VNC sockets, and the log files
 *   files/images      NAND, microSD and radio firmware images
 *
 * QEMU normally runs straight out of nativeLibraryDir, which is what the
 * installer unpacks the jniLibs into.  If that is not there (extractNativeLibs
 * turned off, or an ABI that was never packaged) it is copied out of the APK
 * instead.
 */
public class Paths {
    private static final String TAG = "HtcleoPaths";

    public final File emuDir, binDir, pcBiosDir, runDir, imagesDir;
    private final Context ctx;

    public Paths(Context ctx) {
        this.ctx = ctx;
        File files = ctx.getFilesDir();
        emuDir = new File(files, "emu");
        binDir = new File(emuDir, "bin");
        pcBiosDir = new File(emuDir, "pc-bios");
        runDir = new File(emuDir, "run");
        imagesDir = new File(files, "images");
        for (File d : new File[]{emuDir, binDir, pcBiosDir, runDir, imagesDir}) {
            if (!d.isDirectory() && !d.mkdirs()) throw new RuntimeException("cannot make " + d);
        }
    }

    public String abi() {
        return Build.SUPPORTED_ABIS != null && Build.SUPPORTED_ABIS.length > 0
                ? Build.SUPPORTED_ABIS[0] : Build.CPU_ABI;
    }

    public File qmpSocket() {
        return new File(runDir, "qmp.sock");
    }

    public File vncSocket() {
        return new File(runDir, "vnc.sock");
    }

    public File q6LinkSocket() {
        return new File(runDir, "q6link.sock");
    }

    public File log(String name) {
        return new File(runDir, name + ".log");
    }

    public File dram() {
        return new File(runDir, "dram.bin");
    }

    /** The name the emulator goes by inside the APK. */
    private static String packaged(String name) {
        return "qemu-system-hexagon".equals(name) ? "libhtcleo-q6.so" : "libhtcleo-qemu.so";
    }

    /**
     * The QEMU binary, or throws if it is not in this build.  It normally runs
     * straight out of nativeLibraryDir, which is where the installer puts the
     * jniLibs; if that is missing (extractNativeLibs off, or an ABI that was
     * never packaged) it is unpacked from the APK instead.
     */
    public File qemu(String name) throws IOException {
        File inNative = new File(ctx.getApplicationInfo().nativeLibraryDir, packaged(name));
        if (inNative.isFile() && inNative.canExecute()) return inNative;

        File out = new File(binDir, name);
        if (out.isFile() && out.canExecute()) return out;

        String entry = "lib/" + abi() + "/" + packaged(name);
        ZipFile zip = new ZipFile(ctx.getApplicationInfo().sourceDir);
        try {
            ZipEntry e = zip.getEntry(entry);
            if (e == null) {
                throw new IOException(name + " is not in this build (looked for " + entry + ")");
            }
            copy(zip.getInputStream(e), out);
        } finally {
            try {
                zip.close();
            } catch (IOException ignored) {
            }
        }
        out.setExecutable(true, false);
        return out;
    }

    public boolean hasQemu() {
        try {
            return qemu("qemu-system-arm").canExecute();
        } catch (IOException e) {
            return false;
        }
    }

    /** VNC refuses to start without its keymaps, so make sure they are there. */
    public boolean ensurePcBios() {
        File keymaps = new File(pcBiosDir, "keymaps");
        if (keymaps.isDirectory()) return true;
        try {
            AssetManager assets = ctx.getAssets();
            String[] langs = assets.list("pc-bios/keymaps");
            Log.i(TAG, "keymaps in the APK: " + (langs == null ? "none" : langs.length));
            if (langs == null || langs.length == 0) return false;
            for (String lang : langs) {
                copyTree(assets, "pc-bios/keymaps/" + lang, new File(keymaps, lang));
            }
            return keymaps.isDirectory();
        } catch (IOException e) {
            Log.w(TAG, "cannot unpack the keymaps: " + e);
            return false;
        }
    }

    private void copyTree(AssetManager assets, String asset, File out) throws IOException {
        String[] names = assets.list(asset);
        if (names == null || names.length == 0) {
            copy(assets.open(asset), out);
            return;
        }
        if (!out.isDirectory() && !out.mkdirs()) throw new IOException("mkdir " + out);
        for (String name : names) copyTree(assets, asset + "/" + name, new File(out, name));
    }

    private static void copy(InputStream in, File out) throws IOException {
        File parent = out.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("mkdir " + parent);
        }
        File tmp = new File(out.getPath() + ".part");
        OutputStream o = new FileOutputStream(tmp);
        try {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
            o.flush();
        } finally {
            try {
                o.close();
            } catch (IOException ignored) {
            }
            try {
                in.close();
            } catch (IOException ignored) {
            }
        }
        if (out.exists() && !out.delete()) throw new IOException("replace " + out);
        if (!tmp.renameTo(out)) throw new IOException("rename " + tmp);
    }

    public File image(String name) {
        return new File(imagesDir, name);
    }

    public File freeSpaceCheck() {
        return imagesDir;
    }
}
