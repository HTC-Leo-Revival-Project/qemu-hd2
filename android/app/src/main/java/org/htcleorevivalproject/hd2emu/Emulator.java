package org.htcleorevivalproject.hd2emu;

import android.content.Context;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Runs the emulated HD2 the way tools/start.sh and tools/start-q6.sh do,
 * with two differences: the sockets live in the app's own directory instead of
 * images/, and there is no USB LLE - that needs vhci_hcd and root, so the
 * phone always enumerates itself on the built-in host.
 *
 * One QEMU for the phone, plus a second one for the QDSP6 when the real DSP
 * firmware is selected; they share the DRAM through a file, exactly like the
 * web server does.
 */
public class Emulator {
    private static final String TAG = "HtcleoEmu";

    public enum State { STOPPED, STARTING, RUNNING, STOPPING, CRASHED }

    public interface Listener {
        void onState(State state, String message);

        void onFrame(byte[] fb, int w, int h);

        void onSensors(int battery, String cable);

        void onAudio(byte[] pcm, int length);
    }

    private final Context ctx;
    private final Paths paths;
    private final List<Listener> listeners = new CopyOnWriteArrayList<Listener>();
    private final Object opLock = new Object();

    private volatile State state = State.STOPPED;
    private volatile String message = "";
    private volatile long since = System.currentTimeMillis();
    private volatile Config running;
    private volatile int battery = 100;
    private volatile String cable = "usb";

    private Process arm, hexagon;
    private Qmp qmp, q6qmp;
    private volatile VncClient vnc, q6vnc;
    private PcmPlayer audio;
    private Thread sensors, bootKeys;
    /** Whether the USB passthrough bridge is ours to stop; see {@link #killAll()}. */
    private volatile boolean bridgeRunning;
    /** Whether anyone is looking at the screen; see {@link #setVideo}. */
    private volatile boolean videoOn = true;

    public Emulator(Context ctx) {
        this.ctx = ctx.getApplicationContext();
        this.paths = new Paths(this.ctx);
    }

    public Paths paths() {
        return paths;
    }

    public State state() {
        return state;
    }

    public String message() {
        return message;
    }

    public long since() {
        return since;
    }

    public Config running() {
        return running;
    }

    public int battery() {
        return battery;
    }

    public String cable() {
        return cable;
    }

    public void addListener(Listener l) {
        listeners.add(l);
    }

    public void removeListener(Listener l) {
        listeners.remove(l);
    }

    private void setState(State s, String why) {
        if (s != state) since = System.currentTimeMillis();
        state = s;
        message = why == null ? "" : why;
        for (Listener l : listeners) l.onState(s, message);
    }

    /* ----------------------------------------------------------------- start */

