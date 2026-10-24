import dev.rengi.lens.ThermalPolicy;

/** Drives the phone's thermal policy through a heat-up / cool-down story and checks every step. */
public final class ThermalTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "  ok   " : "  FAIL ") + what);
        if (!ok) failures++;
    }

    public static void main(String[] args) {
        ThermalPolicy p = new ThermalPolicy();
        long t = 0;
        float nan = Float.NaN;

        p.update(t, 0, 0.3f, 33f);
        check(!p.limits().throttled() && p.limits().maxFps >= 60, "cool phone: full quality");

        t += 2000;
        p.update(t, 0, 0.96f, 38f);
        check(p.limits().level == 2 && p.limits().maxFps == 30, "headroom forecast 0.96: drop to 30 fps before Android throttles");

        t += 2000;
        p.update(t, 3, 0.99f, 44f);
        check(p.limits().level == 3 && p.limits().maxPixels == 1920 * 1080, "severe: capped at 1080p");

        t += 2000;
        p.update(t, 4, 1.1f, 47f);
        check(p.limits().level == 4 && p.limits().maxPixels == 1280 * 720 && p.limits().maxFps == 24, "critical: 720p @ 24");

        int[] wh = ThermalPolicy.clampSize(3840, 2160, p.limits().maxPixels);
        check(wh[0] == 1280 && wh[1] == 720, "4K request clamps to 1280x720 (got " + wh[0] + "x" + wh[1] + ")");

        t += 2000;
        boolean changed = p.update(t, 1, 0.5f, 40f);
        check(!changed && p.limits().level == 4, "cooled a bit: no instant jump back (hysteresis)");

        t += ThermalPolicy.COOL_DOWN_MS;
        p.update(t, 1, 0.5f, 40f);
        check(p.limits().level == 3, "after a minute cool: one level back up in quality");

        t += 2000;
        p.update(t, 3, 0.9f, 44f);
        check(p.limits().level == 3, "heat again at the same level: stays");

        for (int i = 0; i < 4; i++) {
            t += ThermalPolicy.COOL_DOWN_MS;
            p.update(t, 0, 0.2f, 34f);
        }
        check(!p.limits().throttled(), "minutes of cool: back to full quality");

        ThermalPolicy old = new ThermalPolicy(); // phone without the thermal API: battery temperature only
        old.update(0, -1, nan, 47f);
        check(old.limits().level == 3, "old phone, battery 47 C: severe limits");
        old.update(2000, -1, nan, 50f);
        check(old.limits().level == 4, "old phone, battery 50 C: critical limits");

        System.out.println(failures == 0 ? "thermal: all passed" : "thermal: " + failures + " failed");
        System.exit(failures == 0 ? 0 : 1);
    }
}
