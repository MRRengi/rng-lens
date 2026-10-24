package dev.rengi.lens;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * One PC connection. Frames are queued from the encoder thread and written by a dedicated writer
 * thread, so a slow network never blocks the codec. If the PC falls behind, non-key frames are
 * dropped until the next key frame and a fresh key frame is requested. Pure Java.
 */
public final class Link {
    public interface Listener {
        void onControl(Link link, String json);

        void onNeedKeyframe(Link link);

        void onClosed(Link link);
    }

    /** About 250 ms at 30 fps: more than this queued means the network is behind. */
    private static final int MAX_QUEUED_FRAMES = 8;

    private static final class Packet {
        final byte[] bytes;
        final int length;
        final boolean frame;

        Packet(byte[] bytes, int length, boolean frame) {
            this.bytes = bytes;
            this.length = length;
            this.frame = frame;
        }
    }

    private static final Packet POISON = new Packet(new byte[0], 0, false);

    private final Socket socket;
    private final Listener listener;
    private final ArrayBlockingQueue<Packet> queue = new ArrayBlockingQueue<Packet>(128);
    private final ArrayBlockingQueue<byte[]> pool = new ArrayBlockingQueue<byte[]>(24);
    private final AtomicInteger queuedFrames = new AtomicInteger();
    private final AtomicBoolean closed = new AtomicBoolean();
    private volatile long bytesSent;
    private boolean dropping; // encoder thread only
    private int droppedFrames; // encoder thread only

    public Link(Socket socket, Listener listener) {
        this.socket = socket;
        this.listener = listener;
    }

    public String peer() {
        return socket.getInetAddress().getHostAddress();
    }

    public long bytesSent() {
        return bytesSent;
    }

    public int droppedFrames() {
        return droppedFrames;
    }

    public boolean isClosed() {
        return closed.get();
    }

    public void start() throws java.io.IOException {
        socket.setTcpNoDelay(true);
        socket.setSendBufferSize(1 << 20);
        socket.setKeepAlive(true);
        final OutputStream out = socket.getOutputStream();
        final InputStream in = socket.getInputStream();
        Thread writer = new Thread("rnglens-writer") {
            @Override
            public void run() {
                writeLoop(out);
            }
        };
        Thread reader = new Thread("rnglens-reader") {
            @Override
            public void run() {
                readLoop(in);
            }
        };
        writer.setPriority(Thread.MAX_PRIORITY);
        writer.start();
        reader.start();
    }

    /** Control-type messages (HELLO, CONFIG, STATE) are never dropped. */
    public void send(byte[] message) {
        if (closed.get()) return;
        if (!queue.offer(new Packet(message, message.length, false))) close();
    }

    /**
     * Audio is tiny next to video, so it is never thinned out with the video; it is only dropped when
     * the link is hopelessly behind (then old audio is worthless anyway).
     */
    public void sendAudio(byte[] message) {
        if (closed.get()) return;
        if (queue.size() > 48) return;
        queue.offer(new Packet(message, message.length, false));
    }

    /** Called from the encoder thread with the codec's own output buffer, positioned at the data. */
    public void sendFrame(ByteBuffer data, int size, long ptsUs, boolean key) {
        if (closed.get()) return;
        if (dropping && !key) {
            droppedFrames++;
            return;
        }
        if (queuedFrames.get() >= MAX_QUEUED_FRAMES) {
            droppedFrames++;
            if (!dropping || key) {
                // A dropped key frame would leave us waiting for the periodic one; ask again.
                dropping = true;
                listener.onNeedKeyframe(this);
            }
            return;
        }
        dropping = false;
        int total = Protocol.HEADER + Protocol.FRAME_PREFIX + size;
        byte[] b = obtain(total);
        Protocol.putHeader(b, 0, Protocol.FRAME, Protocol.FRAME_PREFIX + size);
        Protocol.putU64(b, Protocol.HEADER, ptsUs);
        b[Protocol.HEADER + 8] = (byte) (key ? 1 : 0);
        data.get(b, Protocol.HEADER + Protocol.FRAME_PREFIX, size);
        queuedFrames.incrementAndGet();
        if (!queue.offer(new Packet(b, total, true))) {
            queuedFrames.decrementAndGet();
            recycle(b);
            dropping = true;
            listener.onNeedKeyframe(this);
        }
    }

    public void close() {
        if (!closed.compareAndSet(false, true)) return;
        try {
            socket.close();
        } catch (Exception ignored) {
        }
        queue.clear();
        queue.offer(POISON);
        listener.onClosed(this);
    }

    private byte[] obtain(int size) {
        byte[] b = pool.poll();
        if (b != null && b.length >= size) return b;
        int cap = 64 * 1024;
        while (cap < size) cap <<= 1;
        return new byte[cap];
    }

    private void recycle(byte[] b) {
        pool.offer(b);
    }

    private void writeLoop(OutputStream out) {
        try {
            while (true) {
                Packet p = queue.take();
                if (p == POISON) break;
                out.write(p.bytes, 0, p.length);
                bytesSent += p.length;
                if (p.frame) {
                    queuedFrames.decrementAndGet();
                    recycle(p.bytes);
                }
            }
        } catch (Exception e) {
            close();
        }
    }

    private void readLoop(InputStream in) {
        try {
            while (!closed.get()) {
                Protocol.Message m = Protocol.read(in);
                if (m.type == Protocol.CONTROL) listener.onControl(this, m.text());
            }
        } catch (Exception e) {
            close();
        }
    }
}
