/*
 * Pinnacle Studio 500-USB open driver
 * Copyright (C) 2026 Jonas Cz.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

package jonascz.marvincapture;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Typeface;
import android.animation.ValueAnimator;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.IBinder;
import android.text.method.LinkMovementMethod;
import android.text.util.Linkify;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The one screen: the preview (or, without a picture, Marvin and the core's sentence why), the
 * core's status texts, the level meters, the record button (a manual capture; the stop button
 * while recording), the speaker (audio monitor, muted by default) and the gear (the options
 * panel). Binds to {@link CaptureService} for as long as it exists; finds the USB device and asks
 * for permission. Rotation is handled in place (configChanges), so the service, the session and
 * the preview surface survive it.
 *
 * Developer / demo aid: {@code adb shell am start -n jonascz.marvincapture/.MainActivity
 * --es replay /sdcard/Android/data/jonascz.marvincapture/files/tape.dv} opens the replay device
 * (a DV / HDV recording played back in real time) instead of USB, so the UI runs with a live
 * picture without the hardware (and in the emulator). A relative path is taken from the app's
 * external files directory.
 */
public final class MainActivity extends Activity implements CaptureService.Listener, OptionsPanel.Host {
    private static final String ACTION_USB_PERMISSION = "jonascz.marvincapture.USB_PERMISSION";
    private static final int PINNACLE_VID = 0x2304;
    private static final int REQ_NOTIFICATIONS = 1;

    private CaptureService svc;
    private boolean visible;
    private String replayPath;
    private boolean permissionPending, permissionDenied, askedNotifications, startAfterPermission;

    private MainLayout root;
    private SurfaceView surface;
    private LinearLayout card, recBadge, banner, controls;
    private TextView cardText, countdownText, recText, bannerTitle, bannerText;
    private TextView statusShort, signalLine, framesLine, storageLine, caption;
    private ProgressBar cardProgress, countdownBar;
    private MarvinIdleView marvin;
    private LevelMeterView meters;
    private Glyphs.Button speaker, gear;
    private LinearLayout deckGroup;
    private Glyphs.Button deckRew, deckPlay, deckStop, deckFf;
    private static final int BUTTON_DP = 46;   // the small round buttons (all but the capture button)
    private boolean autoCapture, forceManual, optionsShown, dualApplied;   // the capture button is "rewind and capture" (deck controls on, DV input)
    private Glyphs.RecordButton record;
    private OptionsPanel options;   // built on first use
    private FrameLayout optionsHolder, frame;
    private LibraryView library;
    private Glyphs.Button libraryBtn;
    private View[] infoViews;
    private View scrimTop, scrimBottom;
    private boolean captionEmpty = true;
    private final ColorDrawable panelBg = new ColorDrawable(), optionsBg = new ColorDrawable();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private ValueAnimator panelAnim;
    private boolean overlay, dual, hasVideo, capturingNow, revealed;
    private float shownBg = 1f, shownInfo = 1f, shownOptionsBg = 1f, tgtBg = 1f, tgtInfo = 1f, tgtOptionsBg = 1f;
    private OnBackInvokedCallback backCallback;
    private AlertDialog dialog;
    private final int[] dar = new int[2];
    private final ArrayList<Runnable> dialogQueue = new ArrayList<>();

    // ---- lifecycle -------------------------------------------------------------------------------

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (MarvinApp.startupError != null) {
            TextView t = Ui.text(this, "MarvinCapture cannot start.\n\n" + MarvinApp.startupError, 16, Ui.TEXT);
            t.setPadding(Ui.dp(this, 24), Ui.dp(this, 48), Ui.dp(this, 24), 0);
            setContentView(t);
            return;
        }
        getWindow().setDecorFitsSystemWindows(false);   // edge to edge (enforced from API 35 anyway)
        Ui.applyTheme(this);
        buildViews();
        setContentView(frame);
        // Android 12+: the system splash screen shows the whole Marvin in the middle; instead of
        // cutting to the layout, he flies to his place on the card (SplashExit)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) SplashExit.install(this, marvin);
        replayPath = replayExtra(getIntent());
        applyInputExtra(getIntent());
        applyLibraryExtra(getIntent());

