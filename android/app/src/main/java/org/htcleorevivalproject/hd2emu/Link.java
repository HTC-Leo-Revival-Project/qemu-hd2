package org.htcleorevivalproject.hd2emu;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Build;

import java.io.Closeable;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * One end of a unix domain socket that lives in the app's own directory.
 * QEMU gets the same path for -qmp and -vnc.
 *
 * A unix socket is the only socket an app can own privately: every app on the
 * device shares the loopback interface, so a QMP on 127.0.0.1 would be open to
 * every other app that cares to connect.
 */
public class Link implements Closeable {
    private final Closeable sock;
    private final InputStream in;
    private final OutputStream out;

    private Link(Closeable sock, InputStream in, OutputStream out) {
        this.sock = sock;
        this.in = in;
        this.out = out;
    }

    public static Link connect(String path) throws IOException {
        /* LocalSocket(String) is not public API: make an unnamed socket and
         * connect it to a filesystem name, inside the app's own directory */
        LocalSocket s = new LocalSocket();
        if (Build.VERSION.SDK_INT >= 19) {
            s.connect(new LocalSocketAddress(path, LocalSocketAddress.Namespace.FILESYSTEM));
        } else {
            s.connect(new LocalSocketAddress(path));
        }
        return new Link(s, s.getInputStream(), s.getOutputStream());
    }

    public InputStream in() {
        return in;
    }

    public OutputStream out() {
        return out;
    }

    @Override
    public void close() {
        try {
            out.close();
        } catch (IOException ignored) {
        }
        try {
            in.close();
        } catch (IOException ignored) {
        }
        try {
            sock.close();
        } catch (IOException ignored) {
        }
    }
}
