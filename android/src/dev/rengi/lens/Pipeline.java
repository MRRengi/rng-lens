package dev.rengi.lens;

import android.graphics.Rect;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.util.Range;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.Collections;

/**
 * Camera2 -> encoder input Surface -> MediaCodec (hardware) -> Link.
 * The app never touches a pixel: the camera HAL writes straight into the encoder's buffers.
 * Everything here runs on one HandlerThread owned by Engine.
 */
final class Pipeline {
    interface Sink {
        void onConfig(int codec, int width, int height, ByteBuffer csd, int size);

        void onFrame(ByteBuffer data, int size, long ptsUs, boolean key);

        void onPipelineError(String what);
    }

    private static final String TAG = "rnglens";

    private final CameraManager cameraManager;
    private final Handler handler;
    private final Sink sink;

    private Catalog.Cam cam;
    private Settings settings;
    private MediaCodec encoder;
    private Surface surface;
    private CameraDevice device;
    private CameraCaptureSession session;
    private int generation;
    private int codec, width, height, fps, bitrate;
    private Range<Integer> fpsRange;
    private boolean configSent;
    private float bitrateScale = 1f; // thermal guard

    Pipeline(CameraManager cameraManager, Handler handler, Sink sink) {
        this.cameraManager = cameraManager;
        this.handler = handler;
        this.sink = sink;
    }

    int width() {
        return width;
    }

    int height() {
        return height;
    }

    int fps() {
        return fps;
    }

    int bitrate() {
        return bitrate;
    }

    Catalog.Cam camera() {
        return cam;
    }

    boolean running() {
        return encoder != null;
    }

