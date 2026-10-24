package dev.rengi.lens;

import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.Charset;

/** Wire format shared by the phone and the PC. Pure Java, no Android imports. See docs/SPEC.md. */
public final class Protocol {
    public static final int VERSION = 1;
    public static final int PORT = 38440;
    public static final int DISCOVERY_PORT = 38441;

    public static final int HELLO = 0x01;
    public static final int CONFIG = 0x02;
    public static final int FRAME = 0x03;
    public static final int STATE = 0x04;
    public static final int AUDIO_CONFIG = 0x05;
    public static final int AUDIO = 0x06;
    public static final int CONTROL = 0x10;
    public static final int PING = 0x11;

    public static final int CODEC_H264 = 0;
    public static final int CODEC_HEVC = 1;

    public static final int ACODEC_OPUS = 0;
    public static final int ACODEC_AAC = 1;

    public static final int HEADER = 5;
    public static final int FRAME_PREFIX = 9;
    public static final int MAX_MESSAGE = 64 * 1024 * 1024;

    public static final Charset UTF8 = Charset.forName("UTF-8");

    private Protocol() {}

    public static void putU32(byte[] b, int at, int v) {
        b[at] = (byte) v;
        b[at + 1] = (byte) (v >>> 8);
        b[at + 2] = (byte) (v >>> 16);
        b[at + 3] = (byte) (v >>> 24);
    }

    public static void putU16(byte[] b, int at, int v) {
        b[at] = (byte) v;
        b[at + 1] = (byte) (v >>> 8);
    }

    public static void putU64(byte[] b, int at, long v) {
        for (int i = 0; i < 8; i++) b[at + i] = (byte) (v >>> (8 * i));
    }

    /** Writes the 5-byte message header at {@code at}. */
    public static void putHeader(byte[] b, int at, int type, int payloadLength) {
        b[at] = (byte) type;
        putU32(b, at + 1, payloadLength);
    }

    /** A whole message (header + payload) in one array. */
    public static byte[] message(int type, byte[] payload) {
        byte[] m = new byte[HEADER + payload.length];
        putHeader(m, 0, type, payload.length);
        System.arraycopy(payload, 0, m, HEADER, payload.length);
        return m;
    }

    public static byte[] json(int type, String json) {
        return message(type, json.getBytes(UTF8));
    }

    public static byte[] config(int codec, int width, int height, byte[] csd, int off, int len) {
        byte[] p = new byte[5 + len];
        p[0] = (byte) codec;
        putU16(p, 1, width);
        putU16(p, 3, height);
        System.arraycopy(csd, off, p, 5, len);
        return message(CONFIG, p);
    }

    /** AUDIO_CONFIG: u8 codec, u32 sampleRate, u8 channels, codec-specific config. */
    public static byte[] audioConfig(int codec, int sampleRate, int channels, byte[] csd, int off, int len) {
        byte[] p = new byte[6 + len];
        p[0] = (byte) codec;
        putU32(p, 1, sampleRate);
        p[5] = (byte) channels;
        System.arraycopy(csd, off, p, 6, len);
        return message(AUDIO_CONFIG, p);
    }

    /** AUDIO: u64 ptsUs, one encoded packet. */
    public static byte[] audio(long ptsUs, byte[] data, int off, int len) {
        byte[] m = new byte[HEADER + 8 + len];
        putHeader(m, 0, AUDIO, 8 + len);
        putU64(m, HEADER, ptsUs);
        System.arraycopy(data, off, m, HEADER + 8, len);
        return m;
    }

    public static final class Message {
        public final int type;
        public final byte[] payload;

        Message(int type, byte[] payload) {
            this.type = type;
            this.payload = payload;
        }

        public String text() {
            return new String(payload, UTF8);
        }
    }

    public static Message read(InputStream in) throws IOException {
        byte[] h = new byte[HEADER];
        readFully(in, h, HEADER);
        int type = h[0] & 0xff;
        int len = (h[1] & 0xff) | (h[2] & 0xff) << 8 | (h[3] & 0xff) << 16 | (h[4] & 0xff) << 24;
        if (len < 0 || len > MAX_MESSAGE) throw new IOException("bad message length " + len);
        byte[] p = new byte[len];
        readFully(in, p, len);
        return new Message(type, p);
    }

    private static void readFully(InputStream in, byte[] b, int len) throws IOException {
        int got = 0;
        while (got < len) {
            int n = in.read(b, got, len - got);
            if (n < 0) throw new EOFException();
            got += n;
        }
    }
}
