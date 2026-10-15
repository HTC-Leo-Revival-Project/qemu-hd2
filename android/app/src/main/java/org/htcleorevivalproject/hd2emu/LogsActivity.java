package org.htcleorevivalproject.hd2emu;

import android.app.ActionBar;
import android.app.FragmentTransaction;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
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
 * this is just a tail -f with a Holo face - the action bar's own tabs, the
 * way ICS's apps switched between views of one thing.
 */
@SuppressWarnings("deprecation")       /* ActionBar tabs: deprecated in 5.0, and still exactly ICS */
public class LogsActivity extends HoloActivity {
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

        body = new TextView(this);
        body.setTypeface(android.graphics.Typeface.MONOSPACE);
        body.setTextSize(11f);
        body.setTextColor(Holo.TEXT_2);
        body.setPadding(dp(14), dp(10), dp(14), dp(14));
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
        vs.getViewTreeObserver().addOnScrollChangedListener(
                new android.view.ViewTreeObserver.OnScrollChangedListener() {
                    @Override
                    public void onScrollChanged() {
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
        buildTabs();
        tail();
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

    @Override
    public boolean onCreateOptionsMenu(android.view.Menu menu) {
        menu.add(0, 1, 0, R.string.logs_share).setShowAsAction(android.view.MenuItem.SHOW_AS_ACTION_IF_ROOM);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(android.view.MenuItem item) {
        if (item.getItemId() == 1) {
            share();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }

    /** The log being shown, as text, to whatever app takes it (mail, a chat, a notes app). */
    private void share() {
        CharSequence text = body.getText();
        /* an intent's extras travel in one binder transaction: keep it well under 1 MB */
        if (text.length() > 200 * 1024) text = text.subSequence(text.length() - 200 * 1024, text.length());
        android.content.Intent send = new android.content.Intent(android.content.Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(android.content.Intent.EXTRA_SUBJECT, getString(R.string.logs_share_subject, which));
        send.putExtra(android.content.Intent.EXTRA_TEXT, text.toString());
        startActivity(android.content.Intent.createChooser(send, getString(R.string.logs_share_title, which)));
    }

    /** One tab per log, in the action bar's stacked bar. */
    private void buildTabs() {
        ActionBar bar = getActionBar();
        if (bar == null) return;
        /* the selected tab's blue bar replaces the action bar's own rule */
        bar.setBackgroundDrawable(getResources().getDrawable(R.drawable.holo_actionbar_plain));
        bar.setNavigationMode(ActionBar.NAVIGATION_MODE_TABS);
        String[] names = new String[LOGS.length + 2];
        System.arraycopy(LOGS, 0, names, 0, LOGS.length);
        names[LOGS.length] = MONITOR;
        names[LOGS.length + 1] = ADB;
        for (final String name : names) {
            bar.addTab(bar.newTab().setText(name).setTabListener(new ActionBar.TabListener() {
                @Override
                public void onTabSelected(ActionBar.Tab tab, FragmentTransaction ft) {
                    show(name);
                }

                @Override
                public void onTabUnselected(ActionBar.Tab tab, FragmentTransaction ft) {
                }

                @Override
                public void onTabReselected(ActionBar.Tab tab, FragmentTransaction ft) {
                }
            }), name.equals(which));
        }
    }

    private void show(String name) {
        if (name.equals(which) && body.length() > 0) return;
        which = name;
        stickToBottom = true;
        if (inputRow != null) {
            root.removeView(inputRow);
            inputRow = null;
        }
        if (MONITOR.equals(name)) {
            buildMonitor();
            body.setText(monitorLog.length() > 0 ? monitorLog.toString() : monitorHelp());
        } else if (ADB.equals(name)) {
            buildAdb();
            body.setText(adbLog.length() > 0 ? adbLog.toString() : adbHelp());
        }
        tail();
    }

    private ScrollView scroller;
    private LinearLayout inputRow;
    private EditText input;
    private final StringBuilder monitorLog = new StringBuilder();
    private boolean monitorBusy;

    /** The command line under a log: a Holo field and flat buttons, in a bar. */
    private LinearLayout commandBar(String hint) {
        inputRow = new LinearLayout(this);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        inputRow.setGravity(android.view.Gravity.CENTER_VERTICAL);
        inputRow.setBackgroundResource(R.drawable.holo_keybar);
        inputRow.setPadding(dp(8), dp(4), 0, dp(4));
        root.addView(inputRow, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        input = Holo.field(this);
        input.setTextSize(15f);
        input.setTypeface(android.graphics.Typeface.MONOSPACE);
        input.setHint(hint);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setImeOptions(EditorInfo.IME_ACTION_GO);
        inputRow.addView(input, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return inputRow;
    }

    private Button barButton(String label, View.OnClickListener onClick) {
        Button b = Holo.borderless(this, label);
        b.setOnClickListener(onClick);
        inputRow.addView(b, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return b;
    }

    /** The monitor tab: a command line under the answer, like HMP itself. */
    private void buildMonitor() {
        commandBar("info cpus");
        input.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView v, int actionId, KeyEvent event) {
                runMonitor();
                return true;
            }
        });
        barButton(getString(R.string.run), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                runMonitor();
            }
        });
    }

    private String monitorHelp() {
        return getString(R.string.logs_monitor_help);
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
    private final StringBuilder adbLog = new StringBuilder();

    /**
     * The adb tab: a shell into the Android running inside the emulator, over
     * QEMU's adb bridge on this device's own loopback.  The guest's adbd is
     * not up until it has booted, so the first thing the tab says is which of
     * those two things is true.
     */
    private void buildAdb() {
        commandBar("logcat -v time");
        final View.OnClickListener run = new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                final String cmd = input.getText().toString().trim();
                runAdb(cmd.length() > 0 ? cmd : "logcat -v time");
            }
        };
        input.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView v, int actionId, KeyEvent event) {
                run.onClick(v);
                return true;
            }
        });
        barButton(getString(R.string.run), run);
        barButton(getString(R.string.stop), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (adb != null) {
                    adb.close();
                    adb = null;
                }
            }
        });
    }

    private String adbHelp() {
        Config r = emu.running();
        int port = r != null ? r.adbPort : 5555;
        return getString(R.string.logs_adb_help, port);
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
                        appendAdb(getString(R.string.logs_adb_no_guest) + "\n");
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

    private void appendAdb(final String text) {
        tick.post(new Runnable() {
            @Override
            public void run() {
                adbLog.append(text);
                if (adbLog.length() > 256 * 1024) {
                    adbLog.delete(0, adbLog.length() - 128 * 1024);
                }
                if (ADB.equals(which)) {
                    body.setText(adbLog.toString());
                    if (follow != null) follow.run();
                }
            }
        });
    }

    private void appendMonitor(String s) {
        monitorLog.append(s).append('\n');
        if (MONITOR.equals(which)) {
            body.setText(monitorLog.toString());
            if (follow != null) follow.run();
        }
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
            body.setText(getString(R.string.logs_nothing_yet, which));
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
        return Holo.dp(this, v);
    }
}
