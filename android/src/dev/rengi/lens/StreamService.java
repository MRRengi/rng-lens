package dev.rengi.lens;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.OrientationEventListener;

/**
 * Keeps streaming when the screen is locked or another app is in front. Runs as a foreground service
 * with a "Stop" notification. It lives only while the rng-lens screen is open or a PC is connected:
 * close the app with no PC attached and nothing stays behind.
 */
public final class StreamService extends Service implements Engine.Status {
    static final String ACTION_STOP = "dev.rengi.lens.STOP";
    private static final String CHANNEL = "stream";
    private static final int NOTIFICATION_ID = 1;

    /** The open screen, if any (same process, main thread). */
    static volatile Engine.Status ui;
    static volatile boolean activityOpen;
    private static volatile StreamService instance;

    private final Handler main = new Handler(Looper.getMainLooper());
    private Engine engine;
    private OrientationEventListener orientation;
    private String lastLine = "Starting...";
    private boolean lastStreaming;

    static void start(Context c) {
        Intent i = new Intent(c, StreamService.class);
        if (Build.VERSION.SDK_INT >= 26) {
            try { // startForegroundService, API 26
                Context.class.getMethod("startForegroundService", Intent.class).invoke(c, i);
                return;
            } catch (Exception ignored) {
            }
        }
        c.startService(i);
    }

    /** Called when the screen closes: stop unless a PC is watching. */
    static void screenClosed() {
        activityOpen = false;
        StreamService s = instance;
        if (s != null) s.stopIfIdle();
    }

    static void replayStatus() {
        StreamService s = instance;
        if (s != null && ui != null) ui.onStatus(s.lastLine, s.lastStreaming);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        startForeground(NOTIFICATION_ID, buildNotification("Waiting for the PC app"));
        engine = new Engine(this, this);
        engine.start();
        orientation = new OrientationEventListener(this) {
            @Override
            public void onOrientationChanged(int deg) {
                if (deg != ORIENTATION_UNKNOWN) engine.setDeviceOrientation(((deg + 45) / 90 * 90) % 360);
            }
        };
        if (orientation.canDetectOrientation()) orientation.enable();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            stopSelf();
            return START_NOT_STICKY;
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        instance = null;
        orientation.disable();
        engine.stop();
        stopForeground(true);
        Engine.Status u = ui;
        if (u != null) u.onStatus("Stopped.\nTap here to start again.", false);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onStatus(final String line, final boolean streaming) {
        main.post(new Runnable() {
            @Override
            public void run() {
                boolean changed = streaming != lastStreaming;
                lastLine = line;
                lastStreaming = streaming;
                Engine.Status u = ui;
                if (u != null) u.onStatus(line, streaming);
                if (changed) {
                    NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
                    nm.notify(NOTIFICATION_ID, buildNotification(streaming ? "Streaming to your PC" : "Waiting for the PC app"));
                    if (!streaming) stopIfIdle();
                }
            }
        });
    }

    private void stopIfIdle() {
        if (!activityOpen && engine != null && !engine.connected()) stopSelf();
    }

    @SuppressWarnings("deprecation")
    private Notification buildNotification(String text) {
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            b = channelBuilder();
        } else {
            b = new Notification.Builder(this);
            b.setPriority(Notification.PRIORITY_LOW);
        }
        Intent open = new Intent(this, MainActivity.class).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        Intent stop = new Intent(this, StreamService.class).setAction(ACTION_STOP);
        int piFlags = Build.VERSION.SDK_INT >= 23 ? 0x04000000 : 0; // FLAG_IMMUTABLE
        b.setSmallIcon(R.drawable.ic_launcher)
                .setContentTitle("Lens")
                .setContentText(text)
                .setOngoing(true)
                .setShowWhen(false)
                .setContentIntent(PendingIntent.getActivity(this, 0, open, piFlags))
                .addAction(new Notification.Action.Builder(0, "Stop", PendingIntent.getService(this, 1, stop, piFlags)).build());
        return b.build();
    }

    /** API 26+: notification channel and the channel-aware builder, through reflection (we compile against 23). */
    private Notification.Builder channelBuilder() {
        try {
            Class<?> chCls = Class.forName("android.app.NotificationChannel");
            Object ch = chCls.getConstructor(String.class, CharSequence.class, int.class)
                    .newInstance(CHANNEL, "Streaming", 2 /* IMPORTANCE_LOW */);
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            NotificationManager.class.getMethod("createNotificationChannel", chCls).invoke(nm, ch);
            return Notification.Builder.class.getConstructor(Context.class, String.class).newInstance(this, CHANNEL);
        } catch (Exception e) {
            return new Notification.Builder(this);
        }
    }
}
