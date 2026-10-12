package com.hd2bench;

/** The native tests (jni/bench.c). */
final class Native {
    static boolean loaded;
    static String loadError = "";

    static {
        try {
            System.loadLibrary("bench");
            loaded = true;
        } catch (Throwable t) {
            loadError = t.toString();
        }
    }

    private Native() {
    }

    /* CPU: each runs about secs seconds and returns a rate */
    static native double cpuCrc32(double secs);      /* MB/s */
    static native double cpuIntMix(double secs);     /* Mops/s */
    static native double cpuDgemm(double secs);      /* MFLOPS */
    static native double cpuSaxpy(double secs);      /* MFLOPS */
    static native double memCopy(double secs);       /* MB/s */
    static native double memLatency(double secs);    /* ns per load */

    /*
     * Storage in dir, with a file of mb MiB: sequential write and read
     * (MB/s), random 4K read and synced 4K write (IOPS), free MB; null on
     * error (lastError()).
     */
    static native double[] storage(String dir, int mb);
    static native String lastError();
}
