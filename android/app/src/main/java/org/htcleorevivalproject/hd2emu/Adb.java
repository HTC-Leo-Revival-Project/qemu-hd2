package org.htcleorevivalproject.hd2emu;

import android.util.Log;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.Charset;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The adb client, as far as the app needs it: QEMU's adb bridge is a host
 * adb server on the device's own loopback, so a TCP socket and the adb
 * message protocol is all it takes to reach the running Android inside.
 *
 * No adb binary: the ones that exist for an arm64 phone are built against
 * another app's shared libraries, and the protocol is small - connect, say
 * CNWN, ask for a service, then read WRTE packets until CLSE.
 */
public class Adb {
    private static final String TAG = "HtcleoAdb";
    private static final Charset UTF8 = Charset.forName("UTF-8");

    private static final int A_CNXN = 0x4e584e43;      /* CNXN */
    private static final int A_OPEN = 0x4e45504f;      /* OPEN */
    private static final int A_OKAY = 0x59414b4f;      /* OKAY */
    private static final int A_CLSE = 0x45534c43;      /* CLSE */
    private static final int A_WRTE = 0x45545257;      /* WRTE */
    private static final int A_FAIL = 0x4c494146;      /* FAIL */
    private static final int CNXN_VERSION = 0x01000000;
    private static final int MAX_DATA = 4096;

    /** What the caller hears about: text out of the guest, and where we are. */
    public interface Listener {
        void onBanner(String banner);
        void onOutput(String text);
        void onClosed(String why);
    }

    private final Socket sock;
    private final DataInputStream in;
    private final OutputStream out;
    private final int port;
    private final AtomicInteger nextId = new AtomicInteger(1);
    private Thread reader;
    private volatile boolean closed;

    private Adb(Socket sock, int port) throws IOException {
        this.sock = sock;
        this.port = port;
        this.in = new DataInputStream(sock.getInputStream());
        this.out = sock.getOutputStream();
    }

    /**
     * Talks to the emulator's adb bridge.  The guest's adbd is not up until
     * Android has booted, so a bridge that will not answer yet is not an
     * error - {@link #waitFor} retries until the caller gives up.
     */
    public static Adb open(int port, int timeoutMs) throws IOException {
        Socket s = new Socket();
        s.connect(new InetSocketAddress("127.0.0.1", port), timeoutMs);
        s.setTcpNoDelay(true);
        Adb a = new Adb(s, port);
        a.send(A_CNXN, CNXN_VERSION, CNXN_VERSION | MAX_DATA, null);
        return a;
    }

    /** The bridge's answer: its banner, or null when it has none yet. */
    public String waitFor(int timeoutMs) throws IOException {
        Packet pkt = readPacket(timeoutMs);
        if (pkt == null) {
            return null;
        }
        int cmd = pkt.getInt(0);
        byte[] data = payload(pkt);
        if (cmd != A_CNXN) {
            throw new IOException("expected CNXN, got " + fourcc(cmd));
        }
        return new String(data, UTF8).replace('\0', ' ').trim();
    }

    /**
     * Runs a shell command in the guest and hands the output over as it
     * arrives; returns once the guest closes the stream.
     */
    public void shell(String command, final Listener listener) throws IOException {
        final int id = nextId.getAndIncrement();

        send(A_OPEN, id, 0, ("shell:" + command + "\0").getBytes(UTF8));
        Packet first = readPacket(15000);
        if (first == null) {
            throw new IOException("the guest did not answer");
        }
        if (first.getInt(0) == A_FAIL) {
            throw new IOException(new String(payload(first), UTF8).replace('\0', ' '));
        }
        if (first.getInt(0) != A_OKAY) {
            throw new IOException("expected OKAY, got " + fourcc(first.getInt(0)));
        }
        listener.onBanner("");
        reader = new Thread(new Runnable() {
            @Override
            public void run() {
                String why = "";

                try {
                    for (;;) {
                        Packet p = readPacket(0);
                        if (p == null) {
                            break;
                        }
                        int cmd = p.getInt(0);
                        int remote = p.getInt(8);
                        if (cmd == A_WRTE) {
                            byte[] data = payload(p);
                            /* ack, or the guest waits for us */
                            send(A_OKAY, id, remote, null);
                            if (data.length > 0) {
                                listener.onOutput(new String(data, UTF8));
                            }
                        } else if (cmd == A_CLSE) {
                            send(A_CLSE, id, remote, null);
                            break;
                        }
                    }
                } catch (IOException e) {
                    why = String.valueOf(e.getMessage());
                }
                close();
                listener.onClosed(why);
            }
        }, "adb-shell");
        reader.setDaemon(true);
        reader.start();
    }

