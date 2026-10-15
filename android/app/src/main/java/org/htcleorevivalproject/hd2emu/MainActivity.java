package org.htcleorevivalproject.hd2emu;

import android.app.ActionBar;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Bitmap;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;

/**
 * The screen and the keys.  The machine itself is driven from the action bar
 * (power and screenshot as icons, the rest in the overflow), everything else
 * (settings, sensors, logs, the NAND builder) is another activity behind it,
 * so there is no navigation framework here either.
 */
public class MainActivity extends Activity implements ScreenView.Input {
    private static final int REQ_NOTIFICATIONS = 7;

    private static final int MENU_POWER = 1;
    private static final int MENU_SHOT = 2;
    private static final int MENU_RESTART = 3;
    private static final int MENU_RESET = 4;
    private static final int MENU_SENSORS = 5;
    private static final int MENU_LOGS = 6;
    private static final int MENU_NAND = 7;
    private static final int MENU_SETTINGS = 8;
    private static final int MENU_ABOUT = 9;
    private static final int MENU_IMAGES = 10;
    private static final int MENU_ROOT = 11;

    /**
     * The HD2's keys, left to right: the volume rocker on its side, the five
     * under the screen, and the power key on top.  Null starts a new group.
     */
    private static final Object[][] KEYS = {
            {"voldown", HoloIcons.VOL_DOWN, R.string.key_voldown},
            {"volup", HoloIcons.VOL_UP, R.string.key_volup},
            null,
            {"call", HoloIcons.CALL, R.string.key_call},
            {"home", HoloIcons.HOME, R.string.key_home},
            {"menu", HoloIcons.WINDOWS, R.string.key_menu},
            {"back", HoloIcons.BACK, R.string.key_back},
            {"end", HoloIcons.END, R.string.key_end},
            null,
            {"power", HoloIcons.POWER, R.string.key_power},
    };

    private Emulator emu;
    private EmuService service;
    private ScreenView screen;
    private View empty;
    private ImageView emptyIcon;
    private View spinner;
    private TextView emptyTitle, emptyText, fpsText;
    private LinearLayout emptyButtons, keys;
    private Config config;
    private Emulator.State shown;
    /** Between onStart and onStop: the screen can be seen, so it wants frames. */
    private boolean visible;
    private final Handler clock = new Handler(Looper.getMainLooper());
    private final java.util.Map<String, View> keyViews = new java.util.HashMap<String, View>();

