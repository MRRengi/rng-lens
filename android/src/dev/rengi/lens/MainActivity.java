package dev.rengi.lens;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * The phone side UI is deliberately tiny: status + blackout. Camera controls live in the PC app.
 * While streaming the screen goes pure black at minimum brightness after a few seconds.
 * Look: iOS-style grouped cards, light or dark with the system, the rng orange as accent, system font
 * (no bundled fonts, the APK stays tiny).
 */
public final class MainActivity extends Activity implements Engine.Status {
    private static final int REQ_CAMERA = 1;
    private static final long AUTO_BLACKOUT_MS = 12_000;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private boolean started;
    private TextView statusView, headlineView;
    private View statusDot;
    private View blackout;
    private boolean dark;
    // Palette (set in buildUi from the system light / dark mode)
    private int cBg, cCard, cLabel, cSecondary, cSeparator, cAccent, cGood, cWarn, cBad, cIdle;
    private boolean streaming;

    private final Runnable autoBlackout = new Runnable() {
        @Override
        public void run() {
            if (streaming) setBlackout(true);
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(buildUi());
        boolean cam = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED;
        boolean mic = checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
        if (cam && mic) {
            startEngine();
        } else {
            requestPermissions(new String[]{Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO}, REQ_CAMERA);
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        // The microphone is optional: without it rng-lens streams video only.
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            startEngine();
        } else {
            showStatus("Camera access needed\nGrant it in Settings > Apps > Lens.", false);
        }
    }

    /** The engine lives in StreamService so streaming survives a locked screen. */
    private void startEngine() {
        started = true;
        StreamService.ui = this;
        StreamService.activityOpen = true;
        StreamService.start(this);
        StreamService.replayStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (started) startEngine(); // after Stop in the notification, coming back restarts it
    }

    @Override
    protected void onDestroy() {
        if (StreamService.ui == this) StreamService.ui = null;
        StreamService.screenClosed();
        super.onDestroy();
    }

    @Override
    public void onStatus(final String line, final boolean nowStreaming) {
        ui.post(new Runnable() {
            @Override
            public void run() {
                showStatus(line, nowStreaming);
                if (nowStreaming && !streaming) ui.postDelayed(autoBlackout, AUTO_BLACKOUT_MS);
                if (!nowStreaming) {
                    ui.removeCallbacks(autoBlackout);
                    setBlackout(false);
                }
                streaming = nowStreaming;
            }
        });
    }

    private void setBlackout(boolean on) {
        blackout.setVisibility(on ? View.VISIBLE : View.GONE);
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.screenBrightness = on ? 0.01f : WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
        getWindow().setAttributes(lp);
        int immersive = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
        getWindow().getDecorView().setSystemUiVisibility(on ? immersive : baseUiFlags());
    }

    private int baseUiFlags() {
        return dark ? 0 : View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
    }

    /** First line = headline next to the dot, the rest = details under it. */
    private void showStatus(String line, boolean nowStreaming) {
        int nl = line.indexOf('\n');
        String head = nl < 0 ? line : line.substring(0, nl);
        String rest = nl < 0 ? "" : line.substring(nl + 1);
        headlineView.setText(head);
        statusView.setText(rest);
        statusView.setVisibility(rest.isEmpty() ? View.GONE : View.VISIBLE);
        String l = head.toLowerCase(java.util.Locale.US);
        int c = nowStreaming ? cGood : l.startsWith("waiting") ? cWarn
                : (l.contains("problem") || l.contains("no usable") || l.contains("needed")) ? cBad : cIdle;
        if (l.contains("problem")) c = cWarn;
        ((GradientDrawable) statusDot.getBackground()).setColor(c);
    }

    private GradientDrawable rounded(int color, float radiusDp) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(radiusDp));
        return g;
    }

    private TextView label(String text, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setTypeface(Typeface.create(bold ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        t.setIncludeFontPadding(false);
        return t;
    }

    private LinearLayout card() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackground(rounded(cCard, 14));
        return c;
    }

    private View separator() {
        View v = new View(this);
        v.setBackgroundColor(cSeparator);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, dp(0.5f)));
        lp.leftMargin = dp(16);
        v.setLayoutParams(lp);
        return v;
    }

    private TextView sectionHeader(String text) {
        TextView t = label(text.toUpperCase(java.util.Locale.US), 12.5f, cSecondary, false);
        t.setLetterSpacing(0.04f);
        t.setPadding(dp(16), dp(26), dp(16), dp(8));
        return t;
    }

    private TextView cardRow(String text) {
        TextView t = label(text, 15, cLabel, false);
        t.setLineSpacing(dp(3), 1f);
        t.setPadding(dp(16), dp(13), dp(16), dp(13));
        return t;
    }

    private View buildUi() {
        dark = (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
        if (dark) {
            cBg = Color.BLACK; cCard = Color.rgb(28, 28, 30); cLabel = Color.WHITE;
            cSecondary = Color.argb(153, 235, 235, 245); cSeparator = Color.argb(70, 84, 84, 88);
            cAccent = Color.rgb(255, 106, 61); cGood = Color.rgb(50, 215, 75); cWarn = Color.rgb(255, 159, 10);
            cBad = Color.rgb(255, 69, 58); cIdle = Color.rgb(99, 99, 102);
        } else {
            cBg = Color.rgb(242, 242, 247); cCard = Color.WHITE; cLabel = Color.BLACK;
            cSecondary = Color.argb(153, 60, 60, 67); cSeparator = Color.argb(74, 60, 60, 67);
            cAccent = Color.rgb(242, 88, 44); cGood = Color.rgb(52, 199, 89); cWarn = Color.rgb(255, 149, 0);
            cBad = Color.rgb(255, 59, 48); cIdle = Color.rgb(174, 174, 178);
        }
        Window w = getWindow();
        w.setStatusBarColor(cBg);
        w.setNavigationBarColor(cBg);
        w.getDecorView().setSystemUiVisibility(baseUiFlags());

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(cBg);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(16), dp(28), dp(16), dp(28));
        scroll.addView(col);

        // Large title
        TextView title = label("Lens", 34, cLabel, true);
        title.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        title.setPadding(dp(4), dp(8), 0, 0);
        col.addView(title);
        TextView sub = label("Your phone is the webcam.", 15, cSecondary, false);
        sub.setPadding(dp(4), dp(6), 0, 0);
        col.addView(sub);

        // Status card
        col.addView(sectionHeader("Status"));
        LinearLayout status = card();
        status.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout headRow = new LinearLayout(this);
        headRow.setOrientation(LinearLayout.HORIZONTAL);
        headRow.setGravity(Gravity.CENTER_VERTICAL);
        statusDot = new View(this);
        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(cIdle);
        statusDot.setBackground(dot);
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(dp(10), dp(10));
        dlp.rightMargin = dp(10);
        headRow.addView(statusDot, dlp);
        headlineView = label("Starting...", 17, cLabel, true);
        headRow.addView(headlineView);
        status.addView(headRow);
        statusView = label("", 14, cSecondary, false);
        statusView.setLineSpacing(dp(3), 1f);
        statusView.setPadding(dp(20), dp(8), 0, 0);
        statusView.setVisibility(View.GONE);
        status.addView(statusView);
        status.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (started) startEngine(); // after "Stop" in the notification: tap to start again
            }
        });
        col.addView(status);

        // Primary action
        TextView dark = label("Black out the screen", 16, Color.WHITE, true);
        dark.setGravity(Gravity.CENTER);
        dark.setBackground(new RippleDrawable(ColorStateList.valueOf(Color.argb(60, 255, 255, 255)), rounded(cAccent, 14), null));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(52));
        blp.topMargin = dp(20);
        dark.setLayoutParams(blp);
        dark.setClickable(true);
        dark.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setBlackout(true);
            }
        });
        col.addView(dark);
        TextView darkNote = label("Saves battery and keeps the phone cool. Double-tap the black screen to come back.", 13, cSecondary, false);
        darkNote.setLineSpacing(dp(2), 1f);
        darkNote.setPadding(dp(16), dp(8), dp(16), 0);
        col.addView(darkNote);

        // Tips
        col.addView(sectionHeader("Tips"));
        LinearLayout tips = card();
        tips.addView(cardRow("Lock the screen any time: streaming keeps going. A dark screen is the coolest phone."));
        tips.addView(separator());
        tips.addView(cardRow("Zoom, focus, effects and the microphone are all set in Lens on your PC."));
        String maker = android.os.Build.MANUFACTURER.toLowerCase(java.util.Locale.US);
        if (maker.contains("xiaomi") || maker.contains("redmi") || maker.contains("poco")) {
            tips.addView(separator());
            tips.addView(cardRow("MIUI / HyperOS: Settings > Apps > Lens > Battery saver > No restrictions, "
                    + "and lock Lens in Recents, or the system may stop it."));
        }
        col.addView(tips);

        root.addView(scroll, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        blackout = new View(this);
        blackout.setBackgroundColor(Color.BLACK);
        blackout.setVisibility(View.GONE);
        blackout.setOnTouchListener(new View.OnTouchListener() {
            private long lastTap;

            @Override
            public boolean onTouch(View v, MotionEvent e) {
                if (e.getAction() == MotionEvent.ACTION_DOWN) {
                    long now = e.getEventTime();
                    if (now - lastTap < 400) {
                        setBlackout(false);
                        ui.removeCallbacks(autoBlackout);
                        ui.postDelayed(autoBlackout, AUTO_BLACKOUT_MS);
                    }
                    lastTap = now;
                }
                return true;
            }
        });
        root.addView(blackout, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        return root;
    }

    private int dp(float v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