        IntentFilter f = new IntentFilter(ACTION_USB_PERMISSION);
        f.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        registerReceiver(usbReceiver, f, Context.RECEIVER_NOT_EXPORTED);
        bindService(new Intent(this, CaptureService.class), connection, Context.BIND_AUTO_CREATE);
    }

    /** The system theme changed (the activity handles uiMode itself, see the manifest): rebuilds the
     *  views in the new palette; the service, session and surface carry on. */
    @Override
    public void onConfigurationChanged(android.content.res.Configuration cfg) {
        super.onConfigurationChanged(cfg);
        if (root == null) return;
        boolean wasDark = Ui.dark;
        Ui.applyTheme(this);
        if (wasDark == Ui.dark) return;
        boolean optionsOpen = optionsOpen();
        if (optionsOpen) closeOptions();
        if (library != null && library.isOpen()) library.close();
        options = null;
        updateBack();
        buildViews();
        setContentView(frame);
        frame.requestApplyInsets();
        if (optionsOpen) openOptions();
        if (svc != null) onStatus(svc.status());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        applyInputExtra(intent);
        applyLibraryExtra(intent);
        String r = replayExtra(intent);
        if (r != null && !r.equals(replayPath) && svc != null && !svc.status().isCapturing()) {
            replayPath = r;
            svc.closeSession("");
        }
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) {
            permissionDenied = false;
        }
        findDevice();
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (root == null) return;
        visible = true;
        updateLibraryCount();
        Core.previewSetVisible(MarvinApp.renderer, true);
        if (svc != null) {
            svc.setListener(this, true);
            svc.setMonitorWanted(!Settings.muted());
            findDevice();
        }
    }

    @Override
    protected void onStop() {
        if (root != null) {
            visible = false;
            if (library != null) library.pause();
            Core.previewSetVisible(MarvinApp.renderer, false);
            if (svc != null) {
                svc.setListener(this, false);
                svc.setMonitorWanted(false);
            }
        }
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        if (root != null) {
            if (svc != null) svc.setListener(null, false);
            unbindService(connection);
            unregisterReceiver(usbReceiver);
            if (dialog != null) dialog.dismiss();
        }
        super.onDestroy();
    }

    private final ServiceConnection connection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder b) {
            svc = ((CaptureService.LocalBinder) b).service();
            svc.setListener(MainActivity.this, visible);
            svc.setMonitorWanted(visible && !Settings.muted());
            onSessionChanged();
            if (visible) findDevice();
        }

        @Override
        public void onServiceDisconnected(ComponentName name) { svc = null; }
    };

    /** The launcher shortcuts (res/xml/shortcuts.xml) open the app on one input: the "input" extra.
     *  Not while a capture runs (the selector is locked then too). */
    private void applyInputExtra(Intent i) {
        if (i == null || !i.hasExtra("input")) return;
        int in = Settings.clamp(i.getIntExtra("input", Settings.input()), 0, 2);
        i.removeExtra("input");   // once: a recreated intent must not undo a later choice
        if (svc != null && svc.status().isCapturing()) return;
        inputPicked(in);
        if (options != null) options.setInput(in);
    }

    /** The launcher shortcut "Library": opens the playback screen. */
    private void applyLibraryExtra(Intent i) {
        if (i == null || !i.getBooleanExtra("library", false)) return;
        i.removeExtra("library");
        root.post(this::openLibrary);
    }

    private String replayExtra(Intent i) {
        String r = i != null ? i.getStringExtra("replay") : null;
        if (r == null || r.isEmpty()) return null;
        if (!r.startsWith("/")) {
            File base = getExternalFilesDir(null);
            if (base != null) r = new File(base, r).getPath();
        }
        return r;
    }

    // ---- USB -----------------------------------------------------------------------------------

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent intent) {
            if (ACTION_USB_PERMISSION.equals(intent.getAction())) {
                permissionPending = false;
                @SuppressWarnings("deprecation")
                UsbDevice d = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                if (!granted) {
                    permissionDenied = true;
                    String[] m = d != null ? Core.usbModel(d.getVendorId(), d.getProductId()) : null;
                    if (svc != null && m != null) svc.setCardText(m[0] + "\n\n" + m[3]);
                    return;
                }
                findDevice();
            } else if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) {
                permissionDenied = false;
                findDevice();
            }
        }
    };

    /** The first Pinnacle device UsbManager lists (one device only); the core's model table decides
     *  what it is and whether it can be used (pin_usb_model, pin_device_fd_problem). */
    private void findDevice() {
        if (svc == null || svc.hasSession() || svc.isOpening()) return;
        if (replayPath != null) {
            svc.openReplay(replayPath);
            return;
        }
        UsbManager um = getSystemService(UsbManager.class);
        for (UsbDevice d : um.getDeviceList().values()) {
            if (d.getVendorId() != PINNACLE_VID) continue;
            String[] m = Core.usbModel(d.getVendorId(), d.getProductId());
            if (m == null) continue;
            if (um.hasPermission(d)) {
                svc.openUsb(d);
            } else if (permissionDenied) {
                svc.setCardText(m[0] + "\n\n" + m[3]);
            } else if (!permissionPending) {
                permissionPending = true;
                svc.setCardText(m[0] + "\n\nWaiting for permission to use the USB device…");
                Intent i = new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName());
                um.requestPermission(d, PendingIntent.getBroadcast(this, 0, i, PendingIntent.FLAG_MUTABLE));
            }
            return;
        }
        svc.setCardText(Core.noDevicesHint());
    }

    // ---- views ---------------------------------------------------------------------------------

    private void buildViews() {
        root = new MainLayout(this);
        root.setBackgroundColor(Ui.PANEL);
        frame = new FrameLayout(this);
        frame.addView(root, new FrameLayout.LayoutParams(-1, -1));
        library = new LibraryView(this, this::closeLibrary);
        frame.addView(library, new FrameLayout.LayoutParams(-1, -1));
        frame.setOnApplyWindowInsetsListener((v, insets) -> {
            Insets in = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            root.setInsets(in.left, in.top, in.right, in.bottom);   // the preview goes behind them
            library.setInsets(in.left, in.top, in.right, in.bottom);
            return WindowInsets.CONSUMED;
        });
        root.setModeListener((ov, du) -> {
            overlay = ov;
            dual = du;
            applyMode();
        });
        panelBg.setColor(Ui.PANEL);
        optionsBg.setColor(Ui.PANEL);
        revealed = false;
        optionsShown = dualApplied = false;
        shownBg = shownInfo = shownOptionsBg = tgtBg = tgtInfo = tgtOptionsBg = 1f;

        // preview: the surface, the no-video card, the REC badge (MainLayout.PreviewFrame)
        MainLayout.PreviewFrame preview = new MainLayout.PreviewFrame(this);
        preview.setBackgroundColor(Color.BLACK);
        surface = new SurfaceView(this);
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) {}
            @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int hh) {
                Core.previewSetSurface(MarvinApp.renderer, h.getSurface());
            }
            @Override public void surfaceDestroyed(SurfaceHolder h) {
                Core.previewSetSurface(MarvinApp.renderer, null);
            }
        });
        preview.addView(surface);

        card = Ui.column(this);
        card.setGravity(Gravity.CENTER);
        card.setBackgroundColor(Ui.CARD);
        int pad = Ui.dp(this, 20);
        card.setPadding(pad, pad, pad, pad);
        marvin = new MarvinIdleView(this);
        card.addView(marvin, new LinearLayout.LayoutParams(-1, 0, 1));   // takes what the text leaves
        cardText = Ui.text(this, "", 14, Ui.TEXT2);
        cardText.setGravity(Gravity.CENTER);
        cardText.setMaxWidth(Ui.dp(this, 420));
        cardText.setPadding(0, Ui.dp(this, 12), 0, 0);
        card.addView(cardText, Ui.wrap());
        cardProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        cardProgress.setMax(100);
        card.addView(cardProgress, new LinearLayout.LayoutParams(Ui.dp(this, 220), -2));
        countdownText = Ui.text(this, "", 14, Ui.TEXT);
        countdownText.setTypeface(Typeface.DEFAULT_BOLD);
        countdownText.setPadding(0, Ui.dp(this, 10), 0, 0);
        card.addView(countdownText, Ui.wrap());
        countdownBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        countdownBar.setMax(1000);
        card.addView(countdownBar, new LinearLayout.LayoutParams(Ui.dp(this, 220), -2));
        card.setOnClickListener(v -> {
            // a session in the error state: close it and open the device afresh
            if (svc != null && svc.hasSession() && svc.status().state() == Core.STATE_ERROR) {
                svc.closeSession("");
                root.postDelayed(this::findDevice, 300);
            }
        });
        preview.addView(card);

        recBadge = Ui.row(this);
        GradientDrawable pill = new GradientDrawable();
        pill.setColor(0x99000000);
        pill.setCornerRadius(Ui.dp(this, 14));
        recBadge.setBackground(pill);
        recBadge.setPadding(Ui.dp(this, 10), Ui.dp(this, 4), Ui.dp(this, 12), Ui.dp(this, 4));
        View dot = new View(this);
        GradientDrawable red = new GradientDrawable();
        red.setShape(GradientDrawable.OVAL);
        red.setColor(0xffe5393b);
        dot.setBackground(red);
        recBadge.addView(dot, new LinearLayout.LayoutParams(Ui.dp(this, 10), Ui.dp(this, 10)));
        recText = Ui.line(this, 14, Ui.TEXT);
        recText.setTypeface(Typeface.DEFAULT_BOLD);
        recText.setPadding(Ui.dp(this, 8), 0, 0, 0);
        recBadge.addView(recText, Ui.wrap());
        recBadge.setVisibility(View.GONE);
        preview.addView(recBadge);
        // fades in the theme's panel colour at the top and bottom of the picture, where it goes behind
        // the status and navigation bars, so their icons stay legible over any picture
        int fade = (Ui.PANEL & 0x00ffffff) | 0xb8000000;
        scrimTop = new View(this);
        scrimTop.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] { fade, Ui.PANEL & 0x00ffffff }));
        scrimBottom = new View(this);
        scrimBottom.setBackground(new GradientDrawable(GradientDrawable.Orientation.BOTTOM_TOP, new int[] { fade, Ui.PANEL & 0x00ffffff }));
        preview.addView(scrimTop);
        preview.addView(scrimBottom);
        // a tap on the picture shows the hidden panel for a moment while capturing (overlay mode)
        preview.setOnClickListener(v -> {
            if (overlay && capturingNow) {
                revealed = !revealed;
                scheduleHide();
                updatePanels(true);
            }
        });
        root.addView(preview);

        // controls: status + buttons (the options replace them, or sit beside them on a tablet)
        controls = Ui.column(this);
        controls.setBackground(panelBg);

        ScrollView scroll = new ScrollView(this);
        LinearLayout top = Ui.column(this);
        banner = Ui.row(this);
        GradientDrawable bbg = new GradientDrawable();
        bbg.setColor(0x33ff6b61);   // recoloured by showInfo
        bbg.setCornerRadius(Ui.dp(this, 8));
        banner.setBackground(bbg);
        banner.setPadding(Ui.dp(this, 12), Ui.dp(this, 8), Ui.dp(this, 4), Ui.dp(this, 8));
        LinearLayout btext = Ui.column(this);
        bannerTitle = Ui.text(this, "", 14, Ui.TEXT);
        bannerTitle.setTypeface(Typeface.DEFAULT_BOLD);
        bannerText = Ui.text(this, "", 13, Ui.TEXT2);
        btext.addView(bannerTitle);
        btext.addView(bannerText);
        banner.addView(btext, Ui.weight(1));
        Glyphs.Button bclose = new Glyphs.Button(this, Glyphs.CLOSE, 36);
        bclose.setContentDescription("Dismiss");
        bclose.setOnClickListener(v -> banner.setVisibility(View.GONE));
        banner.addView(bclose);
        banner.setVisibility(View.GONE);
        LinearLayout.LayoutParams bl = Ui.fillWidth();
        bl.bottomMargin = Ui.dp(this, 10);
        top.addView(banner, bl);

        statusShort = Ui.text(this, "", 20, Ui.TEXT);   // the size of the options' title   // a file name, or the core's sentence about the last capture
        statusShort.setTypeface(Typeface.DEFAULT_BOLD);
        statusShort.setMaxLines(2);
        statusShort.setEllipsize(android.text.TextUtils.TruncateAt.END);
        top.addView(statusShort, Ui.fillWidth());
        signalLine = Ui.line(this, 14, Ui.TEXT2);
        signalLine.setPadding(0, Ui.dp(this, 4), 0, 0);
        top.addView(signalLine, Ui.fillWidth());
        framesLine = Ui.line(this, 14, Ui.TEXT2);
        framesLine.setPadding(0, Ui.dp(this, 2), 0, 0);
        framesLine.setOnClickListener(v -> detail("Frames", svc != null ? svc.status().text(Core.SS_FRAMES_DETAIL) : ""));
        top.addView(framesLine, Ui.fillWidth());
        storageLine = Ui.text(this, "", 14, Ui.TEXT2);
        storageLine.setFontFeatureSettings("tnum");
        storageLine.setMaxLines(2);
        storageLine.setPadding(0, Ui.dp(this, 2), 0, 0);
        storageLine.setOnClickListener(v -> detail("Storage", svc != null ? svc.status().text(Core.SS_STORAGE_DETAIL) : ""));
        top.addView(storageLine, Ui.fillWidth());
        // the audio card: the level meters with the monitor's speaker button at their right
        LinearLayout audio = Ui.row(this);
        GradientDrawable abg = new GradientDrawable();
        abg.setColor(Ui.CARD);
        abg.setStroke(Ui.dp(this, 1), Ui.TRACK);
        abg.setCornerRadius(Ui.dp(this, 10));
        audio.setBackground(abg);
        audio.setPadding(Ui.dp(this, 12), Ui.dp(this, 4), Ui.dp(this, 4), Ui.dp(this, 4));
        meters = new LevelMeterView(this, null);
        audio.addView(meters, Ui.weight(1));
        speaker = new Glyphs.Button(this, Glyphs.SPEAKER_OFF, 40);
        speaker.setOnClickListener(v -> toggleMute());
        LinearLayout.LayoutParams spl = new LinearLayout.LayoutParams(-2, -2);
        spl.leftMargin = Ui.dp(this, 10);
        audio.addView(speaker, spl);
        LinearLayout.LayoutParams ml = Ui.fillWidth();
        ml.topMargin = Ui.dp(this, 10);   // as far below the status text as it is below the panel's top
        top.addView(audio, ml);
        scroll.addView(top);
        controls.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));

        caption = Ui.text(this, "", 13, Ui.TEXT2);
        caption.setGravity(Gravity.CENTER);
        caption.setMaxLines(2);
        controls.addView(caption, Ui.fillWidth());

        // deck controls (a setting): rewind, play, stop, fast forward, lit with the deck's state
        deckGroup = Ui.column(this);
        LinearLayout deckRow = Ui.row(this);   // the lit button is the deck's state; the tip has the words
        deckRow.setPadding(0, Ui.dp(this, 2), 0, 0);
        deckRew = deckButton(deckRow, Glyphs.REW, "Rewind", Core.DECK_REW);
        deckPlay = deckButton(deckRow, Glyphs.PLAY, "Play", Core.DECK_PLAY);
        deckStop = deckButton(deckRow, Glyphs.STOP, "Stop", Core.DECK_STOP);
        deckFf = deckButton(deckRow, Glyphs.FF, "Fast forward", Core.DECK_FF);
        deckGroup.addView(deckRow, Ui.fillWidth());
        deckGroup.setVisibility(View.GONE);
        controls.addView(deckGroup, Ui.fillWidth());

        // settings on the left, capture in the middle, library on the right, on the deck row's four columns
        LinearLayout buttons = Ui.row(this);
        buttons.setPadding(0, Ui.dp(this, 4), 0, 0);
        gear = new Glyphs.Button(this, Glyphs.GEAR, BUTTON_DP);
        gear.setContentDescription("Options");
        gear.setOnClickListener(v -> openOptions());
        record = new Glyphs.RecordButton(this);
        record.setOnClickListener(v -> recordClicked());
        // with the deck controls on, holding the capture button records what arrives, without rewinding
        record.setOnLongClickListener(v -> {
            if (!autoCapture || svc == null || !svc.hasSession() || svc.status().isCapturing()) return false;
            Status st = svc.status();
            if (!st.manualAllowed()) {
                showInfo("Manual capture", st.text(Core.SS_MANUAL_BLOCK).isEmpty() ? "Not possible now." : st.text(Core.SS_MANUAL_BLOCK), false);
                return true;
            }
            record.performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);
            forceManual = true;
            recordClicked();
            return true;
        });
        libraryBtn = new Glyphs.Button(this, Glyphs.LIBRARY, BUTTON_DP);
        libraryBtn.setContentDescription("Library: play back captures");
        libraryBtn.setOnClickListener(v -> openLibrary());
        gridCell(buttons, gear, 1);
        gridCell(buttons, record, 2);
        gridCell(buttons, libraryBtn, 1);
        controls.addView(buttons, Ui.fillWidth());
        infoViews = new View[] { scroll, caption, deckGroup };
        root.addView(controls);

        optionsHolder = new FrameLayout(this);   // the options panel joins it when first needed
        optionsHolder.setBackground(optionsBg);
        optionsHolder.setVisibility(View.GONE);
        root.addView(optionsHolder);
        updateSpeaker();
        updateLibraryCount();
    }

    /** The input selector (the first entry of the options). */
    @Override
    public void inputPicked(int i) {
        if (i == Settings.input()) return;
        if (svc != null) svc.setInput(i);
        else Settings.setInt("gui.last_input", i);
        if (options != null) {
            options.updateInputVisibility();
            options.refreshControls();
        }
        onSessionChanged();
    }

    private static void set(TextView t, String s) {
        if (!t.getText().toString().equals(s)) t.setText(s);
    }

    private static void show(View v, boolean on) {
        int vis = on ? View.VISIBLE : View.GONE;
        if (v.getVisibility() != vis) v.setVisibility(vis);
    }

    // ---- status ---------------------------------------------------------------------------------

    @Override
    public void onSessionChanged() {
        if (options != null) options.refreshControls();
        if (svc != null) onStatus(svc.status());
    }

    @Override
    public void onStatus(Status st) {
        if (!visible || svc == null) return;
        boolean has = svc.hasSession();
        int state = has ? st.state() : Core.STATE_CLOSED;
        boolean capturing = has && (st.isCapturing() || svc.startPending());
        boolean finishing = has && state == Core.STATE_STOPPING;

        // preview and the no-video card
        boolean video = has && Core.previewInfo(MarvinApp.renderer, dar) == 1;
        if (video) {
            root.setAspect(dar[0], dar[1]);
        } else {
            // no picture: the card has the shape the picture will have (the chosen aspect)
            int kind = !has ? (Settings.input() == Core.INPUT_DV ? Core.KIND_DV : Core.KIND_ANALOG)
                    : st.analog() ? Core.KIND_ANALOG : svc.streamKind();
            int a = Settings.aspect(kind);
            if (a == 2) root.setAspect(16, 9);
            else if (a == 1 || !has) root.setAspect(4, 3);
        }
        hasVideo = video;
        updateBars(video);
        show(card, !video);
        show(scrimTop, video);
        show(scrimBottom, video);
        if (!video) {
            String why;
            if (!has) {
                why = svc.cardText();
            } else if (state == Core.STATE_PREPARING) {
                why = st.text(Core.SS_DETAIL).isEmpty() ? "Preparing device…" : st.text(Core.SS_DETAIL);
            } else if (state == Core.STATE_ERROR) {
                why = (st.text(Core.SS_ERROR).isEmpty() ? "Device error" : st.text(Core.SS_ERROR)) + "\n\nTap to try again.";
            } else {
                why = !st.text(Core.SS_DETAIL).isEmpty() ? st.text(Core.SS_DETAIL)
                        : st.input() == Core.INPUT_DV ? "No camera or deck signal." : "No video signal.";
            }
            set(cardText, why);
            boolean prep = has && state == Core.STATE_PREPARING;
            show(cardProgress, prep);
            if (prep) {
                int p = st.i[Core.ST_PROGRESS];
                cardProgress.setIndeterminate(p < 0);
                if (p >= 0) cardProgress.setProgress(p);
            }
            boolean counting = has && state == Core.STATE_CAPTURING && !st.signal() && st.d[Core.SD_IDLE_LEFT] >= 0;
            show(countdownText, counting);
            show(countdownBar, counting);
            if (counting) {
                set(countdownText, "Stopping capture in " + st.text(Core.SS_IDLE_LEFT));
                double total = Settings.idleStop(st.analog() ? Core.KIND_ANALOG : svc.streamKind()) * 60.0;
                countdownBar.setProgress(total > 0 ? (int) (1000 * Math.min(1, st.d[Core.SD_IDLE_LEFT] / total)) : 0);
            }
        }

        // the status texts (all the core's)
        set(statusShort, has ? st.text(Core.SS_SHORT) : svc.isOpening() ? "Opening…" : "No device");
        if (has) {
            String time;
            if (st.input() == Core.INPUT_DV) {
                time = st.text(Core.SS_TIMECODE).isEmpty() ? "--:--:--:--" : st.text(Core.SS_TIMECODE);
            } else {
                time = hms(st.d[Core.SD_ELAPSED]);
            }
            set(signalLine, st.text(Core.SS_SIGNAL) + "  ·  " + time);
            set(framesLine, st.text(Core.SS_FRAMES));
            String free = st.text(Core.SS_STORAGE_FREE);
            set(storageLine, st.text(Core.SS_SIZES) + (free.isEmpty() ? "" : " " + free));
            storageLine.setTextColor(st.i[Core.ST_DISK_LOW] != 0 ? Ui.WARN : Ui.TEXT2);
        } else {
            set(signalLine, svc.deviceName());
            set(framesLine, "");
            set(storageLine, "");
        }
        show(signalLine, !signalLine.getText().toString().isEmpty());
        show(framesLine, !framesLine.getText().toString().isEmpty());   // empty lines would add gaps
        show(storageLine, !storageLine.getText().toString().isEmpty());
        meters.push(has ? st.d[Core.SD_PEAK_L] : -144, has ? st.d[Core.SD_PEAK_R] : -144);

        // the record button: the core's enable rules
        autoCapture = Settings.deckControls() && st.input() == Core.INPUT_DV;
        record.setMode(capturing, finishing);
        if (autoCapture && !capturing && !finishing) record.setContentDescription("Rewind the tape, play and capture");
        boolean startOk = autoCapture ? st.i[Core.ST_AUTO_ALLOWED] != 0 : st.manualAllowed();
        record.setEnabled(capturing ? has && st.stopAllowed() : has && startOk && !svc.startPending());
        // the deck group: only with the setting and on the DV input
        boolean deckOn = has && Settings.deckControls() && st.input() == Core.INPUT_DV;
        show(deckGroup, deckOn);
        if (deckOn) {
            deckGroup.setContentDescription(st.text(Core.SS_DECK_TIP));
            deckRew.setEnabled(st.i[Core.ST_DECK_REW_OK] != 0);
            deckPlay.setEnabled(st.i[Core.ST_DECK_PLAY_OK] != 0);
            deckStop.setEnabled(st.i[Core.ST_DECK_STOP_OK] != 0);
            deckFf.setEnabled(st.i[Core.ST_DECK_FF_OK] != 0);
            int ds = st.i[Core.ST_DECK];
            deckRew.setChecked(ds == Core.DECK_STATE_REWINDING);
            deckPlay.setChecked(ds == Core.DECK_STATE_PLAYING || ds == Core.DECK_STATE_RECORDING);
            deckStop.setChecked(ds == Core.DECK_STATE_STOPPED);
            deckFf.setChecked(ds == Core.DECK_STATE_FF);
        }
        String cap;
        if (finishing) cap = st.text(Core.SS_STATE);
        else if (capturing) cap = captureSubtitle(st);
        else if (has && state == Core.STATE_READY && autoCapture)
            cap = st.i[Core.ST_AUTO_ALLOWED] != 0 ? "" : "No camera or deck found";
        else if (has && state == Core.STATE_READY && !st.manualAllowed()) cap = st.text(Core.SS_MANUAL_BLOCK);
        else if (has && state == Core.STATE_READY && st.analog())
            cap = "Capture " + (st.input() == Core.INPUT_SVIDEO ? "S-Video" : "composite") + " input to file";
        else cap = "";
        set(caption, cap);
        captionEmpty = cap.isEmpty();
        if (shownInfo > 0.001f) show(caption, !captionEmpty);   // an empty line would take room from the panel

        show(recBadge, capturing && !finishing && state == Core.STATE_CAPTURING);
        if (recBadge.getVisibility() == View.VISIBLE) set(recText, "REC  " + hms(st.d[Core.SD_ELAPSED]));
        if (options != null) options.setLocked(capturing);
        libraryBtn.setEnabled(!capturing && !finishing);
        capturingNow = capturing;
        if (!capturing) revealed = false;
        updatePanels(true);
        if (options != null) options.setInput(Settings.input());
        boolean keepOn = capturing && Settings.keepScreenOn();
        if (keepOn) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /** Status / navigation bar icons: dark ones on the light theme, except over a black picture. */
    private void updateBars(boolean video) {
        // landscape: the picture is behind the bars and fades into the panel colour there, so the theme's
        // icons do; portrait: the bar is over the preview's black, unless the card (the theme's colour) shows
        boolean lib = library != null && library.isOpen();
        boolean lightBars = !Ui.dark && (lib || !(video && !root.isLandscape()));
        int mask = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
        getWindow().getInsetsController().setSystemBarsAppearance(lightBars ? mask : 0, mask);
    }

    /** The line under the stop button (port of captureSubtitleText): the no-signal countdown if it
     *  runs, else the time limit, else none. */
    private static String captureSubtitle(Status st) {
        if (st.state() != Core.STATE_CAPTURING) return "";
        if (!st.signal() && st.d[Core.SD_IDLE_LEFT] >= 0) return "Stopping in " + st.text(Core.SS_IDLE_LEFT) + ", no signal";
        if (st.d[Core.SD_DURATION_LEFT] >= 0) return "Stopping in " + st.text(Core.SS_DURATION_LEFT);
        return "";
    }

    private static String hms(double seconds) {
        long t = (long) Math.max(0, seconds);
        return String.format(Locale.ROOT, "%02d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    }

    @Override
    public void onEvent(int kind, int a, String text) {
        switch (kind) {
            case Core.EVT_LOG:
                if (a >= 3) showInfo("Error", text, true);
                else if (a == 2) showInfo("Warning", text, false);
                break;
            case Core.EVT_ERROR:
                showInfo(Core.strerror(a), text, true);
                break;
            case Core.EVT_FILE_CLOSED:
                updateLibraryCount();
                if (a != Core.OK) showInfo("File not finalised cleanly", text + ": " + Core.strerror(a), true);
                break;
            case Core.EVT_NO_VIDEO:
                showInfo("No video received", text, false);
                break;
            case Core.EVT_INPUT_FORMAT:
            case Core.EVT_STATE:
                if (options != null) options.refreshControls();
                break;
            default:
                break;
        }
    }

    @Override
    public boolean onCaptureFailed(String title, String text) {
        if (!visible) return false;
        ask(title, text, null, null);
        return true;
    }

    private void showInfo(String title, String message, boolean error) {
        if (!visible) return;
        // pin_strerror's texts are lower case ("not allowed in the current state")
        if (!title.isEmpty()) title = Character.toUpperCase(title.charAt(0)) + title.substring(1);
        bannerTitle.setText(title);
        bannerText.setText(message);
        ((GradientDrawable) banner.getBackground()).setColor(error ? 0x40ff6b61 : 0x40ffc04d);
        banner.setVisibility(View.VISIBLE);
        revealed = true;   // an overlay panel that is hidden while capturing must show it
        scheduleHide();
        updatePanels(true);
    }

    private void detail(String title, String text) {
        if (!text.isEmpty()) ask(title, text, null, null);
    }

    /** One dialog at a time; ok == null shows just "OK". */
    private void ask(String title, String message, String ok, Runnable onOk) {
        Runnable show = () -> {
            AlertDialog.Builder b = new AlertDialog.Builder(this).setTitle(title).setMessage(message);
            if (ok == null) {
                b.setPositiveButton("OK", null);
            } else {
                b.setPositiveButton(ok, (d, w) -> { if (onOk != null) onOk.run(); });
                b.setNegativeButton("Cancel", null);
            }
            dialog = b.create();
            dialog.setOnDismissListener(d -> {
                dialog = null;
                if (!dialogQueue.isEmpty()) dialogQueue.remove(0).run();
            });
            dialog.show();
        };
        if (dialog == null) show.run();
        else dialogQueue.add(show);
    }

    // ---- actions ---------------------------------------------------------------------------------

    private void toggleMute() {
        boolean muted = !Settings.muted();
        Settings.setBool("gui.muted", muted);
        if (svc != null) svc.setMonitorWanted(visible && !muted);
        updateSpeaker();
    }

    /** One cell of the button grid: both button bars are four equal columns, so the buttons line up. */
    private void gridCell(LinearLayout row, View v, float weight) {
        FrameLayout cell = new FrameLayout(this);
        cell.addView(v, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        row.addView(cell, new LinearLayout.LayoutParams(0, -2, weight));
    }

    private Glyphs.Button deckButton(LinearLayout row, int glyph, String name, int cmd) {
        Glyphs.Button b = new Glyphs.Button(this, glyph, BUTTON_DP);
        b.setContentDescription(name);
        b.setOnClickListener(v -> {
            if (svc == null) return;
            int st = svc.deck(cmd);
            if (st != Core.OK) showInfo("Deck", Core.strerror(st), st != Core.ERR_STATE);
        });
        gridCell(row, b, 1);
        return b;
    }

    /** The count on the library button: the captures in the chosen storage place. */
    private void updateLibraryCount() {
        if (libraryBtn != null) libraryBtn.setBadge(Storage.captures(Storage.current().dir).size());
    }

    private void updateSpeaker() {
        boolean muted = Settings.muted();
        speaker.setGlyph(muted ? Glyphs.SPEAKER_OFF : Glyphs.SPEAKER_ON);
        speaker.setContentDescription(muted ? "Unmute the audio monitor" : "Mute the audio monitor");
    }

    private void recordClicked() {
        if (svc == null || !svc.hasSession()) return;
        if (svc.status().isCapturing()) {
            svc.stopCapture();
            return;
        }
        if (Build.VERSION.SDK_INT >= 33 && !askedNotifications
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            // the capture notification (progress, Stop) needs it; asked before the first capture
            askedNotifications = true;
            startAfterPermission = true;
            requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, REQ_NOTIFICATIONS);
            return;
        }
        planCapture();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        if (code == REQ_NOTIFICATIONS && startAfterPermission) {
            startAfterPermission = false;
            planCapture();   // with or without notifications
        }
    }

    /** Port of planCapture: the core checks the output, the app asks what it reports, then starts. */
    private void planCapture() {
        if (svc == null || !svc.hasSession()) return;
        File dir = CaptureService.outputDir();
        if (!dir.isDirectory() && !dir.mkdirs()) {
            showInfo("Can't capture to this location", "Could not create " + dir, true);
            return;
        }
        boolean auto = autoCapture && !forceManual;
        forceManual = false;
        CaptureService.Plan p = svc.buildPlan(auto);
        long[] nums = new long[5];
        String[] strs = { "", "" };
        int st = Core.checkOutput(svc.session(), p.path, p.title, p.opts, nums, strs);
        String message = strs[1] != null ? strs[1] : "";
        if (st == Core.ERR_ARG && !message.isEmpty()) {
            showInfo("Invalid file name", message, true);
            return;
        }
        if (st != Core.OK) {
            showInfo("Can't capture to this location", (Core.strerror(st) + " " + message).trim(), true);
            return;
        }
        boolean fat32 = nums[2] != 0, collision = nums[3] != 0, low = nums[4] != 0;
        List<String[]> confirm = new ArrayList<>();
        if (!message.isEmpty() || fat32) {
            String msg = message.isEmpty() ? "The destination is a FAT32 drive. Files larger than 4 GB will fail." : message;
            confirm.add(new String[] { "Check the destination", msg + "\n\nFree space: " + Core.formatBytes(nums[0])
                    + " (about " + nums[1] + " minutes at this format).", "Capture anyway" });
        }
        if (low) {
            confirm.add(new String[] { "Low disk space", "Only " + Core.formatBytes(nums[0])
                    + " free on this device's storage. Continue?", "OK" });
        }
        if (collision) {
            confirm.add(new String[] { "Replace existing file?", strs[0] + "\n\nalready exists. Do you want to overwrite it?",
                    "Overwrite" });
        }
        confirmThen(confirm, 0, () -> startCapture(p, collision));
    }

    private void confirmThen(List<String[]> list, int i, Runnable then) {
        if (i >= list.size()) {
            then.run();
            return;
        }
        String[] c = list.get(i);
        ask(c[0], c[1], c[2], () -> confirmThen(list, i + 1, then));
    }

    private void startCapture(CaptureService.Plan p, boolean overwrite) {
        if (svc == null || !svc.hasSession()) return;
        int st = svc.startCapture(p, overwrite);
        if (st != Core.OK) {
            showInfo("Start capture", Core.strerror(st), st != Core.ERR_STATE);
            return;
        }
        banner.setVisibility(View.GONE);
        // the service goes to the foreground with the capture notification
        startForegroundService(new Intent(this, CaptureService.class).setAction(CaptureService.ACTION_FOREGROUND));
    }

    // ---- options ---------------------------------------------------------------------------------

    private void ensureOptions() {
        if (options == null) {
            options = new OptionsPanel(this, this);
            optionsHolder.addView(options, new FrameLayout.LayoutParams(-1, -1));
            if (svc != null) options.setLocked(svc.hasSession() && svc.status().isCapturing());
        }
    }

    /** The options slide up over the controls (and down again): about a quarter of a second, scaled by
     *  the system's animation setting. The controls go once the options cover them. */
    private void openOptions() {
        if (dual) return;   // always there
        ensureOptions();
        options.rebuild();
        options.refreshControls();
        optionsShown = true;
        optionsHolder.animate().cancel();
        optionsHolder.setVisibility(View.VISIBLE);
        options.setVisibility(View.VISIBLE);
        int h = optionsHolder.getHeight() > 0 ? optionsHolder.getHeight() : Ui.dp(this, 400);
        optionsHolder.setTranslationY(h * 0.4f);
        optionsHolder.setAlpha(0f);
        optionsHolder.animate().translationY(0).alpha(1f).setDuration(260)
                .setInterpolator(new android.view.animation.DecelerateInterpolator())
                .withEndAction(() -> { if (optionsShown) controls.setVisibility(View.GONE); });
        updateBack();
        updatePanels(true);
    }

    @Override
    public void closeOptions() {
        if (dual || options == null || !optionsShown) return;
        optionsShown = false;
        controls.setVisibility(View.VISIBLE);
        View f = getCurrentFocus();
        if (f != null) {
            f.clearFocus();
            getSystemService(android.view.inputmethod.InputMethodManager.class).hideSoftInputFromWindow(f.getWindowToken(), 0);
        }
        FrameLayout holder = optionsHolder;
        OptionsPanel panel = options;
        holder.animate().cancel();
        holder.animate().translationY(holder.getHeight() * 0.4f).alpha(0f).setDuration(200)
                .setInterpolator(new android.view.animation.AccelerateInterpolator())
                .withEndAction(() -> {
                    if (optionsShown) return;   // opened again meanwhile
                    holder.setVisibility(View.GONE);
                    panel.setVisibility(View.GONE);
                    holder.setTranslationY(0);
                    holder.setAlpha(1f);
                });
        updateBack();
        updatePanels(true);
    }

    private boolean optionsOpen() {
        return !dual && options != null && optionsShown;
    }

    /** The screen's layout mode changed (rotation, fold, window size, or the picture's aspect moving the
     *  docked / overlay boundary): docked / overlay / tablet. Open options stay open, except that they are
     *  always shown on a tablet. */
    private void applyMode() {
        gear.setVisibility(dual ? View.INVISIBLE : View.VISIBLE);   // invisible keeps the capture button centred
        gear.setEnabled(!dual);
        if (dual) {
            ensureOptions();
            optionsShown = false;
            optionsHolder.animate().cancel();
            optionsHolder.setTranslationY(0);
            optionsHolder.setAlpha(1f);
            options.setPermanent(true);
            options.setVisibility(View.VISIBLE);
            optionsHolder.setVisibility(View.VISIBLE);
            controls.setVisibility(View.VISIBLE);
            options.rebuild();
            options.refreshControls();
        } else if (dualApplied && options != null) {   // left the tablet layout: back to the controls
            options.setPermanent(false);
            optionsShown = false;
            optionsHolder.setVisibility(View.GONE);
            controls.setVisibility(View.VISIBLE);
        }
        dualApplied = dual;
        updateBack();
        if (svc != null) onStatus(svc.status());
        updatePanels(false);
    }

    private void scheduleHide() {
        handler.removeCallbacks(hideAgain);
        if (revealed) handler.postDelayed(hideAgain, 5000);
    }

    private final Runnable hideAgain = () -> {
        revealed = false;
        updatePanels(true);
    };

    /**
     * Panel transparency (overlay mode only; docked and tablet panels stay solid): over a live picture
     * the controls are translucent; once a capture runs they fade out completely, except the audio /
     * stop / library / options buttons, until a tap on the picture shows them again. The options
     * panel over the picture is translucent too, so the picture stays visible while it is adjusted.
     */
    private void updatePanels(boolean animate) {
        if (panelBg == null || infoViews == null) return;
        boolean hidden = overlay && capturingNow && !optionsOpen() && !revealed;
        float bg = !overlay ? 1f : hidden ? 0f : hasVideo ? 0.72f : 1f;
        float info = hidden ? 0f : 1f;
        float obg = overlay && hasVideo ? 0.88f : 1f;
        if (bg == tgtBg && info == tgtInfo && obg == tgtOptionsBg) return;
        tgtBg = bg; tgtInfo = info; tgtOptionsBg = obg;
        if (panelAnim != null) panelAnim.cancel();
        float bg0 = shownBg, info0 = shownInfo, obg0 = shownOptionsBg;
        if (!animate || !visible) {
            applyPanels(bg, info, obg);
            return;
        }
        if (info > 0) for (View v : infoViews) v.setVisibility(v == caption && captionEmpty ? View.GONE : View.VISIBLE);
        ValueAnimator a = ValueAnimator.ofFloat(0f, 1f);
        a.setDuration(320);
        a.setInterpolator(new android.view.animation.DecelerateInterpolator());
        a.addUpdateListener(an -> {
            float t = (float) an.getAnimatedValue();
            applyPanels(bg0 + (bg - bg0) * t, info0 + (info - info0) * t, obg0 + (obg - obg0) * t);
        });
        a.addListener(new android.animation.AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(android.animation.Animator an) {
                if (panelAnim == a) panelAnim = null;
                applyPanels(bg, info, obg);
            }
            @Override public void onAnimationCancel(android.animation.Animator an) {
                if (panelAnim == a) panelAnim = null;
            }
        });
        panelAnim = a;
        a.start();
    }

    private void applyPanels(float bg, float info, float obg) {
        shownBg = bg;
        shownInfo = info;
        shownOptionsBg = obg;
        panelBg.setAlpha(Math.round(bg * 255));
        optionsBg.setAlpha(Math.round(obg * 255));
        for (View v : infoViews) {
            v.setAlpha(info);
            int vis = info <= 0.001f ? View.INVISIBLE : v == caption && captionEmpty ? View.GONE : View.VISIBLE;
            if (v.getVisibility() != vis) v.setVisibility(vis);
        }
        controls.invalidate();
    }

    // ---- library ---------------------------------------------------------------------------------

    private void openLibrary() {
        library.open();
        updateBack();
        updateBars(hasVideo);
    }

    private void closeLibrary() {
        updateLibraryCount();   // files may have been deleted
        library.close();
        updateBack();
        updateBars(hasVideo);
    }

    /** Back closes the library or the options (not the tablet's permanent ones). */
    private void updateBack() {
        if (Build.VERSION.SDK_INT < 33) return;
        boolean need = (library != null && library.isOpen()) || optionsOpen();
        if (need && backCallback == null) {
            backCallback = this::handleBack;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        } else if (!need && backCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
            backCallback = null;
        }
    }

    private void handleBack() {
        if (library != null && library.isOpen()) library.back();
        else if (optionsOpen()) closeOptions();
    }

    /** Back on API 30-32 only (from 33 the OnBackInvokedCallback above). */
    @Override
    @SuppressWarnings("deprecation")
    @android.annotation.SuppressLint("GestureBackNavigation")
    public void onBackPressed() {
        if ((library != null && library.isOpen()) || optionsOpen()) handleBack();
        else super.onBackPressed();
    }

    @Override
    public CaptureService service() { return svc; }

    @Override
    public void optionsChanged() {
        updateLibraryCount();   // the storage place may have changed
        if (svc != null) onStatus(svc.status());
    }

    @Override
    public void showAbout() {
        // links for the URLs only (WEB_URLS would take "README.md" for a domain)
        android.text.SpannableString text = new android.text.SpannableString(About.text());
        Linkify.addLinks(text, java.util.regex.Pattern.compile("https://[^\\s)]+"), null);
        TextView t = Ui.text(this, "", 14, Ui.TEXT);
        t.setText(text);
        t.setMovementMethod(LinkMovementMethod.getInstance());
        int p = Ui.dp(this, 22);
        t.setPadding(p, Ui.dp(this, 8), p, 0);
        ScrollView s = new ScrollView(this);
        s.addView(t);
        new AlertDialog.Builder(this).setTitle("MarvinCapture " + Core.appVersion()).setView(s)
                .setPositiveButton("OK", null).show();
    }
}