    private final ServiceConnection binding = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            EmuService.LocalBinder b = (EmuService.LocalBinder) binder;
            service = b.service();
            attach(b.get());
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            service = null;
        }
    };

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            status();
            clock.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        config = new Config(this);
        config.load();
        setContentView(R.layout.activity_main);
        Holo.blackBars(getWindow());

        screen = (ScreenView) findViewById(R.id.screen);
        empty = findViewById(R.id.empty);
        emptyIcon = (ImageView) findViewById(R.id.empty_icon);
        spinner = findViewById(R.id.spinner);
        emptyTitle = (TextView) findViewById(R.id.empty_title);
        emptyText = (TextView) findViewById(R.id.empty_text);
        emptyButtons = (LinearLayout) findViewById(R.id.empty_buttons);
        fpsText = (TextView) findViewById(R.id.fps);
        keys = (LinearLayout) findViewById(R.id.keys);

        screen.setInput(this);
        buildKeys();
        Holo.apply(findViewById(R.id.root), keys);

        emu = EmuService.instance(this);
        bindService(new Intent(this, EmuService.class), binding, Context.BIND_AUTO_CREATE);
        askNotifications();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    @Override
    protected void onStart() {
        super.onStart();
        visible = true;
        if (emu != null) emu.setVideo(true);
    }

    @Override
    protected void onStop() {
        super.onStop();
        visible = false;
        /* nobody sees the screen: QEMU stops sending frames, the sound goes on */
        if (emu != null) emu.setVideo(false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        config.load();
        if (service != null) attach(service.machine());
        else render();
        clock.post(tick);
        screen.requestFocus();
    }

    @Override
    protected void onPause() {
        super.onPause();
        clock.removeCallbacks(tick);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        try {
            unbindService(binding);
        } catch (IllegalArgumentException ignored) {
        }
        if (emu != null) emu.removeListener(listener);
        screen.release();
    }

    private void attach(Emulator e) {
        emu.removeListener(listener);
        emu = e;
        emu.addListener(listener);
        emu.setVideo(visible);
        listener.onState(emu.state(), emu.message());
        render();
    }

    /* ------------------------------------------------------------------ the keys */

    private void buildKeys() {
        int height = getResources().getDimensionPixelSize(R.dimen.key_height);
        for (Object[] spec : KEYS) {
            if (spec == null) {
                View rule = new View(this);
                rule.setBackgroundColor(0x26ffffff);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(1, Holo.dp(this, 28));
                keys.addView(rule, lp);
                continue;
            }
            final String hardKey = (String) spec[0];
            int glyph = (Integer) spec[1];
            int color = "call".equals(hardKey) ? Holo.GREEN
                    : "end".equals(hardKey) ? Holo.RED : HoloIcons.WHITE;

            ImageView v = new ImageView(this);
            v.setImageDrawable(HoloIcons.get(this, glyph, 26, color));
            v.setScaleType(ImageView.ScaleType.CENTER);
            v.setBackgroundResource(R.drawable.holo_key);
            v.setContentDescription(getString((Integer) spec[2]));
            v.setClickable(true);
            v.setOnTouchListener(new View.OnTouchListener() {
                @Override
                public boolean onTouch(View view, MotionEvent e) {
                    if (!view.isEnabled()) return false;
                    switch (e.getActionMasked()) {
                        case MotionEvent.ACTION_DOWN:
                            emu.key(hardKey, true);
                            view.setActivated(true);
                            view.performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
                            return true;
                        case MotionEvent.ACTION_UP:
                        case MotionEvent.ACTION_CANCEL:
                            emu.key(hardKey, false);
                            view.setActivated(false);
                            return true;
                    }
                    return false;
                }
            });
            keys.addView(v, new LinearLayout.LayoutParams(0, height, 1f));
            keyViews.put(hardKey, v);
        }
    }

    private void setKeysEnabled(boolean on) {
        for (View v : keyViews.values()) {
            v.setEnabled(on);
            v.setAlpha(on ? 1f : 0.3f);
        }
    }

    /* ----------------------------------------------------------- the state */

    private void render() {
        if (emu == null) return;
        Emulator.State s = emu.state();
        boolean on = s == Emulator.State.RUNNING;
        screen.setVisibility(on ? View.VISIBLE : View.INVISIBLE);
        empty.setVisibility(on ? View.GONE : View.VISIBLE);
        setKeysEnabled(on);
        if (on && config.keepScreenOn) {
            getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
        if (s != shown) {
            shown = s;
            emptyState(s);
            invalidateOptionsMenu();
        }
        status();
    }

    /** What takes the screen's place while there is no picture. */
    private void emptyState(Emulator.State s) {
        emptyButtons.removeAllViews();
        boolean busy = s == Emulator.State.STARTING || s == Emulator.State.STOPPING;
        spinner.setVisibility(busy ? View.VISIBLE : View.GONE);
        emptyIcon.setVisibility(busy ? View.GONE : View.VISIBLE);
        emptyTitle.setText(stateLabel(s));
        switch (s) {
            case STARTING:
                emptyText.setText(R.string.starting_blurb);
                break;
            case STOPPING:
                emptyText.setText(R.string.stopping_blurb);
                break;
            case CRASHED: {
                emptyIcon.setImageDrawable(HoloIcons.get(this, HoloIcons.WARNING, 96, 0xccff4444));
                String m = emu.message();
                emptyText.setText(m == null || m.isEmpty() ? getString(R.string.crashed_blurb) : m);
                Button logs = Holo.button(this, getString(R.string.view_logs), false);
                logs.setOnClickListener(new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        startActivity(new Intent(MainActivity.this, LogsActivity.class));
                    }
                });
                emptyButtons.addView(logs, buttonParams());
                emptyButtons.addView(powerButton(), buttonParams());
                break;
            }
            default:
                emptyIcon.setImageDrawable(HoloIcons.get(this, HoloIcons.POWER, 96, 0x4dffffff));
                emptyText.setText(R.string.powered_off_blurb);
                emptyButtons.addView(powerButton(), buttonParams());
                break;
        }
    }

    private Button powerButton() {
        Button b = Holo.button(this, getString(R.string.power_on), true);
        b.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                power();
            }
        });
        return b;
    }

    private LinearLayout.LayoutParams buttonParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = Holo.dp(this, 6);
        lp.rightMargin = Holo.dp(this, 6);
        return lp;
    }

    /** The action bar's subtitle: a coloured dot, the state, and how long it has been up. */
    private void status() {
        ActionBar bar = getActionBar();
        if (bar == null || emu == null) return;
        Emulator.State s = emu.state();
        SpannableStringBuilder t = new SpannableStringBuilder("● ");
        t.setSpan(new ForegroundColorSpan(stateColor(s)), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        t.append(stateLabel(s));
        if (s == Emulator.State.RUNNING) {
            long secs = Math.max(0, (System.currentTimeMillis() - emu.since()) / 1000);
            long m = secs / 60, h = m / 60;
            t.append("  ·  ").append(h > 0 ? getString(R.string.uptime_h, (int) h, (int) (m % 60))
                    : m > 0 ? getString(R.string.uptime_m, (int) m, (int) (secs % 60))
                    : getString(R.string.uptime_s, (int) secs));
            Config r = emu.running();
            if (r != null) {
                t.append("  ·  ").append(Config.LLE.equals(r.baseband) ? "AMSS" : "HLE")
                        .append(r.dspRunning() ? " + DSP" : "")
                        .append("  ·  ").append(r.nand);
            }
        }
        bar.setSubtitle(t);

        boolean fps = config.showFps && s == Emulator.State.RUNNING;
        fpsText.setVisibility(fps ? View.VISIBLE : View.GONE);
        if (fps) fpsText.setText(getString(R.string.fps, screen.takeFrames()));
    }

    private static int stateColor(Emulator.State s) {
        switch (s) {
            case RUNNING: return Holo.GREEN;
            case CRASHED: return Holo.RED;
            case STARTING:
            case STOPPING: return Holo.ORANGE;
            default: return 0xff6b6b6b;
        }
    }

    private String stateLabel(Emulator.State s) {
        switch (s) {
            case STARTING: return getString(R.string.state_starting);
            case RUNNING: return getString(R.string.state_running);
            case STOPPING: return getString(R.string.state_stopping);
            case CRASHED: return getString(R.string.state_crashed);
            default: return getString(R.string.state_stopped);
        }
    }

    /* ------------------------------------------------------------ the machine */

    private void power() {
        config.load();
        if (emu.state() == Emulator.State.RUNNING || emu.state() == Emulator.State.STARTING) {
            emu.stop();
        } else {
            startService(new Intent(MainActivity.this, EmuService.class));
            emu.start(config);
        }
    }

    private void restart() {
        config.load();
        emu.stop();
        new Thread(new Runnable() {
            @Override
            public void run() {
                for (int i = 0; i < 60 && emu.state() != Emulator.State.STOPPED; i++) {
                    Emulator.sleep(200);
                }
                startService(new Intent(MainActivity.this, EmuService.class));
                emu.start(config);
            }
        }).start();
    }

    /* ------------------------------------------------------------------ input */

    @Override
    public void onTouch(float nx, float ny, String action) {
        emu.touch(nx, ny, action);
    }

    @Override
    public void onKey(int keyCode, boolean down) {
        String hard = ScreenView.hardKey(keyCode);
        if (hard == null) return;
        emu.key(hard, down);
        View v = keyViews.get(hard);
        if (v != null) v.setActivated(down);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        /* let the phone see Back even when the screen is not focused */
        if (keyCode == KeyEvent.KEYCODE_BACK && emu.isRunning()) {
            emu.key("back", true);
            emu.key("back", false);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    /* --------------------------------------------------------------- the menu */

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        menu.add(0, MENU_POWER, 0, R.string.power_on)
                .setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS | MenuItem.SHOW_AS_ACTION_WITH_TEXT);
        menu.add(0, MENU_SHOT, 1, R.string.screenshot)
                .setIcon(HoloIcons.get(this, HoloIcons.CAMERA, 32))
                .setShowAsAction(MenuItem.SHOW_AS_ACTION_IF_ROOM);
        menu.add(0, MENU_RESTART, 2, R.string.restart);
        menu.add(0, MENU_RESET, 3, R.string.reset_board);
        menu.add(0, MENU_SENSORS, 4, R.string.menu_sensors);
        menu.add(0, MENU_LOGS, 5, R.string.menu_logs);
        menu.add(0, MENU_IMAGES, 6, R.string.menu_images);
        menu.add(0, MENU_NAND, 7, R.string.menu_nand);
        menu.add(0, MENU_ROOT, 8, R.string.menu_root);
        menu.add(0, MENU_SETTINGS, 9, R.string.menu_settings);
        menu.add(0, MENU_ABOUT, 10, R.string.menu_about);
        return true;
    }

    @Override
    public boolean onPrepareOptionsMenu(Menu menu) {
        Emulator.State s = emu == null ? Emulator.State.STOPPED : emu.state();
        boolean on = s == Emulator.State.RUNNING || s == Emulator.State.STARTING;
        MenuItem power = menu.findItem(MENU_POWER);
        power.setTitle(on ? R.string.shut_down : R.string.power_on);
        power.setIcon(HoloIcons.get(this, on ? HoloIcons.STOP : HoloIcons.PLAY, 32));
        return super.onPrepareOptionsMenu(menu);
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        switch (item.getItemId()) {
            case MENU_POWER: power(); return true;
            case MENU_SHOT: screenshot(); return true;
            case MENU_RESTART: restart(); return true;
            case MENU_RESET: emu.reset(); return true;
            case MENU_SENSORS: startActivity(new Intent(this, SensorsActivity.class)); return true;
            case MENU_LOGS: startActivity(new Intent(this, LogsActivity.class)); return true;
            case MENU_NAND: startActivity(new Intent(this, NandActivity.class)); return true;
            case MENU_IMAGES: startActivity(new Intent(this, ImagesActivity.class)); return true;
            case MENU_ROOT: startActivity(new Intent(this, RootActivity.class)); return true;
            case MENU_SETTINGS: startActivity(new Intent(this, SettingsActivity.class)); return true;
            case MENU_ABOUT:
                new Holo(this).alert(getString(R.string.app_name), about());
                return true;
            default: return super.onOptionsItemSelected(item);
        }
    }

    private String about() {
        return getString(R.string.about_text);
    }


    /* ------------------------------------------------------------- screenshot */

    /**
     * Saves the screen as a PNG named for the moment it was taken: into the
     * gallery (Pictures/HTC HD2) on Android 10 and later, which needs no
     * permission there, and into the app's own pictures folder before that.
     */
    private void screenshot() {
        Bitmap last = screen.lastBitmap();
        if (last == null) {
            toast(getString(R.string.no_picture));
            return;
        }
        /* a copy: the view keeps drawing into its own bitmap while this one is written */
        final Bitmap b = last.copy(Bitmap.Config.ARGB_8888, false);
        final String name = "HD2_" + new java.text.SimpleDateFormat("yyyyMMdd_HHmmss",
                java.util.Locale.ROOT).format(new java.util.Date()) + ".png";
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    String where;
                    if (Build.VERSION.SDK_INT >= 29) {
                        android.content.ContentValues v = new android.content.ContentValues();
                        v.put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, name);
                        v.put(android.provider.MediaStore.MediaColumns.MIME_TYPE, "image/png");
                        v.put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH, "Pictures/HTC HD2");
                        android.net.Uri uri = getContentResolver().insert(
                                android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, v);
                        if (uri == null) throw new java.io.IOException(getString(R.string.gallery_refused));
                        java.io.OutputStream o = getContentResolver().openOutputStream(uri);
                        if (o == null) throw new java.io.IOException(getString(R.string.gallery_refused));
                        try {
                            b.compress(Bitmap.CompressFormat.PNG, 100, o);
                        } finally {
                            o.close();
                        }
                        where = "Pictures/HTC HD2/" + name;
                    } else {
                        File dir = getExternalFilesDir(android.os.Environment.DIRECTORY_PICTURES);
                        if (dir == null) dir = getFilesDir();
                        if (!dir.isDirectory()) dir.mkdirs();
                        File out = new File(dir, name);
                        FileOutputStream o = new FileOutputStream(out);
                        try {
                            b.compress(Bitmap.CompressFormat.PNG, 100, o);
                        } finally {
                            o.close();
                        }
                        where = out.getAbsolutePath();
                    }
                    toast(getString(R.string.saved, where));
                } catch (Exception e) {
                    toast(String.valueOf(e.getMessage()));
                } finally {
                    b.recycle();
                }
            }
        }, "screenshot").start();
    }

    private void toast(final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                android.widget.Toast.makeText(MainActivity.this, text,
                        android.widget.Toast.LENGTH_LONG).show();
            }
        });
    }

    private void askNotifications() {
        if (Build.VERSION.SDK_INT >= 33) {
            try {
                requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, REQ_NOTIFICATIONS);
            } catch (Throwable ignored) {
            }
        }
    }

    /* ------------------------------------------------------------- the service */

    private final Emulator.Listener listener = new Emulator.Listener() {
        @Override
        public void onState(final Emulator.State state, final String message) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    render();
                    if (service != null) service.update(state, message);
                }
            });
        }

        @Override
        public void onFrame(final byte[] fb, final int w, final int h) {
            screen.offerFrame(fb, w, h);
        }

        @Override
        public void onSensors(int battery, String cable) {
        }

        @Override
        public void onAudio(byte[] pcm, int length) {
        }
    };
}
