package com.hd2bench;

import android.content.Context;
import android.os.Build;
import android.os.Environment;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

/** Device information, CPU and storage tests (run off the UI thread). */
final class Bench {
    interface Progress {
        void step(String what, int done, int total);
    }

    private static final double SECS = 1.5;
    private static volatile int javaSink;

    private Bench() {
    }

    static String readLine(String path, String prefix) {
        BufferedReader r = null;

        try {
            r = new BufferedReader(new FileReader(path));
            for (String l; (l = r.readLine()) != null; ) {
                if (prefix == null) {
                    return l.trim();
                }
                if (l.startsWith(prefix)) {
                    int c = l.indexOf(':');

                    return (c >= 0 ? l.substring(c + 1) : l).trim();
                }
            }
        } catch (Exception e) {
            /* not there */
        } finally {
            try {
                if (r != null) {
                    r.close();
                }
            } catch (Exception e) {
                /* ignore */
            }
        }
        return null;
    }

    /** The file system a path is on, from /proc/mounts (longest match). */
    static String fsType(String path) {
        String best = "?";
        int bestLen = -1;
        BufferedReader r = null;

        try {
            r = new BufferedReader(new FileReader("/proc/mounts"));
            for (String l; (l = r.readLine()) != null; ) {
                String[] f = l.split(" ");

                if (f.length > 2 && path.startsWith(f[1])
                        && f[1].length() > bestLen) {
                    best = f[2] + " on " + f[0];
                    bestLen = f[1].length();
                }
            }
        } catch (Exception e) {
            /* unknown */
        } finally {
            try {
                if (r != null) {
                    r.close();
                }
            } catch (Exception e) {
                /* ignore */
            }
        }
        return best;
    }

    static void deviceInfo() {
        String cpu = readLine("/proc/cpuinfo", "Processor");
        String hw = readLine("/proc/cpuinfo", "Hardware");
        String bogo = readLine("/proc/cpuinfo", "BogoMIPS");
        String fmax = readLine(
                "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", null);
        String mem = readLine("/proc/meminfo", "MemTotal");

        Results.clear(Results.INFO);
        Results.putText(Results.INFO, "Model",
                        Build.MANUFACTURER + " " + Build.MODEL, "", true);
        Results.putText(Results.INFO, "Android",
                        Build.VERSION.RELEASE + " (" + Build.DISPLAY + ")", "",
                        true);
        Results.putText(Results.INFO, "Kernel",
                        System.getProperty("os.version"), "", true);
        Results.putText(Results.INFO, "CPU",
                        (cpu != null ? cpu : Build.CPU_ABI)
                        + (hw != null ? ", " + hw : ""), "", true);
        if (fmax != null) {
            try {
                Results.put(Results.INFO, "CPU max clock",
                            Integer.parseInt(fmax) / 1000.0, "MHz", true);
            } catch (NumberFormatException e) {
                /* ignore */
            }
        }
        if (bogo != null) {
            Results.putText(Results.INFO, "BogoMIPS", bogo, "", true);
        }
        if (mem != null) {
            Results.putText(Results.INFO, "RAM", mem, "", true);
        }
    }

    /* Dalvik: sieve of Eratosthenes up to 1M, runs per second */
    private static double javaSieve(double secs) {
        boolean[] comp = new boolean[1000000];
        long t0 = System.nanoTime();
        int runs = 0;

        do {
            int count = 0;

            java.util.Arrays.fill(comp, false);
            for (int i = 2; i < comp.length; i++) {
                if (!comp[i]) {
                    count++;
                    for (int j = i * 2; j < comp.length; j += i) {
                        comp[j] = true;
                    }
                }
            }
            javaSink = count;
            runs++;
        } while (System.nanoTime() - t0 < secs * 1e9);
        return runs / ((System.nanoTime() - t0) / 1e9);
    }

