package org.htcleorevivalproject.hd2emu;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.view.View;
import android.widget.Toast;

/**
 * The same knobs the web UI's Config panel has, minus the ones that cannot
 * work in an app: no host GPU (Android has no OpenGL 4.5 with fragment shader
 * interlock), and no USB LLE (that needs vhci_hcd and root).  Everything else
 * takes effect the next time the emulator is started.
 */
public class SettingsActivity extends HoloActivity {
    private static final int REQ_PICK = 1;

    private Config config;
    private Emulator emu;
    private Images images;
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
        config = new Config(this);
        config.load();
        emu = EmuService.instance(this);
        images = new Images(emu.paths());

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

    private void save() {
        config.save();
    }

    /* --------------------------------------------------------------- the page */

    private void build() {
        final boolean lle = Config.LLE.equals(config.baseband);

        if (emu.isRunning()) {
            page.banner(getString(R.string.set_running_banner),
                    0x3333b5e5);
        }

        page.category(getString(R.string.set_cat_processors));
        page.choice(getString(R.string.set_baseband),
                getString(lle ? R.string.set_baseband_lle : R.string.set_baseband_hle),
                new String[]{getString(R.string.set_baseband_hle), getString(R.string.set_baseband_lle)},
                new String[]{getString(R.string.set_baseband_hle_sum),
                        getString(images.amss() ? R.string.set_baseband_lle_sum : R.string.set_needs_amss)},
                lle ? 1 : 0, new Holo.OnPick() {
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
        boolean dsp = Config.LLE.equals(config.dsp);
        page.choice(getString(R.string.set_dsp),
                getString(dsp ? R.string.set_dsp_lle : R.string.set_dsp_off),
                new String[]{getString(R.string.set_dsp_off), getString(R.string.set_dsp_lle)},
                new String[]{getString(R.string.set_dsp_off_sum),
                        getString(images.dsp1() ? R.string.set_dsp_lle_sum : R.string.set_needs_dsp1)},
                dsp ? 1 : 0, new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.dsp = index == 1 ? Config.LLE : Config.HLE;
                        save();
                        rebuild();
                    }
                });
        if (!lle) page.note(getString(R.string.set_dsp_note));
        if (config.dspRunning()) {
            page.choice(getString(R.string.set_dsp_clock), shiftLabel(config.dspShift),
                    new String[]{getString(R.string.set_auto), "shift=0", "shift=1", "shift=2", "shift=3", "shift=4"},
                    new String[]{getString(R.string.set_shift_auto_sum),
                            getString(R.string.set_shift_0_sum), getString(R.string.set_shift_1_sum), "", "", ""},
                    config.dspShift + 1, new Holo.OnPick() {
                        @Override
                        public void onPick(int index) {
                            config.dspShift = index - 1;
                            save();
                            rebuild();
                        }
                    });
        }

        page.category(getString(R.string.set_cat_images));
        final String[] nands = nandLabels();
        int nandIndex = 0;
        for (int i = 0; i < nands.length; i++) if (nands[i].equals(config.nand)) nandIndex = i;
        page.choice(getString(R.string.set_nand), nandSummary(), nands, nandSizes(), nandIndex,
                new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.nand = nands[index];
                        save();
                        rebuild();
                    }
                });
        final java.util.List<String> cards = new java.util.ArrayList<String>();
        cards.add(getString(R.string.set_none));
        for (Images.File2 f : images.list()) if (!f.name.equals(config.nand)) cards.add(f.name);
        final String[] cardLabels = cards.toArray(new String[0]);
        int cardIndex = Math.max(0, cards.indexOf(config.sdcard));
        page.choice(getString(R.string.set_sdcard), config.sdcard.isEmpty() ? getString(R.string.set_none)
                        : images.has(config.sdcard) ? config.sdcard
                        : getString(R.string.set_not_in_folder, config.sdcard),
                cardLabels, null, cardIndex, new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.sdcard = index == 0 ? "" : cardLabels[index];
                        save();
                        rebuild();
                    }
                });
        page.row(getString(R.string.set_manage_images), getString(R.string.set_manage_images_sum),
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        startActivity(new Intent(SettingsActivity.this, ImagesActivity.class));
                    }
                });
        page.row(getString(R.string.set_add_image), getString(R.string.set_add_image_sum),
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        startActivityForResult(Images.picker(), REQ_PICK);
                    }
                });

        page.category(getString(R.string.set_cat_graphics));
        final boolean gles = "gles".equals(config.gpu);
        page.choice(getString(R.string.set_renderer),
                getString(gles ? R.string.set_renderer_gles : R.string.set_renderer_soft),
                new String[]{getString(R.string.set_renderer_soft), getString(R.string.set_renderer_gles)},
                new String[]{getString(R.string.set_renderer_soft_sum), getString(R.string.set_renderer_gles_sum)},
                gles ? 1 : 0, new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.gpu = index == 1 ? "gles" : "soft";
                        save();
                        rebuild();
                    }
                });
        page.seek(getString(R.string.set_gpu_threads), 0, 8, config.gpuThreads, new Holo.Format() {
            @Override
            public String format(int v) {
                return v == 0 ? getString(R.string.set_auto) : v == 1 ? getString(R.string.set_one_thread)
                        : getString(R.string.set_threads, v);
            }
        }, new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                config.gpuThreads = v;
                save();
            }
        });
        page.note(getString(gles ? R.string.set_gles_note : R.string.set_soft_note));
        final boolean sound = "app".equals(config.audio);
        page.choice(getString(R.string.set_audio),
                getString(sound ? R.string.set_audio_phone : R.string.set_audio_muted),
                new String[]{getString(R.string.set_audio_phone), getString(R.string.set_audio_muted)},
                new String[]{getString(R.string.set_audio_phone_sum), getString(R.string.set_audio_muted_sum)},
                sound ? 0 : 1, new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.audio = index == 0 ? "app" : "off";
                        emu.setVolume(config.volume);
                        save();
                        rebuild();
                    }
                });
        page.seek(getString(R.string.set_volume), 0, 100, Math.round(config.volume * 100), new Holo.Format() {
            @Override
            public String format(int v) {
                return getString(R.string.percent, v);
            }
        }, new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                config.volume = v / 100f;
                emu.setVolume(config.volume);
                save();
            }
        });

        page.category(getString(R.string.set_cat_screen));
        page.switchRow(getString(R.string.set_keep_on), getString(R.string.set_keep_on_sum),
                config.keepScreenOn, new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        config.keepScreenOn = on;
                        save();
                    }
                });

        page.category(getString(R.string.set_cat_network));
        page.switchRow(getString(R.string.set_wifi), getString(R.string.set_wifi_sum), config.wifi,
                new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        config.wifi = on;
                        save();
                    }
                });
        page.switchRow(getString(R.string.set_mobile_data), getString(R.string.set_mobile_data_sum), config.mobileData,
                new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        config.mobileData = on;
                        save();
                    }
                });
        page.edit(getString(R.string.set_adb_port), "adb connect 127.0.0.1:" + config.adbPort,
                String.valueOf(config.adbPort), "5555", InputType.TYPE_CLASS_NUMBER,
                new Holo.OnText() {
                    @Override
                    public void onText(String s) {
                        config.adbPort = Math.max(1024, Math.min(65535, number(s, 5555)));
                        save();
                        rebuild();
                    }
                });

        page.category(getString(R.string.set_cat_boot));
        page.choice(getString(R.string.set_boot), bootLabel(config.boot),
                new String[]{getString(R.string.set_boot_nand), getString(R.string.set_boot_recovery),
                        getString(R.string.set_boot_menu)},
                new String[]{getString(R.string.set_boot_nand_sum), getString(R.string.set_boot_recovery_sum),
                        getString(R.string.set_boot_menu_sum)},
                "nand".equals(config.boot) ? 0 : "recovery".equals(config.boot) ? 1 : 2,
                new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        config.boot = index == 0 ? "nand" : index == 1 ? "recovery" : "menu";
                        save();
                        rebuild();
                    }
                });

        page.category(getString(R.string.set_cat_debugging));
        if (lle) {
            page.switchRow(getString(R.string.set_clk_regime), getString(R.string.set_clk_regime_sum),
                    config.clkRegimeFake, new Holo.OnToggle() {
                        @Override
                        public void onToggle(boolean on) {
                            config.clkRegimeFake = on;
                            save();
                        }
                    });
        }
        page.switchRow(getString(R.string.set_show_fps), getString(R.string.set_show_fps_sum), config.showFps,
                new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        config.showFps = on;
                        save();
                    }
                });
        page.edit(getString(R.string.set_extra_args), config.extraArgs.isEmpty() ? getString(R.string.set_none) : config.extraArgs,
                config.extraArgs, "-d guest_errors", 0, new Holo.OnText() {
                    @Override
                    public void onText(String s) {
                        config.extraArgs = s;
                        save();
                        rebuild();
                    }
                });
        page.edit(getString(R.string.set_adb_port), String.valueOf(config.adbPort),
                String.valueOf(config.adbPort), "5555", InputType.TYPE_CLASS_NUMBER,
                new Holo.OnText() {
                    @Override
                    public void onText(String s) {
                        config.adbPort = Math.max(1024, Math.min(65535, number(s, 5555)));
                        save();
                        rebuild();
                    }
                });
        page.note(getString(R.string.set_usb_lle_note));
    }

    private static int number(String s, int fallback) {
        try {
            return Integer.parseInt(s.trim());
        } catch (Exception e) {
            return fallback;
        }
    }

    private String shiftLabel(int shift) {
        return shift < 0 ? getString(R.string.set_auto) : "shift=" + shift;
    }

    private String bootLabel(String boot) {
        return getString("nand".equals(boot) ? R.string.set_boot_nand
                : "recovery".equals(boot) ? R.string.set_boot_recovery : R.string.set_boot_menu);
    }

    private String nandSummary() {
        for (Images.File2 f : images.list()) {
            if (f.name.equals(config.nand)) return f.name + " · " + Config.gb(f.size);
        }
        return getString(R.string.set_not_in_folder, config.nand);
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
        if (out.length == 0) out = new String[]{getString(R.string.set_missing)};
        return out;
    }

    /* --------------------------------------------------------------- picking */

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != REQ_PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        final String name = Images.nameOf(this, uri);
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.adding_image),
                getString(R.string.importing, name));
        new Images.Import().run(this, uri, name, new Images.Import.Progress() {
            @Override
            public void onProgress(long done, long total) {
                bar.progress(done, total);
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
