package org.qemu.htcleo;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.Charset;
import java.util.HashMap;
import java.util.LinkedList;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * QMP, the same protocol the web server speaks (web/server/qmp.ts): one line
 * of JSON each way, a greeting to start with, then one reply per command.
 *
 * One reader thread owns the socket.  Commands are written under a lock so
 * their order is the caller's order, which matters for input events: a tap
 * must never overtake the touch before it.
 */
public class Qmp {
    private static final String TAG = "HtcleoQmp";
    private static final Charset UTF8 = Charset.forName("UTF-8");

    public interface Events {
        /** Anything QEMU tells us about, e.g. SHUTDOWN or RESET. */
        void onEvent(String name, JSONObject body);
    }

    /** The pair of boxes an {@link #exec} fills in. */
    public static final class Reply {
        public Object value;
        public String error;
    }

    private static final class Pending {
        Object value;
        String error;
    }

    private final Link link;
    private final OutputStream out;
    private final Object lock = new Object();
    private final Map<Integer, Pending> waiting = new HashMap<Integer, Pending>();
    private final CountDownLatch greeted = new CountDownLatch(1);
    private final Thread reader;
    private Events events;
    private int nextId = 1;
    private volatile boolean closed;
    private volatile String closeReason = "";

    private Qmp(Link link, Events events) {
        this.link = link;
        this.out = link.out();
        this.events = events;
        this.reader = new Thread(new Runnable() {
            @Override
            public void run() {
                read();
            }
        }, "qmp-reader");
        this.reader.setDaemon(true);
        Thread w = new Thread(new Runnable() {
            @Override
            public void run() {
                watchReplies();
            }
        }, "qmp-watch");
        w.setDaemon(true);
        w.start();
    }

    /** Connects, waits for the greeting and leaves QMP ready for commands. */
    public static Qmp connect(String path, Events events) throws IOException, InterruptedException {
        Qmp q = new Qmp(Link.connect(path), events);
        q.reader.start();
        if (!q.greeted.await(5, TimeUnit.SECONDS)) {
            q.close();
            throw new IOException("QMP did not greet us");
        }
        q.exec("qmp_capabilities", null);
        return q;
    }

    public boolean isClosed() {
        return closed;
    }

    public String closeReason() {
        return closeReason;
    }

    public void setEvents(Events events) {
        this.events = events;
    }

    public void close() {
        fail(null);
    }

    /* ------------------------------------------------------------- plumbing */