    /* Dalvik: double multiply-adds, MFLOPS */
    private static double javaFloat(double secs) {
        long t0 = System.nanoTime();
        double a = 1.0, b = 0.999999, c = 0.5, flops = 0;

        do {
            for (int i = 0; i < 100000; i++) {
                a = a * b + c;
                c = c * b - a * 1e-9;
            }
            flops += 100000 * 4;
        } while (System.nanoTime() - t0 < secs * 1e9);
        javaSink = (int) (a + c);
        return flops / 1e6 / ((System.nanoTime() - t0) / 1e9);
    }

    static void cpu(Progress p) {
        int n = 0, total = 8;

        Results.clear(Results.CPU);
        p.step("Java integer (sieve)", n++, total);
        Results.put(Results.CPU, "Java integer: sieve to 1M", javaSieve(SECS),
                    "runs/s", true);
        p.step("Java floating point", n++, total);
        Results.put(Results.CPU, "Java float64 multiply-add",
                    javaFloat(SECS), "MFLOPS", true);
        if (!Native.loaded) {
            Results.putText(Results.CPU, "Native tests", "unavailable", "",
                            true);
            return;
        }
        p.step("Native integer (CRC32)", n++, total);
        Results.put(Results.CPU, "Native integer: CRC32",
                    Native.cpuCrc32(SECS), "MB/s", true);
        p.step("Native integer mix", n++, total);
        Results.put(Results.CPU, "Native integer: mul/div/shift mix",
                    Native.cpuIntMix(SECS), "Mops/s", true);
        p.step("Native float64 matrix multiply", n++, total);
        Results.put(Results.CPU, "Native float64: 64x64 matrix multiply",
                    Native.cpuDgemm(SECS), "MFLOPS", true);
        p.step("Native float32 SAXPY (NEON)", n++, total);
        Results.put(Results.CPU, "Native float32: SAXPY (NEON)",
                    Native.cpuSaxpy(SECS), "MFLOPS", true);
        p.step("Memory bandwidth", n++, total);
        Results.put(Results.CPU, "Memory: memcpy 8 MB",
                    Native.memCopy(SECS), "MB/s", true);
        p.step("Memory latency", n++, total);
        Results.put(Results.CPU, "Memory: random access latency",
                    Native.memLatency(SECS), "ns", false);
    }

    private static void storageAt(String section, File dir, int mb,
                                  Progress p, int base, int total) {
        double[] r;

        Results.clear(section);
        Results.putText(section, "File system", fsType(dir.getAbsolutePath()),
                        "", true);
        p.step(section + ": writing and reading " + mb + " MB", base, total);
        r = Native.storage(dir.getAbsolutePath(), mb);
        if (r == null) {
            Results.putText(section, "Error", Native.lastError(), "", true);
            return;
        }
        Results.put(section, "Sequential write (" + mb + " MB, synced)", r[0],
                    "MB/s", true);
        Results.put(section, "Sequential read (" + mb + " MB, uncached)", r[1],
                    "MB/s", true);
        Results.put(section, "Random 4K read", r[2], "IOPS", true);
        Results.put(section, "Random 4K write (each synced)", r[3], "IOPS",
                    true);
        Results.put(section, "Free space", r[4], "MB", true);
    }

    static void storage(Context c, Progress p) {
        if (!Native.loaded) {
            Results.clear(Results.NAND);
            Results.putText(Results.NAND, "Error",
                            "native library: " + Native.loadError, "", true);
            return;
        }
        storageAt(Results.NAND, c.getFilesDir(), 16, p, 0, 2);
        if (Environment.MEDIA_MOUNTED.equals(
                Environment.getExternalStorageState())) {
            File d = new File(Environment.getExternalStorageDirectory(),
                              "hd2bench");

            d.mkdirs();
            storageAt(Results.SD, d, 16, p, 1, 2);
        } else {
            Results.clear(Results.SD);
            Results.putText(Results.SD, "Status",
                            "not mounted (" + Environment
                                    .getExternalStorageState() + ")", "", true);
        }
    }
}
