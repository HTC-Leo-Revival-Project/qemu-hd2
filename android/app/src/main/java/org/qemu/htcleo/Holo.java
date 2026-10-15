package org.qemu.htcleo;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * The small Holo widgets the web UI draws in React: a switch, a checkbox and a
 * radio dot, all drawn by hand so nothing here needs Material or even
 * AppCompat.  Plus the dialogs they live in and the window insets that Android
 * 15 and later insist on.
 */
public class Holo {
    private final Activity act;
    private final int blue = 0xff33b5e5;

    public Holo(Activity act) {
        this.act = act;
    }

    /* ------------------------------------------------------------- window insets */

    /** Android 15 draws apps edge to edge; keep the content off the bars. */
    public static void apply(final View root) {
        if (Build.VERSION.SDK_INT < 20) return;
        root.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override
            public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                int top, bottom, left, right;
                if (Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets bars = insets.getInsets(
                            WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                    top = bars.top;
                    bottom = bars.bottom;
                    left = bars.left;
                    right = bars.right;
                } else {
                    top = insets.getSystemWindowInsetTop();
                    bottom = insets.getSystemWindowInsetBottom();
                    left = insets.getSystemWindowInsetLeft();
                    right = insets.getSystemWindowInsetRight();
                }
                v.setPadding(left, top, right, bottom);
                return insets;
            }
        });
    }

    /* ------------------------------------------------------------------ the switch */

    /** The 92x30 ON/OFF slider from web/components/holo/index.tsx. */
    public View switchView(boolean checked, final OnToggle onToggle) {
        final android.view.View v = new android.view.View(act);
        v.setLayoutParams(new LinearLayout.LayoutParams(dp(92), dp(30)));
        v.setBackground(switchBackground(checked));
        v.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                boolean now = view.isActivated();
                view.setActivated(!now);
                view.setBackground(switchBackground(!now));
                if (onToggle != null) onToggle.onToggle(!now);
            }
        });
        v.setActivated(checked);
        v.setContentDescription("on/off");
        return v;
    }

    private GradientDrawable switchBackground(boolean on) {
        GradientDrawable d = new GradientDrawable(
                GradientDrawable.Orientation.TOP_BOTTOM,
                on ? new int[]{0xff3fc0ee, 0xff0099cc} : new int[]{0xff6a6a6a, 0xff474747});
        d.setCornerRadius(6 * act.getResources().getDisplayMetrics().density);
        return d;
    }

    public interface OnToggle {
        void onToggle(boolean on);
    }

    /* ----------------------------------------------------------------- the checkbox */

    /** A 20dp box with the Holo tick, drawn over a transparent CheckBox. */
    public View checkbox(boolean checked, final OnToggle onToggle) {
        final CheckBox box = new CheckBox(act);
        box.setButtonDrawable(null);
        box.setChecked(checked);
        box.setBackground(null);
        box.setPadding(dp(30), 0, 0, 0);
        final HoloDrawable tick = new HoloDrawable(blue, true);
        final HoloDrawable empty = new HoloDrawable(0xff9a9a9a, false);
        box.setCompoundDrawables(checked ? tick : empty, null, null, null);
        box.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                boolean now = ((CheckBox) view).isChecked();
                ((CheckBox) view).setCompoundDrawables(now ? tick : empty, null, null, null);
                if (onToggle != null) onToggle.onToggle(now);
            }
        });
        return box;
    }

    /* ------------------------------------------------------------------ the dialog */

    public void alert(String title, String message) {
        AlertDialog d = new AlertDialog.Builder(act)
                .setTitle(title)
                .setMessage(message)
                .setPositiveButton(R.string.nok, null)
                .create();
        show(d);
    }

    public void list(String title, String[] labels, String[] summaries, int checked, OnPick pick) {
        final LinearLayout box = new LinearLayout(act);
        box.setOrientation(LinearLayout.VERTICAL);
        final AlertDialog[] holder = new AlertDialog[1];
        for (int i = 0; i < labels.length; i++) {
            final int index = i;
            LinearLayout row = new LinearLayout(act);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(16), dp(12), dp(16), dp(12));
            row.setBackgroundResource(R.drawable.holo_row);

            LinearLayout texts = new LinearLayout(act);
            texts.setOrientation(LinearLayout.VERTICAL);
            TextView title_ = new TextView(act);
            title_.setText(labels[i]);
            title_.setTextSize(17f);
            title_.setTextColor(Color.WHITE);
            texts.addView(title_);
            if (summaries != null && i < summaries.length && summaries[i] != null) {
                TextView sum = new TextView(act);
                sum.setText(summaries[i]);
                sum.setTextSize(13f);
                sum.setTextColor(0xff808080);
                texts.addView(sum);
            }
            row.addView(texts, new LinearLayout.LayoutParams(0, android.view.ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            row.addView(radio(i == checked), new LinearLayout.LayoutParams(dp(22), dp(22)));
            row.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    holder[0].dismiss();
                    pick.onPick(index);
                }
            });
            box.addView(row, new LinearLayout.LayoutParams(android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                    android.view.ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        final AlertDialog d = new AlertDialog.Builder(act)
                .setTitle(title)
                .setView(box)
                .setNegativeButton(R.string.cancel, null)
                .create();
        holder[0] = d;
        show(d);
    }

    private View radio(boolean on) {
        android.view.View v = new android.view.View(act);
        v.setLayoutParams(new LinearLayout.LayoutParams(dp(22), dp(22)));
        v.setBackground(new RadioDrawable(blue, on));
        return v;
    }

    /**
     * Shows a dialog with the ICS panel instead of the platform's Holo one,
     * which is a black box inside a light grey frame.  The frame is painted by
     * the window's own background, so the window background and the content
     * behind the platform panel both have to go.
     */
    public void show(AlertDialog d) {
        if (d == null) return;
        d.show();
        Window w = d.getWindow();
        if (w == null) return;
        Drawable panel = act.getResources().getDrawable(R.drawable.holo_dialog_bg);
        w.setBackgroundDrawable(panel);
        View content = w.findViewById(android.R.id.content);
        if (content != null) content.setBackground(panel);
        if (Build.VERSION.SDK_INT >= 21) {
            /* a real drop shadow, which the flat drawable cannot draw */
            w.getDecorView().setElevation(dp(12));
            w.setStatusBarColor(0xff0d1012);
        }
        w.setDimAmount(0.7f);
    }

    /* ------------------------------------------------------------------- the seek */

    public SeekBar seek(int min, int max, int value, final OnSeek onSeek) {
        SeekBar bar = new SeekBar(act);
        bar.setMax(max - min);
        bar.setProgress(value - min);
        bar.setProgressDrawable(act.getResources().getDrawable(R.drawable.holo_seek));
        bar.setThumb(act.getResources().getDrawable(R.drawable.holo_seek_thumb));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                if (onSeek != null) onSeek.onSeek(min + seekBar.getProgress());
            }
        });
        return bar;
    }

    public interface OnSeek {
        void onSeek(int value);
    }

    /** How a seek bar's value is written next to its name. */
    public interface Format {
        String format(int value);
    }

    public interface OnPick {
        void onPick(int index);
    }

    public int dp(int v) {
        return (int) (v * act.getResources().getDisplayMetrics().density + 0.5f);
    }

    /** The Holo tick and the empty box, drawn so no vector support is needed. */
    static class HoloDrawable extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        private final int color;
        private final boolean checked;
        private float d = 1f;

        HoloDrawable(int color, boolean checked) {
            this.color = color;
            this.checked = checked;
        }

        @Override
        protected void onBoundsChange(Rect b) {
            d = Math.min(b.width(), b.height()) / 24f;
            path.reset();
            path.moveTo(b.left + 4 * d, b.top + 12 * d);
            path.lineTo(b.left + 9 * d, b.top + 17 * d);
            path.lineTo(b.left + 20 * d, b.top + 4 * d);
        }

        @Override
        public void draw(Canvas canvas) {
            Rect b = getBounds();
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2.4f * d);
            paint.setStrokeCap(Paint.Cap.SQUARE);
            paint.setColor(checked ? color : 0xff9a9a9a);
            canvas.drawRoundRect(new RectF(b.left + 1.5f * d, b.top + 1.5f * d,
                    b.right - 1.5f * d, b.bottom - 1.5f * d), 1.5f * d, 1.5f * d, paint);
            if (checked) {
                paint.setColor(color);
                canvas.drawPath(path, paint);
            }
        }

        @Override
        public void setAlpha(int alpha) {
            paint.setAlpha(alpha);
        }

        @Override
        public void setColorFilter(ColorFilter cf) {
            paint.setColorFilter(cf);
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }

    /** The 22dp ring with the glowing dot in it. */
    static class RadioDrawable extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final int color;
        private final boolean on;
        private final RectF oval = new RectF();
        private float d = 1f;

        RadioDrawable(int color, boolean on) {
            this.color = color;
            this.on = on;
        }

        @Override
        protected void onBoundsChange(Rect b) {
            d = Math.min(b.width(), b.height()) / 22f;
            oval.set(b.left + d, b.top + d, b.right - d, b.bottom - d);
        }

        @Override
        public void draw(Canvas canvas) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2f * d);
            paint.setColor(0xff9a9a9a);
            canvas.drawOval(oval, paint);
            if (on) {
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(color);
                paint.setShadowLayer(3f * d, 0, 0, 0x9933b5e5);
                canvas.drawOval(oval, paint);
                paint.clearShadowLayer();
            }
        }

        @Override
        public void setAlpha(int alpha) {
            paint.setAlpha(alpha);
        }

        @Override
        public void setColorFilter(ColorFilter cf) {
            paint.setColorFilter(cf);
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }

    /** A row of a settings screen: title, summary and a widget on the right. */
    public static class Row extends LinearLayout {
        public Row(Context c) {
            super(c);
            setOrientation(HORIZONTAL);
            setGravity(Gravity.CENTER_VERTICAL);
            setMinimumHeight(dp(c, 64));
            setPadding(dp(c, 16), dp(c, 10), dp(c, 16), dp(c, 10));
            setBackgroundResource(R.drawable.holo_row);
        }

        public void setTitle(String t, String summary) {
            LinearLayout texts = new LinearLayout(getContext());
            texts.setOrientation(VERTICAL);
            TextView title = new TextView(getContext());
            title.setText(t);
            title.setTextSize(17f);
            title.setTextColor(Color.WHITE);
            texts.addView(title);
            if (summary != null && !summary.isEmpty()) {
                TextView sum = new TextView(getContext());
                sum.setText(summary);
                sum.setTextSize(13.5f);
                sum.setTextColor(0xff808080);
                texts.addView(sum);
            }
            addView(texts, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f));
        }

        public void addWidget(View v) {
            addView(v, new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT));
        }

        static int dp(Context c, int v) {
            return (int) (v * c.getResources().getDisplayMetrics().density + 0.5f);
        }
    }
}
