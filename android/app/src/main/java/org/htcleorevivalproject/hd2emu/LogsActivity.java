package org.qemu.htcleo;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

/**
 * The same four logs the web UI shows: the phone's two UARTs, QEMU's own log
 * and the DSP process's.  They are plain files in the app's run directory, so
 * this is just a tail -f with a Holo face.
 */
public class LogsActivity extends Activity {
    private static final String[] LOGS = {"uart1", "ramcon", "qemu", "q6"};
    /** Two tabs are not files: QEMU's monitor and the guest's adb. */
    private static final String MONITOR = "monitor";
    private static final String ADB = "adb";

    private Emulator emu;
    private LinearLayout root;
    private final Handler tick = new Handler(Looper.getMainLooper());
    private String which = "uart1";
    private TextView body;
    private Runnable follow;
    /** While the reader is at the bottom, the log keeps scrolling under them. */
    private boolean stickToBottom = true;

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            tail();
            tick.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        emu = EmuService.instance(this);

        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        setContentView(root);
        Holo.apply(root);
        if (getActionBar() != null) getActionBar().setDisplayHomeAsUpEnabled(true);

        LinearLayout tabs = new LinearLayout(this);
        tabs.setOrientation(LinearLayout.HORIZONTAL);
        root.addView(tabs, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        body = new TextView(this);
        body.setTypeface(android.graphics.Typeface.MONOSPACE);
        body.setTextSize(11f);
        body.setTextColor(0xffbebebe);
        body.setPadding(dp(12), dp(8), dp(12), dp(12));
        body.setHorizontallyScrolling(true);

        HorizontalScrollView hs = new HorizontalScrollView(this);
        hs.setFillViewport(true);
        hs.addView(body, new HorizontalScrollView.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        final ScrollView vs = new ScrollView(this);
        vs.addView(hs);
        scroller = vs;
        /*
         * Only follow the tail while the reader is already at the bottom:
         * yanking the view down every second makes scrolling up impossible.
         */
        vs.setOnScrollChangeListener(new View.OnScrollChangeListener() {
            @Override
            public void onScrollChange(View v, int x, int y, int oldX, int oldY) {
                stickToBottom = atBottom();
            }
        });
        root.addView(vs, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        follow = new Runnable() {
            @Override
            public void run() {
                if (stickToBottom) vs.post(new Runnable() {
                    @Override
                    public void run() {
                        vs.fullScroll(View.FOCUS_DOWN);
                    }
                });
            }
        };
        buildTabs(tabs);
        tail();
    }

    @Override
    public boolean onOptionsItemSelected(android.view.MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            finish();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }

    @Override
    protected void onResume() {
        super.onResume();
        tick.post(poll);
    }

    @Override
    protected void onPause() {
        super.onPause();
        tick.removeCallbacks(poll);
    }

    /** Four Holo tabs; the one being shown is lit up, the way ICS did it. */
    private void buildTabs(LinearLayout tabs) {
        tabs.removeAllViews();
        int pad = dp(12);
        String[] names = new String[LOGS.length + 2];
        System.arraycopy(LOGS, 0, names, 0, LOGS.length);
        names[LOGS.length] = MONITOR;
        names[LOGS.length + 1] = ADB;
        for (final String name : names) {
            TextView t = new TextView(this);
            t.setText(name);
            t.setTextSize(15f);
            t.setGravity(android.view.Gravity.CENTER);
            t.setPadding(pad, pad, pad, pad);
            boolean on = name.equals(which);
            t.setBackgroundResource(on ? R.drawable.holo_button_primary : R.drawable.holo_button);
            t.setTextColor(on ? 0xff33b5e5 : Color.WHITE);
            t.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    which = name;
                    buildTabs(tabs);
                    if (MONITOR.equals(name)) {
                        if (inputRow != null) root.removeView(inputRow);
                        buildMonitor(root);
                    } else if (ADB.equals(name)) {
                        if (inputRow != null) root.removeView(inputRow);
                        buildAdb(root);
                    } else if (inputRow != null) {
                        root.removeView(inputRow);
                        inputRow = null;
                    }
                    tail();
                }
            });
            tabs.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        }
    }

    private ScrollView scroller;
    private LinearLayout inputRow;
    private EditText input;
    private StringBuilder monitorLog = new StringBuilder();
    private boolean monitorBusy;

    /** The monitor tab: a command line under the answer, like HMP itself. */
    private void buildMonitor(final LinearLayout root) {
        inputRow = new LinearLayout(this);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        root.addView(inputRow, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        input = new EditText(this);
        input.setSingleLine(true);
        input.setTextSize(13f);
        input.setHint("info cpus");
        input.setTextColor(Color.WHITE);
        input.setBackgroundResource(R.drawable.holo_field);
        input.setOnEditorActionListener(new android.widget.TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(android.widget.TextView v, int actionId, android.view.KeyEvent event) {
                runMonitor();
                return true;
            }
        });
        inputRow.addView(input, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        android.widget.Button go = new android.widget.Button(this);
        go.setText("Run");
        go.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                runMonitor();
            }
        });
        inputRow.addView(go, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        if (monitorLog.length() == 0) {
            body.setText("QEMU's monitor (human-monitor-command), e.g.\n"
                    + "  info cpus\n  info registers\n  info status\n"
                    + "  screendump /sdcard/shot.ppm\n  stop / cont");
        }
    }

    private void runMonitor() {
        if (monitorBusy || input == null) return;
        final String cmd = input.getText().toString().trim();
        if (cmd.length() == 0) return;
        monitorBusy = true;
        appendMonitor("> " + cmd);
        input.setText("");
        new Thread(new Runnable() {
            @Override
            public void run() {
                final Qmp.Reply r = emu.hmpSync(cmd);
                tick.post(new Runnable() {
                    @Override
                    public void run() {
                        monitorBusy = false;
                        appendMonitor(r.error != null ? r.error : String.valueOf(r.value));
                    }
                });
            }
        }, "hmp").start();
    }

    /* ------------------------------------------------------------- the guest */

    private Adb adb;
    private StringBuilder adbLog = new StringBuilder();

    /**
     * The adb tab: a shell into the Android running inside the emulator, over
     * QEMU's adb bridge on this device's own loopback.  The guest's adbd is
     * not up until it has booted, so the first thing the tab says is which of
     * those two things is true.
     */
    private void buildAdb(final LinearLayout root) {
        inputRow = new LinearLayout(this);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        root.addView(inputRow, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        input = new EditText(this);
        input.setSingleLine(true);
        input.setTextSize(13f);
        input.setHint("logcat -v time");
        input.setTextColor(Color.WHITE);
        input.setBackgroundResource(R.drawable.holo_field);
        inputRow.addView(input, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        android.widget.Button go = new android.widget.Button(this);
        go.setText("Run");
        go.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                final String cmd = input.getText().toString().trim();

                runAdb(cmd.length() > 0 ? cmd : "logcat -v time");
            }
        });
        inputRow.addView(go, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        android.widget.Button stop = new android.widget.Button(this);
        stop.setText("Stop");
        stop.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (adb != null) {
                    adb.close();
                    adb = null;
                }
            }
        });
        inputRow.addView(stop, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        if (adbLog.length() == 0) {
            body.setText("The emulator's adb bridge, on 127.0.0.1:" +
                    emu.running().adbPort + "\n\n"
                    + "Nothing from the guest until its adbd starts, which is "
                    + "some minutes into the boot.  Then, for example:\n"
                    + "  logcat -v time\n"
                    + "  getprop ro.build.version.release\n"
                    + "  ls /system/app");
        }
    }

    private void runAdb(final String cmd) {
        if (adb != null) {
            adb.close();
            adb = null;
        }
        appendAdb("> " + cmd + "\n");
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Adb a = Adb.open(emu.running().adbPort, 3000);
                    String banner = a.waitFor(2000);
                    if (banner == null) {
                        a.close();
                        appendAdb("The emulator is not running, or its adb "
                                 + "bridge has no guest yet.\n");
                        return;
                    }
                    a.shell(cmd, new Adb.Listener() {
                        @Override
                        public void onBanner(String b) {
                            /* the guest's answer is the output itself */
                        }

                        @Override
                        public void onOutput(String text) {
                            appendAdb(text);
                        }

                        @Override
                        public void onClosed(String why) {
                            if (why != null && why.length() > 0) {
                                appendAdb("\n[" + why + "]\n");
                            }
                        }
                    });
                    adb = a;
                } catch (Exception e) {
                    appendAdb("[" + e + "]\n");
                }
            }
        }, "adb-shell").start();
    }

    private void appendAdb(String text) {
        tick.post(new Runnable() {
            @Override
            public void run() {
                adbLog.append(text);
                if (adbLog.length() > 256 * 1024) {
                    adbLog.delete(0, adbLog.length() - 128 * 1024);
                }
                body.setText(adbLog.toString());
                if (follow != null) follow.run();
            }
        });
    }

    private void appendMonitor(String s) {
        monitorLog.append(s).append('\n');
        body.setText(monitorLog.toString());
        if (follow != null) follow.run();
    }

    private boolean atBottom() {
        if (scroller == null || body == null) return true;
        if (body.getHeight() <= 0) return true;
        return scroller.getScrollY() + scroller.getHeight() >= body.getHeight() - dp(24);
    }

    private void tail() {
        if (MONITOR.equals(which) || ADB.equals(which)) {
            return;                                     /* not a file */
        }
        File f = emu.paths().log(which);
        if (!f.isFile()) {
            body.setText(which + ": nothing yet");
            return;
        }
        try {
            RandomAccessFile raf = new RandomAccessFile(f, "r");
            try {
                int len = (int) Math.min(raf.length(), 64 * 1024);
                raf.seek(raf.length() - len);
                byte[] buf = new byte[len];
                raf.readFully(buf);
                body.setText(new String(buf, "UTF-8"));
            } finally {
                raf.close();
            }
        } catch (IOException e) {
            body.setText(String.valueOf(e.getMessage()));
        }
        if (follow != null) follow.run();
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }
}
