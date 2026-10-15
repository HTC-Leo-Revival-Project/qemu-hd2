package org.qemu.htcleo;

import android.app.Activity;
import android.app.ProgressDialog;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;

/**
 * Builds a NAND image on the phone, from the same inputs tools/hd2_mknand.py
 * takes: cLK (an NBH or a raw lk.bin), a NAND Android ROM zip and optionally a
 * recovery image.  The result lands in the app's images folder, where the
 * settings screen can pick it.
 *
 * The files are copied into the app's own storage first, because a content://
 * Uri from the file picker is not something QEMU (or java.util.zip) can open.
 */
public class NandActivity extends Activity {
    private static final int PICK_NBH = 1;
    private static final int PICK_ROM = 2;
    private static final int PICK_REC = 3;

    private Holo holo;
    private LinearLayout root;
    private TextView status;
    private File nbh, lk, rom, recovery;
    private int systemMb = 220;
    private int cacheMb = 30;
    private boolean softkeys = true;
    private boolean gpu;
    private String wifiMac = "00:23:76:48:44:32";
    private String btMac = "00:23:76:48:44:33";
    private String output = "nand.img";
    private boolean building;

    private File sources() {
        File dir = new File(getFilesDir(), "sources");
        if (!dir.isDirectory()) dir.mkdirs();
        return dir;
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        holo = new Holo(this);
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        setContentView(root);
        Holo.apply(root);
        if (getActionBar() != null) getActionBar().setDisplayHomeAsUpEnabled(true);
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

    private void category(String title) {
        TextView t = new TextView(this);
        t.setText(title);
        t.setTextSize(13f);
        t.setTypeface(null, android.graphics.Typeface.BOLD);
        t.setAllCaps(true);
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

    private void pick(final String title, final String summary, final int request,
                      final File current) {
        Holo.Row row = new Holo.Row(this);
        row.setTitle(title, summary);
        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivityForResult(Images.picker(), request);
            }
        });
        root.addView(row);
    }

    private void switchRow(String title, String summary, boolean on, final Holo.OnToggle toggle) {
        Holo.Row row = new Holo.Row(this);
        row.setTitle(title, summary);
        row.addWidget(holo.switchView(on, toggle));
        root.addView(row);
    }

    private void seekRow(String title, int value, final String suffix, final Holo.OnSeek onSeek) {
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
        val.setText(value + suffix);
        val.setTextSize(13f);
        val.setTypeface(android.graphics.Typeface.MONOSPACE);
        val.setTextColor(0xff33b5e5);
        head.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        head.addView(val);
        box.addView(head);
        box.addView(holo.seek(8, 512, value, new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                val.setText(v + suffix);
                onSeek.onSeek(v);
            }
        }));
        root.addView(box);
    }

    private void textRow(String title, String summary, final String value, final String what) {
        Holo.Row row = new Holo.Row(this);
        row.setTitle(title, summary);
        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                final EditText field = new EditText(NandActivity.this);
                field.setBackgroundResource(R.drawable.holo_field);
                field.setTextColor(Color.WHITE);
                field.setSingleLine(true);
                field.setInputType(InputType.TYPE_CLASS_TEXT);
                field.setText(value);
                LinearLayout box = new LinearLayout(NandActivity.this);
                box.setOrientation(LinearLayout.VERTICAL);
                box.setPadding(holo.dp(20), holo.dp(20), holo.dp(20), holo.dp(20));
                box.addView(field);
                android.app.AlertDialog d = new android.app.AlertDialog.Builder(NandActivity.this)
                        .setTitle(title)
                        .setView(box)
                        .setNegativeButton(R.string.cancel, null)
                        .setPositiveButton(R.string.nok,
                                new android.content.DialogInterface.OnClickListener() {
                                    @Override
                                    public void onClick(android.content.DialogInterface d, int w) {
                                        String s = field.getText().toString().trim();
                                        if (what.equals("out")) output = s;
                                        else if (what.equals("wifi")) wifiMac = s;
                                        else if (what.equals("bt")) btMac = s;
                                        build();
                                    }
                                })
                        .create();
                holo.show(d);
            }
        });
        root.addView(row);
    }

    private void build() {
        root.removeAllViews();
        building = false;

        category("Sources");
        pick("cLK bootloader", nbh != null ? nbh.getName() : lk != null ? lk.getName()
                : "LEOIMG.nbh (or a raw lk.bin)", PICK_NBH, nbh);
        pick("Android ROM zip", rom != null ? rom.getName()
                : "a NAND ROM: it needs kernel/, system/, data/ and mods/clk-ppp-patch/",
                PICK_ROM, rom);
        pick("Recovery image", recovery != null ? recovery.getName() : "optional",
                PICK_REC, recovery);
        note("An SD install zip does not work here: cLK installs from NAND only.");

        category("Layout");
        seekRow("System partition", systemMb, " MiB", new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                systemMb = v;
            }
        });
        seekRow("Cache partition", cacheMb, " MiB", new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                cacheMb = v;
            }
        });
        textRow("Output file", output, output, "out");

        category("Phone");
        switchRow("Soft buttons", "qemu.hw.mainkeys=0: the on-screen bar, like the phone",
                softkeys, new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        softkeys = on;
                    }
                });
        switchRow("Emulated Adreno 200", "Let the ROM use its own GPU libraries (slower, but real)",
                gpu, new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        gpu = on;
                    }
                });
        textRow("WiFi MAC", wifiMac, wifiMac, "wifi");
        textRow("Bluetooth MAC", btMac, btMac, "bt");

        Button go = new Button(this);
        go.setText(getString(R.string.build_nand));
        go.setAllCaps(false);
        go.setTextColor(Color.WHITE);
        go.setTextSize(15f);
        go.setBackgroundResource(R.drawable.holo_button_primary);
        go.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                start();
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                holo.dp(56));
        lp.setMargins(holo.dp(16), holo.dp(16), holo.dp(16), holo.dp(8));
        root.addView(go, lp);

        status = new TextView(this);
        status.setTypeface(android.graphics.Typeface.MONOSPACE);
        status.setTextSize(12f);
        status.setTextColor(0xff99cc00);
        status.setPadding(holo.dp(16), holo.dp(4), holo.dp(16), holo.dp(24));
        root.addView(status);
    }

    /* ------------------------------------------------------------ the build */

    private void start() {
        if ((nbh == null && lk == null) || rom == null) {
            new Holo(this).alert(getString(R.string.nand_builder),
                    getString(R.string.nand_needs_sources));
            return;
        }
        building = true;

        final ProgressDialog bar = new ProgressDialog(this);
        bar.setCancelable(false);
        bar.show();
        bar.setMessage(getString(R.string.preparing));

        final NandBuilder.Options o = new NandBuilder.Options();
        o.nbh = nbh;
        o.lk = lk;
        o.rom = rom;
        o.recovery = recovery;
        o.systemMb = systemMb;
        o.cacheMb = cacheMb;
        o.softkeys = softkeys;
        o.gpu = gpu;
        o.wifiMac = wifiMac;
        o.btMac = btMac;
        o.output = new File(getFilesDir(), "images/" + output);

        final StringBuilder log = new StringBuilder();
        new Thread(new Runnable() {
            @Override
            public void run() {
                new NandBuilder(o, new NandBuilder.Progress() {
                    @Override
                    public void onStep(final String what) {
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                log.append(what).append('\n');
                                bar.setMessage(what);
                                if (status != null) status.setText(log.toString());
                            }
                        });
                    }

                    @Override
                    public void onDone(final File image) {
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                bar.dismiss();
                                building = false;
                                log.append("wrote ").append(image.getName()).append('\n');
                                if (status != null) status.setText(log.toString());
                                Toast.makeText(NandActivity.this,
                                        getString(R.string.nand_done, image.getName()),
                                        Toast.LENGTH_LONG).show();
                            }
                        });
                    }

                    @Override
                    public void onError(final String why) {
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                bar.dismiss();
                                building = false;
                                log.append(why).append('\n');
                                if (status != null) status.setText(log.toString());
                            }
                        });
                    }
                }).run();
            }
        }, "nand-builder").start();
    }

    /* ------------------------------------------------------------- picking */

    @Override
    protected void onActivityResult(int request, int result, android.content.Intent data) {
        super.onActivityResult(request, result, data);
        if (request < PICK_NBH || request > PICK_REC || result != RESULT_OK
                || data == null || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        final String name = Images.nameOf(this, uri);
        final int req = request;
        final ProgressDialog bar = new ProgressDialog(this);
        bar.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);
        bar.setIndeterminate(true);
        bar.setCancelable(false);
        bar.setMessage(getString(R.string.importing, name));
        bar.show();
        new Images.Import().run(this, uri, name, sources(), new Images.Import.Progress() {
            @Override
            public void onProgress(long done, long total) {
                if (total > 0) bar.setMax((int) (total / 1024));
                bar.setProgress((int) (done / 1024));
            }

            @Override
            public void onDone(final File out) {
                bar.dismiss();
                if (req == PICK_NBH) {
                    if (name.toLowerCase().endsWith(".bin")) lk = out;
                    else {
                        nbh = out;
                        lk = null;
                    }
                } else if (req == PICK_ROM) {
                    rom = out;
                } else {
                    recovery = out;
                }
                build();
            }

            @Override
            public void onError(final String why) {
                bar.dismiss();
                Toast.makeText(NandActivity.this, why, Toast.LENGTH_LONG).show();
            }
        });
    }
}
