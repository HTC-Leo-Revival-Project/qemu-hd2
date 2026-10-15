package org.htcleorevivalproject.hd2emu;

import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;

/**
 * Builds a NAND image on the phone, of any of the three kinds the tools make:
 * Android on cLK (tools/hd2_mknand.py: cLK, a NAND ROM zip, optionally a
 * recovery), MAGLDR (tools/hd2_mkmagldr.py, plus a radio the way
 * hd2_radio.py --write-nand adds it) and stock Windows Mobile 6.5
 * (tools/hd2_wmnand.py).  The result lands in the app's images folder, where
 * the settings screen can pick it.
 *
 * The files are copied into the app's own storage first, because a content://
 * Uri from the file picker is not something QEMU (or java.util.zip) can open.
 */
public class NandActivity extends HoloActivity {
    private static final int PICK_NBH = 1;
    private static final int PICK_ROM = 2;
    private static final int PICK_REC = 3;
    private static final int PICK_MAGLDR = 4;
    private static final int PICK_RUU = 5;
    private static final int PICK_RADIO = 6;

    /** The output file each kind starts with, as in the tools' usage lines. */
    private static final String[] OUTPUT = {"nand.img", "nand-magldr.img", "nand-wm.img"};
    private static final int[] KINDS = {R.string.nand_kind_android, R.string.nand_kind_magldr,
            R.string.nand_kind_winmo};

    private Holo.Page page;
    private TextView status;
    private final StringBuilder log = new StringBuilder();
    private int kind = NandBuilder.ANDROID;
    private File nbh, lk, rom, recovery, magldr, ruu, radio;
    private int systemMb = 220;
    private int cacheMb = 30;
    private boolean softkeys = true;
    private boolean gpu;
    private String wifiMac = "00:23:76:48:44:32";
    private String btMac = "00:23:76:48:44:33";
    private String output = "nand.img";

    private final Runnable build = new Runnable() {
        @Override
        public void run() {
            build();
        }
    };

    private File sources() {
        File dir = new File(getFilesDir(), "sources");
        if (!dir.isDirectory()) dir.mkdirs();
        return dir;
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        page = new Holo.Page(this);
        setContentView(page.scroll);
        Holo.apply(page.scroll);
        page.rebuild(build);
    }

