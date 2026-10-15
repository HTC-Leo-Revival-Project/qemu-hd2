package org.qemu.htcleo;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

/**
 * The same knobs the web UI's Config panel has, minus the ones that cannot
 * work in an app: no host GPU (Android has no OpenGL 4.5 with fragment shader
 * interlock), and no USB LLE (that needs vhci_hcd and root).  Everything else
 * takes effect the next time the emulator is started.
 */
public class SettingsActivity extends Activity {
    private static final int REQ_PICK = 1;

    private Config config;
    private Holo holo;
    private Emulator emu;
    private Images images;
    private LinearLayout root;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        config = new Config(this);
        config.load();
        holo = new Holo(this);
        emu = EmuService.instance(this);
        images = new Images(emu.paths());

        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        setContentView(root);
        Holo.apply(root);

        if (android.os.Build.VERSION.SDK_INT >= 11 && getActionBar() != null) {
            getActionBar().setDisplayHomeAsUpEnabled(true);
        }
        build();
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
        rebuild();
    }

    private void rebuild() {
        config.load();
        root.removeAllViews();
        build();
    }

    private void save() {
        config.save();
    }

    /* ------------------------------------------------------------- the rows */

    private void category(String title) {
        TextView t = new TextView(this);
        t.setText(title);
        t.setTextSize(13f);
        t.setTypeface(null, android.graphics.Typeface.BOLD);
        t.setAllCaps(true);
        t.setLetterSpacing(0.04f);
        t.setTextColor(0xff33b5e5);
        t.setPadding(holo.dp(16), holo.dp(20), holo.dp(16), holo.dp(6));
        root.addView(t);
    }

    private void note(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(12.5f);
        t.setTextColor(0xff808080);
        t.setPadding(holo.dp(16), holo.dp(4), holo.dp(16), holo.dp(10));
        t.setLineSpacing(0, 1.2f);
        root.addView(t);
    }

    private void divider() {
        View v = new View(this);
        v.setBackgroundColor(0x1affffff);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 1);
        lp.leftMargin = holo.dp(16);
        lp.rightMargin = holo.dp(16);
        root.addView(v, lp);
    }

    /** A row that opens a list of choices. */
    private void list(String title, final String summary, final String[] labels,
                      final String[] summaries, final String value, final Holo.OnPick pick) {
        Holo.Row row = new Holo.Row(this);
        row.setTitle(title, summary);
        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                int checked = 0;
                for (int i = 0; i < labels.length; i++) {
                    if (labels[i].equals(value)) checked = i;
                }
                holo.list(title, labels, summaries, checked, pick);
            }
        });
        root.addView(row);
    }

    private void listPref(String title, String summary, String[] labels, String[] summaries,
                          String value, final Holo.OnPick pick) {
        list(title, summary, labels, summaries, value, pick);
    }

    private void switchRow(String title, String summary, boolean on, final Holo.OnToggle toggle) {
        Holo.Row row = new Holo.Row(this);
        row.setTitle(title, summary);
        row.addWidget(holo.switchView(on, toggle));
        root.addView(row);
    }

    private void seekRow(String title, int min, int max, int value, final Holo.Format label,
                         final Holo.OnSeek onSeek) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(holo.dp(16), holo.dp(8), holo.dp(16), holo.dp(8));

        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        TextView name = new TextView(this);
        name.setText(title);
        name.setTextSize(17f);
        name.setTextColor(Color.WHITE);
        final TextView val = new TextView(this);
        val.setText(label.format(value));
        val.setTextSize(13f);
        val.setTypeface(android.graphics.Typeface.MONOSPACE);
        val.setTextColor(0xff33b5e5);
        head.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        head.addView(val);
        box.addView(head);

        SeekBar bar = holo.seek(min, max, value, new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                val.setText(label.format(v));
                onSeek.onSeek(v);
            }
        });
        /* -1 is MATCH_CONTENT: API 36's android.jar no longer exposes the constant */
        box.addView(bar, new LinearLayout.LayoutParams(-1,
                android.view.ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(box);
    }

    private void editRow(String title, String summary, String value, final String what) {
        Holo.Row row = new Holo.Row(this);
        row.setTitle(title, summary);
        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                final EditText field = new EditText(SettingsActivity.this);
                field.setBackgroundResource(R.drawable.holo_field);
                field.setTextColor(Color.WHITE);
                field.setSingleLine(true);
                field.setInputType(InputType.TYPE_CLASS_TEXT);
                field.setText(what.equals("adbPort") ? String.valueOf(config.adbPort) : config.extraArgs);
                field.setSelectAllOnFocus(true);

                LinearLayout box = new LinearLayout(SettingsActivity.this);
                box.setOrientation(LinearLayout.VERTICAL);
                box.setPadding(holo.dp(20), holo.dp(20), holo.dp(20), holo.dp(20));
                box.addView(field, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));
                android.app.AlertDialog d = new android.app.AlertDialog.Builder(SettingsActivity.this)
                        .setTitle(title)
                        .setView(box)
                        .setNegativeButton(R.string.cancel, null)
                        .setPositiveButton(R.string.nok, new android.content.DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(android.content.DialogInterface dialog, int which) {
                                String s = field.getText().toString();
                                if (what.equals("adbPort")) {
                                    config.adbPort = Math.max(1024, Math.min(65535, number(s, 5555)));
                                } else {
                                    config.extraArgs = s;
                                }
                                save();
                                rebuild();
                            }
                        })
                        .create();
                holo.show(d);
                field.requestFocus();
            }
        });
        root.addView(row);
    }

    private static int number(String s, int fallback) {
        try {
            return Integer.parseInt(s.trim());
        } catch (Exception e) {
            return fallback;
        }
    }

    /* --------------------------------------------------------------- the page */

    private void build() {
        final boolean lle = Config.LLE.equals(config.baseband);

        category("Processors");
        listPref("Baseband (modem core)", basebandSummary(), new String[]{"Modelled (HLE)",
                        "Real AMSS firmware (LLE)"},
                new String[]{"Fast. Network registration, data, SMS and GPS fixes work.",
                        images.amss() ? "The radio ROM's own ARM9 code. No network signal."
                                      : "Needs amss.elf in the app's images folder"},
                Config.LLE.equals(config.baseband) ? "Real AMSS firmware (LLE)" : "Modelled (HLE)",
                new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.baseband = index == 1 ? Config.LLE : Config.HLE;
                        if (config.baseband.equals(Config.HLE)) {
                            config.dsp = Config.HLE;
                            if ("nand-lle.img".equals(config.nand) && images.has("nand.img")) {
                                config.nand = "nand.img";
                            }
                        } else if ("nand.img".equals(config.nand) && images.has("nand-lle.img")) {
                            config.nand = "nand-lle.img";
                        }
                        save();
                        rebuild();
                    }
                });
        divider();
        listPref("QDSP6 (multimedia DSP)", Config.LLE.equals(config.dsp)
                        ? "Real DSP1 firmware (LLE)" : "Not running",
                new String[]{"Not running", "Real DSP1 firmware (LLE)"},
                new String[]{"Codecs and audio fall back to software",
                        images.dsp1() ? "QDSP6 in a second process: H.264 decode, MPEG-4 encode, audio"
                                      : "Needs dsp1.elf in the app's images folder"},
                Config.LLE.equals(config.dsp) ? "Real DSP1 firmware (LLE)" : "Not running",
                new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.dsp = index == 1 ? Config.LLE : Config.HLE;
                        save();
                        rebuild();
                    }
                });
        if (!lle) note("The DSP firmware runs on top of the real baseband.");
        if (config.dspRunning()) {
            divider();
            listPref("DSP clock (icount shift)", shiftLabel(config.dspShift),
                    new String[]{"auto", "shift=0", "shift=1", "shift=2", "shift=3", "shift=4"},
                    new String[]{"QEMU adjusts the shift to follow real time",
                            "1 ns per instruction", "the default", "", "", ""},
                    shiftLabel(config.dspShift), new Holo.OnPick() {
                        @Override
                        public void onPick(int index) {
                            config.dspShift = index - 1;
                            save();
                            rebuild();
                        }
                    });
        }

        category("Images");
        listPref("NAND flash", nandSummary(), nandLabels(), nandSizes(), config.nand,
                new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.nand = nandLabels()[index];
                        save();
                        rebuild();
                    }
                });
        divider();
        Holo.Row add = new Holo.Row(this);
        add.setTitle("Add an image…", "Copy a NAND or microSD image into the app's own storage");
        add.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivityForResult(Images.picker(), REQ_PICK);
            }
        });
        root.addView(add);

        category("Graphics and sound");
        final boolean gles = "gles".equals(config.gpu);
        listPref("Adreno 200 renderer", gles ? "This phone's GPU" : "Software",
                new String[]{"Software", "This phone's GPU"},
                new String[]{"Everything on the CPU",
                        "The Adreno 200 runs on this phone's own GPU through OpenGL ES 3"},
                gles ? "This phone's GPU" : "Software", new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.gpu = index == 1 ? "gles" : "soft";
                        save();
                        rebuild();
                    }
                });
        note(gles ? "The host GPU is asked for an OpenGL ES 3 context and checked "
                        + "for what the renderer needs; the answer is in the emulator "
                        + "log. Software until the ES renderer lands."
                : "Software only: the phone's GPU has no desktop OpenGL 4.5, which "
                        + "is what the translated shaders ask for today.");
        divider();
        seekRow("GPU threads", 0, 8, config.gpuThreads, new Holo.Format() {
            @Override
            public String format(int v) {
                return threadsLabel(v);
            }
        }, new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                config.gpuThreads = v;
                save();
            }
        });
        note("0 leaves the choice to QEMU; more threads make the software renderer faster on "
                + "a phone with big cores.");
        divider();
        listPref("Audio output", "app".equals(config.audio) ? "This phone" : "Muted",
                new String[]{"This phone", "Muted"},
                new String[]{"QEMU's PCM stream played by AudioTrack", "No sound at all"},
                "app".equals(config.audio) ? "This phone" : "Muted", new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.audio = index == 0 ? "app" : "off";
                        emu.setVolume(config.volume);
                        save();
                        rebuild();
                    }
                });
        divider();
        seekRow("Volume", 0, 100, Math.round(config.volume * 100), new Holo.Format() {
            @Override
            public String format(int v) {
                return volumeLabel(v / 100f);
            }
        }, new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                config.volume = v / 100f;
                emu.setVolume(config.volume);
                save();
            }
        });

        category("Network");
        switchRow("WiFi", "Open network HD2-QEMU with internet through QEMU", config.wifi,
                new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        config.wifi = on;
                        save();
                    }
                });
        divider();
        switchRow("Mobile data", "The modem's packet data link", config.mobileData,
                new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        config.mobileData = on;
                        save();
                    }
                });
        divider();
        editRow("adb port", "adb connect 127.0.0.1:" + config.adbPort, "", "adbPort");

        category("Boot");
        listPref("Boot", bootLabel(config.boot), new String[]{"Boot from NAND", "Recovery",
                        "Stay in the bootloader"},
                new String[]{"Picks BOOT FROM NAND in the cLK menu", "Picks RECOVERY in the cLK menu",
                        "No key presses; also for MAGLDR images"},
                bootLabel(config.boot), new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.boot = index == 0 ? "nand" : index == 1 ? "recovery" : "menu";
                        save();
                        rebuild();
                    }
                });

        category("Debugging");
        switchRow("Show the frame rate", "Frames per second, over the screen", config.showFps,
                new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        config.showFps = on;
                        save();
                    }
                });
        divider();
        editRow("Extra QEMU arguments", config.extraArgs.isEmpty() ? "None" : config.extraArgs,
                "", "extra");
        divider();
        editRow("adb bridge port", String.valueOf(config.adbPort), "", "adbPort");
        note("USB LLE is not here: it hands the emulated phone to the host's own USB stack "
                + "through vhci_hcd, which needs root that an app can never have.");
    }

    private String basebandSummary() {
        return Config.LLE.equals(config.baseband) ? "Real AMSS firmware (LLE)" : "Modelled (HLE)";
    }

    private String shiftLabel(int shift) {
        return shift < 0 ? "auto" : "shift=" + shift;
    }

    private String bootLabel(String boot) {
        return "nand".equals(boot) ? "Boot from NAND" : "recovery".equals(boot) ? "Recovery"
                : "Stay in the bootloader";
    }

    private String threadsLabel(int n) {
        return n == 0 ? "auto" : n + " threads";
    }

    private String volumeLabel(float v) {
        return Math.round(v * 100) + "%";
    }

    private String nandSummary() {
        for (Images.File2 f : images.list()) {
            if (f.name.equals(config.nand)) return f.name + " · " + Config.gb(f.size);
        }
        return config.nand + " (not in the app's images folder)";
    }

    private String[] nandLabels() {
        java.util.List<Images.File2> all = images.list();
        String[] out = new String[all.size()];
        for (int i = 0; i < out.length; i++) out[i] = all.get(i).name;
        if (out.length == 0) out = new String[]{"nand.img"};
        return out;
    }

    private String[] nandSizes() {
        java.util.List<Images.File2> all = images.list();
        String[] out = new String[all.size()];
        for (int i = 0; i < out.length; i++) out[i] = Config.gb(all.get(i).size);
        if (out.length == 0) out = new String[]{"missing"};
        return out;
    }

    /* --------------------------------------------------------------- picking */

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != REQ_PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        final String name = Images.nameOf(this, uri);
        final android.app.ProgressDialog bar = new android.app.ProgressDialog(this);
        bar.setProgressStyle(android.app.ProgressDialog.STYLE_HORIZONTAL);
        bar.setIndeterminate(true);
        bar.setMessage("Copying " + name + "…");
        bar.setCancelable(false);
        bar.show();
        new Images.Import().run(this, uri, name, new Images.Import.Progress() {
            @Override
            public void onProgress(long done, long total) {
                if (total > 0) bar.setMax((int) (total / 1024));
                bar.setProgress((int) (done / 1024));
            }

            @Override
            public void onDone(final java.io.File out) {
                bar.dismiss();
                Toast.makeText(SettingsActivity.this, getString(R.string.imported, out.getName()),
                        Toast.LENGTH_LONG).show();
                rebuild();
            }

            @Override
            public void onError(final String why) {
                bar.dismiss();
                Toast.makeText(SettingsActivity.this, why, Toast.LENGTH_LONG).show();
            }
        });
    }
}
