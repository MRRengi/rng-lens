package dev.rengi.lens;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Owns the server socket, the discovery beacon, the current PC link and the camera pipeline. */
final class Engine implements Link.Listener, Pipeline.Sink, AudioPipeline.Sink {
    interface Status {
        void onStatus(String line, boolean streaming);
    }

    private static final String TAG = "rnglens";

    private final Context context;
    private final Status status;
    private final HandlerThread thread;
    private final Handler handler;
    private final Pipeline pipeline;
    private final AudioPipeline audio = new AudioPipeline(this);
    private boolean audioOn; // camera thread only
    private final Settings settings = new Settings();
    private Catalog catalog;
    private volatile Link link;
    private volatile boolean running;
    private ServerSocket server;
    private DatagramSocket beacon;
    private WifiManager.WifiLock wifiLock;
    private WifiManager.MulticastLock multicastLock;
    private int deviceOrientation;
    private final ThermalPolicy thermal = new ThermalPolicy();
    private Settings running_; // what the pipeline was last started with (after thermal limits)
    private android.os.PowerManager.WakeLock wakeLock;
    private java.lang.reflect.Method thermalStatusM, thermalHeadroomM;
    private android.os.PowerManager powerManager;
    private long lastBytes, lastStatsAt;

    Engine(Context context, Status status) {
        this.context = context.getApplicationContext();
        this.status = status;
        thread = new HandlerThread("rnglens-cam", Process.THREAD_PRIORITY_URGENT_DISPLAY);
        thread.start();
        handler = new Handler(thread.getLooper());
        CameraManager cm = (CameraManager) this.context.getSystemService(Context.CAMERA_SERVICE);
        pipeline = new Pipeline(cm, handler, this);
        powerManager = (android.os.PowerManager) this.context.getSystemService(Context.POWER_SERVICE);
        try {
            thermalStatusM = android.os.PowerManager.class.getMethod("getCurrentThermalStatus"); // API 29
        } catch (Exception ignored) {
        }
        try {
            thermalHeadroomM = android.os.PowerManager.class.getMethod("getThermalHeadroom", int.class); // API 30
        } catch (Exception ignored) {
        }
    }

    boolean connected() {
        return link != null;
    }

    void start() {
        running = true;
        WifiManager wm = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
        if (wm != null) { // lets broadcast probes from the PC reach us
            multicastLock = wm.createMulticastLock("rnglens");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        }
        handler.post(new Runnable() {
            @Override
            public void run() {
                catalog = Catalog.build((CameraManager) context.getSystemService(Context.CAMERA_SERVICE));
                Catalog.Cam cam = catalog.find(null);
                if (cam != null) settings.camera = cam.id;
                Log.i(TAG, "cameras: " + catalog.cams.size() + ", avc " + catalog.avcEncoder + ", hevc " + catalog.hevcEncoder);
                report();
            }
        });
        new Thread("rnglens-server") {
            @Override
            public void run() {
                serve();
            }
        }.start();
        new Thread("rnglens-beacon") {
            @Override
            public void run() {
                broadcast();
            }
        }.start();
        handler.postDelayed(tick, 2000);
    }

    void stop() {
        running = false;
        handler.removeCallbacks(tick);
        try {
            if (server != null) server.close();
        } catch (Exception ignored) {
        }
        if (beacon != null) beacon.close();
        if (multicastLock != null) multicastLock.release();
        Link l = link;
        if (l != null) l.close();
        handler.post(new Runnable() {
            @Override
            public void run() {
                pipeline.stop();
                audio.stop();
                releaseWifiLock();
                releaseWakeLock();
            }
        });
        thread.quitSafely();
    }

    void setDeviceOrientation(final int degrees) {
        handler.post(new Runnable() {
            @Override
            public void run() {
                if (degrees == deviceOrientation) return;
                deviceOrientation = degrees;
                sendState();
            }
        });
    }

    // ---- network ----

