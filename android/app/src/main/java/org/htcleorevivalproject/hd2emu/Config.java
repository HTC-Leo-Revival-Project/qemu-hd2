package org.qemu.htcleo;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.List;

/**
 * Everything the settings screen can change, the same set of knobs the web UI
 * has (web/lib/emulator-types.ts), kept in SharedPreferences.
 *
 * USB host LLE is deliberately missing: it needs vhci_hcd and root, neither of
 * which an app can have, so the emulated phone always uses the built-in host.
 */
public class Config {
    public static final String HLE = "hle";
    public static final String LLE = "lle";

    public String baseband = HLE;
    public String dsp = HLE;
    public String nand = "nand.img";
    public String sdcard = "";
    public String boot = "nand";          // nand | recovery | menu
    public String gpu = "soft";           // soft | gles: the Adreno 200 on the host GPU (OpenGL ES) or on the CPU
    public String audio = "app";          // app | off
    public boolean wifi = true;
    public boolean mobileData = true;
    public int adbPort = 5555;
    public boolean amssLog = false;
    public int dspShift = -1;
    public int gpuThreads = 0;            // 0 = QEMU's own choice
    public float volume = 1.0f;
    public boolean showFps = false;
    public String extraArgs = "";

    private final SharedPreferences prefs;

    public Config(Context ctx) {
        this.prefs = ctx.getSharedPreferences("htcleo", Context.MODE_PRIVATE);
    }

    public static String gb(long n) {
        return n >= 1L << 30 ? String.format("%.1f GB", n / (double) (1L << 30))
                             : String.format("%d MB", Math.round(n / (double) (1L << 20)));
    }

    public static List<String> splitArgs(String s) {
        /* the same shell-ish split tools/start.sh gets: "a b" "c" */
        List<String> out = new ArrayList<String>();
        StringBuilder cur = null;
        char quote = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (quote != 0) {
                if (c == quote) quote = 0;
                else cur.append(c);
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                if (cur == null) cur = new StringBuilder();
                continue;
            }
            if (Character.isWhitespace(c)) {
                if (cur != null) { out.add(cur.toString()); cur = null; }
                continue;
            }
            if (cur == null) cur = new StringBuilder();
            cur.append(c);
        }
        if (cur != null) out.add(cur.toString());
        return out;
    }

    public void load() {
        baseband = prefs.getString("baseband", baseband);
        dsp = prefs.getString("dsp", dsp);
        nand = prefs.getString("nand", nand);
        sdcard = prefs.getString("sdcard", sdcard);
        boot = prefs.getString("boot", boot);
        gpu = prefs.getString("gpu", gpu);
        audio = prefs.getString("audio", audio);
        wifi = prefs.getBoolean("wifi", wifi);
        mobileData = prefs.getBoolean("mobileData", mobileData);
        adbPort = prefs.getInt("adbPort", adbPort);
        amssLog = prefs.getBoolean("amssLog", amssLog);
        dspShift = prefs.getInt("dspShift", dspShift);
        gpuThreads = prefs.getInt("gpuThreads", gpuThreads);
        volume = prefs.getFloat("volume", volume);
        showFps = prefs.getBoolean("showFps", showFps);
        extraArgs = prefs.getString("extraArgs", extraArgs);
    }

    public void save() {
        prefs.edit()
                .putString("baseband", baseband)
                .putString("dsp", dsp)
                .putString("nand", nand)
                .putString("sdcard", sdcard)
                .putString("boot", boot)
                .putString("gpu", gpu)
                .putString("audio", audio)
                .putBoolean("wifi", wifi)
                .putBoolean("mobileData", mobileData)
                .putInt("adbPort", adbPort)
                .putBoolean("amssLog", amssLog)
                .putInt("dspShift", dspShift)
                .putInt("gpuThreads", gpuThreads)
                .putFloat("volume", volume)
                .putBoolean("showFps", showFps)
                .putString("extraArgs", extraArgs)
                .apply();
    }

    public boolean dspRunning() {
        return LLE.equals(baseband) && LLE.equals(dsp);
    }

    /** The QMP qcode for each of the HD2's keys, as in the web UI. */
    public static String qcode(String hardKey) {
        if ("volup".equals(hardKey)) return "pgup";
        if ("voldown".equals(hardKey)) return "pgdn";
        if ("menu".equals(hardKey)) return "f1";
        if ("call".equals(hardKey)) return "f2";
        if ("end".equals(hardKey)) return "f3";
        if ("back".equals(hardKey)) return "esc";
        if ("home".equals(hardKey)) return "home";
        if ("power".equals(hardKey)) return "f12";
        return null;
    }
}
