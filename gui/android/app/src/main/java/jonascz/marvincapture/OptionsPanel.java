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

import android.content.Context;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.BooleanSupplier;

/**
 * The options, behind the gear: TV standard, the output format per kind (analog, DV, HDV)
 * with the format's own settings, aspect, file name, scene split, the two automatic stops, the
 * analog picture and audio-gain controls, screen and performance options, about. In place of the
 * status panel, so the preview stays visible (useful for the picture controls). Every list,
 * label, help text and warning comes from the core (pin_formats, pin_format_options,
 * pin_format_warning, pin_get_control, pin_std_name); the values are stored by Settings. Locked
 * while a capture runs (pin_state_is_capturing), except the picture controls, which are live.
 */
final class OptionsPanel extends LinearLayout {
    interface Host {
        CaptureService service();
        void closeOptions();
        void showAbout();
        void optionsChanged();
    }

    private static final int[] IDLE_PRESETS = { 0, 1, 2, 3, 5, 10, 15, 30, 60 };
    private static final int[] DURATION_PRESETS = { 0, 5, 10, 15, 30, 45, 60, 90, 120, 180, 240, 360, 480 };

    private final Host host;
    private final LinearLayout content;
    private final TextView lockedNote;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ArrayList<View> lockable = new ArrayList<>();
    private final ArrayList<BooleanSupplier> lockableCond = new ArrayList<>();
    private final ArrayList<Slider> sliders = new ArrayList<>();
    private LinearLayout analogBlock, dvBlock, pictureRows;
    private TextView pictureNote;
    private boolean locked;
    private int builtFormatsReady = -1;

    OptionsPanel(Context c, Host host) {
        super(c);
        this.host = host;
        setOrientation(VERTICAL);
        setBackgroundColor(Ui.PANEL);

        LinearLayout bar = Ui.row(c);
        bar.setPadding(Ui.dp(c, 16), Ui.dp(c, 8), Ui.dp(c, 12), Ui.dp(c, 4));
        TextView title = Ui.text(c, "Options", 20, Ui.TEXT);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        bar.addView(title, Ui.weight(1));
        Glyphs.Button close = new Glyphs.Button(c, Glyphs.CLOSE, 44);
        close.setContentDescription("Close the options");
        close.setOnClickListener(v -> host.closeOptions());
        bar.addView(close);
        addView(bar, Ui.fillWidth());

        lockedNote = Ui.help(c, "A capture is running: the options are locked until it ends. The picture "
                + "controls stay live.");
        lockedNote.setTextColor(Ui.WARN);
        lockedNote.setPadding(Ui.dp(c, 16), 0, Ui.dp(c, 16), Ui.dp(c, 4));
        lockedNote.setVisibility(GONE);
        addView(lockedNote, Ui.fillWidth());

        ScrollView scroll = new ScrollView(c);
        content = Ui.column(c);
        content.setPadding(Ui.dp(c, 16), 0, Ui.dp(c, 16), Ui.dp(c, 32));
        scroll.addView(content);
        addView(scroll, new LayoutParams(LayoutParams.MATCH_PARENT, 0, 1));
        build();
        Formats.whenReady(this::build);   // the format lists exist once the encoder probe finished
    }

    // ---- locking --------------------------------------------------------------------------------

    private <T extends View> T lock(T v) { return lock(v, () -> true); }

    private <T extends View> T lock(T v, BooleanSupplier also) {
        lockable.add(v);
        lockableCond.add(also);
        v.setEnabled(!locked && also.getAsBoolean());
        return v;
    }

    void setLocked(boolean l) {
        if (l == locked) return;
        locked = l;
        lockedNote.setVisibility(l ? VISIBLE : GONE);
        applyEnables();
    }

    private void applyEnables() {
        for (int i = 0; i < lockable.size(); i++) {
            lockable.get(i).setEnabled(!locked && lockableCond.get(i).getAsBoolean());
        }
    }

    // ---- building ---------------------------------------------------------------------------------

