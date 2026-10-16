package org.htcleorevivalproject.hd2emu;

import android.content.Context;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * The emulated HD2's USB, on real USB.
 *
 * GADGET - to a PC, through this phone's USB port.  The PC sees an HTC HD2, so
 * its own tools (fastboot, adb, the RUU flashers, nanddump recoveries) talk to
 * the emulated phone.  The phone's USB controller (UDC) is driven through
 * configfs (/config/usb_gadget) with a FunctionFS function whose descriptors
 * and endpoints are the HD2's, and {@code usbffs} (android/native/usbffs.c,
 * built into jniLibs) moves every transfer between those endpoints and the
 * emulator.  QEMU's side of that is the USB/IP stream htcleo_usbip.c already
 * speaks to vhci_hcd for usb-host=lle; with usb-ip-socket it hands the same
 * stream to the bridge over a unix socket instead, and no vhci_hcd is involved
 * or needed.  The phone's own gadget (adb, MTP) is unbound from the UDC while
 * the HD2 holds it, and put back afterwards.
 *
 * HOST - to this phone itself, through vhci_hcd: what usb-host=lle does on a
 * PC, so adb/fastboot running on this phone (Termux) see the HD2.  QEMU writes
 * vhci_hcd's attach file itself, which needs root, and few phone kernels build
 * vhci_hcd.  Not wired up, and not what the passthrough switch uses.
 *
 * What the passthrough takes from the phone is undone when it stops: the bridge
 * gives the UDC back and restarts vendor.usb-hal.  sys.usb.config is never
 * written -- that restarts adbd, and with it wireless adb into this phone.
 */
public final class UsbBridge {
    /** The service the bridge stops while it holds the UDC, and starts again after. */
    public static final String USB_HAL = "vendor.usb-hal";

    /** One thing the probe looked at. */
    public static final class Check {
        public final String title;
        public final boolean ok;
        public final String detail;

        Check(String title, boolean ok, String detail) {
            this.title = title;
            this.ok = ok;
            this.detail = detail;
        }
    }

    /** What the probe found, and what it makes possible. */
    public static final class Probe {
        public final List<Check> checks = new ArrayList<Check>();
        public boolean configfs, functionfs, udc, rawGadget, vhci, usbHal, bridge;
        public String udcName = "";
        public String selinux = "";
        public String gadgets = "";
        public String usbHalName = "";
        public String bridgePath = "";

        /**
         * Exposing the HD2 to a PC: configfs, FunctionFS and a UDC to bind it to,
         * a USB HAL service the bridge can stop while it holds the port, and the
         * bridge binary itself.  vhci_hcd is deliberately not in here: the
         * bridge owns its own host controller and does not need one.
         */
        public boolean gadgetPossible() {
            return configfs && functionfs && udc && usbHal && bridge;
        }

        /** Attaching it to this phone's own USB stack. */
        public boolean hostPossible() {
            return vhci;
        }
    }

    private UsbBridge() {
    }

    private static boolean yes(String test) throws IOException {
        return Root.run("if " + test + "; then echo y; else echo n; fi").out.trim().endsWith("y");
    }

    private static String out(String command) throws IOException {
        return Root.run(command + " 2>/dev/null || true").out.trim();
    }