    private void pick(String title, String summary, final int request) {
        page.row(title, summary, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivityForResult(Images.picker(), request);
            }
        });
    }

    private void text(String title, String value, final String what) {
        page.edit(title, value, value, null, 0, new Holo.OnText() {
            @Override
            public void onText(String s) {
                s = s.trim();
                if (what.equals("out")) output = s;
                else if (what.equals("wifi")) wifiMac = s;
                else if (what.equals("bt")) btMac = s;
                page.rebuild(build);
            }
        });
    }

    private Holo.Format mib() {
        return new Holo.Format() {
            @Override
            public String format(int v) {
                return getString(R.string.nand_mib, v);
            }
        };
    }

    private void build() {
        page.category(getString(R.string.nand_cat_image));
        String[] kinds = new String[KINDS.length];
        for (int i = 0; i < kinds.length; i++) kinds[i] = getString(KINDS[i]);
        page.choice(getString(R.string.nand_kind), kinds[kind], kinds, new String[]{
                        getString(R.string.nand_kind_android_sum),
                        getString(R.string.nand_kind_magldr_sum),
                        getString(R.string.nand_kind_winmo_sum)},
                kind, new Holo.OnPick() {
                    @Override
                    public void onPick(int index) {
                        /* the output name follows the kind until it is typed in */
                        if (output.equals(OUTPUT[kind])) output = OUTPUT[index];
                        kind = index;
                        page.rebuild(build);
                    }
                });
        if (kind == NandBuilder.MAGLDR) {
            buildMagldr();
        } else if (kind == NandBuilder.WINMO) {
            buildWinMo();
        } else {
            buildAndroid();
        }

        Button go = Holo.button(this, getString(R.string.build_nand), true);
        go.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                start();
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Holo.dp(this, 52));
        lp.setMargins(Holo.dp(this, 16), Holo.dp(this, 24), Holo.dp(this, 16), Holo.dp(this, 4));
        page.addLoose(go, lp);

        status = Holo.text(this, log.toString(), 12, Holo.GREEN);
        status.setTypeface(android.graphics.Typeface.MONOSPACE);
        status.setPadding(Holo.dp(this, 16), Holo.dp(this, 8), Holo.dp(this, 16), 0);
        status.setVisibility(log.length() == 0 ? View.GONE : View.VISIBLE);
        page.addLoose(status, null);
    }

    private void buildMagldr() {
        page.category(getString(R.string.nand_cat_sources));
        pick(getString(R.string.nand_magldr), magldr != null ? magldr.getName()
                : getString(R.string.nand_magldr_hint), PICK_MAGLDR);
        pick(getString(R.string.nand_radio), radio != null ? radio.getName()
                : getString(R.string.nand_radio_hint), PICK_RADIO);
        page.note(getString(R.string.nand_magldr_note));

        page.category(getString(R.string.nand_cat_output));
        text(getString(R.string.nand_output), output, "out");

        page.category(getString(R.string.nand_cat_phone));
        text(getString(R.string.nand_wifi_mac), wifiMac, "wifi");
        text(getString(R.string.nand_bt_mac), btMac, "bt");
    }

    private void buildWinMo() {
        page.category(getString(R.string.nand_cat_sources));
        pick(getString(R.string.nand_stock_rom), ruu != null ? ruu.getName()
                : getString(R.string.nand_stock_rom_hint), PICK_RUU);
        pick(getString(R.string.nand_radio), radio != null ? radio.getName()
                : getString(R.string.nand_radio_stock_hint), PICK_RADIO);
        page.note(getString(R.string.nand_winmo_note));

        page.category(getString(R.string.nand_cat_output));
        text(getString(R.string.nand_output), output, "out");
    }

    private void buildAndroid() {
        page.category(getString(R.string.nand_cat_sources));
        pick(getString(R.string.nand_clk), nbh != null ? nbh.getName() : lk != null ? lk.getName()
                : getString(R.string.nand_clk_hint), PICK_NBH);
        pick(getString(R.string.nand_rom), rom != null ? rom.getName()
                : getString(R.string.nand_rom_hint), PICK_ROM);
        pick(getString(R.string.nand_recovery), recovery != null ? recovery.getName()
                : getString(R.string.nand_optional), PICK_REC);
        page.note(getString(R.string.nand_sd_note));

        page.category(getString(R.string.nand_cat_layout));
        page.seek(getString(R.string.nand_system), 8, 512, systemMb, mib(), new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                systemMb = v;
            }
        });
        page.seek(getString(R.string.nand_cache), 8, 512, cacheMb, mib(), new Holo.OnSeek() {
            @Override
            public void onSeek(int v) {
                cacheMb = v;
            }
        });
        text(getString(R.string.nand_output), output, "out");

        page.category(getString(R.string.nand_cat_phone));
        page.switchRow(getString(R.string.nand_softkeys), getString(R.string.nand_softkeys_sum),
                softkeys, new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        softkeys = on;
                    }
                });
        page.switchRow(getString(R.string.nand_gpu), getString(R.string.nand_gpu_sum),
                gpu, new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        gpu = on;
                    }
                });
        text(getString(R.string.nand_wifi_mac), wifiMac, "wifi");
        text(getString(R.string.nand_bt_mac), btMac, "bt");
    }

    private void showLog() {
        if (status == null) return;
        status.setText(log.toString());
        status.setVisibility(View.VISIBLE);
    }

    /* ------------------------------------------------------------ the build */

    private void start() {
        String missing = kind == NandBuilder.MAGLDR
                ? (magldr == null ? getString(R.string.nand_needs_magldr) : null)
                : kind == NandBuilder.WINMO
                ? (ruu == null ? getString(R.string.nand_needs_ruu) : null)
                : (nbh == null && lk == null) || rom == null ? getString(R.string.nand_needs_sources) : null;
        if (missing != null) {
            holo.alert(getString(R.string.nand_builder), missing);
            return;
        }
        /* the image is written whole (4096 blocks of raw pages): make sure it fits first */
        File target = new File(getFilesDir(), "images/" + output);
        long need = (long) Nand.NBLOCKS * Nand.PPB * Nand.RAW_PAGE;
        long free = getFilesDir().getUsableSpace() + (target.isFile() ? target.length() : 0);
        if (free < need + (16L << 20)) {
            holo.alert(getString(R.string.nand_builder),
                    getString(R.string.nand_no_room, Config.gb(need), Config.gb(free)));
            return;
        }
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.nand_builder),
                getString(R.string.preparing));

        final NandBuilder.Options o = new NandBuilder.Options();
        o.kind = kind;
        o.magldr = magldr;
        o.ruu = ruu;
        o.radio = radio;
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

        log.setLength(0);
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
                                bar.message(what);
                                showLog();
                            }
                        });
                    }

                    @Override
                    public void onDone(final File image) {
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                bar.dismiss();
                                log.append("wrote ").append(image.getName()).append('\n');
                                showLog();
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
                                log.append(why).append('\n');
                                showLog();
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
        if (request < PICK_NBH || request > PICK_RADIO || result != RESULT_OK
                || data == null || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        final String name = Images.nameOf(this, uri);
        final int req = request;
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.nand_builder),
                getString(R.string.importing, name));
        new Images.Import().run(this, uri, name, sources(), new Images.Import.Progress() {
            @Override
            public void onProgress(long done, long total) {
                bar.progress(done, total);
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
                } else if (req == PICK_MAGLDR) {
                    magldr = out;
                } else if (req == PICK_RUU) {
                    ruu = out;
                } else if (req == PICK_RADIO) {
                    radio = out;
                } else {
                    recovery = out;
                }
                page.rebuild(build);
            }

            @Override
            public void onError(final String why) {
                bar.dismiss();
                Toast.makeText(NandActivity.this, why, Toast.LENGTH_LONG).show();
            }
        });
    }
}
