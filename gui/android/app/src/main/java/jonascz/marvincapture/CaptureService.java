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

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * Owns the session (pin_session_t) and with it the device: the USB connection, the capture, the
 * foreground notification while a capture runs or is being finalised, the partial wake lock for
 * the same time (a foreground service alone does not keep the CPU awake with the screen off), the
 * audio keep-alive during analog captures, and the event queue. Port of the session half of the
 * macOS WindowModel. The activity binds to it for as long as it exists and gets a status snapshot
 * per tick (5 Hz while it is visible, as on macOS; 1 Hz otherwise).
 *
 * Close order (as on macOS): audio detach, preview detach, then pin_close, which blocks until the
 * files are finalised, so it runs on the core thread; the UsbDeviceConnection stays open until
 * pin_close returned. Opens run on the same thread, after any close.
 *
 * Foreground type: connectedDevice for the USB device. The replay device (a recording played back,
 * for demos and the emulator) has no USB permission to satisfy that type, so it runs as
 * mediaPlayback, which is what it is.
 */
public final class CaptureService extends Service {
    private static final String TAG = MarvinApp.TAG;
    static final String ACTION_FOREGROUND = "jonascz.marvincapture.FOREGROUND";
    static final String ACTION_STOP = "jonascz.marvincapture.STOP";
    private static final String CH_CAPTURE = "capture", CH_RESULT = "result", CH_ALERT = "alert";
    private static final int NOTIF_ONGOING = 1, NOTIF_RESULT = 2;

    interface Listener {
        void onStatus(Status st);
        /** The session opened or closed, or what the card says without one changed. */
        void onSessionChanged();
        /** A session event (after the service handled its part). */
        void onEvent(int kind, int a, String text);
        /** An abnormal capture end; true if the activity showed it (else a notification). */
        boolean onCaptureFailed(String title, String text);
    }

    final class LocalBinder extends Binder {
        CaptureService service() { return CaptureService.this; }
    }

    /** What the app needs to start a capture (port of buildCaptureOpts). */
    static final class Plan {
        String path, title;
        final int[] opts = new int[Core.OPT_COUNT];
    }

    private final IBinder binder = new LocalBinder();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ExecutorService coreThread = Executors.newSingleThreadExecutor(r -> new Thread(r, "Core open/close"));
    private final Status status = new Status();
    private final int[] evA = new int[1];
    private final String[] evText = new String[1];

    private Listener listener;
    private boolean listenerVisible;
    private boolean monitorWanted;

    private long session;
    private int opening;           // opens queued on the core thread
    private int closing;           // closes queued on the core thread
    private int finalizing;        // closes of a session that was capturing (pin_close finalises)
    private long detachedAt;       // the open USB device was unplugged (uptime ms), 0 = no
    private UsbDevice device;      // the open USB device; null for the replay device
    private UsbDeviceConnection connection;
    private boolean replay;
    private String deviceName = "";
    private String cardText = "";  // what the no-video card says while no session is open
    private int lastStreamKind = Core.KIND_DV;
    private int appliedAspect = -1;

    private boolean foreground;
    private double startRequestedAt = -1;
    private PowerManager.WakeLock wakeLock;
    private boolean keepAlive;
    private long lastNotify;
    private long captureStartWall;
    private String pendingFinished;   // a normal end, announced one tick later unless no-video follows
    private int pendingAge;
    // The last abnormal capture end (or background-start refusal) the user has not seen: with the app
    // in the background and notifications denied nothing else shows it. Handed to the activity when
    // it becomes visible.
    private String unseenTitle, unseenText;
    private boolean foregroundDenied;   // startForeground failed for this busy period; not retried every tick