    private void read() {
        BufferedReader r = new BufferedReader(new InputStreamReader(link.in(), UTF8), 1 << 16);
        try {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.length() == 0) continue;
                JSONObject msg;
                try {
                    msg = new JSONObject(line);
                } catch (JSONException e) {
                    continue;
                }
                if (msg.has("QMP")) {
                    greeted.countDown();
                    continue;
                }
                if (msg.has("event")) {
                    Events ev = events;
                    if (ev != null) {
                        try {
                            JSONObject data = msg.optJSONObject("data");
                            ev.onEvent(msg.getString("event"), data == null ? new JSONObject() : data);
                        } catch (JSONException e) {
                            /* an event we cannot read is not worth failing over */
                        }
                    }
                    continue;
                }
                Pending p;
                synchronized (lock) {
                    p = waiting.remove(Integer.valueOf(msg.optInt("id", -1)));
                }
                if (p == null) continue;
                if (msg.has("error")) {
                    try {
                        JSONObject e = msg.getJSONObject("error");
                        p.error = e.optString("desc", "QMP error");
                    } catch (JSONException e) {
                        p.error = "QMP error";
                    }
                } else {
                    p.value = msg.opt("return");
                }
                synchronized (p) {
                    p.notifyAll();
                }
            }
            fail(null);
        } catch (IOException e) {
            fail(closed ? null : e.toString());
        }
    }

    private void fail(String why) {
        synchronized (lock) {
            if (closed) return;
            closed = true;
            closeReason = why == null ? "QMP closed" : why;
            for (Pending p : waiting.values()) {
                p.error = closeReason;
                synchronized (p) {
                    p.notifyAll();
                }
            }
            waiting.clear();
        }
        link.close();
    }

    /** Writes one command and returns its Pending; no thread, no waiting. */
    private Pending write(String name, JSONObject args) throws IOException {
        Pending p = new Pending();
        int id;
        synchronized (lock) {
            if (closed) throw new IOException("QMP is closed");
            id = nextId++;
            /*
             * A command is {"execute":..,"arguments":{..},"id":N}.  The
             * arguments are their own object: QEMU only accepts those three
             * (plus exec-oob) at the top level and answers anything else with
             * "QMP input member 'x' is unexpected".
             */
            JSONObject o = new JSONObject();
            try {
                o.put("execute", name);
                if (args != null && args.length() > 0) {
                    o.put("arguments", args);
                }
                o.put("id", id);
            } catch (JSONException e) {
                throw new IOException(e.toString());
            }
            waiting.put(Integer.valueOf(id), p);
            out.write((o.toString() + "\n").getBytes(UTF8));
            out.flush();
        }
        return p;
    }

    private Object command(String name, JSONObject args, long timeoutMs) throws IOException, InterruptedException {
        Pending p = write(name, args);
        synchronized (p) {
            long end = System.currentTimeMillis() + timeoutMs;
            while (p.value == null && p.error == null) {
                long left = end - System.currentTimeMillis();
                if (left <= 0) break;
                p.wait(left);
            }
        }
        if (p.error != null) throw new IOException(p.error);
        return p.value;
    }

    /* ------------------------------------------------------------- commands */

    /**
     * Fire and forget: taps and key presses go through here.  The reply is not
     * waited for on the caller's thread, but a watcher still picks it up and
     * logs anything QEMU refuses - a dropped input event is otherwise
     * invisible and looks like a broken guest.
     */
    public void send(String name, JSONObject args) {
        try {
            watchForErrors(name, write(name, args));
        } catch (Exception e) {
            Log.w(TAG, name + ": " + e.getMessage());
        }
    }

    private final LinkedList<Object[]> sent = new LinkedList<Object[]>();

    /** Waits for each reply in turn and logs the ones QEMU refused. */
    private void watchForErrors(String name, Pending p) {
        synchronized (sent) {
            sent.add(new Object[] { name, p });
            if (sent.size() > 64) {
                sent.removeFirst();          /* never let this grow without end */
            }
            sent.notifyAll();
        }
    }

    private void watchReplies() {
        synchronized (sent) {
            while (true) {
                while (sent.isEmpty()) {
                    try {
                        sent.wait();
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                Object[] head = sent.removeFirst();
                String name = (String) head[0];
                Pending p = (Pending) head[1];
                synchronized (p) {
                    long end = System.currentTimeMillis() + 5000;
                    while (p.value == null && p.error == null) {
                        long left = end - System.currentTimeMillis();
                        if (left <= 0) break;
                        try {
                            p.wait(left);
                        } catch (InterruptedException e) {
                            return;
                        }
                    }
                }
                if (p.error != null) Log.w(TAG, name + ": " + p.error);
            }
        }
    }

    /** Run this off the caller's thread: it waits for the reply. */
    public void exec(String name, JSONObject args) {
        exec(name, args, new Reply());
    }

    public void exec(final String name, final JSONObject args, final Reply reply) {
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Object v = command(name, args, 8000);
                    synchronized (reply) {
                        reply.value = v;
                        reply.notifyAll();
                    }
                } catch (Exception e) {
                    Log.w(TAG, name + ": " + e.getMessage());
                    synchronized (reply) {
                        reply.error = e.getMessage();
                        reply.notifyAll();
                    }
                }
            }
        }, "qmp-" + name);
        t.setDaemon(true);
        t.start();
    }

    /** Blocks for the answer; only call from a worker thread. */
    public Qmp.Reply execSync(String name, JSONObject args) {
        Qmp.Reply r = new Qmp.Reply();
        exec(name, args, r);
        synchronized (r) {
            long end = System.currentTimeMillis() + 8000;
            while (r.value == null && r.error == null) {
                long left = end - System.currentTimeMillis();
                if (left <= 0) break;
                try {
                    r.wait(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
        }
        return r;
    }

    public void sendKey(String qcode, boolean down) {
        try {
            JSONObject data = new JSONObject().put("down", down)
                    .put("key", new JSONObject().put("type", "qcode").put("data", qcode));
            send("input-send-event", new JSONObject().put("events",
                    new JSONArray().put(new JSONObject().put("type", "key").put("data", data))));
        } catch (JSONException e) {
            Log.w(TAG, "key: " + e);
        }
    }

    private int lastX = -1, lastY = -1;
    private boolean touchDown;

    public void sendTouch(float nx, float ny, String action) {
        try {
            int x = Math.round(Math.max(0f, Math.min(1f, nx)) * 0x7fff);
            int y = Math.round(Math.max(0f, Math.min(1f, ny)) * 0x7fff);
            JSONArray events = new JSONArray();
            if (x != lastX || y != lastY || "down".equals(action)) {
                events.put(new JSONObject().put("type", "abs")
                        .put("data", new JSONObject().put("axis", "x").put("value", x)));
                events.put(new JSONObject().put("type", "abs")
                        .put("data", new JSONObject().put("axis", "y").put("value", y)));
                lastX = x;
                lastY = y;
            }
            if ("down".equals(action) && !touchDown) {
                events.put(new JSONObject().put("type", "btn")
                        .put("data", new JSONObject().put("down", true).put("button", "left")));
            }
            if ("up".equals(action) && touchDown) {
                events.put(new JSONObject().put("type", "btn")
                        .put("data", new JSONObject().put("down", false).put("button", "left")));
            }
            touchDown = "down".equals(action) ? true : "up".equals(action) ? false : touchDown;
            if (events.length() > 0) send("input-send-event", new JSONObject().put("events", events));
        } catch (JSONException e) {
            Log.w(TAG, "touch: " + e);
        }
    }
}