    /* ------------------------------------------------- exec, push, pull, install */

    /** One open adb service stream, used synchronously (no reader thread). */
    private final class Stream {
        final int id = nextId.getAndIncrement();
        int remote;
        byte[] buf = new byte[0];
        int pos;
        boolean eof;

        Stream(String service) throws IOException {
            send(A_OPEN, id, 0, (service + "\0").getBytes(UTF8));
            Packet p = next(15000);
            if (p == null) throw new IOException("the guest did not answer");
            if (p.getInt(0) != A_OKAY) throw new IOException(service + ": " + fourcc(p.getInt(0)));
            remote = p.getInt(4);
        }

        /** next packet for this stream, skipping stray ones */
        Packet next(int timeout) throws IOException {
            Packet p = readPacket(timeout);
            if (p == null) throw new IOException("timeout");
            return p;
        }

        void write(byte[] d, int off, int len) throws IOException {
            while (len > 0) {
                int n = Math.min(len, MAX_DATA);
                byte[] c = new byte[n];
                System.arraycopy(d, off, c, 0, n);
                send(A_WRTE, id, remote, c);
                Packet p = next(20000);
                while (p.getInt(0) == A_WRTE) { /* peer data while we send: keep it */
                    append(payload(p));
                    send(A_OKAY, id, remote, null);
                    p = next(20000);
                }
                if (p.getInt(0) != A_OKAY) throw new IOException("write: " + fourcc(p.getInt(0)));
                off += n;
                len -= n;
            }
        }

        private void append(byte[] more) {
            byte[] nb = new byte[buf.length - pos + more.length];
            System.arraycopy(buf, pos, nb, 0, buf.length - pos);
            System.arraycopy(more, 0, nb, buf.length - pos, more.length);
            buf = nb;
            pos = 0;
        }

        /** fill the buffer with at least n bytes; false at end of stream */
        boolean fill(int n) throws IOException {
            while (buf.length - pos < n) {
                if (eof) return false;
                Packet p = next(30000);
                int cmd = p.getInt(0);
                if (cmd == A_WRTE) {
                    append(payload(p));
                    send(A_OKAY, id, remote, null);
                } else if (cmd == A_CLSE) {
                    eof = true;
                    send(A_CLSE, id, remote, null);
                }
            }
            return true;
        }

        byte[] read(int n) throws IOException {
            if (!fill(n)) throw new IOException("short read");
            byte[] r = new byte[n];
            System.arraycopy(buf, pos, r, 0, n);
            pos += n;
            return r;
        }

        byte[] readAll() throws IOException {
            while (fill(buf.length - pos + 1)) { /* until EOF */ }
            byte[] r = new byte[buf.length - pos];
            System.arraycopy(buf, pos, r, 0, r.length);
            pos = buf.length;
            return r;
        }

        void close() {
            try { send(A_CLSE, id, remote, null); } catch (IOException ignored) { }
        }
    }

    /** Runs a command and returns what it printed (stdout and stderr together). */
    public String exec(String command) throws IOException {
        Stream st = new Stream("shell:" + command);
        try {
            return new String(st.readAll(), UTF8);
        } finally {
            st.close();
        }
    }

    private static byte[] sync(String id, int n) {
        ByteBuffer b = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        b.put(id.getBytes(UTF8)).putInt(n);
        return b.array();
    }