    private final BroadcastReceiver detachReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent intent) {
            @SuppressWarnings("deprecation")
            UsbDevice d = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (d != null && device != null && d.getDeviceName().equals(device.getDeviceName())) {
                // The core notices the loss itself and ends a running capture (finalising the
                // files, PIN_STOP_DEVICE_LOST): the tick closes the session once it has, so that
                // event is not lost with the session.
                Log.i(TAG, "device detached");
                detachedAt = SystemClock.uptimeMillis();
                scheduleTick(0);
            }
        }
    };

    // ---- lifecycle -------------------------------------------------------------------------------

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        NotificationChannel ch = new NotificationChannel(CH_CAPTURE, "Capture in progress", NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("Shown while a capture runs or its files are being finalised");
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
        ch = new NotificationChannel(CH_RESULT, "Capture finished", NotificationManager.IMPORTANCE_DEFAULT);
        ch.setDescription("A capture ended as planned");
        nm.createNotificationChannel(ch);
        ch = new NotificationChannel(CH_ALERT, "Capture problems", NotificationManager.IMPORTANCE_HIGH);
        ch.setDescription("A capture stopped because something failed (device unplugged, disk full, ...)");
        nm.createNotificationChannel(ch);
        PowerManager pm = getSystemService(PowerManager.class);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MarvinCapture:capture");
        wakeLock.setReferenceCounted(false);
        registerReceiver(detachReceiver, new IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED),
                Context.RECEIVER_NOT_EXPORTED);
        cardText = Core.noDevicesHint();
    }

    @Override
    public IBinder onBind(Intent intent) { return binder; }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String a = intent != null ? intent.getAction() : null;
        if (ACTION_STOP.equals(a)) {
            stopCapture();
        } else if (ACTION_FOREGROUND.equals(a)) {
            goForeground();   // startForegroundService: must reach startForeground in any case
        }
        if (!isBusy()) leaveForeground();
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        unregisterReceiver(detachReceiver);
        closeSession("");
        coreThread.shutdown();
        // The close finalises the files; the process may be killed right after this returns.
        try {
            if (!coreThread.awaitTermination(8, TimeUnit.SECONDS)) Log.w(TAG, "core thread still closing at onDestroy");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        Core.audioSetKeepAlive(MarvinApp.monitor, false);
        if (wakeLock.isHeld()) wakeLock.release();
        super.onDestroy();
    }

    // ---- activity link ---------------------------------------------------------------------------

    void setListener(Listener l, boolean visible) {
        listener = l;
        listenerVisible = l != null && visible;
        if (listenerVisible && unseenTitle != null) {
            String t = unseenTitle, x = unseenText;
            unseenTitle = unseenText = null;
            getSystemService(NotificationManager.class).cancel(NOTIF_RESULT);
            if (!l.onCaptureFailed(t, x)) {
                unseenTitle = t;
                unseenText = x;
            }
        }
        scheduleTick(0);
    }

    /** Audio monitor: the activity is visible and unmuted. */
    void setMonitorWanted(boolean on) {
        monitorWanted = on;
        if (session != 0) {
            Core.monitorEnable(session, on);   // the core fills its PCM ring only while wanted
            Core.audioSetRunning(MarvinApp.monitor, on);
        }
    }

    long session() { return session; }
    boolean hasSession() { return session != 0; }
    boolean isOpening() { return opening > 0; }
    boolean isClosing() { return closing > 0; }
    boolean isReplay() { return replay; }
    String deviceName() { return deviceName; }
    String cardText() { return cardText; }
    Status status() { return status; }
    int streamKind() { return lastStreamKind; }

    void setCardText(String text) {
        if (session != 0) return;
        cardText = text;
        if (listener != null) listener.onSessionChanged();
    }

    // ---- open / close ---------------------------------------------------------------------------

    /** Opens a USB device the app has permission for (UsbManager.hasPermission). */
    void openUsb(UsbDevice d) {
        if (session != 0 || opening > 0) return;
        String[] model = Core.usbModel(d.getVendorId(), d.getProductId());
        if (model == null) return;
        if (!model[1].isEmpty() || !model[2].isEmpty()) {   // not drivable from a descriptor / unsupported
            setCardText(model[0] + "\n\n" + (!model[1].isEmpty() ? model[1] : model[2]));
            return;
        }
        UsbManager um = getSystemService(UsbManager.class);
        UsbDeviceConnection c = um.openDevice(d);
        if (c == null) {
            setCardText(model[0] + "\n\n" + model[3]);
            return;
        }
        String id = "fd:" + c.getFileDescriptor();
        open(id, model[0], d, c, false);
    }

    /** Opens the replay device: a DV / HDV recording played back in real time, looping. */
    void openReplay(String path) {
        if (session != 0 || opening > 0) return;
        if (!new File(path).canRead()) {
            setCardText("Cannot read the replay file " + path);
            return;
        }
        Core.setReplayFile(path);
        open(path, "Replay: " + new File(path).getName(), null, null, true);
    }

    private void open(String id, String name, UsbDevice d, UsbDeviceConnection c, boolean isReplay) {
        opening++;
        cardText = "Opening " + name + "…";
        if (listener != null) listener.onSessionChanged();
        Log.i(TAG, "opening " + name);
        coreThread.execute(() -> {
            int[] st = new int[1];
            long t0 = SystemClock.uptimeMillis();
            long s = Core.open(id, st);
            Log.i(TAG, "pin_open: " + Core.strerror(st[0]) + " after " + (SystemClock.uptimeMillis() - t0) + " ms");
            handler.post(() -> {
                opening--;
                if (s == 0) {
                    if (c != null) c.close();
                    cardText = "Could not open " + name + ": " + Core.strerror(st[0]);
                    if (listener != null) listener.onSessionChanged();
                    return;
                }
                session = s;
                device = d;
                connection = c;
                replay = isReplay;
                deviceName = name;
                detachedAt = 0;
                afterOpen();
            });
        });
    }

    /** Port of openSelectedDevice's second half: the saved options go to the fresh session. */
    private void afterOpen() {
        long s = session;
        appliedAspect = -1;
        lastStreamKind = Settings.input() == Core.INPUT_DV ? Core.KIND_DV : Core.KIND_ANALOG;
        applyAspect();
        Core.monitorEnable(s, monitorWanted);
        Core.audioAttach(MarvinApp.monitor, s);
        Core.audioSetRunning(MarvinApp.monitor, monitorWanted);
        Core.previewAttach(MarvinApp.renderer, s);
        Core.setInput(s, Settings.input());
        Core.setStandard(s, Settings.standard());
        for (int c = 0; c < Core.CTL_COUNT; c++) {
            int v = Settings.getInt("gui.control_" + c, Integer.MIN_VALUE);
            if (v != Integer.MIN_VALUE) Core.setControl(s, c, v);
        }
        cardText = "Preparing device…";
        pushOutputHint();
        if (listener != null) listener.onSessionChanged();
        scheduleTick(0);
    }

    /** Closes the session (finalising a running capture) on the core thread; text = the card's. */
    void closeSession(String text) {
        cardText = text;
        if (session == 0) {
            if (listener != null) listener.onSessionChanged();
            return;
        }
        long s = session;
        UsbDeviceConnection c = connection;
        // A fresh read: the last tick's snapshot can be 1 s old, and a capture started since then
        // must keep the foreground service and wake lock until pin_close has finalised it.
        status.read(s);
        boolean wasCapturing = status.isCapturing() || status.state() == Core.STATE_STOPPING;
        session = 0;
        connection = null;
        device = null;
        replay = false;
        Core.audioSetRunning(MarvinApp.monitor, false);
        Core.audioDetach(MarvinApp.monitor);      // never call into a closed session
        Core.previewDetach(MarvinApp.renderer);
        Core.audioSetKeepAlive(MarvinApp.monitor, false);
        keepAlive = false;
        status.i[Core.ST_STATE] = Core.STATE_CLOSED;
        closing++;
        if (wasCapturing) finalizing++;
        coreThread.execute(() -> {
            Core.close(s);                          // blocks until the files are finalised
            if (c != null) c.close();
            handler.post(() -> {
                closing--;
                if (wasCapturing) finalizing--;
                if (!isBusy()) leaveForeground();
                scheduleTick(0);
            });
        });
        if (listener != null) listener.onSessionChanged();
    }

    // ---- options that reach the session ------------------------------------------------------------

    void setInput(int input) {
        Settings.setInt("gui.last_input", input);
        if (session != 0) Core.setInput(session, input);
        lastStreamKind = input == Core.INPUT_DV ? Core.KIND_DV : Core.KIND_ANALOG;
        applyAspect();
        pushOutputHint();
    }

    void setStandard(int std) {
        Settings.setInt("gui.std", std);
        if (session != 0) Core.setStandard(session, std);
    }

    void setControl(int control, int value) {
        Settings.setInt("gui.control_" + control, value);
        if (session != 0) Core.setControl(session, control, value);
    }

    /** The aspect override of what is arriving now (analog, or the detected DV / HDV); Auto where that
     *  kind's format stores no aspect (the setting is disabled). Call again when a format changes. */
    void applyAspect() {
        if (session == 0) return;
        int a = Settings.effectiveAspect(Settings.input() == Core.INPUT_DV ? lastStreamKind : Core.KIND_ANALOG);
        if (a != appliedAspect) {
            Core.setAspect(session, a);
            appliedAspect = a;
        }
    }

    /** Lets the core estimate free space and time left for the next capture. */
    void pushOutputHint() {
        if (session == 0 || status.isCapturing()) return;
        File dir = outputDir();
        if (!dir.isDirectory() && !dir.mkdirs()) Log.w(TAG, "cannot create " + dir);
        Plan p = buildPlan();
        Core.setOutputHint(session, p.path, p.title, p.opts);
    }

    static File outputDir() {
        return Storage.current().dir;
    }

    /** Capture options from the settings: a manual capture, one pass (port of buildCaptureOpts). */
    Plan buildPlan() { return buildPlan(false); }

    /** auto = "rewind and capture": the core rewinds the tape, plays it and captures (DV / HDV only). */
    Plan buildPlan(boolean auto) {
        Plan p = new Plan();
        boolean analog = Settings.input() != Core.INPUT_DV;
        int kind = analog ? Core.KIND_ANALOG : lastStreamKind == Core.KIND_HDV ? Core.KIND_HDV : Core.KIND_DV;
        String name = Settings.name(analog);
        p.path = new File(outputDir(), name).getPath();
        int fa = Settings.format(Core.KIND_ANALOG), fd = Settings.format(Core.KIND_DV), fh = Settings.format(Core.KIND_HDV);
        int now = kind == Core.KIND_ANALOG ? fa : kind == Core.KIND_HDV ? fh : fd;
        int flags = now >= 0 ? Core.formatFlags(now) : 0;
        // A tape can switch between DV and HDV: each file gets its own kind's aspect (Auto where the format
        // stores none) and the name as its title (the core leaves it out of formats that have none). Scene
        // split and the two stops come from the kind arriving now.
        p.title = name;
        int[] o = p.opts;
        o[Core.OPT_FORMAT_ANALOG] = fa;
        o[Core.OPT_FORMAT_DV] = fd;
        o[Core.OPT_FORMAT_HDV] = fh;
        o[Core.OPT_ASPECT] = Settings.effectiveAspect(analog ? Core.KIND_ANALOG : Core.KIND_DV);
        o[Core.OPT_ASPECT_HDV] = Settings.effectiveAspect(Core.KIND_HDV);
        o[Core.OPT_SCENE_SPLIT] = !analog && (Settings.split(kind) || (flags & Core.FMT_SPLIT_FORCED) != 0)
                && (flags & Core.FMT_SPLIT) != 0 ? 1 : 0;
        o[Core.OPT_IDLE_STOP] = Settings.idleStop(kind);
        o[Core.OPT_MAX_DURATION] = Settings.maxDuration(kind);
        o[Core.OPT_FIRST_NUMBER] = Core.nextFileNumber(p.path);   // every file is "name-NNNN.ext"
        int encKind = analog ? Core.KIND_ANALOG : Core.KIND_DV;
        o[Core.OPT_DEINTERLACE] = Settings.deinterlace(encKind);
        o[Core.OPT_VIDEO_QUALITY] = Settings.quality(encKind);
        o[Core.OPT_FFV1] = Settings.ffv1();
        o[Core.OPT_START_DECK] = auto && !analog ? 1 : 0;
        o[Core.OPT_REWIND_FIRST] = auto && !analog ? 1 : 0;
        return p;
    }

    // ---- capture --------------------------------------------------------------------------------

    int startCapture(Plan p, boolean overwrite) {
        if (session == 0) return Core.ERR_STATE;
        int st = Core.captureStart(session, p.path, p.title, p.opts, overwrite);
        if (st == Core.OK) {
            startRequestedAt = SystemClock.uptimeMillis() / 1000.0;
            captureStartWall = System.currentTimeMillis();
            pendingFinished = null;
            unseenTitle = unseenText = null;
            getSystemService(NotificationManager.class).cancel(NOTIF_RESULT);
            updateWakeLock(true);
            // the activity calls startForegroundService(ACTION_FOREGROUND) right after
        }
        return st;
    }

    /** A deck button (Core.DECK_*); the core ignores it where it is not allowed. */
    int deck(int cmd) {
        return session != 0 ? Core.deck(session, cmd) : Core.ERR_STATE;
    }

    void stopCapture() {
        if (session != 0) Core.captureStop(session);
    }

    /** True for a moment after an accepted start, until the status shows the capture (or 3 s). */
    boolean startPending() {
        if (startRequestedAt < 0) return false;
        if (status.isCapturing() || SystemClock.uptimeMillis() / 1000.0 - startRequestedAt > 3) {
            startRequestedAt = -1;
            return false;
        }
        return true;
    }

    /** A capture runs, is being finalised, or a session is being closed (which finalises too). */
    private boolean isBusy() {
        return (session != 0 && (status.isCapturing() || startPending())) || finalizing > 0;
    }

    // ---- tick -------------------------------------------------------------------------------------

    private final Runnable tickRunnable = this::tick;

    private void scheduleTick(long delay) {
        handler.removeCallbacks(tickRunnable);
        handler.postDelayed(tickRunnable, delay);
    }

    private void tick() {
        Core.drainProcessLog();
        if (session != 0) {
            status.read(session);
            int kind;
            while (session != 0 && (kind = Core.pollEvent(session, evA, evText)) >= 0) {
                handleEvent(kind, evA[0], evText[0] != null ? evText[0] : "");
            }
            if (detachedAt != 0 && (!status.isCapturing() || SystemClock.uptimeMillis() - detachedAt > 15000)) {
                // an abnormal end the user has not seen stays the card's text: the hint would hide why
                closeSession(unseenText != null ? unseenText + "\n\n" + Core.noDevicesHint() : Core.noDevicesHint());
            }
            int sk = status.i[Core.ST_STREAM_KIND];
            if (status.input() == Core.INPUT_DV && sk != lastStreamKind && (sk == Core.KIND_DV || sk == Core.KIND_HDV)) {
                lastStreamKind = sk;
                applyAspect();
                pushOutputHint();
            }
        }
        flushPendingFinished();
        boolean busy = isBusy();
        updateWakeLock(busy);
        updateKeepAlive(session != 0 && busy && status.analog() && Settings.audioKeepAlive());
        if (busy) {
            if (!foreground && !foregroundDenied) goForeground();
            else if (SystemClock.uptimeMillis() - lastNotify >= 1000) updateOngoing();
        } else if (foreground) {
            leaveForeground();
        } else {
            foregroundDenied = false;
        }
        if (listener != null) listener.onStatus(status);
        if (session != 0 || opening > 0 || closing > 0 || listener != null) {
            scheduleTick(listenerVisible ? 200 : 1000);
        }
    }

    private void handleEvent(int kind, int a, String text) {
        switch (kind) {
            case Core.EVT_CAPTURE_ENDED:
                if (Core.stopReasonAbnormal(a)) {
                    pendingFinished = null;
                    reportProblem("Capture stopped", text);
                } else {
                    pendingFinished = text;   // a no-video event may follow right away
                    pendingAge = 0;
                }
                break;
            case Core.EVT_NO_VIDEO:
                pendingFinished = null;
                notifyResult(CH_RESULT, "No video received", text);
                break;
            default:
                break;
        }
        if (listener != null) listener.onEvent(kind, a, text);
    }

    /** Shows a problem in the activity if it is visible; else a notification, and keeps it for the next visit. */
    private void reportProblem(String title, String text) {
        if (listener != null && listenerVisible && listener.onCaptureFailed(title, text)) return;
        unseenTitle = title;
        unseenText = text;
        notifyResult(CH_ALERT, title, text);
    }

    private void flushPendingFinished() {
        if (pendingFinished == null) return;
        if (pendingAge++ >= 1) {
            notifyResult(CH_RESULT, "Capture finished", pendingFinished);
            pendingFinished = null;
        }
    }

    private void updateWakeLock(boolean on) {
        if (on && !wakeLock.isHeld()) {
            wakeLock.acquire(24 * 3600 * 1000L);   // released when the capture is finalised
            Log.i(TAG, "wake lock acquired");
        } else if (!on && wakeLock.isHeld()) {
            wakeLock.release();
            Log.i(TAG, "wake lock released");
        }
    }

    private void updateKeepAlive(boolean on) {
        if (on == keepAlive) return;   // a disconnected stream restarts itself (audio.c)
        keepAlive = on;
        Core.audioSetKeepAlive(MarvinApp.monitor, on);
    }

    // ---- notifications ---------------------------------------------------------------------------

    private PendingIntent openAppIntent() {
        Intent i = new Intent(this, MainActivity.class).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private Notification buildOngoing() {
        Notification.Builder b = new Notification.Builder(this, CH_CAPTURE)
                .setSmallIcon(R.drawable.ic_notification)
                .setContentIntent(openAppIntent())
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS);
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        int state = status.state();
        if (session == 0 || state == Core.STATE_STOPPING || state == Core.STATE_CLOSED) {
            b.setContentTitle(state == Core.STATE_STOPPING ? status.text(Core.SS_STATE) : "Finalizing…")
                    .setProgress(0, 0, true);
        } else if (!status.isCapturing()) {
            b.setContentTitle("Starting capture…");
        } else {
            String file = new File(status.text(Core.SS_CURRENT_FILE)).getName();
            b.setContentTitle(status.text(Core.SS_STATE) + (file.isEmpty() ? "" : " · " + file))
                    .setContentText(status.text(Core.SS_SIZES) + " " + status.text(Core.SS_STORAGE_FREE))
                    .setSubText(status.text(Core.SS_FRAMES))
                    .setUsesChronometer(true)
                    .setShowWhen(true)
                    .setWhen(captureStartWall);
            String left = status.signal() ? status.text(Core.SS_DURATION_LEFT) : status.text(Core.SS_IDLE_LEFT);
            if (!left.isEmpty()) b.setStyle(new Notification.BigTextStyle().bigText(
                    status.text(Core.SS_SIZES) + " " + status.text(Core.SS_STORAGE_FREE) + "\nStopping in " + left
                            + (status.signal() ? "" : ", no signal")));
        }
        if (session != 0 && status.stopAllowed()) {
            PendingIntent stop = PendingIntent.getService(this, 1,
                    new Intent(this, CaptureService.class).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE);
            b.addAction(new Notification.Action.Builder(null, "Stop", stop).build());
        }
        return b.build();
    }

    private void goForeground() {
        Notification n = buildOngoing();
        int type = replay ? ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
                          : ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE;
        try {
            startForeground(NOTIF_ONGOING, n, type);
            foreground = true;
            lastNotify = SystemClock.uptimeMillis();
        } catch (RuntimeException e) {
            // e.g. the USB permission is gone: the capture still runs while the app is open
            Log.e(TAG, "startForeground failed", e);
            if (!foregroundDenied) {
                foregroundDenied = true;
                reportProblem("Capture may stop in the background",
                        "Android did not allow the capture to keep running in the background. Keep the app open until it ends.");
            }
        }
    }

    private void updateOngoing() {
        lastNotify = SystemClock.uptimeMillis();
        getSystemService(NotificationManager.class).notify(NOTIF_ONGOING, buildOngoing());
    }

    private void leaveForeground() {
        foregroundDenied = false;
        if (foreground) {
            stopForeground(STOP_FOREGROUND_REMOVE);
            foreground = false;
        }
        stopSelf();   // only ends the service once the activity is unbound too
    }

    private void notifyResult(String channel, String title, String text) {
        Notification n = new Notification.Builder(this, channel)
                .setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(title)
                .setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setContentIntent(openAppIntent())
                .setAutoCancel(true)
                .build();
        getSystemService(NotificationManager.class).notify(NOTIF_RESULT, n);
    }
}
