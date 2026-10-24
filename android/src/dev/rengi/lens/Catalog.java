package dev.rengi.lens;

import android.graphics.Rect;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.util.Range;
import android.util.Rational;
import android.util.Size;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.LinkedHashSet;
import java.util.List;

/** What the phone can do: every camera (including hidden ids), its sizes and limits, and the HW encoders. */
final class Catalog {
    static final String MIME_AVC = "video/avc";
    static final String MIME_HEVC = "video/hevc";

    /** API 30+ keys reached through reflection because we compile against API 23. */
    static final CaptureRequest.Key<Float> ZOOM_RATIO = field(CaptureRequest.class, "CONTROL_ZOOM_RATIO");
    static final CameraCharacteristics.Key<Range<Float>> ZOOM_RATIO_RANGE =
            field(CameraCharacteristics.class, "CONTROL_ZOOM_RATIO_RANGE");

    static final class Mode {
        final int width, height, maxFps;

        Mode(int width, int height, int maxFps) {
            this.width = width;
            this.height = height;
            this.maxFps = maxFps;
        }
    }

    static final class Cam {
        String id;
        int facing; // CameraCharacteristics.LENS_FACING_*
        int sensorOrientation;
        float focal;
        List<Mode> modes = new ArrayList<Mode>();
        List<Range<Integer>> fpsRanges = new ArrayList<Range<Integer>>();
        float zoomMin = 1f, zoomMax = 1f;
        boolean zoomRatio; // true = CONTROL_ZOOM_RATIO, false = crop region
        Rect activeArray;
        boolean flash;
        float minFocus; // diopters, 0 = fixed focus
        int evMin, evMax;
        float evStep = 1f;
        boolean videoStab, ois;
        CameraCharacteristics chars;
    }

    final List<Cam> cams = new ArrayList<Cam>();
    String avcEncoder, hevcEncoder;
    MediaCodecInfo.CodecCapabilities avcCaps, hevcCaps;

    static Catalog build(CameraManager cm) {
        Catalog c = new Catalog();
        c.findEncoders();
        LinkedHashSet<String> ids = new LinkedHashSet<String>();
        try {
            ids.addAll(Arrays.asList(cm.getCameraIdList()));
        } catch (CameraAccessException ignored) {
        }
        // Some vendors keep extra lenses (ultra-wide, tele) out of the public list but still open them.
        for (int i = 0; i < 16; i++) ids.add(String.valueOf(i));
        for (String id : ids) {
            try {
                Cam cam = c.describe(id, cm.getCameraCharacteristics(id));
                if (cam != null) c.cams.add(cam);
            } catch (Throwable ignored) {
            }
        }
        return c;
    }

    Cam find(String id) {
        for (Cam cam : cams) if (cam.id.equals(id)) return cam;
        for (Cam cam : cams) if (cam.facing == CameraCharacteristics.LENS_FACING_BACK) return cam;
        return cams.isEmpty() ? null : cams.get(0);
    }

    MediaCodecInfo.CodecCapabilities caps(int codec) {
        return codec == Protocol.CODEC_HEVC && hevcEncoder != null ? hevcCaps : avcCaps;
    }

    String encoder(int codec) {
        return codec == Protocol.CODEC_HEVC && hevcEncoder != null ? hevcEncoder : avcEncoder;
    }

