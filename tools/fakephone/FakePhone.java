import dev.rengi.lens.Link;
import dev.rengi.lens.Protocol;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Test-bench phone: serves the rng-lens protocol using the phone app's own Protocol and Link classes,
 * with H.264/HEVC from ffmpeg's test pattern standing in for the camera + MediaCodec.
 * Behaves like the phone: SPS/PPS(/VPS) only in CONFIG, frames without them, restarts on size/fps/codec changes.
 *
 *   java -cp out FakePhone [port]
 */
public final class FakePhone implements Link.Listener {
    private volatile Link link;
    private volatile Process ffmpeg;
    private volatile int width = 1280, height = 720, fps = 30, codec = Protocol.CODEC_H264, zoomPct = 100;
    private final Object restartLock = new Object();
    private volatile int generation;

    private volatile int fakeThermal = Integer.parseInt(System.getenv().getOrDefault("FAKE_THERMAL", "0")); // bench: pretend the phone is this hot (0..4)
    private final boolean aac; // which audio codec this fake phone "has": Opus (Android 10+) or AAC
    private volatile Process audioProc;
    private volatile int audioGen;

    private final String image; // null = test pattern; else a picture as the "camera" (suffix +noise adds grain)

    FakePhone(boolean aac, String image) {
        this.aac = aac;
        this.image = image;
    }

    /** java FakePhone [port] [opus|aac] */
    public static void main(String[] args) throws Exception {
        int port = args.length > 0 ? Integer.parseInt(args[0]) : Protocol.PORT;
        boolean aac = args.length > 1 && args[1].equalsIgnoreCase("aac");
        new FakePhone(aac, args.length > 2 ? args[2] : null).serve(port);
    }

    private void serve(int port) throws IOException {
        ServerSocket server = new ServerSocket();
        server.setReuseAddress(true);
        server.bind(new InetSocketAddress(port));
        log("listening on " + port);
        startBeacon(port);
        while (true) {
            Socket s = server.accept();
            Link l = new Link(s, this);
            Link old = link;
            link = l;
            if (old != null) old.close();
            l.start();
            log("PC connected from " + l.peer());
            l.send(Protocol.json(Protocol.HELLO, hello()));
            startStream();
            startAudio();
            startStateTicker(l);
        }
    }

    /** Beacons like the phone does (the PC side's discovery test listens for these). */
    private void startBeacon(final int port) {
        Thread t = new Thread("beacon") {
            @Override
            public void run() {
                try {
                    java.net.DatagramSocket ds = new java.net.DatagramSocket();
                    ds.setBroadcast(true);
                    byte[] msg = ("{\"rnglens\":1,\"name\":\"FakePhone\",\"port\":" + port + "}").getBytes(Protocol.UTF8);
                    while (true) {
                        if (link == null) {
                            ds.send(new java.net.DatagramPacket(msg, msg.length,
                                    java.net.InetAddress.getByName("255.255.255.255"), Protocol.DISCOVERY_PORT));
                        }
                        Thread.sleep(1000);
                    }
                } catch (Exception e) {
                    log("beacon stopped: " + e);
                }
            }
        };
        t.setDaemon(true);
        t.start();
    }

    private String hello() {
        return "{\"proto\":1,\"device\":\"" + System.getenv().getOrDefault("FAKE_DEVICE", "rng-lens FakePhone") + "\",\"hevc\":true,\"encoder\":\"ffmpeg\",\"cameras\":["
                + "{\"id\":\"0\",\"facing\":\"back\",\"sensorOrientation\":90,\"focal\":4.3,"
                + "\"sizes\":[[1920,1080,60],[1280,720,60],[640,480,30]],\"fps\":[[15,30],[30,30],[60,60]],"
                + "\"zoom\":[0.6,8.0],\"flash\":true,\"afManual\":true,\"ev\":[-12,12,0.1667],\"stab\":true},"
                + "{\"id\":\"1\",\"facing\":\"front\",\"sensorOrientation\":270,\"focal\":2.6,"
                + "\"sizes\":[[1280,720,30],[640,480,30]],\"fps\":[[15,30],[30,30]],"
                + "\"zoom\":[1.0,4.0],\"flash\":false,\"afManual\":false,\"ev\":[-8,8,0.25],\"stab\":false}],"
                + "\"state\":" + state() + "}";
    }