    private void build() {
        int ready = Formats.isReady() ? 1 : 0;
        if (ready == builtFormatsReady) return;
        builtFormatsReady = ready;
        Context c = getContext();
        content.removeAllViews();
        lockable.clear();
        lockableCond.clear();
        sliders.clear();

        // analog
        analogBlock = Ui.column(c);
        analogBlock.addView(Ui.header(c, "Analog"));
        analogBlock.addView(Ui.label(c, "TV standard"));
        List<String> stds = new ArrayList<>();
        stds.add("Auto (PAL or NTSC)");
        for (int i = 1; i < Core.STD_COUNT; i++) stds.add(Core.stdName(i));
        analogBlock.addView(lock(Ui.spinner(c, stds, Settings.standard(), i -> {
            CaptureService svc = host.service();
            if (svc != null) svc.setStandard(i);
            else Settings.setInt("gui.std", i);
            handler.postDelayed(this::refreshControls, 300);   // hue is NTSC-only
        })), Ui.fillWidth());
        nameField(analogBlock, true);
        kindBlock(analogBlock, Core.KIND_ANALOG);
        analogBlock.addView(Ui.header(c, "Picture and sound"));
        pictureNote = Ui.help(c, "The picture and audio controls appear once the device is open on an analog input.");
        analogBlock.addView(pictureNote);
        pictureRows = Ui.column(c);
        analogBlock.addView(pictureRows, Ui.fillWidth());
        for (int ctl = 0; ctl < Core.CTL_COUNT; ctl++) {
            Slider s = new Slider(c, ctl);
            sliders.add(s);
            pictureRows.addView(s.row, Ui.fillWidth());
        }
        content.addView(analogBlock, Ui.fillWidth());

        // DV / HDV
        dvBlock = Ui.column(c);
        dvBlock.addView(Ui.header(c, "DV and HDV"));
        dvBlock.addView(Ui.help(c, "DV or HDV is detected from what the camera sends; each has its own settings."));
        nameField(dvBlock, false);
        TextView dv = Ui.label(c, "DV");
        dv.setTypeface(Typeface.DEFAULT_BOLD);
        dv.setPadding(0, Ui.dp(c, 16), 0, 0);
        dvBlock.addView(dv);
        kindBlock(dvBlock, Core.KIND_DV);
        TextView hdv = Ui.label(c, "HDV");
        hdv.setTypeface(Typeface.DEFAULT_BOLD);
        hdv.setPadding(0, Ui.dp(c, 20), 0, 0);
        dvBlock.addView(hdv);
        kindBlock(dvBlock, Core.KIND_HDV);
        content.addView(dvBlock, Ui.fillWidth());

        // screen and performance
        content.addView(Ui.header(c, "Screen and performance"));
        content.addView(toggle(c, "Keep the screen on during a capture",
                "Otherwise the screen turns off as usual; the capture carries on either way.",
                "gui.keep_screen_on", Settings.keepScreenOn(), on -> host.optionsChanged(), false));
        content.addView(toggle(c, "Keep a CPU core busy during analog capture",
                "Keeps the processor out of its deep power-saving states, whose slow wake-ups make analog "
                        + "captures lose frames. Costs battery and heat. Applies from the next capture; DV and HDV "
                        + "don't need it.",
                "gui.cpu_keep_busy", Settings.cpuKeepBusy(),
                on -> Core.setCpuKeepBusy(on ? Core.KEEP_BUSY_AUTO : Core.KEEP_BUSY_OFF), true));
        content.addView(toggle(c, "Low-latency audio keep-alive during analog capture",
                "Runs the audio output in low-latency mode (silent while muted) during an analog capture. On "
                        + "some phones (Pixels among them) that makes the system keep the processor quick to respond, "
                        + "which helps analog captures not to lose frames. Device dependent: on others it does nothing. "
                        + "DV and HDV use the normal audio output only when unmuted.",
                "gui.audio_keepalive", Settings.audioKeepAlive(), on -> {}, true));

        TextView aboutHeader = Ui.header(c, "About");
        aboutHeader.setPadding(0, Ui.dp(c, 40), 0, Ui.dp(c, 6));
        content.addView(aboutHeader);
        content.addView(Ui.help(c, "MarvinCapture " + Core.appVersion() + " · " + Core.versionString()
                + "\nFree software under the GNU AGPL v3."));
        Button about = new Button(c);
        about.setText("About and licences");
        about.setOnClickListener(v -> host.showAbout());
        LinearLayout.LayoutParams ap = Ui.wrap();
        ap.topMargin = Ui.dp(c, 10);
        content.addView(about, ap);

        updateInputVisibility();
        refreshControls();
        applyEnables();
    }