    private Cam describe(String id, CameraCharacteristics ch) {
        StreamConfigurationMap map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if (map == null) return null;
        Size[] sizes = map.getOutputSizes(MediaCodec.class);
        if (sizes == null || sizes.length == 0) return null;
        Cam cam = new Cam();
        cam.id = id;
        cam.chars = ch;
        Integer facing = ch.get(CameraCharacteristics.LENS_FACING);
        cam.facing = facing == null ? CameraCharacteristics.LENS_FACING_EXTERNAL : facing;
        Integer so = ch.get(CameraCharacteristics.SENSOR_ORIENTATION);
        cam.sensorOrientation = so == null ? 0 : so;
        float[] focals = ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
        cam.focal = focals != null && focals.length > 0 ? focals[0] : 0f;
        MediaCodecInfo.VideoCapabilities vc = avcCaps != null ? avcCaps.getVideoCapabilities() : null;
        for (Size s : sizes) {
            int w = s.getWidth(), h = s.getHeight();
            if (w > 4096 || h > 4096 || w < 320) continue;
            if (vc != null && !vc.isSizeSupported(w, h)) continue;
            long minDur = map.getOutputMinFrameDuration(MediaCodec.class, s);
            int maxFps = minDur > 0 ? (int) Math.round(1e9 / minDur) : 30;
            cam.modes.add(new Mode(w, h, Math.min(maxFps, 120)));
        }
        if (cam.modes.isEmpty()) return null;
        java.util.Collections.sort(cam.modes, new Comparator<Mode>() {
            @Override
            public int compare(Mode a, Mode b) {
                return b.width * b.height - a.width * a.height;
            }
        });
        Range<Integer>[] ranges = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if (ranges != null) cam.fpsRanges.addAll(Arrays.asList(ranges));
        cam.activeArray = ch.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        Range<Float> zr = ZOOM_RATIO_RANGE != null ? ch.get(ZOOM_RATIO_RANGE) : null;
        if (zr != null && ZOOM_RATIO != null) {
            cam.zoomRatio = true;
            cam.zoomMin = zr.getLower();
            cam.zoomMax = zr.getUpper();
        } else {
            Float md = ch.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM);
            cam.zoomMax = md == null ? 1f : md;
        }
        Boolean fl = ch.get(CameraCharacteristics.FLASH_INFO_AVAILABLE);
        cam.flash = fl != null && fl;
        Float mf = ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE);
        cam.minFocus = mf == null ? 0f : mf;
        Range<Integer> evr = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE);
        if (evr != null) {
            cam.evMin = evr.getLower();
            cam.evMax = evr.getUpper();
        }
        Rational step = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP);
        if (step != null) cam.evStep = step.floatValue();
        cam.videoStab = contains(ch.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES),
                CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON);
        cam.ois = contains(ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION),
                CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON);
        return cam;
    }

    private void findEncoders() {
        MediaCodecInfo[] infos = new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos();
        for (MediaCodecInfo info : infos) {
            if (!info.isEncoder() || isSoftware(info.getName())) continue;
            for (String t : info.getSupportedTypes()) {
                if (avcEncoder == null && t.equalsIgnoreCase(MIME_AVC)) {
                    avcEncoder = info.getName();
                    avcCaps = info.getCapabilitiesForType(MIME_AVC);
                } else if (hevcEncoder == null && t.equalsIgnoreCase(MIME_HEVC)) {
                    hevcEncoder = info.getName();
                    hevcCaps = info.getCapabilitiesForType(MIME_HEVC);
                }
            }
        }
        if (avcEncoder == null) { // no hardware AVC encoder: accept anything rather than fail
            for (MediaCodecInfo info : infos) {
                if (!info.isEncoder()) continue;
                for (String t : info.getSupportedTypes()) {
                    if (t.equalsIgnoreCase(MIME_AVC)) {
                        avcEncoder = info.getName();
                        avcCaps = info.getCapabilitiesForType(MIME_AVC);
                        return;
                    }
                }
            }
        }
    }

    private static boolean isSoftware(String name) {
        String n = name.toLowerCase();
        return n.startsWith("omx.google.") || n.startsWith("c2.android.") || n.contains(".sw.");
    }

    private static boolean contains(int[] a, int v) {
        if (a == null) return false;
        for (int x : a) if (x == v) return true;
        return false;
    }

    @SuppressWarnings("unchecked")
    private static <T> T field(Class<?> cls, String name) {
        try {
            return (T) cls.getField(name).get(null);
        } catch (Throwable t) {
            return null;
        }
    }

    JSONObject toJson() throws JSONException {
        JSONArray arr = new JSONArray();
        for (Cam cam : cams) {
            JSONObject o = new JSONObject();
            o.put("id", cam.id);
            o.put("facing", cam.facing == CameraCharacteristics.LENS_FACING_FRONT ? "front"
                    : cam.facing == CameraCharacteristics.LENS_FACING_BACK ? "back" : "external");
            o.put("sensorOrientation", cam.sensorOrientation);
            o.put("focal", (double) cam.focal);
            JSONArray modes = new JSONArray();
            for (Mode m : cam.modes) modes.put(new JSONArray().put(m.width).put(m.height).put(m.maxFps));
            o.put("sizes", modes);
            JSONArray fr = new JSONArray();
            for (Range<Integer> r : cam.fpsRanges) fr.put(new JSONArray().put(r.getLower()).put(r.getUpper()));
            o.put("fps", fr);
            o.put("zoom", new JSONArray().put((double) cam.zoomMin).put((double) cam.zoomMax));
            o.put("flash", cam.flash);
            o.put("afManual", cam.minFocus > 0);
            o.put("ev", new JSONArray().put(cam.evMin).put(cam.evMax).put((double) cam.evStep));
            o.put("stab", cam.videoStab);
            arr.put(o);
        }
        JSONObject j = new JSONObject();
        j.put("cameras", arr);
        j.put("hevc", hevcEncoder != null);
        j.put("encoder", avcEncoder == null ? "" : avcEncoder);
        return j;
    }
}
