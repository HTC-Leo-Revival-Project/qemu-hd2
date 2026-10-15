package org.qemu.htcleo;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The NAND, microSD and radio firmware files the emulator runs on, which live
 * in the app's own images folder.  They get there through the system file
 * picker, so nothing here needs a storage permission and nothing has to
 * survive scoped storage.
 */
public class Images {
    public static class File2 {
        public final String name;
        public final long size;
        public final boolean sdcard;

        File2(String name, long size, boolean sdcard) {
            this.name = name;
            this.size = size;
            this.sdcard = sdcard;
        }
    }

    private final Paths paths;

    public Images(Paths paths) {
        this.paths = paths;
    }

    /** Every *.img in the folder: "sdcard*" is the microSD, the rest is NAND. */
    public List<File2> list() {
        List<File2> out = new ArrayList<File2>();
        File[] files = paths.imagesDir.listFiles();
        if (files == null) return out;
        for (File f : files) {
            if (!f.isFile() || !f.getName().endsWith(".img")) continue;
            out.add(new File2(f.getName(), f.length(), f.getName().startsWith("sdcard")));
        }
        Collections.sort(out, new java.util.Comparator<File2>() {
            @Override
            public int compare(File2 a, File2 b) {
                return a.name.compareTo(b.name);
            }
        });
        return out;
    }

    public boolean has(String name) {
        return name != null && paths.image(name).isFile();
    }

    public boolean amss() {
        return paths.image("amss.elf").isFile();
    }

    public boolean dsp1() {
        return paths.image("dsp1.elf").isFile();
    }

    public void delete(String name) {
        File f = paths.image(name);
        if (f.isFile()) f.delete();
    }

    /** The system file picker, so the app needs no storage permission. */
    public static Intent picker() {
        Intent i = new Intent(Build.VERSION.SDK_INT >= 19
                ? Intent.ACTION_OPEN_DOCUMENT : Intent.ACTION_GET_CONTENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        return i;
    }

    public static String nameOf(Activity act, Uri uri) {
        String name = null;
        if (Build.VERSION.SDK_INT >= 19) {
            try {
                Cursor c = act.getContentResolver().query(uri, null, null, null, null);
                if (c != null && c.moveToFirst()) {
                    int i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (i >= 0) name = c.getString(i);
                }
                if (c != null) c.close();
            } catch (Exception ignored) {
            }
        }
        if (name == null) name = uri.getLastPathSegment();
        if (name == null) name = "image.img";
        return name.replaceAll("[^A-Za-z0-9._-]", "_");
    }

    /**
     * Copies a picked file into a folder of its own.  A NAND image is 512 MiB
     * and a ROM zip nearly as much, so this wants a progress dialog and a
     * thread of its own.
     */
    public static class Import {
        public interface Progress {
            void onProgress(long done, long total);

            void onDone(File out);

            void onError(String why);
        }

        private final Handler main = new Handler(Looper.getMainLooper());

        public void run(final Activity act, final Uri uri, final String name, final File dir,
                        final Progress p) {
            new Thread(new Runnable() {
                @Override
                public void run() {
                    final ContentResolver cr = act.getContentResolver();
                    if (!dir.isDirectory() && !dir.mkdirs()) {
                        post(p, 0, 0, null, "cannot make " + dir, false);
                        return;
                    }
                    File out = new File(dir, name);
                    File tmp = new File(out.getPath() + ".part");
                    long total = 0;
                    InputStream in = null;
                    OutputStream os = null;
                    try {
                        in = cr.openInputStream(uri);
                        if (in == null) throw new IOException("cannot open " + uri);
                        total = in.available();
                        os = new FileOutputStream(tmp);
                        byte[] buf = new byte[1 << 18];
                        long done = 0;
                        int n;
                        long last = 0;
                        while ((n = in.read(buf)) > 0) {
                            os.write(buf, 0, n);
                            done += n;
                            if (done - last > (4 << 20)) {
                                last = done;
                                post(p, done, total, null, null, true);
                            }
                        }
                        os.flush();
                        os.close();
                        os = null;
                        if (out.exists()) out.delete();
                        if (!tmp.renameTo(out)) throw new IOException("cannot rename " + tmp);
                        post(p, done, total, out, null, false);
                    } catch (Exception e) {
                        if (tmp.exists()) tmp.delete();
                        post(p, 0, total, null, String.valueOf(e.getMessage()), false);
                    } finally {
                        try {
                            if (os != null) os.close();
                        } catch (IOException ignored) {
                        }
                        try {
                            if (in != null) in.close();
                        } catch (IOException ignored) {
                        }
                    }
                }
            }, "image-import").start();
        }

        public void run(Activity act, Uri uri, String name, Progress p) {
            run(act, uri, name, new File(act.getFilesDir(), "images"), p);
        }

        private void post(final Progress p, final long done, final long total,
                          final File out, final String err, final boolean more) {
            /* everything the caller does with it touches views, so it has to
             * come back on the main thread */
            main.post(new Runnable() {
                @Override
                public void run() {
                    if (more) p.onProgress(done, total);
                    else if (err != null) p.onError(err);
                    else p.onDone(out);
                }
            });
        }
    }
}
