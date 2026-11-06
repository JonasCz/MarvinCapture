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

import android.view.Surface;

/**
 * The native layer (libmarvin-jni.so, src/main/cpp): thin wrappers over src/api/pin_api.h, plus
 * the preview renderer and the audio monitor. Every text and enable rule the app shows comes from
 * here. Sessions are opaque {@code long} handles (0 = none). The constants mirror the enums of
 * pin_api.h that the app needs (keep them in step); the ST_ / SL_ / SD_ / SS_ / OPT_ indices
 * mirror marvin_jni.c.
 */
final class Core {
    private Core() {}

    static final int API_VERSION = 3;

    // pin_status_t
    static final int OK = 0, ERR_ARG = 2, ERR_STATE = 3;

    // pin_state_t
    static final int STATE_CLOSED = 0, STATE_PREPARING = 1, STATE_READY = 2, STATE_CAPTURING = 3,
            STATE_STOPPING = 4, STATE_REWINDING = 5, STATE_ERROR = 6;

    // pin_input_t
    static final int INPUT_DV = 0, INPUT_SVIDEO = 1, INPUT_COMPOSITE = 2;

    // pin_kind_t
    static final int KIND_ANALOG = 0, KIND_DV = 1, KIND_HDV = 2;

    // pin_std_t: AUTO = 0 .. PIN_STD_COUNT - 1
    static final int STD_COUNT = 9;

    // pin_control_t
    static final int CTL_BRIGHTNESS = 0, CTL_CONTRAST = 1, CTL_SATURATION = 2, CTL_HUE = 3,
            CTL_SHARPNESS = 4, CTL_AUDIO_GAIN = 5, CTL_COUNT = 6;

    // pin_keep_busy_t
    static final int KEEP_BUSY_AUTO = 0, KEEP_BUSY_OFF = 2;

    // pin_event_kind_t
    static final int EVT_STATE = 0, EVT_FILE_OPENED = 1, EVT_FILE_CLOSED = 2, EVT_LOG = 7,
            EVT_ERROR = 8, EVT_INPUT_FORMAT = 6, EVT_CAPTURE_ENDED = 11, EVT_NO_VIDEO = 13;

    // formatFlags()
    static final int FMT_TITLE = 1, FMT_SPLIT = 2, FMT_DEFAULT = 8, FMT_ASPECT = 16, FMT_SPLIT_FORCED = 32;

    // formatOptionInts()
    static final int FO_HAS_QUALITY = 0, FO_HAS_DEINTERLACE = 1, FO_HAS_FFV1 = 2, FO_QUALITY_MIN = 3,
            FO_QUALITY_MAX = 4, FO_QUALITY_DEFAULT = 5, FO_DEINTERLACE_DEFAULT = 6, FO_FFV1_DEFAULT = 7;
    // formatOptionTexts()
    static final int FT_QUALITY_LABEL = 0, FT_QUALITY_LOW = 1, FT_QUALITY_HIGH = 2, FT_QUALITY_HELP = 3,
            FT_DEINT_LABEL = 4, FT_DEINT_KEEP = 5, FT_DEINT_DOUBLE = 6, FT_DEINT_KEEP_HELP = 7,
            FT_DEINT_DOUBLE_HELP = 8, FT_FFV1_LABEL = 9, FT_FFV1_FAST = 10, FT_FFV1_SMALLER = 11,
            FT_FFV1_SMALLEST = 12, FT_FFV1_HELP = 13, FT_NOTE = 14;
    static final int DEINTERLACE_KEEP_FIELDS = 0;

    // getControl()
    static final int CT_MIN = 0, CT_MAX = 1, CT_STEP = 2, CT_DEF = 3, CT_VALUE = 4, CT_ENABLED = 5;

    // capture options (marvin_jni.c build_opts); OPT_ASPECT: analog and DV files, OPT_ASPECT_HDV: HDV files
    static final int OPT_FORMAT_ANALOG = 0, OPT_FORMAT_DV = 1, OPT_FORMAT_HDV = 2, OPT_ASPECT = 3,
            OPT_SCENE_SPLIT = 4, OPT_IDLE_STOP = 5, OPT_MAX_DURATION = 6, OPT_FIRST_NUMBER = 7,
            OPT_DEINTERLACE = 8, OPT_VIDEO_QUALITY = 9, OPT_FFV1 = 10, OPT_START_DECK = 11,
            OPT_REWIND_FIRST = 12, OPT_ASPECT_HDV = 13, OPT_COUNT = 14;

    // pin_deck_cmd_t, pin_deck_state_t
    static final int DECK_PLAY = 0, DECK_PAUSE = 1, DECK_STOP = 2, DECK_FF = 3, DECK_REW = 4;
    static final int DECK_STATE_STOPPED = 1, DECK_STATE_PLAYING = 2, DECK_STATE_FF = 4, DECK_STATE_REWINDING = 5,
            DECK_STATE_RECORDING = 6;