    private void serve() {
        try {
            server = new ServerSocket();
            server.setReuseAddress(true);
            server.bind(new InetSocketAddress(Protocol.PORT));
            while (running) {
                Socket s = server.accept();
                final Link l = new Link(s, this);
                Link old = link;
                link = l;
                if (old != null) old.close();
                l.start();
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        onConnected(l);
                    }
                });
            }
        } catch (Exception e) {
            if (running) Log.e(TAG, "server stopped", e);
        }
    }

    /**
     * One UDP socket on the discovery port: beacons to every broadcast address while idle, and a unicast
     * answer to PC probes (Windows Firewall lets replies to its own broadcast through without asking).
     */
    private void broadcast() {
        try {
            beacon = new DatagramSocket(null);
            beacon.setReuseAddress(true);
            beacon.setBroadcast(true);
            beacon.bind(new InetSocketAddress(Protocol.DISCOVERY_PORT));
            beacon.setSoTimeout(1000);
            byte[] msg = ("{\"rnglens\":" + Protocol.VERSION + ",\"name\":\"" + Build.MODEL.replace("\"", "")
                    + "\",\"port\":" + Protocol.PORT + "}").getBytes(Protocol.UTF8);
            byte[] buf = new byte[512];
            long lastBeacon = 0;
            while (running) {
                long now = System.currentTimeMillis();
                if (link == null && now - lastBeacon >= 1000) {
                    lastBeacon = now;
                    for (InetAddress to : broadcastAddresses()) {
                        try {
                            beacon.send(new DatagramPacket(msg, msg.length, to, Protocol.DISCOVERY_PORT));
                        } catch (Exception ignored) { // no network right now
                        }
                    }
                }
                try {
                    DatagramPacket in = new DatagramPacket(buf, buf.length);
                    beacon.receive(in);
                    String text = new String(in.getData(), 0, in.getLength(), Protocol.UTF8);
                    if (text.contains("rnglensProbe")) {
                        beacon.send(new DatagramPacket(msg, msg.length, in.getAddress(), in.getPort()));
                    }
                } catch (java.net.SocketTimeoutException ignored) {
                }
            }
        } catch (Exception e) {
            if (running) Log.e(TAG, "discovery stopped", e);
        }
    }

    private static List<InetAddress> broadcastAddresses() {
        List<InetAddress> out = new ArrayList<InetAddress>();
        try {
            out.add(InetAddress.getByName("255.255.255.255"));
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (java.net.InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    if (ia.getBroadcast() != null && !out.contains(ia.getBroadcast())) out.add(ia.getBroadcast());
                }
            }
        } catch (Exception ignored) {
        }
        return out;
    }

    private void onConnected(Link l) {
        if (l != link || catalog == null) return;
        try {
            JSONObject hello = catalog.toJson();
            hello.put("proto", Protocol.VERSION);
            hello.put("device", Build.MANUFACTURER + " " + Build.MODEL);
            try {
                hello.put("version", context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName);
            } catch (Exception ignored) {
            }
            hello.put("state", stateJson());
            hello.put("mic", micAllowed());
            l.send(Protocol.json(Protocol.HELLO, hello.toString()));
        } catch (JSONException ignored) {
        }
        acquireWifiLock();
        acquireWakeLock();
        startPipeline();
        updateAudio(true);
    }

    private boolean micAllowed() {
        return context.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    /** Runs on the camera thread. Starts, restarts or stops the microphone to match the settings. */
    private void updateAudio(boolean restart) {
        boolean want = link != null && settings.audio && micAllowed();
        if (want && (restart || !audioOn)) {
            audio.start("raw".equals(settings.mic));
            audioOn = true;
        } else if (!want && audioOn) {
            audio.stop();
            audioOn = false;
        }
    }

    @Override
    public void onAudioConfig(byte[] message) {
        Link l = link;
        if (l != null) l.send(message);
    }

    @Override
    public void onAudio(byte[] message) {
        Link l = link;
        if (l != null) l.sendAudio(message);
    }

    @Override
    public void onControl(final Link l, final String json) {
        handler.post(new Runnable() {
            @Override
            public void run() {
                if (l != link) return;
                int change;
                try {
                    change = settings.apply(new JSONObject(json));
                } catch (JSONException e) {
                    return;
                }
                if ((change & Settings.CHANGE_RESTART) != 0) {
                    startPipeline();
                } else {
                    if ((change & Settings.CHANGE_REQUEST) != 0) {
                        // The camera runs on a thermal-limited copy of the settings: hand it the new controls.
                        if (running_ != null) running_.copyRequestFrom(settings);
                        pipeline.updateRequest(true);
                    }
                    if ((change & Settings.CHANGE_BITRATE) != 0)
                        pipeline.setBitrate(settings.bitrate, thermal.limits().bitrateScale);
                    if ((change & Settings.CHANGE_KEYFRAME) != 0) pipeline.requestKeyframe();
                }
                if ((change & Settings.CHANGE_AUDIO) != 0) updateAudio(true);
                sendState();
            }
        });
    }

    @Override
    public void onNeedKeyframe(Link l) {
        handler.post(new Runnable() {
            @Override
            public void run() {
                pipeline.requestKeyframe();
            }
        });
    }

    @Override
    public void onClosed(final Link l) {
        handler.post(new Runnable() {
            @Override
            public void run() {
                if (link != l) return;
                link = null;
                pipeline.stop(); // nobody watching: camera, encoder and mic off
                updateAudio(false);
                releaseWifiLock();
                releaseWakeLock();
                report();
            }
        });
    }

    // ---- pipeline ----

    private void startPipeline() {
        Catalog.Cam cam = catalog.find(settings.camera);
        if (cam == null) {
            status.onStatus("No usable camera found", false);
            return;
        }
        settings.camera = cam.id;
        running_ = effective();
        pipeline.start(catalog, cam, running_, thermal.limits().bitrateScale);
        report();
    }

    /** The user's settings with the thermal limits applied. */
    private Settings effective() {
        Settings e = settings.copy();
        ThermalPolicy.Limits lim = thermal.limits();
        int[] wh = ThermalPolicy.clampSize(e.width, e.height, lim.maxPixels);
        e.width = wh[0];
        e.height = wh[1];
        e.fps = Math.min(e.fps, lim.maxFps);
        return e;
    }

    /** Runs on the camera thread every tick. */
    private void readThermal() {
        int status = -1;
        float headroom = Float.NaN, batteryC = Float.NaN;
        try {
            if (thermalStatusM != null) status = (Integer) thermalStatusM.invoke(powerManager);
        } catch (Exception ignored) {
        }
        try {
            if (thermalHeadroomM != null) headroom = (Float) thermalHeadroomM.invoke(powerManager, 10);
        } catch (Exception ignored) {
        }
        Intent bat = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (bat != null) batteryC = bat.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10f;
        if (!thermal.update(System.currentTimeMillis(), status, headroom, batteryC) || link == null || running_ == null) return;
        Log.i(TAG, "thermal level " + thermal.limits().level + ": " + thermal.limits().describe());
        Settings e = effective();
        if (e.width != running_.width || e.height != running_.height || e.fps != running_.fps) {
            startPipeline();
        } else {
            pipeline.setBitrate(settings.bitrate, thermal.limits().bitrateScale);
        }
        sendState();
    }

    @Override
    public void onConfig(int codec, int width, int height, ByteBuffer csd, int size) {
        Link l = link;
        if (l == null) return;
        byte[] b = new byte[size];
        csd.get(b);
        l.send(Protocol.config(codec, width, height, b, 0, size));
    }

    @Override
    public void onFrame(ByteBuffer data, int size, long ptsUs, boolean key) {
        Link l = link;
        if (l != null) l.sendFrame(data, size, ptsUs, key);
    }

    @Override
    public void onPipelineError(final String what) {
        Log.e(TAG, what);
        status.onStatus("Camera problem: " + what + " - retrying", true);
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (link != null) startPipeline();
            }
        }, 1500);
    }

    // ---- state ----

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            readThermal();
            if (link != null) sendState();
            report();
            handler.postDelayed(this, 2000);
        }
    };

    private JSONObject stateJson() throws JSONException {
        JSONObject s = settings.toJson();
        s.put("orient", deviceOrientation);
        s.put("streamWidth", pipeline.width());
        s.put("streamHeight", pipeline.height());
        s.put("streamFps", pipeline.fps());
        s.put("streamBitrate", pipeline.bitrate());
        Intent bat = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (bat != null) {
            int lvl = bat.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = bat.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            s.put("battery", lvl < 0 ? -1 : lvl * 100 / Math.max(1, scale));
            s.put("tempC", bat.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0);
            s.put("charging", bat.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0);
        }
        Link l = link;
        if (l != null) s.put("dropped", l.droppedFrames());
        JSONObject t = new JSONObject();
        ThermalPolicy.Limits lim = thermal.limits();
        t.put("level", lim.level);
        t.put("status", thermal.status());
        if (!Float.isNaN(thermal.headroom())) t.put("headroom", (double) thermal.headroom());
        t.put("throttled", lim.throttled());
        t.put("limit", lim.describe());
        s.put("thermal", t);
        return s;
    }

    private void sendState() {
        Link l = link;
        if (l == null) return;
        try {
            l.send(Protocol.json(Protocol.STATE, stateJson().toString()));
        } catch (JSONException ignored) {
        }
    }

    private void report() {
        Link l = link;
        if (l == null) {
            List<String> ips = localIps();
            String ip = ips.isEmpty() ? "no Wi-Fi" : android.text.TextUtils.join(", ", ips);
            status.onStatus("Waiting for the PC app\nWi-Fi: " + ip + "\nUSB: plug the cable in and press USB in the PC app", false);
            return;
        }
        long now = System.currentTimeMillis(), bytes = l.bytesSent();
        long kbps = lastStatsAt == 0 ? 0 : (bytes - lastBytes) * 8 / Math.max(1, now - lastStatsAt);
        lastBytes = bytes;
        lastStatsAt = now;
        Catalog.Cam cam = pipeline.camera();
        String lens = cam == null ? "" : (cam.facing == CameraCharacteristics.LENS_FACING_FRONT ? "front" : "back") + " #" + cam.id;
        String heat = thermal.limits().throttled() ? "\nHot: lowered to " + thermal.limits().describe() : "";
        status.onStatus("Streaming to " + l.peer() + "\n" + pipeline.width() + "x" + pipeline.height() + " @ " + pipeline.fps()
                + " fps, " + lens + "\n" + String.format(java.util.Locale.US, "%.1f", kbps / 1000.0) + " Mbit/s" + heat, true);
    }

    private static List<String> localIps() {
        List<String> out = new ArrayList<String>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InetAddress a : Collections.list(ni.getInetAddresses())) {
                    if (a instanceof Inet4Address) out.add(a.getHostAddress());
                }
            }
        } catch (Exception ignored) {
        }
        return out;
    }

    private void acquireWifiLock() {
        if (wifiLock != null) return;
        WifiManager wm = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
        if (wm == null) return;
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "rnglens");
        wifiLock.setReferenceCounted(false);
        wifiLock.acquire();
    }

    private void acquireWakeLock() {
        if (wakeLock != null || powerManager == null) return;
        // Keeps the CPU awake for the camera and network while the screen is locked.
        wakeLock = powerManager.newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK, "rnglens:stream");
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire();
    }

    private void releaseWakeLock() {
        if (wakeLock != null) {
            wakeLock.release();
            wakeLock = null;
        }
    }

    private void releaseWifiLock() {
        if (wifiLock != null) {
            wifiLock.release();
            wifiLock = null;
        }
    }
}