    private interface BoolSink { void set(boolean on); }

    private View toggle(Context c, String title, String helpText, String key, boolean value, BoolSink after,
                        boolean lockWhileCapturing) {
        LinearLayout col = Ui.column(c);
        Switch sw = new Switch(c);
        sw.setText(title);
        sw.setTextSize(15);
        sw.setTextColor(Ui.TEXT);
        sw.setChecked(value);
        sw.setPadding(0, Ui.dp(c, 8), 0, 0);
        sw.setOnCheckedChangeListener((b, on) -> {
            Settings.setBool(key, on);
            after.set(on);
        });
        if (lockWhileCapturing) lock(sw);
        col.addView(sw, Ui.fillWidth());
        col.addView(Ui.help(c, helpText));
        return col;
    }

    private void nameField(LinearLayout parent, boolean analog) {
        Context c = getContext();
        parent.addView(Ui.label(c, "File name"));
        EditText e = new EditText(c);
        e.setSingleLine(true);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        e.setImeOptions(EditorInfo.IME_ACTION_DONE);
        e.setText(Settings.name(analog));
        String key = analog ? "gui.name_analog" : "gui.name_dv";
        Runnable save = () -> {
            Settings.set(key, e.getText().toString().trim());
            pushHint();
        };
        e.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int d) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int d) {}
            @Override public void afterTextChanged(Editable s) {
                handler.removeCallbacks(save);
                handler.postDelayed(save, 400);
            }
        });
        parent.addView(lock(e), Ui.fillWidth());
        parent.addView(Ui.help(c, "Saved in Movies/MarvinCapture as name-0001, name-0002, …, and written into "
                + "the file as its title where the format has one."));
    }

    /** Format (+ its settings), aspect, scene split (DV / HDV), the two automatic stops. */
    private void kindBlock(LinearLayout parent, int kind) {
        Context c = getContext();
        String kk = Settings.kindKey(kind);
        int[] formats = Formats.list(kind);
        parent.addView(Ui.label(c, "Format"));
        LinearLayout optionsBlock = Ui.column(c);
        Switch[] split = new Switch[1];
        if (formats.length == 0) {
            parent.addView(Ui.help(c, "Looking for the phone's video encoders…"));
        } else {
            List<String> labels = new ArrayList<>();
            int sel = 0, cur = Settings.format(kind);
            for (int i = 0; i < formats.length; i++) {
                labels.add(Core.formatLabel(formats[i]));
                if (formats[i] == cur) sel = i;
            }
            parent.addView(lock(Ui.spinner(c, labels, sel, i -> {
                Settings.setInt("gui.format_" + kk, formats[i]);
                fillFormatOptions(optionsBlock, kind, formats[i]);
                if (split[0] != null) split[0].setEnabled(!locked && splitAllowed(kind));
                pushHint();
            })), Ui.fillWidth());
            fillFormatOptions(optionsBlock, kind, cur);
        }
        parent.addView(optionsBlock, Ui.fillWidth());

        parent.addView(Ui.label(c, "Aspect ratio"));
        parent.addView(lock(Ui.spinner(c, Arrays.asList("Auto", "4:3", "16:9"), Settings.aspect(kind), i -> {
            Settings.setInt("gui.aspect_" + kk, i);
            CaptureService svc = host.service();
            if (svc != null) svc.applyAspect();
            pushHint();
        })), Ui.fillWidth());

        if (kind != Core.KIND_ANALOG) {
            Switch sw = new Switch(c);
            sw.setText("Split into scenes (a new file for every recording on the tape)");
            sw.setTextSize(15);
            sw.setTextColor(Ui.TEXT);
            sw.setPadding(0, Ui.dp(c, 12), 0, Ui.dp(c, 4));
            sw.setChecked(Settings.split(kind));
            sw.setOnCheckedChangeListener((b, on) -> Settings.setBool("gui.split_" + kk, on));
            split[0] = lock(sw, () -> splitAllowed(kind));
            parent.addView(sw, Ui.fillWidth());
        }

        parent.addView(Ui.label(c, "Stop after no signal for"));
        parent.addView(lock(minutesSpinner(IDLE_PRESETS, Settings.idleStop(kind), m -> Settings.setInt("gui.idle_" + kk, m))),
                Ui.fillWidth());
        parent.addView(Ui.label(c, "Stop after a total time of"));
        parent.addView(lock(minutesSpinner(DURATION_PRESETS, Settings.maxDuration(kind),
                m -> Settings.setInt("gui.duration_" + kk, m))), Ui.fillWidth());
    }

    private static boolean splitAllowed(int kind) {
        int f = Settings.format(kind);
        return f >= 0 && (Core.formatFlags(f) & Core.FMT_SPLIT) != 0;
    }

    private interface IntSink { void set(int v); }

    private Spinner minutesSpinner(int[] presets, int value, IntSink sink) {
        ArrayList<Integer> vals = new ArrayList<>();
        for (int p : presets) vals.add(p);
        if (!vals.contains(value)) { vals.add(value); vals.sort(Integer::compare); }
        List<String> labels = new ArrayList<>();
        for (int v : vals) labels.add(Ui.minutes(v));
        return Ui.spinner(getContext(), labels, vals.indexOf(value), i -> {
            sink.set(vals.get(i));
            pushHint();
        });
    }

    /** The core's warning, note and settings (deinterlace, quality, FFV1 compression) of format f. */
    private void fillFormatOptions(LinearLayout block, int kind, int f) {
        Context c = getContext();
        block.removeAllViews();
        if (f < 0) return;
        String warning = Core.formatWarning(f);
        if (!warning.isEmpty()) {
            TextView w = Ui.help(c, warning);
            w.setTextColor(Ui.WARN);
            block.addView(w);
        }
        int[] oi = Core.formatOptionInts(f);
        String[] ot = Core.formatOptionTexts(f);
        if (oi == null || ot == null) return;
        // the panel note says much the same as the warning: one of them
        if (warning.isEmpty() && !ot[Core.FT_NOTE].isEmpty()) block.addView(Ui.help(c, ot[Core.FT_NOTE]));
        int encKind = kind == Core.KIND_ANALOG ? Core.KIND_ANALOG : Core.KIND_DV;
        String ek = Settings.kindKey(encKind);
        int[] deint = { Settings.deinterlace(encKind) < 0 ? oi[Core.FO_DEINTERLACE_DEFAULT] : Settings.deinterlace(encKind) };
        TextView[] qualityHint = new TextView[1];
        int[] quality = { Settings.quality(encKind) < 0 ? oi[Core.FO_QUALITY_DEFAULT]
                : Settings.clamp(Settings.quality(encKind), oi[Core.FO_QUALITY_MIN], oi[Core.FO_QUALITY_MAX]) };
        Runnable updateHint = () -> {
            if (qualityHint[0] != null) {
                Status st = host.service() != null ? host.service().status() : null;
                boolean is60 = st != null && st.i[Core.ST_IS_60HZ] != 0;
                qualityHint[0].setText(Core.formatQualityHint(f, quality[0], deint[0], is60));
            }
        };

        if (oi[Core.FO_HAS_DEINTERLACE] != 0) {
            block.addView(Ui.label(c, ot[Core.FT_DEINT_LABEL]));
            RadioGroup g = new RadioGroup(c);
            RadioButton keep = new RadioButton(c), dbl = new RadioButton(c);
            keep.setText(ot[Core.FT_DEINT_KEEP]);
            dbl.setText(ot[Core.FT_DEINT_DOUBLE]);
            keep.setId(View.generateViewId());
            dbl.setId(View.generateViewId());
            g.addView(dbl);   // the default first
            g.addView(keep);
            lock(keep);
            lock(dbl);
            (deint[0] == Core.DEINTERLACE_KEEP_FIELDS ? keep : dbl).setChecked(true);
            TextView help = Ui.help(c, "");
            Runnable showHelp = () -> {
                boolean k = deint[0] == Core.DEINTERLACE_KEEP_FIELDS;
                help.setText(k ? ot[Core.FT_DEINT_KEEP_HELP] : ot[Core.FT_DEINT_DOUBLE_HELP]);
                help.setTextColor(k ? Ui.WARN : Ui.TEXT2);
            };
            showHelp.run();
            g.setOnCheckedChangeListener((gr, id) -> {
                deint[0] = id == keep.getId() ? Core.DEINTERLACE_KEEP_FIELDS : 1 - Core.DEINTERLACE_KEEP_FIELDS;
                Settings.setInt("gui.deinterlace_" + ek, deint[0]);
                showHelp.run();
                updateHint.run();
                pushHint();
            });
            block.addView(g, Ui.fillWidth());
            block.addView(help);
        }

        if (oi[Core.FO_HAS_QUALITY] != 0) {
            int lo = oi[Core.FO_QUALITY_MIN], hi = oi[Core.FO_QUALITY_MAX];
            TextView label = Ui.label(c, ot[Core.FT_QUALITY_LABEL] + ": " + quality[0]);
            block.addView(label);
            SeekBar bar = new SeekBar(c);
            bar.setMax(hi - lo);
            bar.setProgress(quality[0] - lo);
            bar.setContentDescription(ot[Core.FT_QUALITY_LABEL]);
            lock(bar);
            block.addView(bar, Ui.fillWidth());
            LinearLayout ends = Ui.row(c);
            TextView left = Ui.text(c, ot[Core.FT_QUALITY_LOW], 12, Ui.TEXT3);
            TextView right = Ui.text(c, ot[Core.FT_QUALITY_HIGH], 12, Ui.TEXT3);
            right.setGravity(Gravity.END);
            ends.addView(left, Ui.weight(1));
            ends.addView(right, Ui.weight(1));
            block.addView(ends, Ui.fillWidth());
            qualityHint[0] = Ui.help(c, "");
            qualityHint[0].setTextColor(Ui.TEXT);
            block.addView(qualityHint[0]);
            block.addView(Ui.help(c, ot[Core.FT_QUALITY_HELP]));
            updateHint.run();
            bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                    quality[0] = lo + p;
                    label.setText(ot[Core.FT_QUALITY_LABEL] + ": " + quality[0]);
                    updateHint.run();
                }
                @Override public void onStartTrackingTouch(SeekBar s) {}
                @Override public void onStopTrackingTouch(SeekBar s) {
                    Settings.setInt("gui.quality_" + ek, quality[0]);
                    pushHint();
                }
            });
        }

        if (oi[Core.FO_HAS_FFV1] != 0) {
            block.addView(Ui.label(c, ot[Core.FT_FFV1_LABEL]));
            int v = Settings.ffv1() < 0 ? oi[Core.FO_FFV1_DEFAULT] : Settings.ffv1();
            block.addView(lock(Ui.spinner(c, Arrays.asList(ot[Core.FT_FFV1_FAST], ot[Core.FT_FFV1_SMALLER],
                    ot[Core.FT_FFV1_SMALLEST]), v, i -> {
                Settings.setInt("gui.ffv1_compression", i);
                pushHint();
            })), Ui.fillWidth());
            block.addView(Ui.help(c, ot[Core.FT_FFV1_HELP]));
        }
    }

    private void pushHint() {
        CaptureService svc = host.service();
        if (svc != null) svc.pushOutputHint();
    }

    // ---- input dependent parts -----------------------------------------------------------------

    void updateInputVisibility() {
        boolean analog = Settings.input() != Core.INPUT_DV;
        if (analogBlock != null) analogBlock.setVisibility(analog ? VISIBLE : GONE);
        if (dvBlock != null) dvBlock.setVisibility(analog ? GONE : VISIBLE);
    }

    /** Re-reads the analog controls from the core (ranges, values, hue enabled for NTSC only). */
    void refreshControls() {
        CaptureService svc = host.service();
        long s = svc != null ? svc.session() : 0;
        boolean any = false;
        for (Slider sl : sliders) any |= sl.load(s);
        if (pictureRows != null) pictureRows.setVisibility(any ? VISIBLE : GONE);
        if (pictureNote != null) pictureNote.setVisibility(any ? GONE : VISIBLE);
    }

    /** One analog control: label, value, slider, reset (port of SliderRow; live while capturing). */
    private final class Slider {
        final int control;
        final LinearLayout row;
        final TextView label, value;
        final SeekBar bar;
        final Button reset;
        int min, step = 1, def;
        boolean loading;

        Slider(Context c, int control) {
            this.control = control;
            row = Ui.column(c);
            LinearLayout top = Ui.row(c);
            label = Ui.text(c, "", 15, Ui.TEXT);
            value = Ui.text(c, "", 13, Ui.TEXT2);
            value.setFontFeatureSettings("tnum");
            top.addView(label, Ui.weight(1));
            top.addView(value);
            top.setPadding(0, Ui.dp(c, 8), 0, 0);
            row.addView(top, Ui.fillWidth());
            LinearLayout bottom = Ui.row(c);
            bar = new SeekBar(c);
            bottom.addView(bar, Ui.weight(1));
            reset = new Button(c, null, android.R.attr.borderlessButtonStyle);
            reset.setText("Reset");
            reset.setTextSize(13);
            bottom.addView(reset);
            row.addView(bottom, Ui.fillWidth());
            bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                    if (loading) return;
                    int v = min + p * step;
                    showValue(v);
                    CaptureService svc = host.service();
                    if (svc != null) svc.setControl(control, v);
                }
                @Override public void onStartTrackingTouch(SeekBar s) {}
                @Override public void onStopTrackingTouch(SeekBar s) {}
            });
            reset.setOnClickListener(v -> bar.setProgress((def - min) / step));
        }

        void showValue(int v) {
            value.setText(control == Core.CTL_AUDIO_GAIN ? String.format(java.util.Locale.ROOT, "%.1f dB", v / 10.0)
                    : Integer.toString(v));
        }

        /** Takes what the core reports without echoing it back; false if the control is unknown. */
        boolean load(long session) {
            int[] info = session != 0 ? Core.getControl(session, control) : null;
            if (info == null) {
                row.setVisibility(GONE);
                return false;
            }
            loading = true;
            min = info[Core.CT_MIN];
            step = Math.max(1, info[Core.CT_STEP]);
            def = info[Core.CT_DEF];
            String l = Core.controlLabel(session, control);
            label.setText(control == Core.CTL_AUDIO_GAIN ? "Audio gain" : l);
            bar.setContentDescription(label.getText());
            bar.setMax(Math.max(1, (info[Core.CT_MAX] - min) / step));
            bar.setProgress((info[Core.CT_VALUE] - min) / step);
            showValue(info[Core.CT_VALUE]);
            boolean en = info[Core.CT_ENABLED] != 0;
            bar.setEnabled(en);
            reset.setEnabled(en);
            row.setAlpha(en ? 1f : 0.5f);
            row.setVisibility(VISIBLE);
            loading = false;
            return true;
        }
    }
}