    // getStatus(): ints
    static final int ST_STATE = 0, ST_INPUT = 1, ST_STREAM_KIND = 2, ST_SIGNAL = 3, ST_IS_60HZ = 4,
            ST_DAR_NUM = 5, ST_DAR_DEN = 6, ST_DECK = 7, ST_CAMERA_PRESENT = 8, ST_PROGRESS = 9,
            ST_DISK_LOW = 10, ST_STOP_REASON = 11, ST_STOP_NO_VIDEO = 12, ST_PASS = 13, ST_PASSES = 14,
            ST_LAST_ERROR = 15, ST_IS_CAPTURING = 16, ST_MANUAL_ALLOWED = 17, ST_STOP_ALLOWED = 18,
            ST_DECK_BUSY = 19, ST_WIDTH = 20, ST_HEIGHT = 21, ST_INTERLACED = 22, ST_STOP_ABNORMAL = 23,
            ST_HUB = 24, ST_DECK_REW_OK = 25, ST_DECK_PLAY_OK = 26, ST_DECK_STOP_OK = 27,
            ST_DECK_FF_OK = 28, ST_AUTO_ALLOWED = 29, ST_COUNT = 30;
    // longs
    static final int SL_FRAMES = 0, SL_FRAMES_ERROR = 1, SL_FRAMES_DROPPED = 2, SL_BYTES_WRITTEN = 3,
            SL_TOTAL_BYTES = 4, SL_DISK_FREE = 5, SL_WRITE_DROPPED = 6, SL_COUNT = 7;
    // doubles
    static final int SD_ELAPSED = 0, SD_IDLE_LEFT = 1, SD_DURATION_LEFT = 2, SD_PEAK_L = 3, SD_PEAK_R = 4,
            SD_EST_LEFT = 5, SD_RMS_L = 6, SD_RMS_R = 7, SD_COUNT = 8;
    // strings
    static final int SS_SHORT = 0, SS_STATE = 1, SS_SIGNAL = 2, SS_FRAMES = 3, SS_FRAMES_DETAIL = 4,
            SS_SIZES = 5, SS_STORAGE_FREE = 6, SS_STORAGE_DETAIL = 7, SS_DETAIL = 8, SS_ERROR = 9,
            SS_STOP_TEXT = 10, SS_CURRENT_FILE = 11, SS_VIDEO_LABEL = 12, SS_TIMECODE = 13,
            SS_STATUS_LINE = 14, SS_MANUAL_BLOCK = 15, SS_IDLE_LEFT = 16, SS_DURATION_LEFT = 17,
            SS_DECK_STATE = 18, SS_DECK_TIP = 19, SS_COUNT = 20;

    // ---- process-wide ----------------------------------------------------------------------

    static native int apiVersion();
    static native String versionString();
    static native String appVersion();
    static native String strerror(int status);
    static native String noDevicesHint();
    static native int setAppDirs(String config, String runtime, String temp);
    static native int setFirmwareDir(String dir);
    static native int encodersProbe();
    static native void setCpuKeepBusy(int mode);
    static native void setLogLevel(int level);
    static native void setReplayFile(String path);
    /** {display name, fd problem, unsupported problem, no-permission problem}, or null. */
    static native String[] usbModel(int vid, int pid);
    static native String settingsGet(String key);
    static native boolean settingsSet(String key, String value);
    /** The address of the published VERSION file ("" = the update check is off). */
    static native String updateUrl();
    /** True when a check is on and none succeeded in the last week (now: seconds since 1970). */
    static native boolean updateDue(long now);
    /** Hands the downloaded VERSION file to the core; true if it was valid (the check is then recorded). */
    static native boolean updateCheck(byte[] json, long now);
    /** {notice, latest version, download address or "", release notes or ""} if the last check found a newer version, else null. */
    static native String[] updateKnown();
    static native String stdName(int std);
    static native int[] formats(int kind);
    static native String formatLabel(int format);
    static native int formatFlags(int format);
    static native String formatWarning(int format);
    /** The file name field's help: which formats store the title (input INPUT_DV: format = DV, formatHdv =
     *  HDV; an analog input: format = analog). -1 = no list yet. */
    static native String formatTitleHelp(int input, int format, int formatHdv);
    /** The aspect setting's help for a format: what it does, or why it is off (FMT_ASPECT not set). */
    static native String formatAspectHelp(int format);
    /** The scene split setting's help for a format: what it does, or why it is always on (FMT_SPLIT_FORCED). */
    static native String formatSceneSplitHelp(int format);
    static native int[] formatOptionInts(int format);
    static native String[] formatOptionTexts(int format);
    static native String formatQualityHint(int format, int quality, int deinterlace, boolean is60hz);
    static native String formatBytes(long bytes);
    static native String formatRemaining(double seconds);
    static native int nextFileNumber(String path);
    static native boolean stopReasonAbnormal(int reason);
    static native boolean stateIsCapturing(int state);
    static native void drainProcessLog();

    // ---- session ---------------------------------------------------------------------------

    static native long open(String deviceId, int[] status);
    static native void close(long s);
    static native int setInput(long s, int input);
    static native int setStandard(long s, int std);
    static native int[] getControl(long s, int control);
    static native String controlLabel(long s, int control);
    static native int setControl(long s, int control, int value);
    static native void setAspect(long s, int aspect);
    static native void previewEnable(long s, boolean on);
    static native void monitorEnable(long s, boolean on);
    static native int checkOutput(long s, String path, String title, int[] opts, long[] nums, String[] strs);
    static native int setOutputHint(long s, String path, String title, int[] opts);
    static native int captureStart(long s, String path, String title, int[] opts, boolean overwrite);
    static native int captureStop(long s);
    static native int deck(long s, int cmd);
    static native int pollEvent(long s, int[] a, String[] text);
    static native int getStatus(long s, int[] ints, long[] longs, double[] doubles, String[] strings);

    // ---- preview renderer (preview.c) --------------------------------------------------------

    static native long previewCreate();
    static native void previewSetSurface(long renderer, Surface surface);
    static native void previewAttach(long renderer, long session);
    static native void previewDetach(long renderer);
    static native void previewSetVisible(long renderer, boolean visible);
    static native int previewInfo(long renderer, int[] dar);

    // ---- audio (audio.c) -----------------------------------------------------------------------

    static native long audioCreate();
    static native void audioAttach(long monitor, long session);
    static native void audioDetach(long monitor);
    static native void audioSetRunning(long monitor, boolean on);
    static native void audioSetKeepAlive(long monitor, boolean on);
}
