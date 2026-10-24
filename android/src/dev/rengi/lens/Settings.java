package dev.rengi.lens;

import org.json.JSONException;
import org.json.JSONObject;

/** Everything the PC can change. Applied from CONTROL JSON; reports what kind of change happened. */
final class Settings {
    static final int CHANGE_NONE = 0;
    static final int CHANGE_REQUEST = 1; // repeating capture request only
    static final int CHANGE_BITRATE = 2; // live encoder parameter
    static final int CHANGE_RESTART = 4; // camera/encoder rebuild
    static final int CHANGE_KEYFRAME = 8;

    String camera = null; // null = first back camera
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int codec = Protocol.CODEC_H264;
    int bitrate = 0; // 0 = auto from size and fps
    float zoom = 1f;
    String af = "auto"; // auto | lock | manual
    float focus = 0f; // manual focus 0 = infinity .. 1 = closest
    String ae = "auto"; // auto | lock
    int ev = 0; // exposure compensation index
    String awb = "auto"; // auto | lock
    boolean torch = false;
    boolean stab = true;

    int apply(JSONObject j) {
        int c = CHANGE_NONE;
        if (j.has("camera")) {
            String v = j.optString("camera", null);
            if (v != null && !v.equals(camera)) {
                camera = v;
                zoom = 1f;
                torch = false;
                c |= CHANGE_RESTART;
            }
        }
        int w = j.optInt("width", width), h = j.optInt("height", height), f = j.optInt("fps", fps);
        int k = j.optInt("codec", codec);
        if (w != width || h != height || f != fps || k != codec) {
            width = w;
            height = h;
            fps = f;
            codec = k;
            c |= CHANGE_RESTART;
        }
        if (j.has("bitrate") && j.optInt("bitrate", bitrate) != bitrate) {
            bitrate = j.optInt("bitrate", bitrate);
            c |= CHANGE_BITRATE;
        }
        float z = (float) j.optDouble("zoom", zoom);
        float fo = (float) j.optDouble("focus", focus);
        String a = j.optString("af", af), e = j.optString("ae", ae), wb = j.optString("awb", awb);
        int evv = j.optInt("ev", ev);
        boolean t = j.optBoolean("torch", torch), s = j.optBoolean("stab", stab);
        if (z != zoom || fo != focus || !a.equals(af) || !e.equals(ae) || !wb.equals(awb) || evv != ev
                || t != torch || s != stab) {
            zoom = z;
            focus = fo;
            af = a;
            ae = e;
            awb = wb;
            ev = evv;
            torch = t;
            stab = s;
            c |= CHANGE_REQUEST;
        }
        if (j.optBoolean("keyframe", false)) c |= CHANGE_KEYFRAME;
        return c;
    }

    JSONObject toJson() {
        JSONObject j = new JSONObject();
        try {
            j.put("camera", camera);
            j.put("width", width);
            j.put("height", height);
            j.put("fps", fps);
            j.put("codec", codec);
            j.put("bitrate", bitrate);
            j.put("zoom", (double) zoom);
            j.put("af", af);
            j.put("focus", (double) focus);
            j.put("ae", ae);
            j.put("ev", ev);
            j.put("awb", awb);
            j.put("torch", torch);
            j.put("stab", stab);
        } catch (JSONException ignored) {
        }
        return j;
    }
}
