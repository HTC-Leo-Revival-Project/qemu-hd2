package org.htcleorevivalproject.hd2emu;

import android.content.Context;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * The emulated HD2's USB, on real USB - the first root feature, and the
 * reason the root framework exists.  Not wired to QEMU yet: this is what it
 * will need, and the probe that tells whether this phone has it.
 *
 * Two ways the HD2 can come out of the emulator:
 *
 * GADGET - to a PC, through this phone's USB port.  The PC sees an HTC HD2,
 * so its own tools (fastboot, adb, the RUU flashers, nanddump recoveries)
 * talk to the emulated phone.  The phone's USB controller (UDC) is driven
 * through configfs (/config/usb_gadget), with a FunctionFS function whose
 * descriptors and endpoints are the HD2's: a bridge copies the HD2 gadget's
 * descriptors to FunctionFS's ep0 and moves every transfer between the
 * FunctionFS endpoints and the emulator.  QEMU's side of that is the USB/IP
 * stream htcleo_usbip.c already speaks to vhci_hcd for usb-host=lle; it needs
 * a way to hand that stream to the app (a unix socket) instead of attaching
 * it to vhci_hcd.  The phone's own gadget (adb, MTP) has to be unbound from
 * the UDC while the HD2 holds it, and put back afterwards.
 *
 * HOST - to this phone itself, through vhci_hcd: what usb-host=lle does on a
 * PC, so adb/fastboot running on this phone (Termux) see the HD2.  QEMU
 * writes vhci_hcd's attach file itself, which needs root, and few phone
 * kernels build vhci_hcd.
 */
public final class UsbBridge {
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
        public boolean configfs, functionfs, udc, rawGadget, vhci;
        public String udcName = "";
        public String selinux = "";
        public String gadgets = "";

        /** Exposing the HD2 to a PC: configfs, FunctionFS and a UDC to bind it to. */
        public boolean gadgetPossible() {
            return configfs && functionfs && udc;
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

        p.vhci = yes("[ -d /sys/bus/platform/drivers/vhci_hcd ] || [ -d /sys/module/vhci_hcd ]");
        p.checks.add(new Check("vhci_hcd", p.vhci, p.vhci
                ? c.getString(R.string.usb_vhci_yes) : c.getString(R.string.usb_vhci_no)));

        p.selinux = out("getenforce");
        p.checks.add(new Check("SELinux", true, p.selinux.isEmpty() ? c.getString(R.string.usb_unknown)
                : "Enforcing".equalsIgnoreCase(p.selinux)
                ? c.getString(R.string.usb_selinux_enforcing, p.selinux) : p.selinux));
        return p;
    }
}
