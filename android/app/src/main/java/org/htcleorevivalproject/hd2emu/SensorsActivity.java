package org.htcleorevivalproject.hd2emu;

import android.os.Bundle;
import android.text.InputType;

/**
 * Battery, charger and GPS: the three things the emulated phone's hardware
 * exposes that the machine lets you poke over QMP.
 */
public class SensorsActivity extends HoloActivity {
    private Emulator emu;
    private Holo.Page page;

    private final Runnable build = new Runnable() {
        @Override
        public void run() {
            build();
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        emu = EmuService.instance(this);
        page = new Holo.Page(this);
        setContentView(page.scroll);
        Holo.apply(page.scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        emu.refreshSensors();
        page.rebuild(build);
    }

    private void build() {
        final boolean on = emu.isRunning();
        page.category(getString(R.string.sen_cat_battery));
        android.widget.SeekBar charge = page.seek(getString(R.string.sen_charge), 0, 100, emu.battery(), new Holo.Format() {
            @Override
            public String format(int v) {
                return getString(R.string.percent, v);
            }
        }, new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                emu.setBattery(v);
            }
        });
        page.note(getString(R.string.sen_battery_note));

        page.category(getString(R.string.sen_cat_charger));
        String cable = emu.cable();
        page.choice(getString(R.string.sen_cable), cableLabel(cable),
                new String[]{getString(R.string.sen_usb), getString(R.string.sen_ac), getString(R.string.sen_nothing)},
                new String[]{getString(R.string.sen_usb_sum), getString(R.string.sen_ac_sum),
                        getString(R.string.sen_nothing_sum)},
                "usb".equals(cable) ? 0 : "ac".equals(cable) ? 1 : 2, new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        emu.setCable(index == 0 ? "usb" : index == 1 ? "ac" : "none");
                        page.rebuild(build);
                    }
                });

        page.category(getString(R.string.sen_cat_gps));
        page.edit(getString(R.string.sen_position), getString(R.string.sen_position_sum), "",
                "48.137, 11.575, 520",
                InputType.TYPE_CLASS_TEXT, new Holo.OnText() {
                    @Override
                    public void onText(String s) {
                        emu.setGps(s.trim());
                    }
                });
        page.note(getString(R.string.sen_gps_note));

        if (!on) page.note(getString(R.string.sen_off_note));
    }

    private String cableLabel(String cable) {
        return getString("usb".equals(cable) ? R.string.sen_usb : "ac".equals(cable) ? R.string.sen_ac
                : R.string.sen_nothing);
    }
}
