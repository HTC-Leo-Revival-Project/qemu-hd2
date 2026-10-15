package com.hd2bench;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Bundle;
import android.os.Environment;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileWriter;
import java.util.List;

public class MainActivity extends Activity {
    private static final int BG = 0xff0e1215;
    private static final int CARD = 0xff171d23;
    private static final int LINE = 0xff252e37;
    private static final int TEXT = 0xffdce4ec;
    private static final int MUTED = 0xff8693a1;
    private static final int ACCENT = 0xff3ddc97;
    private static final int ACCENT_DOWN = 0xff2fb57b;

    private static final int REQ_GL = 1;

    private LinearLayout results;
    private TextView status, summary;
    private ProgressBar progress;
    private View runAll, runCpu, runGpu, runStorage;
    private boolean running, allPending;

    private int dp(float v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);

        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) {
            t.setTypeface(Typeface.DEFAULT_BOLD);
        }
        return t;
    }

    private Drawable shape(int fill, int stroke, float radius) {
        GradientDrawable g = new GradientDrawable();

        g.setColor(fill);
        g.setCornerRadius(dp(radius));
        if (stroke != 0) {
            g.setStroke(dp(1), stroke);
        }
        return g;
    }

    private View button(String label, boolean primary, View.OnClickListener l) {
        TextView b = text(label, 15, primary ? 0xff06130d : TEXT, true);
        StateListDrawable s = new StateListDrawable();

        s.addState(new int[] { android.R.attr.state_pressed },
                   shape(primary ? ACCENT_DOWN : LINE, primary ? 0 : ACCENT,
                         8));
        s.addState(new int[0], shape(primary ? ACCENT : CARD,
                                     primary ? 0 : 0xff35414d, 8));
        b.setBackgroundDrawable(s);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(8), dp(13), dp(8), dp(13));
        b.setClickable(true);
        b.setOnClickListener(l);
        return b;
    }

    private LinearLayout.LayoutParams weighted(int left, int right) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);

        p.setMargins(dp(left), 0, dp(right), 0);
        return p;
    }

    private LinearLayout.LayoutParams full(int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);

        p.setMargins(0, dp(top), 0, 0);
        return p;
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        Results.load(this);
        if (Results.section(Results.INFO).isEmpty()) {
            Bench.deviceInfo();
        }

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG);
        scroll.setFillViewport(true);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(20), dp(16), dp(24));
        scroll.addView(root);

        root.addView(text("HD2 Bench", 28, Color.WHITE, true));
        TextView sub = text("CPU, GPU and storage speed, Android 4.0", 13,
                            MUTED, false);
        root.addView(sub, full(2));
        summary = text("", 12, 0xffb3c0cc, false);
        summary.setTypeface(Typeface.MONOSPACE);
        root.addView(summary, full(12));

        runAll = button("Run all tests", true, new View.OnClickListener() {
            public void onClick(View v) {
                start(true, true, true);
            }
        });
        root.addView(runAll, full(16));
        LinearLayout row = new LinearLayout(this);
        runCpu = button("CPU", false, new View.OnClickListener() {
            public void onClick(View v) {
                start(true, false, false);
            }
        });
        runGpu = button("GPU", false, new View.OnClickListener() {
            public void onClick(View v) {
                start(false, true, false);
            }
        });
        runStorage = button("Storage", false, new View.OnClickListener() {
            public void onClick(View v) {
                start(false, false, true);
            }
        });
        row.addView(runCpu, weighted(0, 4));
        row.addView(runGpu, weighted(4, 4));
        row.addView(runStorage, weighted(4, 0));
        root.addView(row, full(8));

        progress = new ProgressBar(this, null,
                                   android.R.attr.progressBarStyleHorizontal);
        progress.setVisibility(View.GONE);
        root.addView(progress, full(14));
        status = text("", 13, ACCENT, false);
        status.setVisibility(View.GONE);
        root.addView(status, full(4));

        results = new LinearLayout(this);
        results.setOrientation(LinearLayout.VERTICAL);
        root.addView(results, full(6));

        LinearLayout out = new LinearLayout(this);
        out.addView(button("Copy results", false, new View.OnClickListener() {
            public void onClick(View v) {
                copy();
            }
        }), weighted(0, 4));
        out.addView(button("Save to SD card", false,
                           new View.OnClickListener() {
            public void onClick(View v) {
                saveFile();
            }
        }), weighted(4, 0));
        root.addView(out, full(18));
        TextView note = text("Close other apps first and keep the phone on its "
                             + "charger. The storage test writes a 16 MB "
                             + "file to internal storage and to the SD card, "
                             + "and deletes it afterwards. The GPU test takes "
                             + "the full screen for about 15 seconds.",
                             12, MUTED, false);
        root.addView(note, full(14));
        if (!Native.loaded) {
            root.addView(text("Native library failed to load: "
                              + Native.loadError, 12, 0xffff7a6b, false),
                         full(10));
        }
        setContentView(scroll);
        /* scripted runs: am start -n com.hd2bench/.MainActivity -e run cpu,gpu,storage|all */
        String run = getIntent().getStringExtra("run");
        if (run != null) {
            boolean all = run.contains("all");
            start(all || run.contains("cpu"), all || run.contains("gpu"),
                  all || run.contains("storage"));
        }
        refresh();
    }

    private void addSection(String title, String emptyHint) {
        List<Results.Item> items = Results.section(title);
        LinearLayout card = new LinearLayout(this);

        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundDrawable(shape(CARD, LINE, 10));
        card.setPadding(dp(14), dp(12), dp(14), dp(8));
        TextView head = text(title.toUpperCase(), 12, MUTED, true);
        card.addView(head);
        if (items.isEmpty()) {
            card.addView(text(emptyHint, 13, MUTED, false), full(8));
            card.addView(new View(this), new LinearLayout.LayoutParams(1,
                                                                        dp(4)));
        }
        boolean first = true;
        for (Results.Item i : items) {
            if (!first) {
                View sep = new View(this);

                sep.setBackgroundColor(LINE);
                card.addView(sep, new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT, 1));
            }
            first = false;
            card.addView(itemRow(i));
        }
        results.addView(card, full(12));
    }

    private View itemRow(Results.Item i) {
        LinearLayout r = new LinearLayout(this);
        boolean numeric = i.unit.length() > 0;

        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, dp(8), 0, dp(8));
        TextView name = text(i.name, 14, TEXT, false);
        r.addView(name, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, numeric ? 1 : 0.8f));
        if (numeric) {
            TextView v = text(i.value, 17, ACCENT, true);
            v.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
            v.setGravity(Gravity.RIGHT);
            r.addView(v);
            TextView u = text(" " + i.unit, 12, MUTED, false);
            u.setMinWidth(dp(i.unit.length() > 6 ? 0 : 50));
            r.addView(u);
        } else {
            TextView v = text(i.value, 13, 0xffb3c0cc, false);
            v.setGravity(Gravity.RIGHT);
            v.setEllipsize(TextUtils.TruncateAt.END);
            v.setMaxLines(3);
            r.addView(v, new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.2f));
        }
        return r;
    }

    private void refresh() {
        StringBuilder s = new StringBuilder();

        for (Results.Item i : Results.section(Results.INFO)) {
            if (i.name.equals("Model") || i.name.equals("Android")
                    || i.name.equals("CPU")) {
                s.append(i.value).append('\n');
            }
        }
        summary.setText(s.toString().trim());
        results.removeAllViews();
        addSection(Results.CPU, "Not run yet. Tap CPU.");
        addSection(Results.GPU, "Not run yet. Tap GPU.");
        addSection(Results.NAND, "Not run yet. Tap Storage.");
        addSection(Results.SD, "Not run yet. Tap Storage.");
        addSection(Results.INFO, "");
    }

    private void setRunning(boolean on) {
        running = on;
        for (View v : new View[] { runAll, runCpu, runGpu, runStorage }) {
            v.setEnabled(!on);
            v.setAlpha(on ? 0.4f : 1f);
        }
        progress.setVisibility(on ? View.VISIBLE : View.GONE);
        status.setVisibility(on ? View.VISIBLE : View.GONE);
        if (on) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            getWindow().clearFlags(
                    WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
    }

    private void step(final String what, final int done, final int total) {
        runOnUiThread(new Runnable() {
            public void run() {
                status.setText(what + "…");
                progress.setMax(total);
                progress.setProgress(done);
            }
        });
    }

    private void start(final boolean cpu, final boolean gpu,
                       final boolean storage) {
        if (running) {
            return;
        }
        setRunning(true);
        allPending = gpu;
        new Thread(new Runnable() {
            public void run() {
                Bench.Progress p = new Bench.Progress() {
                    public void step(String what, int done, int total) {
                        MainActivity.this.step(what, done, total);
                    }
                };

                Bench.deviceInfo();
                if (cpu) {
                    Bench.cpu(p);
                }
                if (storage) {
                    Bench.storage(MainActivity.this, p);
                }
                runOnUiThread(new Runnable() {
                    public void run() {
                        Results.save(MainActivity.this);
                        refresh();
                        if (gpu) {
                            status.setText("GPU test running full screen…");
                            startActivityForResult(new Intent(
                                    MainActivity.this, GlBenchActivity.class),
                                    REQ_GL);
                        } else {
                            finished();
                        }
                    }
                });
            }
        }, "bench").start();
    }

    private void finished() {
        setRunning(false);
        android.util.Log.i("hd2bench", "done");
        Results.save(this);
        refresh();
        Toast.makeText(this, "Done. Tap \"Copy results\" to share them.",
                       Toast.LENGTH_LONG).show();
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        if (req == REQ_GL && allPending) {
            allPending = false;
            if (res != RESULT_OK) {
                Results.putText(Results.GPU, "Status",
                                "interrupted (screen off or app switched)", "",
                                true);
            }
            finished();
        }
    }

    private void copy() {
        ClipboardManager cm =
                (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);

        cm.setPrimaryClip(ClipData.newPlainText("HD2 Bench",
                                                Results.asText()));
        Toast.makeText(this, "Results copied", Toast.LENGTH_SHORT).show();
    }

    private void saveFile() {
        File f = new File(Environment.getExternalStorageDirectory(),
                          "hd2bench-results.txt");
        FileWriter w = null;

        try {
            w = new FileWriter(f);
            w.write(Results.asText());
            Toast.makeText(this, "Saved " + f.getAbsolutePath(),
                           Toast.LENGTH_LONG).show();
        } catch (Exception e) {
            Toast.makeText(this, "Could not save: " + e.getMessage(),
                           Toast.LENGTH_LONG).show();
        } finally {
            try {
                if (w != null) {
                    w.close();
                }
            } catch (Exception e) {
                /* ignore */
            }
        }
    }
}
