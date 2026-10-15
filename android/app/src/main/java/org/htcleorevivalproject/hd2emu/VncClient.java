package org.qemu.htcleo;

import android.util.Log;

import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.Charset;

/**
 * A minimal RFB 3.8 client for QEMU's VNC server on a unix socket: raw 32-bit
 * BGRX updates, desktop resizes and QEMU's audio extension (signed 16-bit
 * stereo PCM at 48 kHz).  This is the same protocol the web server speaks in
 * web/server/vnc.ts, with a thread and callbacks instead of promises.
 *
 * The framebuffer is handed over as one reusable byte[]; the view copies it
 * into a Bitmap, so a frame costs one memcpy and nothing else.
 */
public class VncClient {
    private static final String TAG = "HtcleoVnc";
    private static final Charset LATIN1 = Charset.forName("ISO-8859-1");

    private static final int ENC_RAW = 0;
    private static final int ENC_DESKTOP_SIZE = -223;
    private static final int ENC_QEMU_AUDIO = -259;

    public interface Listener {
        /** A whole new frame.  fb is w*h*4 bytes, BGRX, and is reused. */
        void onFrame(byte[] fb, int w, int h);

        /** PCM, signed 16-bit little endian, stereo, 48 kHz. */
        void onAudio(byte[] pcm, int length);

        void onClosed(String reason);
    }

    private final Link link;
    private final DataInputStream in;
    private final OutputStream out;
    private final Listener listener;
    private final Thread thread;
    private volatile boolean closed;
    private volatile String reason = "";

    private int width, height;
    private byte[] fb;
    private byte[] audio = new byte[0];

    private VncClient(Link link, Listener listener, boolean wantVideo, boolean wantAudio) throws IOException {
        this.link = link;
        this.in = new DataInputStream(link.in());
        this.out = link.out();
        this.listener = listener;
        this.thread = new Thread(new Runnable() {
            @Override
            public void run() {
                handshake(wantVideo, wantAudio);
            }
        }, "vnc");
        this.thread.setDaemon(true);
    }

    public static VncClient connect(String path, Listener listener, boolean video, boolean audio)
            throws IOException {
        VncClient v = new VncClient(Link.connect(path), listener, video, audio);
        v.thread.start();
        return v;
    }

    public int width() {
        return width;
    }

    public int height() {
        return height;
    }

    public void close() {
        closed = true;
        link.close();
    }

    /* ------------------------------------------------------------- plumbing */

    private void write(byte[] b) throws IOException {
        synchronized (out) {
            out.write(b);
            out.flush();
        }
    }

    private byte[] readFully(int n) throws IOException {
        byte[] b = new byte[n];
        in.readFully(b);
        return b;
    }

    private int u8() throws IOException {
        return in.readUnsignedByte();
    }

    private int u16() throws IOException {
        return in.readUnsignedShort();
    }

    private long u32() throws IOException {
        return in.readInt() & 0xffffffffL;
    }

    /* ------------------------------------------------------------ handshake */

    private void handshake(boolean wantVideo, boolean wantAudio) {
        try {
            byte[] ver = readFully(12);
            if (!new String(ver, LATIN1).startsWith("RFB ")) throw new IOException("not a VNC server");
            write("RFB 003.008\n".getBytes(LATIN1));

            int ntypes = u8();
            if (ntypes == 0) {
                long len = u32();
                throw new IOException(new String(readFully((int) len), LATIN1));
            }
            byte[] types = readFully(ntypes);
            boolean none = true;
            for (byte t : types) {
                if (t == 1) none = false;
            }
            if (none) throw new IOException("the VNC server wants a password");
            write(new byte[]{1});
            if (u32() != 0) throw new IOException("VNC security handshake failed");
            write(new byte[]{1});                                   // shared session

            byte[] init = readFully(24);
            resize(u16(init, 0), u16(init, 2));
            readFully((int) u32(init, 20));                        // the server's name

            /*
             * 32bpp little endian, true colour, and the shifts that make the
             * bytes come out R, G, B, X - which is what an ARGB_8888 Bitmap
             * holds in memory, so a frame is a straight copy.  (Ask for
             * red-shift 16 instead and every colour arrives with red and blue
             * swapped.)
             */
            byte[] pf = new byte[20];
            pf[0] = 0;
            pf[4] = 32;                                             // bits per pixel
            pf[5] = 24;                                             // depth
            pf[6] = 0;                                              // little endian
            pf[7] = 1;                                              // true colour
            put16(pf, 8, 255);                                      // red max
            put16(pf, 10, 255);                                     // green max
            put16(pf, 12, 255);                                     // blue max
            pf[14] = 0;                                             // red shift
            pf[15] = 8;                                             // green shift
            pf[16] = 16;                                            // blue shift
            write(pf);

            int[] encs = wantAudio ? new int[]{ENC_RAW, ENC_DESKTOP_SIZE, ENC_QEMU_AUDIO}
                                   : new int[]{ENC_RAW, ENC_DESKTOP_SIZE};
            byte[] se = new byte[4 + 4 * encs.length];
            se[0] = 2;
            put16(se, 2, encs.length);
            for (int i = 0; i < encs.length; i++) put32(se, 4 + 4 * i, encs[i]);
            write(se);

            if (wantAudio) {
                byte[] fmt = new byte[10];
                fmt[0] = (byte) 255;                            // the audio extension
                fmt[1] = 1;                                     // start playing
                put16(fmt, 2, 2);                               // 2 channels
                fmt[4] = 3;                                     // S16
                fmt[5] = 2;                                     // stereo
                put32(fmt, 6, 48000);
                write(fmt);
                write(new byte[]{(byte) 255, 1, 0, 0});          // and then silence
            }
            if (wantVideo) requestUpdate(false);
            loop();
        } catch (EOFException e) {
            close("QEMU closed the VNC connection");
        } catch (IOException e) {
            close(closed ? "closed" : String.valueOf(e.getMessage()));
        } catch (RuntimeException e) {
            close(String.valueOf(e.getMessage()));
        }
    }

