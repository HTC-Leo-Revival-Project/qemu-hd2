package org.htcleorevivalproject.hd2emu;

import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.Charset;

/**
 * Root, for the features an app cannot have without it - the first of them
 * planned is handing the emulated HD2's USB to this phone's own USB port, so a
 * PC's flash tools see a real HD2 (see {@link UsbBridge}).
 *
 * Root here is one long-lived `su` shell, opened the first time it is asked
 * for: the root manager (Magisk, KernelSU, APatch...) asks the user once, and
 * every command after that goes down the same shell instead of a new su each
 * time.  Commands are framed with a marker line carrying their exit status,
 * so stdout can be read back command by command.
 *
 * Nothing here runs on the main thread: every call blocks.
 */
public final class Root {
    private static final String TAG = "HtcleoRoot";
    private static final Charset UTF8 = Charset.forName("UTF-8");

    public enum Status {
        /** not asked yet */
        UNKNOWN,
        /** no su on this phone */
        ABSENT,
        /** su is there, but said no (or the user did) */
        DENIED,
        GRANTED
    }

    /** What a command printed, and how it exited. */
    public static final class Result {
        public final int code;
        public final String out;

        Result(int code, String out) {
            this.code = code;
            this.out = out;
        }

        public boolean ok() {
            return code == 0;
        }

        @Override
        public String toString() {
            return out.trim();
        }
    }

    private static final String[] SU_PATHS = {
            "/system/bin/su", "/system/xbin/su", "/sbin/su", "/su/bin/su", "/debug_ramdisk/su",
            "/data/adb/ksu/bin/su", "/data/adb/ap/bin/su", "/system/sbin/su", "/vendor/bin/su",
    };

    private static Shell shell;
    private static volatile Status status = Status.UNKNOWN;
    private static volatile String manager = "";

    private Root() {
    }

    public static Status status() {
        return status;
    }

    /** The root manager and its version, as `su -v` puts it (e.g. "27.0:MAGISK"), once granted. */
    public static String manager() {
        return manager;
    }

    /** Whether there is an su binary at all - a guess, managers can hide it; no prompt. */
    public static boolean present() {
        for (String p : SU_PATHS) if (new File(p).exists()) return true;
        String path = System.getenv("PATH");
        if (path != null) {
            for (String dir : path.split(":")) if (new File(dir, "su").exists()) return true;
        }
        return false;
    }

    /**
     * Asks for root (the manager shows its prompt the first time) and keeps the
     * shell.  Blocks until the user has answered, or for up to a minute.
     */
    public static synchronized Status request() {
        if (shell != null && shell.alive()) return status = Status.GRANTED;
        shell = null;
        Shell s;
        try {
            s = new Shell();
        } catch (IOException e) {
            Log.i(TAG, "no su: " + e.getMessage());
            return status = present() ? Status.DENIED : Status.ABSENT;
        }
        try {
            Result id = s.run("id -u", 60000);
            if (!id.ok() || !"0".equals(id.out.trim())) {
                s.close();
                return status = Status.DENIED;
            }
        } catch (IOException e) {
            s.close();
            return status = Status.DENIED;
        }
        shell = s;
        try {
            manager = s.run("su -v 2>/dev/null || true", 5000).out.trim();
        } catch (IOException ignored) {
            manager = "";
        }
        return status = Status.GRANTED;
    }

    /** A command as root: asks for root first if it has to. */
    public static Result run(String command) throws IOException {
        return run(command, 15000);
    }

    public static synchronized Result run(String command, long timeoutMs) throws IOException {
        if (shell == null || !shell.alive()) {
            if (request() != Status.GRANTED) throw new IOException("no root access");
        }
        try {
            return shell.run(command, timeoutMs);
        } catch (IOException e) {
            /* a shell that timed out is out of step with its output: start over */
            shell.close();
            shell = null;
            throw e;
        }
    }

    /** Quotes one argument for the shell. */
    public static String quote(String s) {
        return "'" + s.replace("'", "'\\''") + "'";
    }

    /** One `su` process; commands go in on stdin, framed by a marker. */
    static final class Shell {
        private final Process p;
        private final OutputStream in;
        private final BufferedReader out;
        private int serial;

        Shell() throws IOException {
            this("su");
        }

        /** Any shell that reads commands on stdin; su in the app, sh to test the framing. */
        Shell(String... command) throws IOException {
            ProcessBuilder pb = new ProcessBuilder(command);
            pb.redirectErrorStream(true);
            p = pb.start();
            in = p.getOutputStream();
            out = new BufferedReader(new InputStreamReader(p.getInputStream(), UTF8));
        }

        boolean alive() {
            try {
                p.exitValue();
                return false;
            } catch (IllegalThreadStateException running) {
                return true;
            }
        }

        Result run(String command, long timeoutMs) throws IOException {
            final String marker = "__htcleo_" + (++serial) + "_" + System.nanoTime() + "__";
            /* stderr is folded into stdout; the marker line carries the status */
            String framed = "{ " + command + "\n} 2>&1 </dev/null; echo \"" + marker + " $?\"\n";
            in.write(framed.getBytes(UTF8));
            in.flush();

            final StringBuilder text = new StringBuilder();
            final int[] code = {-1};
            final IOException[] failed = new IOException[1];
            Thread reader = new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        String line;
                        while ((line = out.readLine()) != null) {
                            int at = line.indexOf(marker);
                            if (at >= 0) {
                                text.append(line, 0, at);
                                try {
                                    code[0] = Integer.parseInt(line.substring(at + marker.length()).trim());
                                } catch (NumberFormatException e) {
                                    code[0] = -1;
                                }
                                return;
                            }
                            text.append(line).append('\n');
                        }
                        failed[0] = new IOException("the root shell went away");
                    } catch (IOException e) {
                        failed[0] = e;
                    }
                }
            }, "root-read");
            reader.setDaemon(true);
            reader.start();
            try {
                reader.join(timeoutMs);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new IOException("interrupted");
            }
            if (reader.isAlive()) throw new IOException("no answer from the root shell within "
                    + Math.max(1, timeoutMs / 1000) + " s: " + command);
            if (failed[0] != null) throw failed[0];
            return new Result(code[0], text.toString());
        }

        void close() {
            try {
                in.write("exit\n".getBytes(UTF8));
                in.flush();
            } catch (IOException ignored) {
            }
            p.destroy();
        }
    }
}
