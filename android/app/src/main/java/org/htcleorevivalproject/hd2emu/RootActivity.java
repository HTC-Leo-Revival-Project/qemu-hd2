package org.htcleorevivalproject.hd2emu;

import android.os.Bundle;
import android.view.View;
import android.widget.ImageView;

import java.io.IOException;

/**
 * Root & USB: whether the app has root, and what this phone's kernel offers
 * the root features - the emulated HD2's USB on real USB first.  The features
 * themselves are listed with what they still need, so the screen shows the
 * state of things before they work.
 */
public class RootActivity extends HoloActivity {
    private Holo.Page page;
    private Config config;
    private UsbBridge.Probe probe;
    private String probeError;
    private boolean busy;
    /** Whether the passthrough bridge is up on the phone right now. */
    private boolean bridgeUp;
    /** Where the emulator's files are, for talking to the bridge. */
    private Paths paths;

    private final Runnable build = new Runnable() {
        @Override
        public void run() {
            build();
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        page = new Holo.Page(this);
        setContentView(page.scroll);
        Holo.apply(page.scroll);
        config = new Config(this);
        paths = new Paths(this);
        page.rebuild(build);
        /*
         * Ask on entry, so the screen is usable when it opens rather than after
         * a tap: the manager has usually granted root already (this is the screen
         * that asks for it), and then this is instant and shows no prompt.  It
         * also means the passthrough switch below is live, and the probe behind
         * it has run, without the user having to find the row above.
         */
        check(Root.status() != Root.Status.GRANTED);
    }

    private void build() {
        page.category(getString(R.string.root_cat_access));
        Root.Status s = Root.status();
        String summary;
        switch (s) {
            case GRANTED:
                summary = getString(R.string.root_granted) + (Root.manager().isEmpty() ? "" : " · " + Root.manager());
                break;
            case DENIED:
                summary = getString(R.string.root_denied);
                break;
            case ABSENT:
                summary = getString(R.string.root_absent);
                break;
            default:
                summary = getString(Root.present() ? R.string.root_not_asked : R.string.root_hidden);
        }
        Holo.Row access = page.row(getString(R.string.root_title), summary, s == Root.Status.GRANTED ? null : new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                check(true);
            }
        });
        access.addWidget(mark(s == Root.Status.GRANTED ? Boolean.TRUE
                : s == Root.Status.UNKNOWN ? null : Boolean.FALSE));
        page.note(getString(R.string.root_never_needed));

        page.category(getString(R.string.root_cat_usb));
        if (s != Root.Status.GRANTED) {
            page.note(getString(R.string.root_usb_needs_root));
        } else if (probeError != null) {
            page.note(getString(R.string.root_check_failed, probeError));
        } else if (probe == null) {
            page.note(getString(busy ? R.string.root_looking : R.string.root_not_checked));
        } else {
            for (UsbBridge.Check c : probe.checks) {
                page.row(c.title, c.detail, null).addWidget(mark(c.ok));
            }
        }
        if (s == Root.Status.GRANTED) {
            page.row(getString(R.string.root_check_again), getString(R.string.root_check_again_sum), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    check(false);
                }
            });
        }

        page.category(getString(R.string.root_cat_features));
        passthrough(s == Root.Status.GRANTED);
        feature(getString(R.string.root_host), getString(R.string.root_host_sum),
                probe == null ? null : probe.hostPossible(), getString(R.string.root_host_needs));
        page.note(getString(R.string.root_planned_note));
    }

    /**
     * The USB passthrough switch.  Always there, so the setting is visible and
     * can be turned off again; greyed out, with the reason on it, until root and
     * the probe say this phone's kernel has everything the bridge needs --
     * vhci_hcd is not one of those things, which is the whole point: the bridge
     * owns its own host controller.  While the bridge is up it says so rather
     * than offering to change a setting that is already in force.
     */
    private void passthrough(boolean root) {
        String what = getString(R.string.root_gadget_sum);
        String state;
        boolean can = false;

        if (!root) {
            state = getString(R.string.root_gadget_needs);
        } else if (probeError != null) {
            state = getString(R.string.root_check_failed, probeError);
        } else if (probe == null) {
            state = getString(busy ? R.string.root_looking : R.string.root_not_checked);
        } else if (!probe.gadgetPossible()) {
            state = getString(R.string.root_gadget_lacks);
        } else if (bridgeUp) {
            state = getString(R.string.root_gadget_holding);
        } else {
            state = "";
            can = true;
        }
        Holo.Row r = page.switchRow(getString(R.string.root_gadget),
                state.isEmpty() ? what : what + "\n" + state, config.usbPassthrough,
                can ? new Holo.OnToggle() {
                    @Override
                    public void onToggle(boolean on) {
                        config.usbPassthrough = on;
                        config.save();
                    }
                } : null);
        if (!can) {
            r.setEnabled(false);
        }
    }

    private void feature(String title, String what, Boolean possible, String needs) {
        String state = possible == null ? getString(R.string.root_later)
                : possible ? getString(R.string.root_later_ready)
                : getString(R.string.root_later_lacks, needs);
        Holo.Row row = page.switchRow(title, what + "\n" + state, false, null);
        row.setEnabled(false);
    }

    /** A small check, cross, or nothing (not known yet). */
    private View mark(Boolean ok) {
        ImageView v = new ImageView(this);
        if (ok != null) {
            v.setImageDrawable(HoloIcons.get(this, ok ? HoloIcons.CHECK : HoloIcons.CROSS, 24,
                    ok ? Holo.GREEN : 0xccff4444));
        }
        v.setMinimumWidth(Holo.dp(this, 24));
        return v;
    }

    /** Asks for root if {@code ask} (the manager's prompt), then probes; off the main thread. */
    private void check(final boolean ask) {
        if (busy) return;
        busy = true;
        final Holo.Progress bar = ask ? new Holo.Progress(this, getString(R.string.root_title), getString(R.string.root_waiting)) : null;
        page.rebuild(build);
        new Thread(new Runnable() {
            @Override
            public void run() {
                if (ask || Root.status() != Root.Status.GRANTED) Root.request();
                UsbBridge.Probe p = null;
                String err = null;
                boolean up = false;
                if (Root.status() == Root.Status.GRANTED) {
                    try {
                        p = UsbBridge.probe(RootActivity.this);
                        up = UsbBridge.running(paths);
                    } catch (IOException e) {
                        err = e.getMessage();
                    }
                }
                final UsbBridge.Probe found = p;
                final String error = err;
                final boolean running = up;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        busy = false;
                        probe = found;
                        probeError = error;
                        bridgeUp = running;
                        if (bar != null) bar.dismiss();
                        if (!isFinishing()) page.rebuild(build);
                    }
                });
            }
        }, "root-check").start();
    }
}
