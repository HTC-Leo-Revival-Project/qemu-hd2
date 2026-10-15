package org.htcleorevivalproject.hd2emu;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.util.Arrays;
import java.util.Comparator;

/**
 * The images folder: every NAND, microSD and firmware file the emulator can
 * use, with what each one is for - and the radio firmware (amss.elf,
 * dsp1.elf) taken out of a NAND image that has a radio in it, or out of a
 * radio NBH, so the real baseband and DSP can run.
 */
public class ImagesActivity extends HoloActivity {
    private static final int REQ_IMAGE = 1;
    private static final int REQ_RADIO = 2;

    private Holo.Page page;
    private Config config;
    private Emulator emu;

    private final Runnable build = new Runnable() {
        @Override
        public void run() {
            build();
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        config = new Config(this);
        emu = EmuService.instance(this);
        page = new Holo.Page(this);
        setContentView(page.scroll);
        Holo.apply(page.scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        rebuild();
    }

    private void rebuild() {
        config.load();
        page.rebuild(build);
    }

    private File dir() {
        return emu.paths().imagesDir;
    }

    private void build() {
        page.category(getString(R.string.img_cat_folder));
        File[] files = dir().listFiles();
        if (files == null) files = new File[0];
        Arrays.sort(files, new Comparator<File>() {
            @Override
            public int compare(File a, File b) {
                return a.getName().compareToIgnoreCase(b.getName());
            }
        });
        int shown = 0;
        for (final File f : files) {
            if (!f.isFile() || f.getName().endsWith(".part")) continue;
            shown++;
            page.row(f.getName(), Config.gb(f.length()) + " · " + role(f.getName()), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    actions(f);
                }
            });
        }
        if (shown == 0) page.note(getString(R.string.img_empty));
        page.row(getString(R.string.set_add_image), getString(R.string.img_add_sum),
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        startActivityForResult(Images.picker(), REQ_IMAGE);
                    }
                });
        page.note(getString(R.string.img_free, Config.gb(dir().getUsableSpace())));

        page.category(getString(R.string.img_cat_radio));
        boolean amss = new File(dir(), "amss.elf").isFile(), dsp = new File(dir(), "dsp1.elf").isFile();
        page.note(getString(R.string.img_radio_note) + " "
                + getString(amss ? R.string.img_amss_here : R.string.img_amss_missing) + " "
                + getString(dsp ? R.string.img_dsp_here : R.string.img_dsp_missing));
        page.row(getString(R.string.img_from_nand), getString(R.string.img_from_nand_sum, config.nand),
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        extract(new File(dir(), config.nand), true);
                    }
                });
        page.row(getString(R.string.img_from_nbh), getString(R.string.img_from_nbh_sum),
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        startActivityForResult(Images.picker(), REQ_RADIO);
                    }
                });
    }

    /** What the emulator uses a file for, as far as its name tells. */
    private String role(String name) {
        boolean running = emu.isRunning();
        if (name.equals(config.nand)) return getString(running ? R.string.img_role_nand_running : R.string.img_role_nand);
        if (name.equals(config.sdcard)) return getString(running ? R.string.img_role_sd_running : R.string.img_role_sd);
        if ("amss.elf".equals(name)) return getString(R.string.img_role_amss);
        if ("dsp1.elf".equals(name)) return getString(R.string.img_role_dsp);
        if (name.startsWith("sdcard")) return getString(R.string.img_role_sd_image);
        if (name.endsWith(".img")) return getString(R.string.img_role_nand_image);
        return getString(R.string.img_role_file);
    }

    private void actions(final File f) {
        final String name = f.getName();
        final boolean img = name.endsWith(".img");
        final String[] labels = img
                ? new String[]{getString(R.string.img_use_nand), getString(R.string.img_use_sd),
                        getString(R.string.img_delete)}
                : new String[]{getString(R.string.img_delete)};
        holo.list(name, labels, null, -1, new Holo.OnPick() {
            @Override
            public void onPick(int index) {
                int what = img ? index : 2;
                if (what == 0) {
                    config.nand = name;
                    if (name.equals(config.sdcard)) config.sdcard = "";
                    config.save();
                    toast(getString(R.string.img_now_nand, name));
                } else if (what == 1) {
                    if (name.equals(config.nand)) {
                        holo.alert(name, getString(R.string.img_is_nand));
                        return;
                    }
                    config.sdcard = name;
                    config.save();
                    toast(getString(R.string.img_now_sd, name));
                } else {
                    delete(f);
                    return;
                }
                rebuild();
            }
        });
    }

    private void delete(final File f) {
        final String name = f.getName();
        if (emu.isRunning() && (name.equals(config.nand) || name.equals(config.sdcard))) {
            holo.alert(name, getString(R.string.img_in_use));
            return;
        }
        new Holo.Box(this, getString(R.string.img_delete_title, name))
                .message(getString(R.string.img_delete_text, Config.gb(f.length())))
                .button(getString(R.string.cancel), null)
                .button(getString(R.string.img_delete), new Runnable() {
                    @Override
                    public void run() {
                        if (!f.delete()) {
                            toast(getString(R.string.img_delete_failed, name));
                        } else if (name.equals(config.sdcard)) {
                            config.sdcard = "";
                            config.save();
                        }
                        rebuild();
                    }
                }).show();
    }

    /** Pulls amss.elf and dsp1.elf out of a NAND image or a radio NBH, off the main thread. */
    private void extract(final File from, final boolean nand) {
        if (!from.isFile()) {
            holo.alert(getString(R.string.img_cat_radio), getString(R.string.img_not_in_folder, from.getName()));
            return;
        }
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.img_cat_radio),
                getString(R.string.img_reading, from.getName()));
        new Thread(new Runnable() {
            @Override
            public void run() {
                String result;
                try {
                    Radio.Firmware fw = nand ? Radio.fromNand(from) : Radio.fromNbh(from);
                    fw.save(dir());
                    result = "amss.elf: " + Config.gb(fw.amss.length)
                            + (fw.dsp != null ? "\ndsp1.elf: " + Config.gb(fw.dsp.length)
                                              : "\n" + getString(R.string.img_no_dsp));
                } catch (IOException e) {
                    result = null;
                    final String why = e.getMessage();
                    runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            bar.dismiss();
                            holo.alert(getString(R.string.img_cat_radio), why);
                        }
                    });
                }
                if (result == null) return;
                final String done = result;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        bar.dismiss();
                        holo.alert(getString(R.string.img_cat_radio), getString(R.string.img_taken_from, from.getName(), done));
                        rebuild();
                    }
                });
            }
        }, "radio-extract").start();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        final String name = Images.nameOf(this, uri);
        final boolean radio = request == REQ_RADIO;
        final File into = radio ? new File(getCacheDir(), "radio") : dir();
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.adding_image),
                getString(R.string.importing, name));
        new Images.Import().run(this, uri, name, into, new Images.Import.Progress() {
            @Override
            public void onProgress(long done, long total) {
                bar.progress(done, total);
            }

            @Override
            public void onDone(File out) {
                bar.dismiss();
                if (radio) {
                    extract(out, false);
                } else {
                    toast(getString(R.string.imported, out.getName()));
                    rebuild();
                }
            }

            @Override
            public void onError(String why) {
                bar.dismiss();
                holo.alert(getString(R.string.adding_image), why);
            }
        });
    }

    private void toast(String text) {
        Toast.makeText(this, text, Toast.LENGTH_LONG).show();
    }
}
