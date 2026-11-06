package org.htcleorevivalproject.hd2emu;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.text.InputType;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * The SD-card editor: creates, opens and edits microSD images (FAT12/16/32, with or
 * without an MBR) in place, through {@link FatImage}: add files, make folders, delete,
 * rename, move, and copy files out.  No mounting and no root.
 *
 * Simple on purpose: one list, tap a folder to go in, tap a file for its actions, and the
 * menu adds a file or a folder to the folder you are in.  The image must not be in use by a
 * running emulator.
 */
public class SdEditorActivity extends HoloActivity {
    private static final int REQ_ADD = 41, REQ_EXPORT = 42;
    private static final int MENU_ADD = 1, MENU_FOLDER = 2, MENU_UP = 3, MENU_CLOSE = 4;

    private Holo.Page page;
    private Config config;
    private Emulator emu;
    private File image;                    // null: the chooser is shown
    private FatImage fat;
    private String dir = "/";
    private String exporting;              // path waiting for the save-as picker

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
        page = new Holo.Page(this);
        setContentView(page.scroll);
        Holo.apply(page.scroll);
        page.rebuild(build);
    }

    @Override
    protected void onDestroy() {
        closeImage();
        super.onDestroy();
    }

    private File imagesDir() {
        return emu.paths().imagesDir;
    }

    private void closeImage() {
        if (fat != null) {
            try {
                fat.close();
            } catch (IOException ignored) {
            }
            fat = null;
        }
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }

    /* --------------------------------------------------------------- screens */

    private void build() {
        invalidateOptionsMenu();
        if (fat == null) {
            chooser();
        } else {
            browser();
        }
    }

    /** Every image in the folder that is a FAT volume (NAND images are skipped by size). */
    private void chooser() {
        page.category(getString(R.string.sd_title));
        File[] files = imagesDir().listFiles();
        if (files == null) files = new File[0];
        java.util.Arrays.sort(files);
        int shown = 0;
        for (final File f : files) {
            if (!f.isFile() || !f.getName().endsWith(".img") || f.length() >= (long) Nand.NBLOCKS * Nand.PPB * Nand.RAW_PAGE) {
                continue;
            }
            shown++;
            page.row(f.getName(), Config.gb(f.length()) + (f.getName().equals(config.sdcard) ? " · " + getString(R.string.img_role_sd) : ""),
                    new View.OnClickListener() {
                        @Override
                        public void onClick(View v) {
                            open(f);
                        }
                    });
        }
        if (shown == 0) page.note(getString(R.string.sd_none));
        page.row(getString(R.string.sd_new), getString(R.string.sd_new_sum), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                createNew();
            }
        });
    }

    private void open(File f) {
        if (emu.isRunning() && f.getName().equals(config.sdcard)) {
            holo.alert(f.getName(), getString(R.string.img_in_use));
            return;
        }
        try {
            fat = FatImage.open(f);
            image = f;
            dir = "/";
            page.rebuild(build);
        } catch (IOException e) {
            holo.alert(f.getName(), getString(R.string.sd_not_fat, String.valueOf(e.getMessage())));
        }
    }

    private void createNew() {
        final String[] labels = {"64 MB", "256 MB", "1 GB", "2 GB", "4 GB"};
        final int[] mb = {64, 256, 1024, 2048, 4096};
        holo.list(getString(R.string.sd_new), labels, null, -1, new Holo.OnPick() {
            @Override
            public void onPick(final int index) {
                File out = new File(imagesDir(), "sdcard.img");
                for (int n = 2; out.exists(); n++) out = new File(imagesDir(), "sdcard" + n + ".img");
                final File target = out;
                if (imagesDir().getUsableSpace() < mb[index] * 1048576L + (16L << 20)) {
                    toast(getString(R.string.nand_no_room, Config.gb(mb[index] * 1048576L), Config.gb(imagesDir().getUsableSpace())));
                    return;
                }
                try {
                    FatImage.create(target, mb[index], "HD2SD");
                    open(target);
                } catch (IOException e) {
                    toast(String.valueOf(e.getMessage()));
                }
            }
        });
    }

    private void browser() {
        page.category(image.getName() + "  " + dir);
        List<FatImage.Entry> list;
        try {
            list = fat.list(dir);
        } catch (IOException e) {
            toast(String.valueOf(e.getMessage()));
            dir = "/";
            list = new ArrayList<FatImage.Entry>();
        }
        Collections.sort(list, new Comparator<FatImage.Entry>() {
            @Override
            public int compare(FatImage.Entry a, FatImage.Entry b) {
                if (a.dir != b.dir) return a.dir ? -1 : 1;
                return a.name.compareToIgnoreCase(b.name);
            }
        });
        if (!"/".equals(dir)) {
            page.row("..", getString(R.string.sd_up), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    up();
                }
            });
        }
        for (final FatImage.Entry e : list) {
            page.row((e.dir ? "[ ] " : "") + e.name, e.dir ? getString(R.string.sd_folder) : Config.gb(e.size),
                    new View.OnClickListener() {
                        @Override
                        public void onClick(View v) {
                            if (e.dir) {
                                dir = join(dir, e.name);
                                page.rebuild(build);
                            } else {
                                actions(e);
                            }
                        }
                    });
        }
        if (list.isEmpty()) page.note(getString(R.string.sd_empty));
        try {
            page.note(getString(R.string.sd_free, Config.gb(fat.freeBytes())));
        } catch (RuntimeException ignored) {
        }
    }

    private static String join(String dir, String name) {
        return "/".equals(dir) ? "/" + name : dir + "/" + name;
    }

    private void up() {
        int i = dir.lastIndexOf('/');
        dir = i <= 0 ? "/" : dir.substring(0, i);
        page.rebuild(build);
    }

    /* --------------------------------------------------------------- actions */

    private void actions(final FatImage.Entry e) {
        final String path = join(dir, e.name);
        String[] labels = {getString(R.string.sd_export), getString(R.string.sd_rename),
                getString(R.string.sd_move), getString(R.string.sd_delete)};
        holo.list(e.name, labels, null, -1, new Holo.OnPick() {
            @Override
            public void onPick(int index) {
                if (index == 0) {
                    exporting = path;
                    Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("*/*");
                    i.putExtra(Intent.EXTRA_TITLE, e.name);
                    startActivityForResult(i, REQ_EXPORT);
                } else if (index == 1) {
                    holo.prompt(getString(R.string.sd_rename), e.name, "", InputType.TYPE_CLASS_TEXT, new Holo.OnText() {
                        @Override
                        public void onText(String text) {
                            try {
                                fat.rename(path, text.trim());
                                page.rebuild(build);
                            } catch (IOException ex) {
                                toast(String.valueOf(ex.getMessage()));
                            }
                        }
                    });
                } else if (index == 2) {
                    holo.prompt(getString(R.string.sd_move), "/", getString(R.string.sd_move_hint), InputType.TYPE_CLASS_TEXT,
                            new Holo.OnText() {
                                @Override
                                public void onText(String text) {
                                    try {
                                        fat.move(path, text.trim(), e.name);
                                        page.rebuild(build);
                                    } catch (IOException ex) {
                                        toast(String.valueOf(ex.getMessage()));
                                    }
                                }
                            });
                } else {
                    try {
                        fat.delete(path);
                        page.rebuild(build);
                    } catch (IOException ex) {
                        toast(String.valueOf(ex.getMessage()));
                    }
                }
            }
        });
        /* a folder's actions are the same except export, which is for files */
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        menu.add(0, MENU_ADD, 0, R.string.sd_add);
        menu.add(0, MENU_FOLDER, 1, R.string.sd_folder_new);
        menu.add(0, MENU_UP, 2, R.string.sd_up);
        menu.add(0, MENU_CLOSE, 3, R.string.sd_close);
        return true;
    }

    @Override
    public boolean onPrepareOptionsMenu(Menu menu) {
        boolean open = fat != null;
        for (int i = 1; i <= 4; i++) menu.findItem(i).setVisible(open);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        switch (item.getItemId()) {
            case MENU_ADD:
                startActivityForResult(Images.picker(), REQ_ADD);
                return true;
            case MENU_FOLDER:
                holo.prompt(getString(R.string.sd_folder_new), "", "", InputType.TYPE_CLASS_TEXT, new Holo.OnText() {
                    @Override
                    public void onText(String text) {
                        try {
                            fat.mkdir(join(dir, text.trim()));
                            page.rebuild(build);
                        } catch (IOException e) {
                            toast(String.valueOf(e.getMessage()));
                        }
                    }
                });
                return true;
            case MENU_UP:
                up();
                return true;
            case MENU_CLOSE:
                closeImage();
                image = null;
                page.rebuild(build);
                return true;
            default:
                return super.onOptionsItemSelected(item);
        }
    }

    @Override
    public void onBackPressed() {
        if (fat != null && !"/".equals(dir)) {
            up();
        } else if (fat != null) {
            closeImage();
            image = null;
            page.rebuild(build);
        } else {
            super.onBackPressed();
        }
    }

    /* -------------------------------------------------------------- file I/O */

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null || fat == null) return;
        final Uri uri = data.getData();
        try {
            if (request == REQ_ADD) {
                String name = Images.nameOf(this, uri);
                ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
                try {
                    InputStream in = new ParcelFileDescriptor.AutoCloseInputStream(pfd);
                    fat.write(dir, name, in, pfd.getStatSize());
                } finally {
                    pfd.close();
                }
                page.rebuild(build);
            } else if (request == REQ_EXPORT && exporting != null) {
                OutputStream out = getContentResolver().openOutputStream(uri);
                try {
                    fat.read(exporting, out);
                } finally {
                    out.close();
                }
                toast(getString(R.string.sd_exported));
            }
        } catch (IOException e) {
            toast(String.valueOf(e.getMessage()));
        }
    }
}