    private String state() {
        return "{\"camera\":\"0\",\"width\":" + width + ",\"height\":" + height + ",\"fps\":" + fps + ",\"codec\":" + codec
                + ",\"bitrate\":0,\"zoom\":" + zoomPct / 100.0 + ",\"af\":\"auto\",\"focus\":0,\"ae\":\"auto\",\"ev\":0,"
                + "\"awb\":\"auto\",\"torch\":false,\"stab\":true,\"orient\":0,\"streamWidth\":" + width
                + ",\"streamHeight\":" + height + ",\"streamFps\":" + fps + ",\"streamBitrate\":4000000,"
                + "\"battery\":77,\"tempC\":" + (31.5 + fakeThermal * 4) + ",\"charging\":true,\"dropped\":" + (link == null ? 0 : link.droppedFrames())
                + ",\"thermal\":{\"level\":" + fakeThermal + ",\"status\":" + fakeThermal + ",\"throttled\":" + (fakeThermal >= 2)
                + ",\"limit\":\"" + (fakeThermal >= 2 ? "720p, 24 fps, 40% bitrate" : "full quality") + "\"}}";
    }

    private void startStateTicker(final Link l) {
        Thread t = new Thread("state") {
            @Override
            public void run() {
                while (!l.isClosed()) {
                    l.send(Protocol.json(Protocol.STATE, state()));
                    try {
                        Thread.sleep(1000);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
        };
        t.setDaemon(true);
        t.start();
    }

    // ---- Link.Listener

    @Override
    public void onControl(Link l, String json) {
        log("CONTROL " + json);
        boolean restart = false;
        Integer w = intField(json, "width"), h = intField(json, "height"), f = intField(json, "fps"), c = intField(json, "codec");
        if (w != null && w != width) { width = w; restart = true; }
        if (h != null && h != height) { height = h; restart = true; }
        if (f != null && f != fps) { fps = f; restart = true; }
        if (c != null && c != codec) { codec = c; restart = true; }
        Integer ft = intField(json, "fakeThermal");
        if (ft != null) fakeThermal = ft;
        Double z = doubleField(json, "zoom");
        if (z != null) zoomPct = (int) Math.round(z * 100);
        if (restart) startStream();
        l.send(Protocol.json(Protocol.STATE, state()));
    }

    @Override
    public void onNeedKeyframe(Link l) {
        log("network behind: dropping until key frame");
    }

    @Override
    public void onClosed(Link l) {
        if (l != link) return;
        link = null;
        stopStream();
        stopAudio();
        log("PC disconnected");
    }

    // ---- "camera + encoder"

    // ---- "microphone": a 440 Hz tone through ffmpeg, framed like MediaCodec output

    private void stopAudio() {
        audioGen++;
        Process p = audioProc;
        audioProc = null;
        if (p != null) p.destroyForcibly();
    }

    private void startAudio() {
        stopAudio();
        final int gen = audioGen;
        String[] cmd = aac
                ? new String[]{"ffmpeg", "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi", "-i",
                        "sine=frequency=440:sample_rate=48000", "-ac", "1", "-c:a", "aac", "-b:a", "128k", "-flush_packets", "1", "-f", "adts", "-"}
                : new String[]{"ffmpeg", "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi", "-i",
                        "sine=frequency=440:sample_rate=48000", "-ac", "1", "-c:a", "libopus", "-b:a", "64k",
                        "-frame_duration", "20", "-application", "lowdelay", "-page_duration", "20000", "-flush_packets", "1", "-f", "ogg", "-"};
        try {
            audioProc = new ProcessBuilder(cmd).redirectError(ProcessBuilder.Redirect.INHERIT).start();
        } catch (IOException e) {
            log("audio ffmpeg failed: " + e);
            return;
        }
        final InputStream in = new BufferedInputStream(audioProc.getInputStream(), 1 << 16);
        Thread t = new Thread("audio") {
            @Override
            public void run() {
                try {
                    if (aac) pumpAdts(gen, in);
                    else pumpOgg(gen, in);
                } catch (IOException ignored) {
                }
            }
        };
        t.setDaemon(true);
        t.start();
        log("microphone: 440 Hz " + (aac ? "AAC" : "Opus"));
    }

    private void pumpAdts(int gen, InputStream in) throws IOException {
        byte[] h = new byte[7];
        long frames = 0;
        boolean configSent = false;
        while (gen == audioGen && readFully(in, h, 7)) {
            boolean crc = (h[1] & 1) == 0;
            int profile = (h[2] >> 6) & 3, sr = (h[2] >> 2) & 15, ch = ((h[2] & 1) << 2) | ((h[3] >> 6) & 3);
            int len = ((h[3] & 3) << 11) | ((h[4] & 0xff) << 3) | ((h[5] & 0xff) >> 5);
            if (crc && !readFully(in, new byte[2], 2)) return;
            byte[] data = new byte[len - 7 - (crc ? 2 : 0)];
            if (!readFully(in, data, data.length)) return;
            Link l = link;
            if (l == null) continue;
            if (!configSent) {
                int asc = ((profile + 1) << 11) | (sr << 7) | (ch << 3);
                byte[] csd = {(byte) (asc >> 8), (byte) asc};
                l.send(Protocol.audioConfig(Protocol.ACODEC_AAC, 48000, ch, csd, 0, 2));
                configSent = true;
            }
            l.sendAudio(Protocol.audio(frames * 1_000_000L / 48000, data, 0, data.length));
            frames += 1024;
        }
    }

    /** Minimal Ogg demuxer: packet 0 is OpusHead (sent as config), packet 1 OpusTags (skipped). */
    private void pumpOgg(int gen, InputStream in) throws IOException {
        byte[] hdr = new byte[27];
        ByteArrayOutputStream packet = new ByteArrayOutputStream();
        int index = 0;
        long frames = 0;
        while (gen == audioGen && readFully(in, hdr, 27)) {
            if (hdr[0] != 'O' || hdr[1] != 'g' || hdr[2] != 'g' || hdr[3] != 'S') return;
            int segs = hdr[26] & 0xff;
            byte[] table = new byte[segs];
            if (!readFully(in, table, segs)) return;
            for (int i = 0; i < segs; i++) {
                int n = table[i] & 0xff;
                byte[] seg = new byte[n];
                if (!readFully(in, seg, n)) return;
                packet.write(seg, 0, n);
                if (n == 255) continue; // packet continues in the next segment
                byte[] p = packet.toByteArray();
                packet.reset();
                Link l = link;
                if (index == 0 && l != null) l.send(Protocol.audioConfig(Protocol.ACODEC_OPUS, 48000, 1, p, 0, p.length));
                else if (index >= 2 && l != null) {
                    l.sendAudio(Protocol.audio(frames * 1_000_000L / 48000, p, 0, p.length));
                    frames += 960;
                }
                index++;
            }
        }
    }

    private static boolean readFully(InputStream in, byte[] b, int len) throws IOException {
        int got = 0;
        while (got < len) {
            int n = in.read(b, got, len - got);
            if (n < 0) return false;
            got += n;
        }
        return true;
    }

    private void stopStream() {
        synchronized (restartLock) {
            generation++;
            Process p = ffmpeg;
            ffmpeg = null;
            if (p != null) p.destroyForcibly();
        }
    }

    private void startStream() {
        synchronized (restartLock) {
            stopStream();
            final int gen = generation;
            boolean hevc = codec == Protocol.CODEC_HEVC;
            List<String> cmd = new ArrayList<String>();
            if (image == null) {
                String[] base = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi", "-i",
                        "testsrc2=size=" + width + "x" + height + ":rate=" + fps, "-pix_fmt", "yuv420p"};
                for (String s : base) cmd.add(s);
            } else {
                boolean noise = image.endsWith("+noise");
                String path = noise ? image.substring(0, image.length() - 6) : image;
                String vf = "scale=" + width + ":" + height + ":force_original_aspect_ratio=increase,crop=" + width + ":" + height
                        + (noise ? ",noise=alls=18:allf=t" : "") + ",format=yuv420p";
                String[] base = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-re", "-loop", "1", "-framerate", "" + fps,
                        "-i", path, "-vf", vf};
                for (String s : base) cmd.add(s);
            }
            if (hevc) {
                String[] x = {"-c:v", "libx265", "-preset", "ultrafast", "-tune", "zerolatency",
                        "-x265-params", "aud=1:keyint=" + fps * 2 + ":log-level=error", "-b:v", "3M", "-f", "hevc", "-"};
                for (String s : x) cmd.add(s);
            } else {
                String[] x = {"-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-profile:v", "high",
                        "-x264-params", "aud=1:keyint=" + fps * 2, "-b:v", image != null && image.endsWith("+noise") ? "15M" : "3M",
                        "-f", "h264", "-"};
                for (String s : x) cmd.add(s);
            }
            try {
                ffmpeg = new ProcessBuilder(cmd).redirectError(ProcessBuilder.Redirect.INHERIT).start();
            } catch (IOException e) {
                log("ffmpeg failed: " + e);
                return;
            }
            final InputStream in = new BufferedInputStream(ffmpeg.getInputStream(), 1 << 20);
            final int w = width, h = height, k = codec;
            Thread t = new Thread("encoder") {
                @Override
                public void run() {
                    pump(gen, in, w, h, k);
                }
            };
            t.setDaemon(true);
            t.start();
            log("streaming " + w + "x" + h + "@" + fps + (hevc ? " HEVC" : " H.264"));
        }
    }

    /** Splits the AnnexB stream into access units at AUDs and feeds them like MediaCodec would. */
    private void pump(int gen, InputStream in, int w, int h, int k) {
        boolean hevc = k == Protocol.CODEC_HEVC;
        NalReader nals = new NalReader(in);
        ByteArrayOutputStream config = new ByteArrayOutputStream(), au = new ByteArrayOutputStream();
        boolean configSent = false, key = false;
        long pts = 0, frameUs = 1_000_000L / fps;
        try {
            byte[] nal;
            while ((nal = nals.next()) != null && gen == generation) {
                int type = hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
                boolean aud = hevc ? type == 35 : type == 9;
                boolean param = hevc ? type >= 32 && type <= 34 : type == 7 || type == 8;
                boolean sei = hevc ? type == 39 || type == 40 : type == 6;
                boolean idr = hevc ? type >= 16 && type <= 21 : type == 5;
                if (aud) {
                    if (au.size() > 0) {
                        if (!configSent && config.size() > 0) {
                            byte[] c = config.toByteArray();
                            sendTo(Protocol.config(k, w, h, c, 0, c.length));
                            configSent = true;
                        }
                        Link l = link;
                        if (l != null && configSent) {
                            byte[] a = au.toByteArray();
                            l.sendFrame(ByteBuffer.wrap(a), a.length, pts, key);
                        }
                        pts += frameUs;
                    }
                    au.reset();
                    key = false;
                    continue;
                }
                if (param) {
                    if (!configSent) writeNal(config, nal);
                    continue;
                }
                if (sei) continue;
                if (idr) key = true;
                writeNal(au, nal);
            }
        } catch (IOException ignored) {
        }
    }

    private void sendTo(byte[] message) {
        Link l = link;
        if (l != null) l.send(message);
    }

    private static void writeNal(ByteArrayOutputStream out, byte[] nal) {
        out.write(0);
        out.write(0);
        out.write(0);
        out.write(1);
        out.write(nal, 0, nal.length);
    }

    /** Minimal AnnexB NAL splitter. */
    private static final class NalReader {
        private final InputStream in;
        private boolean started;

        NalReader(InputStream in) {
            this.in = in;
        }

        byte[] next() throws IOException {
            ByteArrayOutputStream nal = new ByteArrayOutputStream();
            int zeros = 0;
            while (true) {
                int b = in.read();
                if (b < 0) return nal.size() > 0 ? nal.toByteArray() : null;
                if (b == 0) {
                    zeros++;
                    continue;
                }
                if (b == 1 && zeros >= 2) {
                    if (started) return nal.toByteArray();
                    started = true;
                    zeros = 0;
                    continue;
                }
                for (; zeros > 0; zeros--) nal.write(0);
                nal.write(b);
            }
        }
    }

    private static Integer intField(String json, String key) {
        Matcher m = Pattern.compile("\"" + key + "\"\\s*:\\s*(-?\\d+)").matcher(json);
        return m.find() ? Integer.valueOf(m.group(1)) : null;
    }

    private static Double doubleField(String json, String key) {
        Matcher m = Pattern.compile("\"" + key + "\"\\s*:\\s*(-?[\\d.]+)").matcher(json);
        return m.find() ? Double.valueOf(m.group(1)) : null;
    }

    private static void log(String s) {
        System.out.println("[fakephone] " + s);
        System.out.flush();
    }
}