    void start(Catalog catalog, Catalog.Cam cam, Settings s, float bitrateScale) {
        stop();
        this.bitrateScale = bitrateScale;
        final int gen = ++generation;
        this.cam = cam;
        this.settings = s;
        configSent = false;
        codec = catalog.encoder(s.codec) == null ? Protocol.CODEC_H264 : s.codec;
        if (codec == Protocol.CODEC_HEVC && catalog.hevcEncoder == null) codec = Protocol.CODEC_H264;
        Catalog.Mode mode = pickMode(cam, s.width, s.height);
        width = mode.width;
        height = mode.height;
        fps = Math.max(1, Math.min(s.fps, mode.maxFps));
        MediaCodecInfo.CodecCapabilities caps = catalog.caps(codec);
        if (caps != null) {
            MediaCodecInfo.VideoCapabilities vc = caps.getVideoCapabilities();
            while (fps > 15 && !vc.areSizeAndRateSupported(width, height, fps)) fps = fps > 30 ? 30 : fps - 5;
        }
        fpsRange = pickRange(cam, fps);
        bitrate = (int) ((s.bitrate > 0 ? s.bitrate : autoBitrate(width, height, fps, codec)) * bitrateScale);
        if (caps != null) bitrate = caps.getVideoCapabilities().getBitrateRange().clamp(bitrate);

        // Some encoders reject the low-latency keys or the size/rate; step down instead of failing.
        boolean ok = startEncoder(gen, catalog, true) || startEncoder(gen, catalog, false);
        while (!ok && (long) width * height > 1280L * 720) {
            Catalog.Mode smaller = pickMode(cam, width > 1920 || height > 1920 ? 1920 : 1280,
                    width > 1920 || height > 1920 ? 1080 : 720);
            if (smaller.width * smaller.height >= width * height) break;
            width = smaller.width;
            height = smaller.height;
            fps = Math.min(fps, Math.min(30, smaller.maxFps));
            fpsRange = pickRange(cam, fps);
            bitrate = (int) (autoBitrate(width, height, fps, codec) * bitrateScale);
            ok = startEncoder(gen, catalog, false);
        }
        if (!ok) {
            sink.onPipelineError("the phone's video encoder refused every setting");
            return;
        }

        try {
            cameraManager.openCamera(cam.id, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice d) {
                    if (gen != generation) {
                        d.close();
                        return;
                    }
                    device = d;
                    createSession(gen);
                }

                @Override
                public void onDisconnected(CameraDevice d) {
                    d.close();
                    if (gen == generation) {
                        device = null;
                        sink.onPipelineError("camera disconnected");
                    }
                }

                @Override
                public void onError(CameraDevice d, int error) {
                    d.close();
                    if (gen == generation) {
                        device = null;
                        sink.onPipelineError("camera error " + error);
                    }
                }
            }, handler);
        } catch (SecurityException | CameraAccessException | IllegalArgumentException e) {
            Log.e(TAG, "openCamera failed", e);
            sink.onPipelineError("open camera: " + e.getMessage());
        }
    }

    private boolean startEncoder(final int gen, Catalog catalog, boolean tuned) {
        String mime = codec == Protocol.CODEC_HEVC ? Catalog.MIME_HEVC : Catalog.MIME_AVC;
        MediaFormat f = MediaFormat.createVideoFormat(mime, width, height);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        f.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
        f.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
        if (tuned) {
            f.setInteger(MediaFormat.KEY_PRIORITY, 0); // realtime
            f.setInteger(MediaFormat.KEY_OPERATING_RATE, fps);
            f.setInteger("latency", 1); // KEY_LATENCY, API 30+, ignored before
            f.setInteger("max-bframes", 0); // KEY_MAX_B_FRAMES, API 29+
        }
        try {
            encoder = MediaCodec.createByCodecName(catalog.encoder(codec));
            encoder.setCallback(new MediaCodec.Callback() {
                @Override
                public void onInputBufferAvailable(MediaCodec c, int index) {
                }

                @Override
                public void onOutputBufferAvailable(MediaCodec c, int index, MediaCodec.BufferInfo info) {
                    if (gen != generation) return;
                    try {
                        if (info.size > 0) {
                            ByteBuffer b = c.getOutputBuffer(index);
                            b.position(info.offset);
                            b.limit(info.offset + info.size);
                            if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                                configSent = true;
                                sink.onConfig(codec, width, height, b, info.size);
                            } else {
                                boolean key = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                                sink.onFrame(b, info.size, info.presentationTimeUs, key);
                            }
                        }
                        c.releaseOutputBuffer(index, false);
                    } catch (IllegalStateException ignored) {
                    }
                }

                @Override
                public void onError(MediaCodec c, MediaCodec.CodecException e) {
                    if (gen != generation) return;
                    Log.e(TAG, "encoder error", e);
                    sink.onPipelineError("encoder: " + e.getMessage());
                }

                @Override
                public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
                    if (gen != generation || configSent) return;
                    // Rare encoders only report codec config through the format.
                    ByteBuffer a = format.getByteBuffer("csd-0"), b = format.getByteBuffer("csd-1");
                    if (a == null) return;
                    ByteBuffer all = ByteBuffer.allocate(a.remaining() + (b == null ? 0 : b.remaining()));
                    all.put(a.duplicate());
                    if (b != null) all.put(b.duplicate());
                    all.flip();
                    configSent = true;
                    sink.onConfig(codec, width, height, all, all.remaining());
                }
            }, handler);
            encoder.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            surface = encoder.createInputSurface();
            encoder.start();
            return true;
        } catch (Exception e) {
            Log.w(TAG, "encoder " + width + "x" + height + "@" + fps + (tuned ? " tuned" : " basic") + " failed: " + e);
            releaseEncoder();
            return false;
        }
    }

    private void releaseEncoder() {
        if (encoder != null) {
            try {
                encoder.stop();
            } catch (Exception ignored) {
            }
            try {
                encoder.release();
            } catch (Exception ignored) {
            }
            encoder = null;
        }
        if (surface != null) {
            surface.release();
            surface = null;
        }
    }

    private void createSession(final int gen) {
        try {
            device.createCaptureSession(Collections.singletonList(surface), new CameraCaptureSession.StateCallback() {
                @Override
                public void onConfigured(CameraCaptureSession s) {
                    if (gen != generation) {
                        s.close();
                        return;
                    }
                    session = s;
                    updateRequest(true);
                }

                @Override
                public void onConfigureFailed(CameraCaptureSession s) {
                    if (gen == generation) sink.onPipelineError("capture session failed");
                }
            }, handler);
        } catch (CameraAccessException | IllegalStateException e) {
            sink.onPipelineError("session: " + e.getMessage());
        }
    }

    /** Re-applies zoom/focus/exposure/torch to the running session. */
    void updateRequest(boolean triggerFocus) {
        if (session == null || device == null) return;
        Settings s = settings;
        try {
            CaptureRequest.Builder b = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
            b.addTarget(surface);
            b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
            if (fpsRange != null) b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange);
            applyZoom(b, s.zoom);

            boolean lockFocus = "lock".equals(s.af);
            if ("manual".equals(s.af) && cam.minFocus > 0) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF);
                b.set(CaptureRequest.LENS_FOCUS_DISTANCE, clamp(s.focus, 0f, 1f) * cam.minFocus);
            } else if (lockFocus) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO);
            } else {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
            }

            b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
            b.set(CaptureRequest.CONTROL_AE_LOCK, "lock".equals(s.ae));
            b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, Math.max(cam.evMin, Math.min(cam.evMax, s.ev)));
            b.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO);
            b.set(CaptureRequest.CONTROL_AWB_LOCK, "lock".equals(s.awb));
            b.set(CaptureRequest.FLASH_MODE, s.torch && cam.flash ? CaptureRequest.FLASH_MODE_TORCH
                    : CaptureRequest.FLASH_MODE_OFF);
            if (cam.videoStab) {
                b.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, s.stab
                        ? CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON
                        : CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF);
            }
            if (cam.ois) {
                b.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON);
            }

            if (lockFocus && triggerFocus) {
                b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START);
                session.capture(b.build(), null, handler);
                b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE);
            }
            session.setRepeatingRequest(b.build(), null, handler);
        } catch (CameraAccessException | IllegalStateException | IllegalArgumentException e) {
            Log.e(TAG, "request failed", e);
        }
    }

    void setBitrate(int bps, float scale) {
        if (encoder == null) return;
        bitrateScale = scale;
        bitrate = (int) ((bps > 0 ? bps : autoBitrate(width, height, fps, codec)) * scale);
        Bundle p = new Bundle();
        p.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrate);
        try {
            encoder.setParameters(p);
        } catch (IllegalStateException ignored) {
        }
    }

    void requestKeyframe() {
        if (encoder == null) return;
        Bundle p = new Bundle();
        p.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
        try {
            encoder.setParameters(p);
        } catch (IllegalStateException ignored) {
        }
    }

    void stop() {
        generation++;
        if (session != null) {
            try {
                session.close();
            } catch (Exception ignored) {
            }
            session = null;
        }
        if (device != null) {
            try {
                device.close();
            } catch (Exception ignored) {
            }
            device = null;
        }
        releaseEncoder();
    }

    private void applyZoom(CaptureRequest.Builder b, float zoom) {
        float z = clamp(zoom, cam.zoomMin, cam.zoomMax);
        if (cam.zoomRatio) {
            b.set(Catalog.ZOOM_RATIO, z);
        } else if (cam.activeArray != null && z > 1.001f) {
            Rect a = cam.activeArray;
            int cw = Math.round(a.width() / z), chh = Math.round(a.height() / z);
            int x = a.left + (a.width() - cw) / 2, y = a.top + (a.height() - chh) / 2;
            b.set(CaptureRequest.SCALER_CROP_REGION, new Rect(x, y, x + cw, y + chh));
        }
    }

    static Catalog.Mode pickMode(Catalog.Cam cam, int w, int h) {
        Catalog.Mode best = null;
        long want = (long) w * h;
        double aspect = (double) w / h;
        for (Catalog.Mode m : cam.modes) { // modes are sorted large -> small
            if (m.width == w && m.height == h) return m;
        }
        // Same aspect, biggest that is not larger than requested.
        for (Catalog.Mode m : cam.modes) {
            if ((long) m.width * m.height <= want && Math.abs((double) m.width / m.height - aspect) < 0.02) return m;
        }
        for (Catalog.Mode m : cam.modes) {
            if ((long) m.width * m.height <= want) {
                best = m;
                break;
            }
        }
        return best != null ? best : cam.modes.get(cam.modes.size() - 1);
    }

    static Range<Integer> pickRange(Catalog.Cam cam, int fps) {
        Range<Integer> best = null;
        for (Range<Integer> r : cam.fpsRanges) {
            if (r.getUpper() != fps) continue;
            if (best == null || r.getLower() > best.getLower()) best = r; // fixed [fps,fps] wins
        }
        if (best != null) return best;
        for (Range<Integer> r : cam.fpsRanges) {
            if (r.contains(fps) && (best == null || r.getUpper() < best.getUpper())) best = r;
        }
        return best;
    }

    static int autoBitrate(int w, int h, int fps, int codec) {
        double bits = (double) w * h * Math.min(fps, 60) * 0.12;
        if (codec == Protocol.CODEC_HEVC) bits *= 0.65;
        return (int) Math.max(1_500_000, Math.min(60_000_000, bits));
    }

    private static float clamp(float v, float lo, float hi) {
        return Math.max(lo, Math.min(hi, v));
    }
}
