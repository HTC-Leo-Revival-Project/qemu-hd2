package org.qemu.htcleo;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Battery, charger and GPS: the three things the emulated phone's hardware
 * exposes that the machine lets you poke over QMP.
 */
public class SensorsActivity extends Activity {
    private Emulator emu;
    private Holo holo;
    private LinearLayout root;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        emu = EmuService.instance(this);
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

    @Override
    protected void onResume() {
        super.onResume();
        emu.refreshSensors();
        build();
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

    private void build() {
        final boolean on = emu.isRunning();

        category("Battery");
        seek("Charge level", 0, 100, emu.battery(), emu.battery() + "%", new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                emu.setBattery(v);
            }
        });
        note("The gauge is a 1230 mAh battery: it drains while the phone is up and charges "
                + "while it is plugged in.");

        category("Charger");
        list("Cable", cableLabel(emu.cable()), new String[]{"USB", "AC", "Nothing"},
                new String[]{"", "", ""}, cableIndex(emu.cable()), new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        emu.setCable(index == 0 ? "usb" : index == 1 ? "ac" : "none");
                        build();
                    }
                });

        category("GPS");
        gps();
        note("A fix moves the phone to where you tell it. With the real baseband (LLE) the "
                + "modem firmware has no GPS, so this is HLE only.");

        if (!on) note("Start the emulator to change any of this.");
    }

    private void gps() {
        Holo.Row row = new Holo.Row(this);
        row.setTitle("Set a position", "Latitude, longitude and altitude, like "
                + "qom-set /machine/modem gps-fix");
        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                final EditText field = new EditText(SensorsActivity.this);
                field.setBackgroundResource(R.drawable.holo_field);
                field.setTextColor(Color.WHITE);
                field.setSingleLine(true);
                field.setInputType(InputType.TYPE_CLASS_TEXT);
                field.setHint("48.137, 11.575, 520");
                LinearLayout box = new LinearLayout(SensorsActivity.this);
                box.setOrientation(LinearLayout.VERTICAL);
                box.setPadding(holo.dp(20), holo.dp(20), holo.dp(20), holo.dp(20));
                box.addView(field);
                android.app.AlertDialog d = new android.app.AlertDialog.Builder(SensorsActivity.this)
                        .setTitle("GPS fix")
                        .setView(box)
                        .setNegativeButton(R.string.cancel, null)
                        .setPositiveButton(R.string.nok,
                                new android.content.DialogInterface.OnClickListener() {
                                    @Override
                                    public void onClick(android.content.DialogInterface d, int w) {
                                        emu.setGps(field.getText().toString().trim());
                                    }
                                })
                        .create();
                holo.show(d);
            }
        });
        root.addView(row);
    }

    private String cableLabel(String cable) {
        return "usb".equals(cable) ? "USB" : "ac".equals(cable) ? "AC" : "Nothing";
    }

    private int cableIndex(String cable) {
        return "usb".equals(cable) ? 0 : "ac".equals(cable) ? 1 : 2;
    }

    private void seek(String title, int min, int max, int value, String label,
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
        val.setText(label);
        val.setTextSize(13f);
        val.setTypeface(android.graphics.Typeface.MONOSPACE);
        val.setTextColor(0xff33b5e5);
        head.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        head.addView(val);
        box.addView(head);
        box.addView(holo.seek(min, max, value, new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                val.setText(v + "%");
                onSeek.onSeek(v);
            }
        }));
        root.addView(box);
    }

    private void list(String title, String summary, String[] labels, String[] summaries,
                      int checked, final Holo.OnPick pick) {
        Holo.Row row = new Holo.Row(this);
        row.setTitle(title, summary);
        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                holo.list(title, labels, summaries, checked, pick);
            }
        });
        root.addView(row);
    }

    private void note(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(12.5f);
        t.setTextColor(0xff808080);
        t.setPadding(holo.dp(16), holo.dp(6), holo.dp(16), holo.dp(12));
        t.setLineSpacing(0, 1.2f);
        root.addView(t);
    }
}