    /** Looks, as root, at what this phone's kernel offers.  Blocks; asks for root if needed. */
    public static Probe probe(Context c) throws IOException {
        Probe p = new Probe();

        p.configfs = yes("[ -d /config/usb_gadget ]");
        p.gadgets = out("ls /config/usb_gadget").replace('\n', ' ').trim();
        p.checks.add(new Check(c.getString(R.string.usb_configfs), p.configfs, p.configfs
                ? "/config/usb_gadget" + (p.gadgets.isEmpty() ? "" : ": " + p.gadgets)
                : c.getString(R.string.usb_configfs_no)));

        p.functionfs = yes("grep -qw functionfs /proc/filesystems");
        p.checks.add(new Check("FunctionFS", p.functionfs, p.functionfs
                ? c.getString(R.string.usb_ffs_yes) : c.getString(R.string.usb_ffs_no)));

        p.udcName = out("ls /sys/class/udc").replace('\n', ' ').trim();
        p.udc = !p.udcName.isEmpty();
        String bound = "";
        if (p.configfs && !p.gadgets.isEmpty()) {
            bound = out("for g in /config/usb_gadget/*; do [ -f \"$g/UDC\" ] && cat \"$g/UDC\"; done")
                    .replace('\n', ' ').trim();
        }
        p.checks.add(new Check(c.getString(R.string.usb_udc), p.udc, p.udc
                ? c.getString(bound.isEmpty() ? R.string.usb_udc_free : R.string.usb_udc_busy, p.udcName)
                : c.getString(R.string.usb_udc_no)));

        p.rawGadget = yes("[ -e /dev/raw-gadget ]");
        p.checks.add(new Check("raw-gadget", p.rawGadget, p.rawGadget
                ? c.getString(R.string.usb_raw_yes) : c.getString(R.string.usb_raw_no)));

        /*
         * The bridge holds the UDC by stopping the USB HAL and taking the UDC
         * file away from whatever gadget has it, so a HAL service it can stop
         * is part of being able to do this at all.  Only the service: writing
         * sys.usb.config would restart adbd.
         */
        /*
         * The service the bridge stops is vendor.usb-hal, so look for that one
         * specifically: a plain grep for "usb" in init.svc.* finds usbd first on
         * most phones (usbd is adbd's helper, and never running), which would
         * both misname the row and pass a check for a service that does not do
         * what the bridge needs.
         */
        p.usbHalName = out("getprop init.svc." + USB_HAL);
        p.usbHal = !p.usbHalName.isEmpty();
        p.checks.add(new Check(c.getString(R.string.usb_hal), p.usbHal, p.usbHal
                ? c.getString(R.string.usb_hal_yes, USB_HAL, p.usbHalName)
                : c.getString(R.string.usb_hal_no)));

        try {
            p.bridgePath = new Paths(c).usbffs().getAbsolutePath();
            p.bridge = true;
        } catch (IOException e) {
            p.bridge = false;
        }
        p.checks.add(new Check("usbffs", p.bridge, p.bridge
                ? p.bridgePath : c.getString(R.string.usb_bridge_no)));

        p.vhci = yes("[ -d /sys/bus/platform/drivers/vhci_hcd ] || [ -d /sys/module/vhci_hcd ]");
        p.checks.add(new Check("vhci_hcd", p.vhci, c.getString(
                p.vhci ? R.string.usb_vhci_yes : R.string.usb_vhci_unused)));

        p.selinux = out("getenforce");
        p.checks.add(new Check("SELinux", true, p.selinux.isEmpty() ? c.getString(R.string.usb_unknown)
                : "Enforcing".equalsIgnoreCase(p.selinux)
                ? c.getString(R.string.usb_selinux_enforcing, p.selinux) : p.selinux));
        return p;
    }

    /* ---- running it -------------------------------------------------------- */

    /**
     * Starts the bridge against QEMU's USB/IP socket.  QEMU has to be listening
     * first; the bridge waits for it anyway, since the guest needs a moment to
     * boot before it answers anything.  Returns when the bridge is running, or
     * throws with what it said -- it exits rather than limping along if it
     * cannot bind the UDC.
     */
    public static void start(Paths paths) throws IOException {
        final File bin = paths.usbffs();
        final File sock = paths.usbIpSocket();
        final File log = paths.usbBridgeLog();

        stop();
        log.delete();
        /* setsid so it outlives this shell; its own output goes to the log, so
         * it never holds the root shell's stdout open */
        Root.run("setsid " + bin.getAbsolutePath() + " --usbip " + sock.getAbsolutePath()
                + " --verbose > " + log.getAbsolutePath() + " 2>&1 &");
        for (int i = 0; i < 60 && !running(); i++) {
            try {
                Thread.sleep(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        if (!running()) {
            throw new IOException(log(paths));
        }
    }

    /**
     * Stops the bridge.  SIGTERM, so it gives the UDC back and restarts the USB
     * HAL on the way out; anything still holding the port after that is a bug,
     * and gets a SIGKILL.
     */
    public static void stop() {
        try {
            Root.run("pkill -x usbffs || true", 10000);
            for (int i = 0; i < 20 && running(); i++) {
                Thread.sleep(250);
            }
            if (running()) Root.run("pkill -9 -x usbffs || true", 10000);
        } catch (IOException ignored) {
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    public static boolean running() {
        try {
            return Root.run("pidof usbffs 2>/dev/null || true", 5000).out.trim().length() > 0;
        } catch (IOException e) {
            return false;
        }
    }

    /** The tail of the bridge's own log, for when it could not start. */
    public static String log(Paths paths) {
        String tail = "";
        try {
            List<String> lines = new ArrayList<String>();
            java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(paths.usbBridgeLog()));
            try {
                String line;
                while ((line = r.readLine()) != null) {
                    lines.add(line);
                    if (lines.size() > 12) lines.remove(0);
                }
            } finally {
                r.close();
            }
            StringBuilder sb = new StringBuilder();
            for (String line : lines) sb.append(line).append('\n');
            tail = sb.toString().trim();
        } catch (IOException ignored) {
        }
        return tail.isEmpty() ? "usbffs did not stay running" : tail;
    }

    /** Whether the phone's USB is the emulated HD2's right now. */
    public static boolean holdingPort() {
        try {
            return Root.run("cat /config/usb_gadget/hd2/UDC 2>/dev/null || true", 5000)
                    .out.trim().length() > 0;
        } catch (IOException e) {
            return false;
        }
    }
}