    private void loop() throws IOException {
        for (;;) {
            int type = u8();
            switch (type) {
                case 0: {
                    byte[] hdr = readFully(3);
                    int n = u16(hdr, 1);
                    boolean pixels = false;
                    for (int i = 0; i < n; i++) {
                        byte[] r = readFully(12);
                        int x = u16(r, 0), y = u16(r, 2), w = u16(r, 4), h = u16(r, 6);
                        int enc = (int) u32(r, 8);
                        if (enc == ENC_DESKTOP_SIZE) {
                            resize(w, h);
                        } else if (enc == ENC_QEMU_AUDIO) {
                            /* the server acknowledging the audio extension */
                        } else if (enc == ENC_RAW) {
                            pixels = true;
                            int stride = width * 4;
                            if (x == 0 && w == width && y * stride + h * stride <= fb.length) {
                                /* the common case: one full frame, straight into place */
                                in.readFully(fb, y * stride, w * h * 4);
                            } else {
                                byte[] rows = readFully(w * h * 4);
                                for (int row = 0; row < h; row++) {
                                    System.arraycopy(rows, row * w * 4,
                                            fb, (y + row) * stride + x * 4, w * 4);
                                }
                            }
                        } else {
                            throw new IOException("unexpected VNC encoding " + enc);
                        }
                    }
                    if (pixels) {
                        /* BGRX: Android's ARGB_8888 is the same four bytes, if the
                         * fourth one is opaque */
                        byte[] px = fb;
                        for (int i = 3; i < px.length; i += 4) px[i] = (byte) 0xff;
                        listener.onFrame(px, width, height);
                    }
                    requestUpdate(true);
                    break;
                }
                case 1: {                                            // SetColourMapEntries
                    byte[] h = readFully(5);
                    readFully(u16(h, 3) * 6);
                    break;
                }
                case 2:                                             // bell
                    break;
                case 3: {                                            // server cut text
                    byte[] h = readFully(7);
                    readFully((int) u32(h, 3));
                    break;
                }
                case 255: {                                          // QEMU's own messages
                    byte[] h = readFully(3);
                    if (h[0] != 1) throw new IOException("unknown QEMU message " + h[0]);
                    if (u16(h, 1) == 2) {                            // audio data
                        int len = (int) u32();
                        if (len > audio.length) audio = new byte[Math.max(len, audio.length * 2)];
                        in.readFully(audio, 0, len);
                        listener.onAudio(audio, len);
                    }
                    break;
                }
                default:
                    throw new IOException("unknown VNC message " + type);
            }
        }
    }

    private void requestUpdate(boolean incremental) throws IOException {
        byte[] b = new byte[10];
        b[0] = 3;
        b[1] = (byte) (incremental ? 1 : 0);
        put16(b, 6, width);
        put16(b, 8, height);
        write(b);
    }

    private void resize(int w, int h) {
        width = w;
        height = h;
        fb = new byte[w * h * 4];
        Log.i(TAG, "screen is now " + w + "x" + h);
    }

    private void close(String why) {
        if (closed) return;
        closed = true;
        reason = why == null ? "" : why;
        listener.onClosed(reason);
        link.close();
    }

    /* ---------------------------------------------------------------- bytes */

    private static int u16(byte[] b, int off) {
        return ((b[off] & 0xff) << 8) | (b[off + 1] & 0xff);
    }

    private static long u32(byte[] b, int off) {
        return ((long) (b[off] & 0xff) << 24) | ((b[off + 1] & 0xff) << 16)
                | ((b[off + 2] & 0xff) << 8) | (b[off + 3] & 0xff);
    }

    private static void put16(byte[] b, int off, int v) {
        b[off] = (byte) (v >> 8);
        b[off + 1] = (byte) v;
    }

    private static void put32(byte[] b, int off, int v) {
        b[off] = (byte) (v >> 24);
        b[off + 1] = (byte) (v >> 16);
        b[off + 2] = (byte) (v >> 8);
        b[off + 3] = (byte) v;
    }

    /** Not used yet, but the audio extension can also change format. */
    static InputStream stream(Link l) {
        return l.in();
    }
}
