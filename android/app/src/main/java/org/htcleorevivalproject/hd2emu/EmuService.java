package org.qemu.htcleo;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;

/**
 * Holds the emulator while the app is in the background.  The emulator is a
 * child process of this app: if Android kills the app, it kills that too, so a
 * plain foreground service (with the classic Holo notification) is what keeps
 * the phone running while you read its settings or look at the logs.
 */
public class EmuService extends Service {
    public static final String CHANNEL = "htcleo";
    public static final int NOTIFY = 1;

    private final IBinder binder = new LocalBinder();
    private Emulator machine;

    public class LocalBinder extends Binder {
        public Emulator get() {
            return machine;
        }

        public EmuService service() {
            return EmuService.this;
        }
    }

    public Emulator machine() {
        return machine;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        machine = new Emulator(this);
        synchronized (EmuService.class) {
            shared = machine;
        }
        channel();
    }

    private static Emulator shared;

    /**
     * The one emulator, whichever activity needs it.  The service is created
     * by onCreate when the app binds it; if an activity asks before that, it
     * gets an emulator of its own, and the service adopts it later.
     */
    public static synchronized Emulator instance(Context ctx) {
        if (shared == null) shared = new Emulator(ctx.getApplicationContext());
        return shared;
    }

    public void stopSelfWhenIdle() {
        stopSelf();
    }

    /* ---------------------------------------------------------- notification */

    private void channel() {
        if (Build.VERSION.SDK_INT < 26) return;
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null || nm.getNotificationChannel(CHANNEL) != null) return;
        NotificationChannel c = new NotificationChannel(CHANNEL, getString(R.string.app_name),
                NotificationManager.IMPORTANCE_LOW);
        c.setDescription("The emulated HTC HD2");
        c.setShowBadge(false);
        nm.createNotificationChannel(c);
    }

    private Notification notification(Emulator.State state, String message) {
        Intent open = new Intent(this, MainActivity.class);
        open.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, pendingIntentFlags());

        Intent stop = new Intent(this, Receiver.class);
        stop.setAction(Receiver.STOP);
        PendingIntent stopPi = PendingIntent.getBroadcast(this, 1, stop, pendingIntentFlags());

        Notification.Builder b = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL)
                : new Notification.Builder(this);
        b.setContentTitle(getString(R.string.app_name))
                .setContentText(describe(state, message))
                .setSmallIcon(android.R.drawable.ic_menu_manage)
                .setOngoing(state != Emulator.State.STOPPED && state != Emulator.State.CRASHED)
                .setContentIntent(pi);
        if (state == Emulator.State.RUNNING || state == Emulator.State.STARTING) {
            b.addAction(new Notification.Action.Builder(null, getString(R.string.shut_down), stopPi).build());
        }
        return b.build();
    }

    static int pendingIntentFlags() {
        int f = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23) f |= PendingIntent.FLAG_IMMUTABLE;
        return f;
    }

    private String describe(Emulator.State state, String message) {
        switch (state) {
            case STARTING: return getString(R.string.state_starting);
            case RUNNING: return getString(R.string.state_running);
            case STOPPING: return getString(R.string.state_stopping);
            case CRASHED: return message == null || message.isEmpty()
                    ? getString(R.string.state_crashed) : getString(R.string.state_crashed) + ": " + message;
            default: return getString(R.string.state_stopped);
        }
    }

    public void update(Emulator.State state, String message) {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;
        nm.notify(NOTIFY, notification(state, message));
        if (state == Emulator.State.STOPPED || state == Emulator.State.CRASHED) {
            stopSelf();
        }
    }

    /** Puts the service in the foreground; safe to call again. */
    public void foreground() {
        Notification n = notification(Emulator.State.STARTING, "");
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIFY, n,
                    android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
                            | android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFY, n);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && Receiver.STOP.equals(intent.getAction())) {
            Emulator e = instance(this);
            if (e != null) e.stop();
            return START_NOT_STICKY;
        }
        foreground();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        synchronized (EmuService.class) {
            if (shared == machine) shared = null;
        }
    }

    /** The notification's Shut down button. */
    public static class Receiver extends android.content.BroadcastReceiver {
        public static final String STOP = "org.qemu.htcleo.STOP";

        @Override
        public void onReceive(Context context, Intent intent) {
            if (!STOP.equals(intent.getAction())) return;
            Emulator e = instance(context);
            if (e != null) e.stop();
        }
    }
}
