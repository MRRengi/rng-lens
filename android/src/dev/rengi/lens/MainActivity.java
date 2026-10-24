package dev.rengi.lens;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.OrientationEventListener;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The phone side UI is deliberately tiny: status + blackout. Camera controls live in the PC app.
 * While streaming the screen goes pure black at minimum brightness after a few seconds.
 */
public final class MainActivity extends Activity implements Engine.Status {
    private static final int REQ_CAMERA = 1;
    private static final long AUTO_BLACKOUT_MS = 12_000;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private Engine engine;
    private TextView statusView;
    private View blackout;
    private OrientationEventListener orientation;
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
        orientation = new OrientationEventListener(this) {
            @Override
            public void onOrientationChanged(int deg) {
                if (deg == ORIENTATION_UNKNOWN || engine == null) return;
                engine.setDeviceOrientation(((deg + 45) / 90 * 90) % 360);
            }
        };
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
            statusView.setText("rng-lens needs camera access to work.\nGrant it in Settings > Apps > rng-lens.");
        }
    }

    private void startEngine() {
        if (engine != null) return;
        engine = new Engine(this, this);
        engine.start();
        if (orientation.canDetectOrientation()) orientation.enable();
    }

    @Override
    protected void onDestroy() {
        orientation.disable();
        if (engine != null) engine.stop();
        super.onDestroy();
    }

    @Override
    public void onStatus(final String line, final boolean nowStreaming) {
        ui.post(new Runnable() {
            @Override
            public void run() {
                statusView.setText(line);
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
        getWindow().getDecorView().setSystemUiVisibility(on ? immersive : 0);
    }

    private View buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(12, 12, 14));

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER);
        int pad = dp(28);
        col.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("rng-lens");
        title.setTextColor(Color.WHITE);
        title.setTextSize(34);
        title.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        title.setGravity(Gravity.CENTER);
        col.addView(title);

        TextView sub = new TextView(this);
        sub.setText("your phone, your webcam, free");
        sub.setTextColor(Color.rgb(140, 140, 150));
        sub.setTextSize(14);
        sub.setGravity(Gravity.CENTER);
        sub.setPadding(0, dp(4), 0, dp(36));
        col.addView(sub);

        statusView = new TextView(this);
        statusView.setTextColor(Color.rgb(220, 220, 228));
        statusView.setTextSize(16);
        statusView.setGravity(Gravity.CENTER);
        statusView.setLineSpacing(dp(4), 1f);
        statusView.setText("Starting...");
        col.addView(statusView);

        Button dark = new Button(this);
        dark.setText("Blackout (saves battery)");
        dark.setAllCaps(false);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        blp.topMargin = dp(36);
        dark.setLayoutParams(blp);
        dark.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setBlackout(true);
            }
        });
        col.addView(dark);

        TextView hint = new TextView(this);
        hint.setText("Keep this app open while streaming.\nDouble-tap the black screen to come back.");
        hint.setTextColor(Color.rgb(110, 110, 120));
        hint.setTextSize(12);
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(0, dp(18), 0, 0);
        col.addView(hint);

        root.addView(col, new FrameLayout.LayoutParams(
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

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
