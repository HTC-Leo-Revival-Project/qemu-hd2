package org.qemu.htcleo;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Bitmap;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;

/**
 * The screen, the keys, and the power buttons.  Everything else (settings,
 * sensors, logs, the NAND builder) is another activity behind the menu, so
 * there is no navigation framework here either.
 */
public class MainActivity extends Activity implements ScreenView.Input {
    private static final int REQ_NOTIFICATIONS = 7;

    private static final String[] KEYS = {
            "call|F2", "home|HOME", "menu|F1", "back|ESC",
            "end|F3", "power|F12", "volup|PGUP", "voldown|PGDN",
    };

    private Emulator emu;
    private EmuService service;
    private ScreenView screen;
    private TextView stateText, modeText, fpsText, blurb;
    private View dot;
    private GridLayout keys;
    private LinearLayout buttons;
    private Config config;
    private long bootAt;
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
            uptime();
            clock.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        config = new Config(this);
        config.load();
        setContentView(R.layout.activity_main);

        screen = (ScreenView) findViewById(R.id.screen);
        stateText = (TextView) findViewById(R.id.state);
        modeText = (TextView) findViewById(R.id.mode);
        fpsText = (TextView) findViewById(R.id.fps);
        blurb = (TextView) findViewById(R.id.blurb);
        dot = findViewById(R.id.dot);
        keys = (GridLayout) findViewById(R.id.keys);
        buttons = (LinearLayout) findViewById(R.id.buttons);

        screen.setInput(this);
        buildKeys();
        buildButtons();
        Holo.apply(findViewById(R.id.root));

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
    protected void onResume() {
        super.onResume();
        if (service != null) attach(service.machine());
        clock.postDelayed(tick, 1000);
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
        listener.onState(emu.state(), emu.message());
        render();
    }

    /* ------------------------------------------------------------------ the view */

