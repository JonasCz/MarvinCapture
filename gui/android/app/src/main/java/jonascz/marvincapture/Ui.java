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
import android.content.res.Configuration;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;

import java.util.List;
import java.util.function.IntConsumer;

/** Helpers for the views built in code (the app has no layout XML: two screens, mostly dynamic). */
final class Ui {
    private Ui() {}

    /** The palette (res/values/colors.xml, values-night for dark): set by applyTheme, which the
     *  activity calls before it builds its views and again when the system theme changes. */
    static int TEXT, TEXT2, TEXT3, WARN, ERROR, ACCENT, ON_ACCENT, CARD, PANEL, BUTTON, TRACK;
    static boolean dark = true;

    static void applyTheme(Context c) {
        dark = (c.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_YES;
        TEXT = c.getColor(R.color.ui_text);
        TEXT2 = c.getColor(R.color.ui_text2);
        TEXT3 = c.getColor(R.color.ui_text3);
        WARN = c.getColor(R.color.ui_warn);
        ERROR = c.getColor(R.color.ui_error);
        ACCENT = c.getColor(R.color.ui_accent);
        ON_ACCENT = c.getColor(R.color.ui_on_accent);
        CARD = c.getColor(R.color.ui_card);
        PANEL = c.getColor(R.color.ui_panel);
        BUTTON = c.getColor(R.color.ui_button);
        TRACK = c.getColor(R.color.ui_track);
    }

    static int dp(Context c, float v) {
        return Math.round(v * c.getResources().getDisplayMetrics().density);
    }

    static TextView text(Context c, String s, float sp, int color) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    static TextView line(Context c, float sp, int color) {
        TextView t = text(c, "", sp, color);
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.END);
        t.setFontFeatureSettings("tnum");   // digits that don't jump while counting
        return t;
    }

    static TextView header(Context c, String s) {
        TextView t = text(c, s, 13, ACCENT);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setAllCaps(true);
        t.setLetterSpacing(0.06f);
        t.setPadding(0, dp(c, 22), 0, dp(c, 6));
        return t;
    }

    static TextView label(Context c, String s) {
        TextView t = text(c, s, 15, TEXT);
        t.setPadding(0, dp(c, 10), 0, dp(c, 2));
        return t;
    }

    static TextView help(Context c, String s) {
        TextView t = text(c, s, 12.5f, TEXT2);
        t.setPadding(0, dp(c, 2), 0, dp(c, 4));
        return t;
    }

    static LinearLayout column(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    static LinearLayout row(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    static LinearLayout.LayoutParams fillWidth() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    static LinearLayout.LayoutParams weight(float w) {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, w);
    }

    /** A drop-down of labels; onPick gets the index of a choice the user made (not the initial one). */
    static Spinner spinner(Context c, List<String> items, int selected, IntConsumer onPick) {
        Spinner sp = new Spinner(c, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> a = new ArrayAdapter<String>(c, android.R.layout.simple_spinner_item, items) {
            @Override
            public View getDropDownView(int pos, View convert, ViewGroup parent) {
                // the chosen entry in the accent colour and bold: the device's own highlight can
                // be too dark to see on the dark theme
                TextView t = (TextView) super.getDropDownView(pos, convert, parent);
                boolean cur = pos == sp.getSelectedItemPosition();
                t.setTextColor(cur ? ACCENT : TEXT);
                t.setTypeface(cur ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
                return t;
            }
        };
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(a);
        sp.setSelection(Math.max(0, Math.min(items.size() - 1, selected)), false);
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            int last = sp.getSelectedItemPosition();

            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                if (pos == last) return;
                last = pos;
                onPick.accept(pos);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        return sp;
    }

    /** A segmented choice (one of several, always visible): equal-width labels, the chosen one filled
     *  with the accent colour. The view's tag holds the chosen index; setChosen changes it without
     *  calling onPick. */
    static final class Segmented extends LinearLayout {
        private final TextView[] items;
        private int chosen = -1;

        Segmented(Context c, String[] names, int chosen, IntConsumer onPick) {
            super(c);
            setOrientation(HORIZONTAL);
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(BUTTON);
            bg.setCornerRadius(dp(c, 10));
            setBackground(bg);
            setPadding(dp(c, 3), dp(c, 3), dp(c, 3), dp(c, 3));
            items = new TextView[names.length];
            for (int i = 0; i < names.length; i++) {
                TextView t = text(c, names[i], 13, TEXT);
                t.setGravity(Gravity.CENTER);
                t.setSingleLine(true);
                t.setPadding(dp(c, 4), dp(c, 8), dp(c, 4), dp(c, 8));
                t.setClickable(true);
                t.setFocusable(true);
                int idx = i;
                t.setOnClickListener(v -> {
                    if (idx == this.chosen) return;
                    setChosen(idx);
                    onPick.accept(idx);
                });
                items[i] = t;
                addView(t, new LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            }
            setChosen(chosen);
        }

        void setChosen(int i) {
            if (i == chosen) return;
            chosen = i;
            for (int k = 0; k < items.length; k++) {
                boolean on = k == i;
                GradientDrawable d = new GradientDrawable();
                d.setCornerRadius(dp(getContext(), 8));
                d.setColor(on ? ACCENT : 0);
                items[k].setBackground(d);
                items[k].setTextColor(on ? ON_ACCENT : TEXT);
                items[k].setTypeface(on ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
                items[k].setSelected(on);
            }
        }

        @Override
        public void setEnabled(boolean e) {
            super.setEnabled(e);
            for (TextView t : items) t.setEnabled(e);
            setAlpha(e ? 1f : 0.5f);
        }
    }

    /** "Never", "1 min", "1 h 30 min" for a minute count. */
    static String minutes(int m) {
        if (m <= 0) return "Never";
        if (m < 60) return m + " min";
        return m % 60 == 0 ? (m / 60) + " h" : (m / 60) + " h " + (m % 60) + " min";
    }
}