    /** adb push: copies a local file to the guest. */
    public void push(java.io.File local, String remote) throws IOException {
        Stream st = new Stream("sync:");
        try {
            String spec = remote + ",33188";
            byte[] sb = spec.getBytes(UTF8);
            st.write(sync("SEND", sb.length), 0, 8);
            st.write(sb, 0, sb.length);
            java.io.FileInputStream fi = new java.io.FileInputStream(local);
            try {
                byte[] chunk = new byte[16384];
                int n;
                while ((n = fi.read(chunk)) > 0) {
                    st.write(sync("DATA", n), 0, 8);
                    st.write(chunk, 0, n);
                }
            } finally {
                fi.close();
            }
            st.write(sync("DONE", (int) (local.lastModified() / 1000)), 0, 8);
            byte[] r = st.read(8);
            String tag = new String(r, 0, 4, UTF8);
            if (!tag.equals("OKAY")) {
                int len = ByteBuffer.wrap(r, 4, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
                throw new IOException("push failed: " + new String(st.read(len), UTF8));
            }
            st.write(sync("QUIT", 0), 0, 8);
        } finally {
            st.close();
        }
    }

    /** adb pull: copies a guest file to a local one. */
    public void pull(String remote, java.io.File local) throws IOException {
        Stream st = new Stream("sync:");
        try {
            byte[] rb = remote.getBytes(UTF8);
            st.write(sync("RECV", rb.length), 0, 8);
            st.write(rb, 0, rb.length);
            java.io.FileOutputStream fo = new java.io.FileOutputStream(local);
            try {
                for (;;) {
                    byte[] h = st.read(8);
                    String tag = new String(h, 0, 4, UTF8);
                    int len = ByteBuffer.wrap(h, 4, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
                    if (tag.equals("DATA")) {
                        fo.write(st.read(len));
                    } else if (tag.equals("DONE")) {
                        break;
                    } else {
                        throw new IOException("pull failed: " + new String(st.read(len), UTF8));
                    }
                }
            } finally {
                fo.close();
            }
            st.write(sync("QUIT", 0), 0, 8);
        } finally {
            st.close();
        }
    }

    /** adb install: pushes the apk to /data/local/tmp and has pm install it. */
    public String install(java.io.File apk) throws IOException {
        String tmp = "/data/local/tmp/" + apk.getName().replaceAll("[^A-Za-z0-9._-]", "_");
        push(apk, tmp);
        String r = exec("pm install -r '" + tmp + "'");
        exec("rm '" + tmp + "'");
        return r;
    }

    /** `adb devices` for this bridge: the guest's serial, or null. */
    public String serial() throws IOException {
        String s = exec("getprop ro.serialno").trim();
        return s.isEmpty() ? null : s;
    }

    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        try {
            sock.close();
        } catch (IOException e) {
            /* it is going away anyway */
        }
    }

    public boolean isClosed() {
        return closed;
    }

    /* ------------------------------------------------------------- the wire */

    private static final class Packet {
        final ByteBuffer b;

        Packet(byte[] raw) {
            b = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
        }

        int getInt(int at) {
            return b.getInt(at);
        }
    }

    private void send(int cmd, int arg0, int arg1, byte[] data) throws IOException {
        byte[] d = data == null ? new byte[0] : data;
        ByteBuffer b = ByteBuffer.allocate(24 + d.length).order(ByteOrder.LITTLE_ENDIAN);

        b.putInt(cmd);
        b.putInt(arg0);
        b.putInt(arg1);
        b.putInt(d.length);
        b.putInt(crc32(d));
        b.putInt(cmd ^ 0xffffffff);
        b.put(d);
        synchronized (out) {
            out.write(b.array());
            out.flush();
        }
    }

    /** A packet, or null when there is nothing within the timeout. */
    private Packet readPacket(int timeoutMs) throws IOException {
        if (timeoutMs > 0) {
            sock.setSoTimeout(timeoutMs);
        }
        byte[] head = new byte[24];

        try {
            in.readFully(head);
        } catch (java.net.SocketTimeoutException e) {
            return null;
        }
        int len = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN).getInt(12);
        byte[] raw = new byte[24 + len];

        System.arraycopy(head, 0, raw, 0, 24);
        if (len > 0) {
            in.readFully(raw, 24, len);
        }
        return new Packet(raw);
    }

    private static byte[] payload(Packet p) {
        int len = p.getInt(12);
        byte[] out = new byte[len];

        System.arraycopy(p.b.array(), 24, out, 0, len);
        return out;
    }

    private static String fourcc(int cmd) {
        return "" + (char) (cmd & 0xff) + (char) ((cmd >> 8) & 0xff) +
                   (char) ((cmd >> 16) & 0xff) + (char) ((cmd >> 24) & 0xff);
    }

    private static int crc32(byte[] data) {
        java.util.zip.CRC32 c = new java.util.zip.CRC32();

        c.update(data, 0, data.length);
        return (int) c.getValue();
    }
}