package dev.rengi.lens;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.media.MediaRecorder;
import android.media.audiofx.AcousticEchoCanceler;
import android.media.audiofx.AutomaticGainControl;
import android.media.audiofx.NoiseSuppressor;
import android.os.Process;
import android.util.Log;

import java.nio.ByteBuffer;

/**
 * Phone microphone -> Opus (Android 10+) or AAC-LC -> Link. 48 kHz mono, 20 ms packets.
 * "voice" uses the communication source plus the platform's echo canceller and noise suppressor,
 * "raw" uses the unprocessed source for music and streams.
 */
final class AudioPipeline {
    interface Sink {
        void onAudioConfig(byte[] message);

        void onAudio(byte[] message);
    }

    static final int SAMPLE_RATE = 48000;
    private static final String TAG = "rnglens";
    private static final String MIME_OPUS = "audio/opus";
    private static final int FRAME_SAMPLES = SAMPLE_RATE / 50; // 20 ms
    private static final int SOURCE_UNPROCESSED = 9; // MediaRecorder.AudioSource.UNPROCESSED, API 24

    private final Sink sink;
    private volatile Thread thread;
    private volatile boolean running;

    AudioPipeline(Sink sink) {
        this.sink = sink;
    }

    static boolean hasOpus() {
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (!info.isEncoder()) continue;
            for (String t : info.getSupportedTypes()) if (t.equalsIgnoreCase(MIME_OPUS)) return true;
        }
        return false;
    }

    synchronized void start(final boolean raw) {
        stop();
        running = true;
        Thread t = new Thread("rnglens-audio") {
            @Override
            public void run() {
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
                try {
                    loop(raw);
                } catch (Throwable e) {
                    Log.e(TAG, "audio stopped", e);
                }
            }
        };
        thread = t;
        t.start();
    }

    synchronized void stop() {
        running = false;
        Thread t = thread;
        thread = null;
        if (t != null) {
            try {
                t.join(1000);
            } catch (InterruptedException ignored) {
            }
        }
    }

    private void loop(boolean raw) throws Exception {
        int minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
        int bufSize = Math.max(minBuf, FRAME_SAMPLES * 2 * 4);
        int[] sources = raw
                ? new int[]{SOURCE_UNPROCESSED, MediaRecorder.AudioSource.MIC}
                : new int[]{MediaRecorder.AudioSource.VOICE_COMMUNICATION, MediaRecorder.AudioSource.MIC};
        AudioRecord rec = null;
        for (int src : sources) {
            if (src == SOURCE_UNPROCESSED && android.os.Build.VERSION.SDK_INT < 24) continue;
            try {
                rec = new AudioRecord(src, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, bufSize);
                if (rec.getState() == AudioRecord.STATE_INITIALIZED) break;
                rec.release();
            } catch (Exception ignored) {
            }
            rec = null;
        }
        if (rec == null) {
            Log.e(TAG, "microphone unavailable");
            return;
        }
        Object[] effects = attachEffects(rec.getAudioSessionId(), !raw);

        boolean opus = hasOpus();
        int codec = opus ? Protocol.ACODEC_OPUS : Protocol.ACODEC_AAC;
        MediaFormat f = MediaFormat.createAudioFormat(opus ? MIME_OPUS : MediaFormat.MIMETYPE_AUDIO_AAC, SAMPLE_RATE, 1);
        f.setInteger(MediaFormat.KEY_BIT_RATE, raw ? 128_000 : 64_000);
        f.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, FRAME_SAMPLES * 2 * 2);
        if (!opus) f.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC);
        MediaCodec enc = MediaCodec.createEncoderByType(opus ? MIME_OPUS : MediaFormat.MIMETYPE_AUDIO_AAC);
        enc.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        enc.start();
        rec.startRecording();

        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        byte[] pcm = new byte[FRAME_SAMPLES * 2];
        byte[] out = new byte[8192];
        long samples = 0;
        boolean configSent = false;
        try {
            while (running) {
                int got = rec.read(pcm, 0, pcm.length);
                if (got <= 0) continue;
                int in = enc.dequeueInputBuffer(10_000);
                if (in >= 0) {
                    ByteBuffer b = enc.getInputBuffer(in);
                    b.clear();
                    int n = Math.min(got, b.remaining());
                    b.put(pcm, 0, n);
                    enc.queueInputBuffer(in, 0, n, samples * 1_000_000L / SAMPLE_RATE, 0);
                    samples += n / 2;
                }
                int idx;
                while ((idx = enc.dequeueOutputBuffer(info, 0)) >= 0) {
                    if (info.size > 0) {
                        if (out.length < info.size) out = new byte[info.size * 2];
                        ByteBuffer b = enc.getOutputBuffer(idx);
                        b.position(info.offset);
                        b.get(out, 0, info.size);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                            sink.onAudioConfig(Protocol.audioConfig(codec, SAMPLE_RATE, 1, out, 0, info.size));
                            configSent = true;
                        } else {
                            if (!configSent) { // encoder without a config buffer
                                sink.onAudioConfig(Protocol.audioConfig(codec, SAMPLE_RATE, 1, out, 0, 0));
                                configSent = true;
                            }
                            sink.onAudio(Protocol.audio(info.presentationTimeUs, out, 0, info.size));
                        }
                    }
                    enc.releaseOutputBuffer(idx, false);
                }
            }
        } finally {
            try {
                rec.stop();
            } catch (Exception ignored) {
            }
            rec.release();
            try {
                enc.stop();
            } catch (Exception ignored) {
            }
            enc.release();
            for (Object e : effects) {
                if (e instanceof android.media.audiofx.AudioEffect) ((android.media.audiofx.AudioEffect) e).release();
            }
        }
    }

    /** Voice mode: make sure echo cancel, noise suppression and AGC are on; raw mode: off. */
    private static Object[] attachEffects(int session, boolean voice) {
        Object[] fx = new Object[3];
        try {
            if (AcousticEchoCanceler.isAvailable()) {
                AcousticEchoCanceler a = AcousticEchoCanceler.create(session);
                if (a != null) a.setEnabled(voice);
                fx[0] = a;
            }
            if (NoiseSuppressor.isAvailable()) {
                NoiseSuppressor n = NoiseSuppressor.create(session);
                if (n != null) n.setEnabled(voice);
                fx[1] = n;
            }
            if (AutomaticGainControl.isAvailable()) {
                AutomaticGainControl g = AutomaticGainControl.create(session);
                if (g != null) g.setEnabled(voice);
                fx[2] = g;
            }
        } catch (Throwable ignored) {
        }
        return fx;
    }
}
