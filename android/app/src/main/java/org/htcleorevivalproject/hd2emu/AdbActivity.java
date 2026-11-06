package org.htcleorevivalproject.hd2emu;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.view.View;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * In-app ADB: talks to the running guest's adbd through the emulator's adb bridge, with the
 * app's own adb client ({@link Adb}), so there is no adb binary anywhere.  What the host's adb
 * does day to day: devices, shell, push, pull, install, logcat.
 */
public class AdbActivity extends HoloActivity {
    private static final int REQ_PUSH = 51, REQ_INSTALL = 52, REQ_PULL = 53;

    private Holo.Page page;
    private Emulator emu;
    private String status = "";
    private File pullTo;                   // local temp file for a pull waiting for the save-as picker
    private String pullFrom;
    private String pushTo;

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
        page.rebuild(build);
        if (emu.isRunning()) probe();
    }

    private void build() {
        page.category(getString(R.string.adb_title));
        page.note(status.isEmpty() ? (emu.isRunning() ? getString(R.string.adb_checking)
                : getString(R.string.emu_not_running)) : status);
        action(R.string.adb_shell, R.string.adb_shell_sum, new Runnable() {
            @Override
            public void run() {
                holo.prompt(getString(R.string.adb_shell), "", "ls -l /system", InputType.TYPE_CLASS_TEXT,
                        new Holo.OnText() {
                            @Override
                            public void onText(String cmd) {
                                exec(cmd);
                            }
                        });
            }
        });
        action(R.string.adb_logcat, R.string.adb_logcat_sum, new Runnable() {
            @Override
            public void run() {
                exec("logcat -d -t 200");
            }
        });
        action(R.string.adb_push, R.string.adb_push_sum, new Runnable() {
            @Override
            public void run() {
                startActivityForResult(Images.picker(), REQ_PUSH);
            }
        });
        action(R.string.adb_pull, R.string.adb_pull_sum, new Runnable() {
            @Override
            public void run() {
                holo.prompt(getString(R.string.adb_pull), "/sdcard/", "/system/build.prop", InputType.TYPE_CLASS_TEXT,
                        new Holo.OnText() {
                            @Override
                            public void onText(String remote) {
                                pullFrom = remote.trim();
                                Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                                i.addCategory(Intent.CATEGORY_OPENABLE);
                                i.setType("*/*");
                                i.putExtra(Intent.EXTRA_TITLE, new File(pullFrom).getName());
                                startActivityForResult(i, REQ_PULL);
                            }
                        });
            }
        });
        action(R.string.adb_install, R.string.adb_install_sum, new Runnable() {
            @Override
            public void run() {
                startActivityForResult(Images.picker(), REQ_INSTALL);
            }
        });
    }

    private void action(int title, int sum, final Runnable r) {
        page.row(getString(title), getString(sum), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!emu.isRunning()) {
                    toast(getString(R.string.emu_not_running));
                    return;
                }
                r.run();
            }
        });
    }

    private void toast(final String s) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Toast.makeText(AdbActivity.this, s, Toast.LENGTH_LONG).show();
            }
        });
    }

    /** One connection per job; the adb bridge accepts as many as we like. */
    private Adb connect() throws IOException {
        Adb a = Adb.open(emu.running().adbPort, 3000);
        String banner = a.waitFor(3000);
        if (banner == null) {
            a.close();
            throw new IOException(getString(R.string.logs_adb_no_guest));
        }
        return a;
    }

    private void probe() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                String s;
                try {
                    Adb a = connect();
                    try {
                        String model = a.exec("getprop ro.product.model").trim();
                        s = getString(R.string.adb_connected, model.isEmpty() ? "?" : model);
                    } finally {
                        a.close();
                    }
                } catch (IOException e) {
                    s = getString(R.string.adb_offline, String.valueOf(e.getMessage()));
                }
                status = s;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        page.rebuild(build);
                    }
                });
            }
        }, "adb-probe").start();
    }

    private void exec(final String cmd) {
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.adb_title), cmd);
        new Thread(new Runnable() {
            @Override
            public void run() {
                String out;
                try {
                    Adb a = connect();
                    try {
                        out = a.exec(cmd);
                    } finally {
                        a.close();
                    }
                } catch (IOException e) {
                    out = String.valueOf(e.getMessage());
                }
                final String text = out.isEmpty() ? "(no output)" : out;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        bar.dismiss();
                        holo.alert(cmd, text.length() > 20000 ? text.substring(text.length() - 20000) : text);
                    }
                });
            }
        }, "adb-exec").start();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        if (request == REQ_PULL) {
            pullFile(uri);
            return;
        }
        final String name = Images.nameOf(this, uri);
        final File tmp = new File(getCacheDir(), "adb-" + name.replaceAll("[^A-Za-z0-9._-]", "_"));
        final boolean install = request == REQ_INSTALL;
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.adb_title), getString(R.string.importing, name));
        new Images.Import().run(this, uri, name, getCacheDir(), new Images.Import.Progress() {
            @Override
            public void onProgress(long done, long total) {
                bar.progress(done, total);
            }

            @Override
            public void onDone(final File local) {
                bar.dismiss();
                if (install) {
                    runJob(getString(R.string.adb_install) + " " + name, new Job() {
                        @Override
                        public String run(Adb a) throws IOException {
                            return a.install(local);
                        }
                    });
                } else {
                    holo.prompt(getString(R.string.adb_push), "/sdcard/" + local.getName(), "", InputType.TYPE_CLASS_TEXT,
                            new Holo.OnText() {
                                @Override
                                public void onText(final String dest) {
                                    runJob(getString(R.string.adb_push) + " " + dest, new Job() {
                                        @Override
                                        public String run(Adb a) throws IOException {
                                            a.push(local, dest.trim());
                                            return getString(R.string.adb_pushed, dest);
                                        }
                                    });
                                }
                            });
                }
            }

            @Override
            public void onError(String why) {
                bar.dismiss();
                toast(why);
            }
        });
    }

    private void pullFile(final Uri uri) {
        final String remote = pullFrom;
        runJob(getString(R.string.adb_pull) + " " + remote, new Job() {
            @Override
            public String run(Adb a) throws IOException {
                File tmp = new File(getCacheDir(), "pull.tmp");
                a.pull(remote, tmp);
                java.io.InputStream in = new java.io.FileInputStream(tmp);
                java.io.OutputStream out = getContentResolver().openOutputStream(uri);
                try {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                } finally {
                    in.close();
                    out.close();
                    tmp.delete();
                }
                return getString(R.string.adb_pulled, remote);
            }
        });
    }

    private interface Job {
        String run(Adb a) throws IOException;
    }

    private void runJob(final String title, final Job job) {
        final Holo.Progress bar = new Holo.Progress(this, getString(R.string.adb_title), title);
        new Thread(new Runnable() {
            @Override
            public void run() {
                String out;
                try {
                    Adb a = connect();
                    try {
                        out = job.run(a);
                    } finally {
                        a.close();
                    }
                } catch (IOException e) {
                    out = String.valueOf(e.getMessage());
                }
                final String text = out;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        bar.dismiss();
                        holo.alert(title, text);
                    }
                });
            }
        }, "adb-job").start();
    }
}