    private void buildKeys() {
        LayoutInflater in = LayoutInflater.from(this);
        keys.setRowCount(2);
        for (String spec : KEYS) {
            final String[] parts = spec.split("\\|");
            final String hardKey = parts[0];
            View v = in.inflate(R.layout.item_key, keys, false);
            ((TextView) v.findViewById(R.id.label)).setText(label(parts[0]));
            ((TextView) v.findViewById(R.id.hint)).setText(parts[1]);
            v.setOnTouchListener(new View.OnTouchListener() {
                @Override
                public boolean onTouch(View view, MotionEvent e) {
                    switch (e.getActionMasked()) {
                        case MotionEvent.ACTION_DOWN:
                            emu.key(hardKey, true);
                            view.setActivated(true);
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
            GridLayout.LayoutParams lp = new GridLayout.LayoutParams();
            lp.width = 0;
            lp.height = getResources().getDimensionPixelSize(R.dimen.key_height);
            lp.columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1, 1f);
            lp.setMargins(4, 4, 4, 4);
            keys.addView(v, lp);
            keyViews.put(hardKey, v);
        }
    }

    private String label(String hardKey) {
        if ("volup".equals(hardKey)) return getString(R.string.key_volup);
        if ("voldown".equals(hardKey)) return getString(R.string.key_voldown);
        if ("call".equals(hardKey)) return getString(R.string.key_call);
        if ("end".equals(hardKey)) return getString(R.string.key_end);
        if ("back".equals(hardKey)) return getString(R.string.key_back);
        if ("home".equals(hardKey)) return getString(R.string.key_home);
        if ("menu".equals(hardKey)) return getString(R.string.key_menu);
        return getString(R.string.key_power);
    }

    private void buildButtons() {
        final Button power = button(getString(R.string.power_on), true);
        power.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                config.load();
                if (emu.state() == Emulator.State.RUNNING || emu.state() == Emulator.State.STARTING) {
                    emu.stop();
                } else {
                    startService(new Intent(MainActivity.this, EmuService.class));
                    emu.start(config);
                }
            }
        });
        buttons.addView(power, weight());

        Button restart = button(getString(R.string.restart), false);
        restart.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                config.load();
                emu.stop();
                waitThenStart();
            }
        });
        buttons.addView(restart, weight());

        Button reset = button(getString(R.string.reset_board), false);
        reset.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                emu.reset();
            }
        });
        buttons.addView(reset, weight());

        Button shot = button(getString(R.string.screenshot), false);
        shot.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                screenshot();
            }
        });
        buttons.addView(shot, weight());
    }

    private Button button(String text, boolean primary) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextSize(14f);
        b.setAllCaps(false);
        b.setBackgroundResource(primary ? R.drawable.holo_button_primary : R.drawable.holo_button);
        b.setTextColor(getResources().getColor(R.color.holo_text));
        b.setPadding(8, 0, 8, 0);
        return b;
    }

    private LinearLayout.LayoutParams weight() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMargins(4, 0, 4, 0);
        return lp;
    }

    private void waitThenStart() {
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

    private void uptime() {
        if (emu == null) return;
        Emulator.State s = emu.state();
        if (s != Emulator.State.RUNNING) return;
        long secs = Math.max(0, (System.currentTimeMillis() - emu.since()) / 1000);
        long m = secs / 60, h = m / 60;
        String up = h > 0 ? h + " h " + (m % 60) + " min" : m > 0 ? m + " min " + (secs % 60) + " s"
                : secs + " s";
        Config r = emu.running();
        String mode = "";
        if (r != null) {
            mode = (Config.LLE.equals(r.baseband) ? "Real AMSS baseband" : "Modelled baseband")
                    + (r.dspRunning() ? ", real DSP1 on Hexagon" : "")
                    + ", " + r.nand + (r.sdcard.isEmpty() ? "" : " + " + r.sdcard);
        }
        modeText.setText("Up " + up + (mode.isEmpty() ? "" : "  ·  " + mode));
    }

    private void render() {
        Emulator.State s = emu.state();
        int color;
        switch (s) {
            case RUNNING: color = 0xff99cc00; break;
            case CRASHED: color = 0xffff4444; break;
            case STARTING:
            case STOPPING: color = 0xffffbb33; break;
            default: color = 0xff6b6b6b;
        }
        GradientDrawable d = (GradientDrawable) dot.getBackground().mutate();
        d.setColor(color);
        dot.setBackground(d);
        dot.invalidate();

        stateText.setText(label(s, emu.message()));
        boolean on = s == Emulator.State.RUNNING;
        blurb.setVisibility(on ? View.GONE : View.VISIBLE);
        screen.setVisibility(on ? View.VISIBLE : View.INVISIBLE);
        fpsText.setVisibility(config.showFps && on ? View.VISIBLE : View.GONE);
        setKeysEnabled(on);
    }

    private String label(Emulator.State s, String message) {
        switch (s) {
            case STARTING: return getString(R.string.state_starting);
            case RUNNING: return getString(R.string.state_running);
            case STOPPING: return getString(R.string.state_stopping);
            case CRASHED: return message == null || message.isEmpty()
                    ? getString(R.string.state_crashed)
                    : getString(R.string.state_crashed) + ": " + message;
            default: return getString(R.string.state_stopped);
        }
    }

    private void setKeysEnabled(boolean on) {
        for (View v : keyViews.values()) v.setEnabled(on);
        for (int i = 0; i < buttons.getChildCount(); i++) buttons.getChildAt(i).setEnabled(true);
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
        menu.add(0, 1, 0, R.string.menu_settings);
        menu.add(0, 2, 1, R.string.menu_sensors);
        menu.add(0, 3, 2, R.string.menu_logs);
        menu.add(0, 4, 3, R.string.menu_nand);
        menu.add(0, 5, 4, R.string.menu_about);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        switch (item.getItemId()) {
            case 1: startActivity(new Intent(this, SettingsActivity.class)); return true;
            case 2: startActivity(new Intent(this, SensorsActivity.class)); return true;
            case 3: startActivity(new Intent(this, LogsActivity.class)); return true;
            case 4: startActivity(new Intent(this, NandActivity.class)); return true;
            case 5:
                new Holo(this).alert(getString(R.string.app_name), about());
                return true;
            default: return super.onOptionsItemSelected(item);
        }
    }

    private String about() {
        return "HTC HD2 (Qualcomm QSD8250, Scorpion) on QEMU's htcleo machine.\n\n"
                + "The picture comes from QEMU's VNC server on a socket inside this app's "
                + "own directory, the keys and taps go back through QMP, and the sound is "
                + "the VNC audio extension played by AudioTrack.\n\n"
                + "No Material anywhere: Theme.Holo, the platform widgets, and a "
                + "framework-only build with no dependencies at all.";
    }

    /* ------------------------------------------------------------- screenshot */

    private void screenshot() {
        final Bitmap b = screen.lastBitmap();
        if (b == null) {
            toast(getString(R.string.no_picture));
            return;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    File out = new File(getExternalFilesDir(null), "screenshot.png");
                    if (out.getParentFile() != null && !out.getParentFile().isDirectory()) {
                        out.getParentFile().mkdirs();
                    }
                    FileOutputStream o = new FileOutputStream(out);
                    try {
                        b.compress(Bitmap.CompressFormat.PNG, 100, o);
                    } finally {
                        o.close();
                    }
                    toast(getString(R.string.saved, out.getAbsolutePath()));
                } catch (Exception e) {
                    toast(String.valueOf(e.getMessage()));
                }
            }
        }).start();
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
        public void onState(Emulator.State state, String message) {
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