    public void start(final Config cfg) {
        synchronized (opLock) {
            if (state == State.RUNNING || state == State.STARTING) {
                setState(state, ctx.getString(R.string.emu_already_running));
                return;
            }
            setState(State.STARTING, ctx.getString(R.string.emu_launching));
        }
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    launch(cfg);
                    setState(State.RUNNING, "");
                } catch (Throwable e) {
                    Log.e(TAG, "start failed", e);
                    killAll();
                    setState(State.CRASHED, describe(e));
                }
            }
        }, "emu-start");
        t.setDaemon(true);
        t.start();
    }

    private void launch(Config cfg) throws Exception {
        running = cfg;
        File nand = paths.image(cfg.nand);
        if (!nand.isFile()) throw new IOException(ctx.getString(R.string.emu_nand_missing, cfg.nand));
        boolean dsp = cfg.dspRunning();
        boolean needAmss = Config.LLE.equals(cfg.baseband) && !paths.image("amss.elf").isFile();
        boolean needDsp = dsp && !paths.image("dsp1.elf").isFile();
        if (needAmss || needDsp) firmwareFromNand(nand, needAmss, needDsp);
        if (Config.LLE.equals(cfg.baseband) && !paths.image("amss.elf").isFile()) {
            throw new IOException(ctx.getString(R.string.emu_amss_missing));
        }
        if (dsp && !paths.image("dsp1.elf").isFile()) {
            throw new IOException(ctx.getString(R.string.emu_dsp_missing));
        }
        if (!paths.ensurePcBios()) throw new IOException(ctx.getString(R.string.emu_keymaps_missing));

        File qemu = paths.qemu("qemu-system-arm");
        cleanSockets();

        List<String> args = new ArrayList<String>();
        args.add("-L");
        args.add(paths.pcBiosDir.getAbsolutePath());
        String machine = "htcleo";
        if (Config.LLE.equals(cfg.baseband)) {
            machine += ",baseband=lle,baseband-firmware=" + paths.image("amss.elf").getAbsolutePath()
                    + ",baseband-trace=off";
        }
        /*
         * USB passthrough: the host is not us but a bridge, which owns a host
         * controller we cannot reach -- a FunctionFS gadget on this phone's own
         * USB port (see UsbBridge).  QEMU listens on the socket and the bridge
         * connects, so the USB/IP stream vhci_hcd would have carried goes there
         * instead.  No vhci_hcd, and no root in QEMU.
         */
        /*
         * Asked for but not available is a failure, not a quiet fallback: the
         * UDC, configfs and FunctionFS are all root-only, so without root the
         * passthrough cannot work at all, and carrying on as if it were off would
         * leave the phone's USB alone and the PC seeing nothing, with nothing in
         * the log to say why.
         */
        boolean passthrough = cfg.usbPassthrough;
        if (passthrough) {
            if (Root.status() != Root.Status.GRANTED) {
                throw new IOException(ctx.getString(R.string.emu_usb_needs_root));
            }
            if (!paths.hasUsbBridge()) {
                throw new IOException(ctx.getString(R.string.emu_usbffs_missing));
            }
            machine += ",usb-host=lle,usb-ip-socket=" + paths.usbIpSocket().getAbsolutePath();
        }
        args.add("-M");
        args.add(machine);
        args.add("-drive");
        args.add("if=mtd,format=raw,file=" + nand.getAbsolutePath());
        args.add("-display");
        args.add("none");
        args.add("-vnc");
        args.add("unix:" + paths.vncSocket() + (soundOn(cfg) ? ",audiodev=snd0" : ""));
        args.add("-serial");
        args.add("file:" + paths.log("uart1"));
        args.add("-serial");
        args.add("file:" + paths.log("ramcon"));
        args.add("-qmp");
        args.add("unix:" + paths.qmpSocket() + ",server=on,wait=off");
        args.add("-audiodev");
        args.add("none,id=snd0");
        args.add("-global");
        args.add("msm-modem.audiodev=snd0");
        args.add("-global");
        args.add("msm-adreno.backend=" + ("gles".equals(cfg.gpu) ? "gles" : "soft"));
        if (Config.LLE.equals(cfg.baseband) && cfg.clkRegimeFake) {
            /*
             * Workaround: the apps core toggles ADM_CLK once per datamover
             * command, so the YAFFS2 mount is thousands of clock-regime
             * proc_comm round trips.  AMSS's clkrgm_rm.c livelocks on them
             * and never answers, and proc_comm_wait_for() has no timeout, so
             * the boot wedges in the mount.  Answering these two commands
             * here instead keeps the stock kernel booting.  Turn it off to
             * watch the stall, or once the kernel stops toggling the clock.
             */
            args.add("-global");
            args.add("msm-modem.clk-regime-fake=on");
        }
        if (cfg.gpuThreads > 0) {
            args.add("-global");
            args.add("msm-adreno.threads=" + cfg.gpuThreads);
        }
        args.add("-d");
        args.add("unimp,guest_errors");
        args.add("-D");
        args.add(paths.log("qemu").getAbsolutePath());
        if (cfg.sdcard != null && !cfg.sdcard.isEmpty() && paths.image(cfg.sdcard).isFile()) {
            args.add("-drive");
            args.add("if=sd,format=raw,file=" + paths.image(cfg.sdcard).getAbsolutePath());
        }
        if (cfg.wifi) args.addAll(java.util.Arrays.asList("-nic", "user,model=bcm4329"));
        if (cfg.mobileData) {
            args.addAll(java.util.Arrays.asList("-nic", "user,model=msm-modem,net=10.0.3.0/24"));
        }
        if (cfg.adbPort > 0 && !passthrough) {
            args.add("-chardev");
            args.add("socket,id=adb,host=127.0.0.1,port=" + cfg.adbPort + ",server=on,wait=off");
            args.add("-global");
            args.add("msm-hsusb.adb=adb");
        }
        List<String> mem = new ArrayList<String>();
        if (dsp) {
            /*
             * The guest's RAM comes up through this file, and RAM has to start
             * out empty: start-q6.sh and web/server/emulator.ts delete it for
             * the same reason.  Keeping it would hand the next boot the last
             * one's leftovers - the ram_console ring among them, which then
             * makes a guest that is still sitting in the bootloader look like
             * one that booted.  Recreating it leaves a sparse file of zeroes.
             */
            File dram = paths.dram();
            dram.delete();
            try {
                RandomAccessFile f = new RandomAccessFile(dram, "rw");
                try {
                    f.setLength(1L << 30);
                } finally {
                    f.close();
                }
            } catch (FileNotFoundException e) {
                throw new IOException("cannot create " + dram + ": " + e.getMessage());
            }
            args.add("-object");
            args.add("memory-backend-file,id=htcleo.ram,size=1G,mem-path=" + dram.getAbsolutePath() + ",share=on");
            args.add("-machine");
            args.add("memory-backend=htcleo.ram,qdsp6=lle,qdsp6-link=q6link");
            args.add("-chardev");
            args.add("socket,id=q6link,path=" + paths.q6LinkSocket() + ",server=on,wait=off");
        }
        args.addAll(Config.splitArgs(cfg.extraArgs));

        arm = spawn(qemu, args, "qemu.out", true);

        if (!waitForSocket(paths.qmpSocket(), 15000)) {
            throw new IOException(ctx.getString(R.string.emu_no_monitor, tail("qemu.out")));
        }
        qmp = Qmp.connect(paths.qmpSocket().getAbsolutePath(), new Qmp.Events() {
            @Override
            public void onEvent(String name, JSONObject body) {
                if ("SHUTDOWN".equals(name)) {
                    note(ctx.getString(R.string.emu_powered_off_by_qemu));
                }
            }
        });

        connectVnc(cfg);

        if (dsp) {
            File q6 = paths.qemu("qemu-system-hexagon");
            List<String> q6args = new ArrayList<String>();
            q6args.add("-L");
            q6args.add(paths.pcBiosDir.getAbsolutePath());
            q6args.add("-M");
            q6args.add("htcleo-q6,link=q6link,apps-irq=11,modem-irq=2,trace=off");
            q6args.add("-kernel");
            q6args.add(paths.image("dsp1.elf").getAbsolutePath());
            q6args.add("-icount");
            q6args.add("shift=" + (cfg.dspShift < 0 ? "auto" : Integer.toString(cfg.dspShift)));
            q6args.add("-object");
            q6args.add("memory-backend-file,id=htcleo.ram,size=1G,mem-path="
                    + paths.dram().getAbsolutePath() + ",share=on");
            q6args.add("-machine");
            q6args.add("memory-backend=htcleo.ram");
            q6args.add("-chardev");
            q6args.add("socket,id=q6link,path=" + paths.q6LinkSocket());
            q6args.add("-audiodev");
            q6args.add("none,id=q6snd");
            q6args.add("-global");
            q6args.add("htcleo-q6-audif.audiodev=q6snd");
            q6args.add("-display");
            q6args.add("none");
            q6args.add("-d");
            q6args.add("guest_errors,unimp");
            q6args.add("-D");
            q6args.add(paths.log("q6").getAbsolutePath());
            hexagon = spawn(q6, q6args, "q6.out", false);
        }

        watch();
        startSensors();
        if (!"menu".equals(cfg.boot)) startBootKeys(cfg);
        if (passthrough) {
            /*
             * Now that QEMU is listening: the bridge waits for the socket, and
             * then for the guest to answer, which is the better part of a boot.
             * It stops the USB HAL and takes the UDC; if it cannot bind, it
             * exits rather than limping along, so this is where that surfaces.
             */
            note(ctx.getString(R.string.emu_usb_starting));
            UsbBridge.start(paths);
            bridgeRunning = true;
            note(ctx.getString(R.string.emu_usb_on_port));
        }
    }

    /**
     * The real baseband and DSP run from amss.elf and dsp1.elf, but a NAND
     * image with a radio in it (a stock ROM's, or MAGLDR's with a radio)
     * already holds both: take the missing ones from there, as Images -> Radio
     * firmware would.  If it has none, say where else they come from.
     */
    private void firmwareFromNand(File nand, boolean amss, boolean dsp) throws IOException {
        note(ctx.getString(R.string.emu_taking_firmware, nand.getName()));
        Radio.Firmware fw;
        try {
            fw = Radio.fromNand(nand);
        } catch (IOException e) {
            throw new IOException(ctx.getString(R.string.emu_firmware_failed, amss ? "amss.elf" : "dsp1.elf",
                    e.getMessage()));
        }
        if (amss) Radio.Firmware.write(paths.image("amss.elf"), fw.amss);
        if (dsp && fw.dsp != null) Radio.Firmware.write(paths.image("dsp1.elf"), fw.dsp);
    }

    private boolean soundOn(Config cfg) {
        return "app".equals(cfg.audio);
    }

    private void connectVnc(Config cfg) {
        final boolean sound = soundOn(cfg);
        if (sound) {
            audio = new PcmPlayer();
            audio.setVolume(cfg.volume);
            audio.start();
        }
        for (int tries = 0; tries < 20 && state != State.CRASHED; tries++) {
            try {
                vnc = VncClient.connect(paths.vncSocket().getAbsolutePath(), new VncClient.Listener() {
                    @Override
                    public void onFrame(byte[] fb, int w, int h) {
                        for (Listener l : listeners) l.onFrame(fb, w, h);
                    }

                    @Override
                    public void onAudio(byte[] pcm, int length) {
                        if (audio != null) audio.write(pcm, length);
                        for (Listener l : listeners) l.onAudio(pcm, length);
                    }

                    @Override
                    public void onClosed(String why) {
                        if (!why.isEmpty()) Log.i(TAG, "vnc closed: " + why);
                    }
                }, videoOn, sound);
                return;
            } catch (IOException e) {
                if (tries == 19) note(ctx.getString(R.string.emu_no_picture, e.getMessage()));
                sleep(250);
            }
        }
    }

    private Process spawn(File bin, List<String> args, String outName, boolean watch) throws IOException {
        ProcessBuilder pb = new ProcessBuilder(args);
        pb.command().add(0, bin.getAbsolutePath());
        pb.directory(paths.runDir);
        pb.redirectErrorStream(true);
        pb.redirectOutput(new File(paths.runDir, outName));
        java.util.Map<String, String> env = pb.environment();
        env.put("HOME", paths.emuDir.getAbsolutePath());
        env.put("TMPDIR", paths.runDir.getAbsolutePath());
        /* QEMU's Adreno host GPU backend reads this one */
        if (outName.startsWith("qemu")) {
            env.put("ADRENO_GLES_PBUFFER", "1");
        }
        env.remove("LD_LIBRARY_PATH");
        return pb.start();
    }

    /** Notices when QEMU goes away by itself. */
    private void watch() {
        final Process p = arm;
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    int code = p.waitFor();
                    if (state == State.STOPPING || state == State.STOPPED) return;
                    killAll();
                    if (code == 0) setState(State.STOPPED, ctx.getString(R.string.emu_powered_off));
                    else setState(State.CRASHED, ctx.getString(R.string.emu_exit_code, code, tail("qemu.out")));
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
        }, "emu-watch");
        t.setDaemon(true);
        t.start();
    }

    private void startSensors() {
        sensors = new Thread(new Runnable() {
            @Override
            public void run() {
                while (state == State.RUNNING) {
                    refreshSensors();
                    sleep(3000);
                }
            }
        }, "emu-sensors");
        sensors.setDaemon(true);
        sensors.start();
    }

    /**
     * The cLK bootloader wants a key press to pick BOOT FROM NAND, which is
     * what tools/start.sh does too.  How long that takes depends entirely on
     * the host: on a phone the whole boot runs a good many times slower than
     * on a workstation, so one press at a fixed moment is a coin toss.  Press
     * it a few times over the first half minute instead.
     *
     * Every press is released again even if we give up halfway, so a key can
     * never be left held down in the guest, and nothing is sent at all once
     * Linux has the machine - on a running phone F2 is the call key.
     */
    private void startBootKeys(final Config cfg) {
        bootKeys = new Thread(new Runnable() {
            @Override
            public void run() {
                sleep(3000);
                String[] keys = "recovery".equals(cfg.boot) ? new String[]{"pgdn", "pgdn", "f2"}
                                                           : new String[]{"f2"};
                for (int round = 0; round < 6; round++) {
                    if (qmp == null || qmp.isClosed()) return;
                    for (String k : keys) {
                        if (qmp == null || qmp.isClosed()) return;
                        if (booted()) return;
                        qmp.sendKey(k, true);
                        sleep(200);
                        qmp.sendKey(k, false);
                        sleep(300);
                    }
                    sleep(3200);
                }
            }
        }, "emu-bootkeys");
        bootKeys.setDaemon(true);
        bootKeys.start();
    }

    /** The guest has handed over to Linux: the kernel writes to ramcon.log. */
    private boolean booted() {
        File ramcon = paths.log("ramcon");
        return ramcon.isFile() && ramcon.length() > 0;
    }

    /* ------------------------------------------------------------------ stop */

    public void stop() {
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                setState(State.STOPPING, ctx.getString(R.string.state_stopping));
                killAll();
                setState(State.STOPPED, "");
            }
        }, "emu-stop");
        t.setDaemon(true);
        t.start();
    }

    private void killAll() {
        if (bootKeys != null) bootKeys.interrupt();
        if (sensors != null) sensors.interrupt();
        /* the bridge first: it gives the UDC back and restarts the USB HAL, and
         * the emulator should not be yanking the USB out from under it */
        if (bridgeRunning) {
            bridgeRunning = false;
            UsbBridge.stop();
        }
        Qmp[] ms = {qmp, q6qmp};
        for (Qmp m : ms) {
            if (m == null) continue;
            try {
                m.execSync("quit", null);
            } catch (Exception ignored) {
            }
        }
        sleep(800);
        for (Qmp m : ms) {
            if (m != null) m.close();
        }
        qmp = q6qmp = null;
        if (vnc != null) vnc.close();
        if (q6vnc != null) q6vnc.close();
        vnc = q6vnc = null;
        if (audio != null) {
            audio.stop();
            audio = null;
        }
        for (Process p : new Process[]{arm, hexagon}) {
            if (p != null) {
                p.destroy();
                try {
                    p.waitFor();
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
                if (p.isAlive()) p.destroyForcibly();
            }
        }
        arm = hexagon = null;
        running = null;
        cleanSockets();
    }

    private void cleanSockets() {
        for (File f : new File[]{paths.qmpSocket(), paths.vncSocket(), paths.q6LinkSocket(),
                paths.usbIpSocket()}) {
            if (f.exists()) f.delete();
        }
    }

    public void reset() {
        if (qmp == null) {
            note(ctx.getString(R.string.emu_not_running));
            return;
        }
        qmp.exec("system_reset", null, new Qmp.Reply());
        final Config cfg = running;
        if (cfg != null && !"menu".equals(cfg.boot)) startBootKeys(cfg);
    }

    /* --------------------------------------------------------------- sensors */

    public void refreshSensors() {
        if (qmp == null) return;
        Qmp.Reply r = qmp.execSync("qom-get", arg("path", "/machine", "property", "battery-capacity"));
        if (r.value instanceof Number) battery = ((Number) r.value).intValue();
        Qmp.Reply c = qmp.execSync("qom-get", arg("path", "/machine", "property", "cable"));
        if (c.value instanceof String) cable = (String) c.value;
        for (Listener l : listeners) l.onSensors(battery, cable);
    }

    public void setBattery(final int level) {
        final int v = Math.max(0, Math.min(100, level));
        if (qmp == null) return;
        qmp.exec("qom-set", arg("path", "/machine", "property", "battery-capacity", "value", v),
                new Qmp.Reply());
        battery = v;
        for (Listener l : listeners) l.onSensors(battery, cable);
    }

    public void setCable(final String what) {
        if (qmp == null) return;
        qmp.exec("qom-set", arg("path", "/machine", "property", "cable", "value", what),
                new Qmp.Reply());
        cable = what;
        for (Listener l : listeners) l.onSensors(battery, cable);
    }

    public void setGps(final String latLonAlt) {
        if (qmp == null) {
            note(ctx.getString(R.string.emu_not_running));
            return;
        }
        if (running != null && Config.LLE.equals(running.baseband)) {
            note(ctx.getString(R.string.emu_gps_hle_only));
            return;
        }
        qmp.exec("qom-set", arg("path", "/machine/modem", "property", "gps-fix", "value", latLonAlt),
                new Qmp.Reply());
    }

    /**
     * The picture only while it is on screen: the main activity turns it off
     * when it goes into the background, and QEMU stops sending frames.
     */
    public void setVideo(boolean on) {
        videoOn = on;
        VncClient v = vnc;
        if (v != null) v.setVideo(on);
    }

    public void setVolume(float v) {
        if (audio != null) audio.setVolume(v);
    }

    /* ---------------------------------------------------------------- input */

    public boolean isRunning() {
        return state == State.RUNNING && qmp != null;
    }

    public void key(String hardKey, boolean down) {
        if (qmp == null) return;
        String qcode = Config.qcode(hardKey);
        if (qcode != null) qmp.sendKey(qcode, down);
    }

    public void touch(float nx, float ny, String action) {
        if (qmp == null) return;
        qmp.sendTouch(nx, ny, action);
    }

    /** Send a shell command into the guest over the adb bridge (if it is up). */
    public Qmp.Reply hmp(final String command) {
        Qmp.Reply r = new Qmp.Reply();
        if (qmp == null) {
            r.error = ctx.getString(R.string.emu_not_running);
            return r;
        }
        qmp.exec("human-monitor-command", arg("command-line", command), r);
        return r;
    }

    /**
     * The same, but waiting for the answer.  Call it off the main thread: the
     * monitor belongs to the guest, so a slow command (a screendump, say)
     * takes as long as the guest takes.
     */
    public Qmp.Reply hmpSync(final String command) {
        Qmp.Reply r = new Qmp.Reply();
        if (qmp == null) {
            r.error = ctx.getString(R.string.emu_not_running);
            return r;
        }
        qmp.exec("human-monitor-command", arg("command-line", command), r);
        synchronized (r) {
            long end = System.currentTimeMillis() + 20000;
            while (r.value == null && r.error == null) {
                long left = end - System.currentTimeMillis();
                if (left <= 0) break;
                try {
                    r.wait(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
        }
        if (r.value == null && r.error == null) r.error = "no answer within 20 s";
        return r;
    }

    /* --------------------------------------------------------------- helpers */

    private static JSONObject arg(Object... kv) {
        JSONObject o = new JSONObject();
        try {
            for (int i = 0; i + 1 < kv.length; i += 2) {
                o.put(String.valueOf(kv[i]), kv[i + 1]);
            }
        } catch (JSONException e) {
            throw new RuntimeException(e);
        }
        return o;
    }

    private static boolean waitForSocket(File f, long ms) {
        long end = System.currentTimeMillis() + ms;
        while (System.currentTimeMillis() < end) {
            /* exists(), not isFile(): a unix socket is not a regular file */
            if (f.exists() && canConnect(f)) return true;
            sleep(100);
        }
        return false;
    }

    private static boolean canConnect(File f) {
        Link l = null;
        try {
            l = Link.connect(f.getAbsolutePath());
            return true;
        } catch (IOException e) {
            return false;
        } finally {
            if (l != null) l.close();
        }
    }

    private void cleanRun() {
        for (String n : new String[]{"uart1", "ramcon", "qemu", "q6", "qemu.out", "q6.out"}) {
            File f = new File(paths.runDir, n + ".log");
            if (!n.endsWith(".out")) f = new File(paths.runDir, n);
            if (f.exists()) f.delete();
        }
    }

    private String tail(String name) {
        return tail(name, 6);
    }

    private String tail(String name, int lines) {
        try {
            File f = new File(paths.runDir, name);
            if (!f.isFile()) return "";
            RandomAccessFile raf = new RandomAccessFile(f, "r");
            try {
                int len = (int) Math.min(raf.length(), 8192);
                raf.seek(raf.length() - len);
                byte[] buf = new byte[len];
                raf.readFully(buf);
                String[] all = new String(buf, "UTF-8").trim().split("\n");
                StringBuilder sb = new StringBuilder();
                for (int i = Math.max(0, all.length - lines); i < all.length; i++) {
                    if (sb.length() > 0) sb.append("\n");
                    sb.append(all[i]);
                }
                return sb.toString();
            } finally {
                raf.close();
            }
        } catch (IOException e) {
            return "";
        }
    }

    private static String describe(Throwable e) {
        String m = e.getMessage();
        return m == null ? e.toString() : m;
    }

    private void note(String why) {
        Log.i(TAG, why);
        for (Listener l : listeners) l.onState(state, why);
    }

    static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    public File newLog(String name) {
        try {
            File f = new File(paths.runDir, name);
            FileOutputStream o = new FileOutputStream(f);
            o.close();
            return f;
        } catch (IOException e) {
            return paths.runDir;
        }
    }
}
