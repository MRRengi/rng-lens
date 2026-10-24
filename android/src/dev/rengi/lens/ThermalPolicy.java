package dev.rengi.lens;

/**
 * Turns the phone's heat readings into stream limits, before Android starts throttling on its own.
 * Pure Java (unit-tested on the PC by tools/thermal-test).
 *
 * Inputs every ~2 s: Android's thermal status (0 none .. 6 shutdown, -1 if the phone can't tell),
 * the thermal headroom forecast (1.0 = throttling imminent, NaN if unknown) and the battery temperature.
 * Heating up reacts at once; cooling down waits {@link #COOL_DOWN_MS} per level so quality doesn't flap.
 */
public final class ThermalPolicy {
    public static final long COOL_DOWN_MS = 60_000;

    /** Limits for one level. maxPixels 0 = no limit. */
    public static final class Limits {
        public final int level, maxPixels, maxFps;
        public final float bitrateScale;

        Limits(int level, int maxPixels, int maxFps, float bitrateScale) {
            this.level = level;
            this.maxPixels = maxPixels;
            this.maxFps = maxFps;
            this.bitrateScale = bitrateScale;
        }

        public boolean throttled() {
            return level >= 2;
        }

        public String describe() {
            if (!throttled()) return "full quality";
            String res = maxPixels == 0 ? "" : maxPixels <= 1280 * 720 ? "720p, " : "1080p, ";
            return res + maxFps + " fps, " + Math.round(bitrateScale * 100) + "% bitrate";
        }
    }

    private static final Limits[] LEVELS = {
        new Limits(0, 0, 120, 1f),
        new Limits(1, 0, 120, 1f),           // light: watch only
        new Limits(2, 0, 30, 0.75f),         // moderate: no 60 fps, a bit less bitrate
        new Limits(3, 1920 * 1080, 30, 0.5f),
        new Limits(4, 1280 * 720, 24, 0.4f), // critical and above
    };

    private int level;
    private long belowSince = -1;
    private int lastStatus = -1;
    private float lastHeadroom = Float.NaN, lastBatteryC = Float.NaN;

    /** Feeds one reading; returns true if the limits changed. */
    public boolean update(long nowMs, int status, float headroom, float batteryC) {
        lastStatus = status;
        lastHeadroom = headroom;
        lastBatteryC = batteryC;
        int target = rawLevel(status, headroom, batteryC);
        if (target > level) {
            level = target;
            belowSince = -1;
            return true;
        }
        if (target < level) {
            if (belowSince < 0) belowSince = nowMs;
            if (nowMs - belowSince >= COOL_DOWN_MS) {
                level--;
                belowSince = level > target ? nowMs : -1; // keep stepping down one level per minute
                return true;
            }
        } else {
            belowSince = -1;
        }
        return false;
    }

    static int rawLevel(int status, float headroom, float batteryC) {
        int l = 0;
        if (status >= 0) l = Math.min(status, 4);
        if (!Float.isNaN(headroom)) { // forecast: act a little before Android does
            if (headroom >= 0.95f) l = Math.max(l, 2);
            else if (headroom >= 0.85f) l = Math.max(l, 1);
        }
        if (status < 0 && !Float.isNaN(batteryC)) { // old phones: only the battery temperature
            if (batteryC >= 49) l = Math.max(l, 4);
            else if (batteryC >= 46) l = Math.max(l, 3);
            else if (batteryC >= 43) l = Math.max(l, 2);
            else if (batteryC >= 40) l = Math.max(l, 1);
        }
        return l;
    }

    public Limits limits() {
        return LEVELS[Math.min(level, LEVELS.length - 1)];
    }

    public int status() {
        return lastStatus;
    }

    public float headroom() {
        return lastHeadroom;
    }

    public float batteryC() {
        return lastBatteryC;
    }

    /** Clamps a requested size to the level's pixel budget, keeping the aspect ratio. Returns {w, h}. */
    public static int[] clampSize(int w, int h, int maxPixels) {
        if (maxPixels <= 0 || (long) w * h <= maxPixels) return new int[]{w, h};
        double s = Math.sqrt((double) maxPixels / ((long) w * h));
        return new int[]{(int) (w * s) & ~15, (int) (h * s) & ~15};
    }
}
