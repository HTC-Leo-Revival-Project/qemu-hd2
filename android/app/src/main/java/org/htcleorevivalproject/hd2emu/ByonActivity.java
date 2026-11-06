package org.htcleorevivalproject.hd2emu;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Toast;

import java.io.File;

/**
 * BYON, bring your own NAND.  The app ships only the SPLs (see Spl); everything else of the
 * user's NAND comes from a STOCK RUU the user picks (never a custom ROM: the RUU's radio,
 * OS area and stock SPL are what the base image is made of).  Shown on first launch, and
 * from Settings at any time.
 *
 * Two ways in:
 * <ul>
 * <li>pick a stock RUU: builds the base NAND (the Windows Mobile build of NandBuilder);</li>
 * <li>create the minimal NAND: just the HSPL and the boot chain's records, enough to boot the
 *     SPL and flash an NBH from the card with it.</li>
 * </ul>
 */
public class ByonActivity extends HoloActivity {
    private static final int REQ_RUU = 31;

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
        page.rebuild(build);
    }

    private void build() {
        page.category(getString(R.string.byon_title));
        page.note(getString(R.string.byon_intro));
        page.row(getString(R.string.byon_pick), getString(R.string.byon_pick_sum), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivityForResult(Images.picker(), REQ_RUU);
            }
        });
        page.row(getString(R.string.byon_minimal), getString(R.string.byon_minimal_sum), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                minimal();
            }
        });
        page.note(getString(R.string.byon_stock_only));
    }

    private File images() {
        return emu.paths().imagesDir;
    }

    private boolean roomFor(File target) {
        long need = (long) Nand.NBLOCKS * Nand.PPB * Nand.RAW_PAGE;
        long free = images().getUsableSpace() + (target.isFile() ? target.length() : 0);
        if (free < need + (16L << 20)) {
            holo.alert(getString(R.string.byon_title),
                    getString(R.string.nand_no_room, Config.gb(need), Config.gb(free)));
            return false;
        }
        return true;
    }

    private void useNand(String name) {
        config.load();
        config.nand = name;
        config.save();
        Toast.makeText(this, getString(R.string.img_now_nand, name), Toast.LENGTH_LONG).show();
    }

    private void minimal() {
        final File out = new File(images(), "nand-minimal.img");
        if (emu.isRunning() && "nand-minimal.img".equals(config.nand)) {
            holo.alert(getString(R.string.byon_title), getString(R.string.img_in_use));
            return;
        }
        if (!roomFor(out)) return;
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.byon_title), getString(R.string.preparing));
        new Thread(new Runnable() {
            @Override
            public void run() {
                String err = null;
                try {
                    Spl.createMinimal(out, Spl.hspl(ByonActivity.this));
                } catch (Throwable e) {
                    err = String.valueOf(e.getMessage());
                }
                final String why = err;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        bar.dismiss();
                        if (why != null) {
                            Toast.makeText(ByonActivity.this, why, Toast.LENGTH_LONG).show();
                            return;
                        }
                        useNand(out.getName());
                        config.load();
                        config.splLle = true;
                        config.save();
                    }
                });
            }
        }, "byon-minimal").start();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != REQ_RUU || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        final String name = Images.nameOf(this, uri);
        final File sources = new File(getFilesDir(), "sources");
        if (!sources.isDirectory()) sources.mkdirs();
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.byon_title),
                getString(R.string.importing, name));
        new Images.Import().run(this, uri, name, sources, new Images.Import.Progress() {
            @Override
            public void onProgress(long done, long total) {
                bar.progress(done, total);
            }

            @Override
            public void onDone(File ruu) {
                bar.dismiss();
                fromRuu(ruu);
            }

            @Override
            public void onError(String why) {
                bar.dismiss();
                Toast.makeText(ByonActivity.this, why, Toast.LENGTH_LONG).show();
            }
        });
    }

    private void fromRuu(final File ruu) {
        final File out = new File(images(), "nand.img");
        if (emu.isRunning() && "nand.img".equals(config.nand)) {
            holo.alert(getString(R.string.byon_title), getString(R.string.img_in_use));
            return;
        }
        if (!roomFor(out)) return;
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.byon_title), getString(R.string.preparing));
        final NandBuilder.Options o = new NandBuilder.Options();
        o.kind = NandBuilder.WINMO;
        o.ruu = ruu;
        o.output = out;
        new Thread(new Runnable() {
            @Override
            public void run() {
                new NandBuilder(o, new NandBuilder.Progress() {
                    @Override
                    public void onStep(final String what) {
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                bar.message(what);
                            }
                        });
                    }

                    @Override
                    public void onDone(File image) {
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                bar.dismiss();
                                useNand("nand.img");
                                config.load();
                                config.splLle = true;
                                config.save();
                            }
                        });
                    }

                    @Override
                    public void onError(final String why) {
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                bar.dismiss();
                                holo.alert(getString(R.string.byon_title), why);
                            }
                        });
                    }
                }).run();
            }
        }, "byon-ruu").start();
    }
}
