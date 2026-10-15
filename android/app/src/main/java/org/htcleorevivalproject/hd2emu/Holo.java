package org.htcleorevivalproject.hd2emu;

import android.animation.ValueAnimator;
import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Build;
import android.text.InputType;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * Android 4.0's Holo, by hand: the ON/OFF switch, the radio dot, the text
 * field with its underline, the dialog with the blue title rule, and the
 * preference screen they all live on.  The platform's own Theme.Holo
 * widgets are drawn differently from one Android release to the next (and on
 * 15+ some of them are Material underneath), so everything that has to look
 * like ICS everywhere is drawn here - and nothing needs AppCompat.
 */
public class Holo {
    public static final int BLUE = 0xff33b5e5;
    public static final int BLUE_DARK = 0xff0099cc;
    public static final int GREEN = 0xff99cc00;
    public static final int RED = 0xffff4444;
    public static final int ORANGE = 0xffffbb33;
    public static final int TEXT = 0xffffffff;
    public static final int TEXT_2 = 0xffbebebe;
    public static final int TEXT_3 = 0xff8a8a8a;
    public static final int DIVIDER = 0x1fffffff;
    public static final int PRESSED = 0x9933b5e5;

    private final Activity act;

    public Holo(Activity act) {
        this.act = act;
    }

    public int dp(int v) {
        return dp(act, v);
    }

    public static int dp(Context c, float v) {
        return (int) (v * c.getResources().getDisplayMetrics().density + 0.5f);
    }

    /* ------------------------------------------------------------- window insets */

    /** Android 15 draws apps edge to edge; keep the content off the bars. */
    public static void apply(final View root) {
        apply(root, root);
    }

    /**
     * The same, but the bottom inset goes to {@code bottom} (a bar that should
     * reach under the navigation bar, the way ICS's split action bar sat right
     * on top of the soft keys), on top of the padding it already has.
     */
    public static void apply(final View root, final View bottom) {
        if (Build.VERSION.SDK_INT < 20) return;
        final int bl = bottom.getPaddingLeft(), bt = bottom.getPaddingTop();
        final int br = bottom.getPaddingRight(), bb = bottom.getPaddingBottom();
        root.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override
            public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                int top, bot, left, right;
                if (Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets bars = insets.getInsets(
                            WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                    top = bars.top;
                    bot = bars.bottom;
                    left = bars.left;
                    right = bars.right;
                } else {
                    top = insets.getSystemWindowInsetTop();
                    bot = insets.getSystemWindowInsetBottom();
                    left = insets.getSystemWindowInsetLeft();
                    right = insets.getSystemWindowInsetRight();
                }
                if (bottom == root) {
                    v.setPadding(left, top, right, bot);
                } else {
                    v.setPadding(left, top, right, 0);
                    bottom.setPadding(bl, bt, br, bb + bot);
                }
                return insets;
            }
        });
    }

    /** ICS had a black status bar; keep it that way where the app may say so. */
    @SuppressWarnings("deprecation")
    public static void blackBars(Window w) {
        if (w == null || Build.VERSION.SDK_INT < 21 || Build.VERSION.SDK_INT >= 35) return;
        w.setStatusBarColor(Color.BLACK);
        w.setNavigationBarColor(Color.BLACK);
    }

    /** View.setBackground is API 16; the old name does the same everywhere. */
    @SuppressWarnings("deprecation")
    public static void background(View v, Drawable d) {
        v.setBackgroundDrawable(d);
    }

    /* ---------------------------------------------------------- small factories */

    public static TextView text(Context c, CharSequence s, float sp, int color) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    /** The blue capitals over a group of rows, with the thin rule under them. */
    public static View category(Context c, String title) {
        LinearLayout box = new LinearLayout(c);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(c, 8), dp(c, 18), dp(c, 8), 0);
        TextView t = text(c, title.toUpperCase(java.util.Locale.ROOT), 14, BLUE);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(dp(c, 8), 0, dp(c, 8), dp(c, 5));
        box.addView(t);
        View rule = new View(c);
        rule.setBackgroundColor(0x9933b5e5);
        box.addView(rule, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(c, 1)));
        return box;
    }

    public static View divider(Context c) {
        View v = new View(c);
        v.setBackgroundColor(DIVIDER);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 1);
        lp.leftMargin = dp(c, 16);
        lp.rightMargin = dp(c, 16);
        v.setLayoutParams(lp);
        return v;
    }

    /** A flat button for a bar: no frame, a blue wash while held. */
    public static Button borderless(Context c, String label) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        b.setTextColor(textColors());
        background(b, pressable(c, Color.TRANSPARENT));
        b.setMinHeight(dp(c, 48));
        b.setMinimumHeight(dp(c, 48));
        b.setPadding(dp(c, 16), 0, dp(c, 16), 0);
        if (Build.VERSION.SDK_INT >= 21) b.setStateListAnimator(null);
        return b;
    }

    /** The ICS grey button, or with {@code primary} the blue one. */
    public static Button button(Context c, String label, boolean primary) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        b.setTextColor(textColors());
        b.setBackgroundResource(primary ? R.drawable.holo_button_primary : R.drawable.holo_button);
        b.setMinHeight(dp(c, 48));
        b.setMinimumHeight(dp(c, 48));
        b.setPadding(dp(c, 24), 0, dp(c, 24), 0);
        if (Build.VERSION.SDK_INT >= 21) b.setStateListAnimator(null);
        return b;
    }

    static android.content.res.ColorStateList textColors() {
        return new android.content.res.ColorStateList(
                new int[][]{{-android.R.attr.state_enabled}, {}},
                new int[]{0x4dffffff, TEXT});
    }

    /** Transparent (or {@code idle}) at rest, the Holo blue wash when pressed. */
    public static Drawable pressable(Context c, int idle) {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[]{android.R.attr.state_pressed}, new ColorDrawable(PRESSED));
        s.addState(new int[]{android.R.attr.state_activated}, new ColorDrawable(PRESSED));
        s.addState(new int[]{android.R.attr.state_focused}, new ColorDrawable(0x4033b5e5));
        s.addState(new int[]{}, new ColorDrawable(idle));
        if (Build.VERSION.SDK_INT >= 11) s.setExitFadeDuration(200);
        return s;
    }

    /* ------------------------------------------------------------ the text field */

    /** An EditText with the ICS underline: grey with little end ticks, blue and thicker in focus. */
    public static EditText field(Context c) {
        EditText e = new EditText(c);
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[]{android.R.attr.state_focused}, new FieldLine(c, true));
        bg.addState(new int[]{}, new FieldLine(c, false));
        background(e, bg);
        e.setTextColor(TEXT);
        e.setHintTextColor(0xff6a6a6a);
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        e.setSingleLine(true);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        e.setPadding(dp(c, 6), dp(c, 8), dp(c, 6), dp(c, 10));
        return e;
    }

    static final class FieldLine extends Drawable {
        private final Paint p = new Paint();
        private final boolean focused;
        private final float d;

        FieldLine(Context c, boolean focused) {
            this.focused = focused;
            this.d = c.getResources().getDisplayMetrics().density;
            p.setColor(focused ? BLUE : 0xff6a6a6a);
        }

        @Override
        public void draw(Canvas canvas) {
            Rect b = getBounds();
            float line = focused ? 2 * d : 1 * d;
            float bottom = b.bottom - 4 * d;
            canvas.drawRect(b.left + 2 * d, bottom - line, b.right - 2 * d, bottom, p);
            /* the ticks at both ends, a third of the field's height on ICS */
            float tick = 5 * d;
            canvas.drawRect(b.left + 2 * d, bottom - tick, b.left + 2 * d + line, bottom, p);
            canvas.drawRect(b.right - 2 * d - line, bottom - tick, b.right - 2 * d, bottom, p);
        }

        @Override
        public boolean getPadding(Rect padding) {
            padding.set((int) (6 * d), (int) (8 * d), (int) (6 * d), (int) (10 * d));
            return true;
        }

        @Override
        public void setAlpha(int alpha) {
            p.setAlpha(alpha);
        }

        @Override
        public void setColorFilter(ColorFilter cf) {
            p.setColorFilter(cf);
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }

    /* ------------------------------------------------------------------ the switch */

    /**
     * The ICS switch: a dark track, and a thumb that says OFF in grey on the
     * left or ON in blue on the right, sliding across when it is tapped.
     */
    public static class Switch extends View {
        private final Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint thumb = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();
        private final float d;
        private boolean checked;
        private float pos;                      /* 0 = off, 1 = on */
        private OnToggle onToggle;
        private ValueAnimator anim;
        private final String onText, offText;

        public Switch(Context c, boolean checked, OnToggle onToggle) {
            super(c);
            d = c.getResources().getDisplayMetrics().density;
            this.checked = checked;
            this.pos = checked ? 1f : 0f;
            this.onToggle = onToggle;
            onText = c.getString(R.string.switch_on);
            offText = c.getString(R.string.switch_off);
            track.setColor(0xff1f1f1f);
            edge.setStyle(Paint.Style.STROKE);
            edge.setStrokeWidth(d);
            label.setTextAlign(Paint.Align.CENTER);
            label.setTypeface(Typeface.DEFAULT_BOLD);
            label.setTextSize(11.5f * c.getResources().getDisplayMetrics().scaledDensity);
            setClickable(true);
            setFocusable(true);
            setContentDescription(checked ? "on" : "off");
            setOnClickListener(new OnClickListener() {
                @Override
                public void onClick(View v) {
                    setChecked(!Switch.this.checked, true);
                    if (Switch.this.onToggle != null) Switch.this.onToggle.onToggle(Switch.this.checked);
                }
            });
        }

        public boolean isChecked() {
            return checked;
        }

        public void setChecked(boolean on, boolean animate) {
            checked = on;
            setContentDescription(on ? "on" : "off");
            if (anim != null) anim.cancel();
            if (!animate) {
                pos = on ? 1f : 0f;
                invalidate();
                return;
            }
            anim = ValueAnimator.ofFloat(pos, on ? 1f : 0f);
            anim.setDuration(160);
            anim.addUpdateListener(new ValueAnimator.AnimatorUpdateListener() {
                @Override
                public void onAnimationUpdate(ValueAnimator a) {
                    pos = (Float) a.getAnimatedValue();
                    invalidate();
                }
            });
            anim.start();
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(resolveSize((int) (92 * d), w), resolveSize((int) (34 * d), h));
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            float th = 26 * d, top = (h - th) / 2f;
            float tw = Math.min(88 * d, w);
            float left = w - tw;
            r.set(left, top, left + tw, top + th);
            c.drawRoundRect(r, 2 * d, 2 * d, track);
            edge.setColor(0xff3a3a3a);
            c.drawRoundRect(r, 2 * d, 2 * d, edge);

            float knob = tw / 2f + 2 * d;
            float kx = left + (tw - knob) * pos;
            r.set(kx, top, kx + knob, top + th);
            boolean on = pos > 0.5f;
            int topColor = on ? 0xff3fc0ee : 0xff6b6b6b;
            int botColor = on ? 0xff0099cc : 0xff4a4a4a;
            if (!isEnabled()) {
                topColor = 0xff3a3a3a;
                botColor = 0xff333333;
            }
            thumb.setShader(new LinearGradient(0, top, 0, top + th, topColor, botColor, Shader.TileMode.CLAMP));
            c.drawRoundRect(r, 2 * d, 2 * d, thumb);
            edge.setColor(on ? 0xff33b5e5 : 0xff8a8a8a);
            if (isEnabled()) c.drawRoundRect(r, 2 * d, 2 * d, edge);

            label.setColor(!isEnabled() ? 0x66ffffff : on ? Color.WHITE : 0xffdcdcdc);
            Paint.FontMetrics fm = label.getFontMetrics();
            float ty = top + th / 2f - (fm.ascent + fm.descent) / 2f;
            c.drawText(on ? onText : offText, r.centerX(), ty, label);
        }
    }

    public interface OnToggle {
        void onToggle(boolean on);
    }

    /** The radio ring, with the glowing blue dot in it when it is the one picked. */
    static class RadioDrawable extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final boolean on;
        private final RectF oval = new RectF();
        private float u = 1f;

        RadioDrawable(boolean on) {
            this.on = on;
        }

        @Override
        protected void onBoundsChange(Rect b) {
            u = Math.min(b.width(), b.height()) / 24f;
            float cx = b.exactCenterX(), cy = b.exactCenterY();
            oval.set(cx - 9 * u, cy - 9 * u, cx + 9 * u, cy + 9 * u);
        }

        @Override
        public void draw(Canvas canvas) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(1.6f * u);
            paint.setColor(on ? 0xff9a9a9a : 0xff7a7a7a);
            canvas.drawOval(oval, paint);
            if (on) {
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(0x4033b5e5);
                float g = 2.5f * u;
                canvas.drawOval(new RectF(oval.left + g, oval.top + g, oval.right - g, oval.bottom - g), paint);
                paint.setColor(BLUE);
                float i = 4.5f * u;
                canvas.drawOval(new RectF(oval.left + i, oval.top + i, oval.right - i, oval.bottom - i), paint);
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

    /* ------------------------------------------------------------------ the dialog */

    /**
     * The ICS alert: a dark panel, the title in blue over a blue rule, and the
     * buttons along the bottom as flat cells - Cancel on the left, OK on the
     * right, the way 4.0 ordered them.
     */
    public static class Box {
        private final Activity act;
        private final Dialog dialog;
        private final LinearLayout panel, content, bar;
        private boolean hasButtons;

        public Box(Activity act, String title) {
            this.act = act;
            dialog = new Dialog(act, android.R.style.Theme_Holo_Dialog_NoActionBar);
            dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
            panel = new LinearLayout(act);
            panel.setOrientation(LinearLayout.VERTICAL);
            panel.setBackgroundResource(R.drawable.holo_dialog_bg);

            if (title != null) {
                TextView t = text(act, title, 20, BLUE);
                t.setSingleLine(true);
                t.setEllipsize(TextUtils.TruncateAt.END);
                t.setGravity(Gravity.CENTER_VERTICAL);
                t.setMinHeight(dp(act, 60));
                t.setPadding(dp(act, 16), 0, dp(act, 16), 0);
                panel.addView(t);
                View rule = new View(act);
                rule.setBackgroundColor(BLUE);
                panel.addView(rule, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(act, 2)));
            }

            content = new LinearLayout(act);
            content.setOrientation(LinearLayout.VERTICAL);
            ScrollView scroll = new ScrollView(act);
            scroll.setFillViewport(false);
            scroll.addView(content);
            /* weighted, so a long list shrinks to fit and the buttons stay on screen */
            panel.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

            bar = new LinearLayout(act);
            bar.setOrientation(LinearLayout.HORIZONTAL);
            dialog.setContentView(panel);
        }

        public Box message(CharSequence text) {
            TextView t = Holo.text(act, text, 16, TEXT_2);
            t.setPadding(dp(act, 16), dp(act, 16), dp(act, 16), dp(act, 18));
            t.setLineSpacing(0, 1.15f);
            content.addView(t);
            return this;
        }

        /** A view with the dialog's own padding around it. */
        public Box view(View v) {
            LinearLayout pad = new LinearLayout(act);
            pad.setPadding(dp(act, 16), dp(act, 12), dp(act, 16), dp(act, 16));
            pad.addView(v, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));
            content.addView(pad);
            return this;
        }

        /** A view that runs from edge to edge, like a list of choices. */
        public Box bare(View v) {
            content.addView(v);
            return this;
        }

        public Box button(String label, final Runnable onClick) {
            if (!hasButtons) {
                View rule = new View(act);
                rule.setBackgroundColor(DIVIDER);
                panel.addView(rule, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1));
                panel.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));
                hasButtons = true;
            } else {
                View v = new View(act);
                v.setBackgroundColor(DIVIDER);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(1, ViewGroup.LayoutParams.MATCH_PARENT);
                lp.topMargin = dp(act, 10);
                lp.bottomMargin = dp(act, 10);
                bar.addView(v, lp);
            }
            Button b = borderless(act, label);
            b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
            b.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    dialog.dismiss();
                    if (onClick != null) onClick.run();
                }
            });
            bar.addView(b, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            return this;
        }

        public Box cancelable(boolean yes) {
            dialog.setCancelable(yes);
            return this;
        }

        public Dialog dialog() {
            return dialog;
        }

        public void dismiss() {
            try {
                dialog.dismiss();
            } catch (RuntimeException ignored) {
                /* the activity went away first */
            }
        }

        public Box show() {
            Window w = dialog.getWindow();
            if (w != null) {
                w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
                w.setDimAmount(0.6f);
                if (Build.VERSION.SDK_INT >= 21) w.getDecorView().setElevation(dp(act, 12));
            }
            dialog.show();
            if (w != null) {
                int screen = act.getResources().getDisplayMetrics().widthPixels;
                int width = Math.min(screen - dp(act, 32), dp(act, 440));
                w.setLayout(width, WindowManager.LayoutParams.WRAP_CONTENT);
            }
            return this;
        }
    }

    public void alert(String title, CharSequence message) {
        new Box(act, title).message(message).button(act.getString(R.string.nok), null).show();
    }

    /**
     * A dialog with a single-choice list: tapping a row picks it and closes.
     * With {@code checked} below zero it is a list of actions, without radios.
     */
    public void list(String title, String[] labels, String[] summaries, int checked, final OnPick pick) {
        final Box box = new Box(act, title);
        LinearLayout rows = new LinearLayout(act);
        rows.setOrientation(LinearLayout.VERTICAL);
        for (int i = 0; i < labels.length; i++) {
            final int index = i;
            if (i > 0) rows.addView(divider(act));
            LinearLayout row = new LinearLayout(act);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setMinimumHeight(dp(56));
            row.setPadding(dp(16), dp(10), dp(12), dp(10));
            background(row, pressable(act, Color.TRANSPARENT));

            LinearLayout texts = new LinearLayout(act);
            texts.setOrientation(LinearLayout.VERTICAL);
            texts.addView(text(act, labels[i], 18, TEXT));
            if (summaries != null && i < summaries.length && summaries[i] != null && !summaries[i].isEmpty()) {
                TextView sum = text(act, summaries[i], 14, TEXT_3);
                sum.setPadding(0, dp(2), 0, 0);
                texts.addView(sum);
            }
            row.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            if (checked >= 0) {
                View radio = new View(act);
                background(radio, new RadioDrawable(i == checked));
                LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(dp(32), dp(32));
                rlp.leftMargin = dp(12);
                row.addView(radio, rlp);
            }
            row.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    box.dismiss();
                    pick.onPick(index);
                }
            });
            rows.addView(row);
        }
        box.bare(rows).button(act.getString(R.string.cancel), null).show();
    }

    /** A dialog with one text field, and Cancel / OK. */
    public void prompt(String title, String value, String hint, int inputType, final OnText done) {
        final EditText field = field(act);
        if (value != null) field.setText(value);
        if (hint != null) field.setHint(hint);
        if (inputType != 0) field.setInputType(inputType);
        field.setSelectAllOnFocus(true);
        final Box box = new Box(act, title).view(field);
        box.button(act.getString(R.string.cancel), null)
                .button(act.getString(R.string.nok), new Runnable() {
                    @Override
                    public void run() {
                        done.onText(field.getText().toString());
                    }
                });
        Window w = box.dialog().getWindow();
        if (w != null) w.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
        box.show();
        field.requestFocus();
    }

    public interface OnText {
        void onText(String text);
    }

    /** The ICS progress dialog, with the blue bar; indeterminate until told the size. */
    public static class Progress {
        private final Box box;
        private final TextView message;
        private final ProgressBar bar;

        public Progress(Activity act, String title, String text) {
            message = Holo.text(act, text, 15, TEXT_2);
            message.setPadding(0, 0, 0, dp(act, 12));
            bar = new ProgressBar(act, null, android.R.attr.progressBarStyleHorizontal);
            bar.setProgressDrawable(act.getResources().getDrawable(R.drawable.holo_progress));
            bar.setIndeterminate(true);
            LinearLayout col = new LinearLayout(act);
            col.setOrientation(LinearLayout.VERTICAL);
            col.addView(message);
            col.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(act, 16)));
            box = new Box(act, title).view(col).cancelable(false);
            box.show();
        }

        public void message(String text) {
            message.setText(text);
        }

        public void progress(long done, long total) {
            if (total <= 0) return;
            bar.setIndeterminate(false);
            bar.setMax(1000);
            bar.setProgress((int) (done * 1000 / total));
        }

        public void dismiss() {
            box.dismiss();
        }
    }

    /* ------------------------------------------------------------------- the seek */

    public SeekBar seek(final int min, int max, int value, final OnSeek onMove, final OnSeek onSeek) {
        SeekBar bar = new SeekBar(act);
        bar.setMax(max - min);
        bar.setProgress(value - min);
        bar.setProgressDrawable(act.getResources().getDrawable(R.drawable.holo_seek));
        bar.setThumb(act.getResources().getDrawable(R.drawable.holo_seek_thumb));
        bar.setThumbOffset(dp(4));
        bar.setPadding(dp(16), dp(10), dp(16), dp(10));
        if (Build.VERSION.SDK_INT >= 21) bar.setSplitTrack(false);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (onMove != null) onMove.onSeek(min + progress);
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

    /* -------------------------------------------------------------------- the row */

    /** A row of a settings screen: title, summary and a widget on the right. */
    public static class Row extends LinearLayout {
        private TextView title, summary;

        public Row(Context c) {
            super(c);
            setOrientation(HORIZONTAL);
            setGravity(Gravity.CENTER_VERTICAL);
            setMinimumHeight(dp(c, 64));
            setPadding(dp(c, 16), dp(c, 10), dp(c, 12), dp(c, 10));
            background(this, pressable(c, Color.TRANSPARENT));
        }

        public Row setTitle(String t, String sum) {
            LinearLayout texts = new LinearLayout(getContext());
            texts.setOrientation(VERTICAL);
            title = text(getContext(), t, 18, TEXT);
            title.setSingleLine(true);
            title.setEllipsize(TextUtils.TruncateAt.END);
            texts.addView(title);
            summary = text(getContext(), sum == null ? "" : sum, 14, TEXT_3);
            summary.setPadding(0, dp(getContext(), 2), 0, 0);
            summary.setMaxLines(4);
            summary.setEllipsize(TextUtils.TruncateAt.END);
            summary.setVisibility(sum == null || sum.isEmpty() ? GONE : VISIBLE);
            texts.addView(summary);
            addView(texts, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f));
            return this;
        }

        public void setSummary(String s) {
            if (summary == null) return;
            summary.setText(s);
            summary.setVisibility(s == null || s.isEmpty() ? GONE : VISIBLE);
        }

        public void addWidget(View v) {
            LayoutParams lp = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
            lp.leftMargin = dp(getContext(), 12);
            addView(v, lp);
        }

        @Override
        public void setEnabled(boolean on) {
            super.setEnabled(on);
            setAlpha(on ? 1f : 0.4f);
            for (int i = 0; i < getChildCount(); i++) getChildAt(i).setEnabled(on);
        }
    }

    /* ------------------------------------------------------------------- the page */

    /**
     * A preference screen: a scrolling column of categories, rows, notes and
     * seek bars, with the dividers put in between rows by itself.  Rebuilding
     * it keeps the scroll position, so picking a value does not jump.
     */
    public static class Page {
        private final Activity act;
        private final Holo holo;
        public final ScrollView scroll;
        public final LinearLayout list;
        private boolean afterRow;

        public Page(Activity act) {
            this.act = act;
            this.holo = new Holo(act);
            scroll = new ScrollView(act);
            scroll.setFillViewport(true);
            scroll.setClipToPadding(false);
            list = new LinearLayout(act);
            list.setOrientation(LinearLayout.VERTICAL);
            list.setPadding(0, 0, 0, dp(act, 24));
            scroll.addView(list, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));
        }

        /** Empties the page; {@code then} fills it again at the same scroll position. */
        public void rebuild(Runnable then) {
            final int y = scroll.getScrollY();
            list.removeAllViews();
            afterRow = false;
            then.run();
            scroll.post(new Runnable() {
                @Override
                public void run() {
                    scroll.scrollTo(0, y);
                }
            });
        }

        public void category(String title) {
            list.addView(Holo.category(act, title));
            afterRow = false;
        }

        public TextView note(String text) {
            TextView t = Holo.text(act, text, 13.5f, TEXT_3);
            t.setPadding(dp(act, 16), dp(act, 2), dp(act, 16), dp(act, 12));
            t.setLineSpacing(0, 1.2f);
            list.addView(t);
            afterRow = false;
            return t;
        }

        /** A strip across the top for something the whole page depends on. */
        public TextView banner(String text, int color) {
            TextView t = Holo.text(act, text, 14, TEXT);
            t.setPadding(dp(act, 16), dp(act, 12), dp(act, 16), dp(act, 12));
            t.setBackgroundColor(color);
            t.setLineSpacing(0, 1.15f);
            list.addView(t);
            afterRow = false;
            return t;
        }

        public void add(View v) {
            if (afterRow) list.addView(divider(act));
            list.addView(v);
            afterRow = true;
        }

        /** A plain view with no divider bookkeeping (a button, a log). */
        public void addLoose(View v, LinearLayout.LayoutParams lp) {
            if (lp == null) list.addView(v);
            else list.addView(v, lp);
            afterRow = false;
        }

        public Row row(String title, String summary, View.OnClickListener onClick) {
            Row row = new Row(act).setTitle(title, summary);
            if (onClick != null) row.setOnClickListener(onClick);
            else row.setClickable(false);
            add(row);
            return row;
        }

        /** A row that opens a single-choice dialog. */
        public Row choice(final String title, String summary, final String[] labels,
                          final String[] summaries, final int checked, final OnPick pick) {
            return row(title, summary, new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    holo.list(title, labels, summaries, checked, pick);
                }
            });
        }

        public Row switchRow(String title, String summary, boolean on, final OnToggle toggle) {
            Row row = new Row(act).setTitle(title, summary);
            final Switch sw = new Switch(act, on, toggle);
            row.addWidget(sw);
            /* the whole row flips it, as in ICS's Settings */
            row.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    sw.performClick();
                }
            });
            add(row);
            return row;
        }

        /** A text row that opens a field in a dialog. */
        public Row edit(final String title, String summary, final String value, final String hint,
                        final int inputType, final OnText done) {
            return row(title, summary, new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    holo.prompt(title, value, hint, inputType, done);
                }
            });
        }

        public SeekBar seek(String title, int min, int max, int value, final Format format,
                            final OnSeek onSeek) {
            LinearLayout box = new LinearLayout(act);
            box.setOrientation(LinearLayout.VERTICAL);
            box.setPadding(0, dp(act, 12), 0, dp(act, 6));

            LinearLayout head = new LinearLayout(act);
            head.setOrientation(LinearLayout.HORIZONTAL);
            head.setGravity(Gravity.CENTER_VERTICAL);
            head.setPadding(dp(act, 16), 0, dp(act, 16), 0);
            head.addView(Holo.text(act, title, 18, TEXT),
                    new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            final TextView val = Holo.text(act, format.format(value), 15, BLUE);
            head.addView(val);
            box.addView(head);

            SeekBar bar = holo.seek(min, max, value, new OnSeek() {
                @Override
                public void onSeek(int v) {
                    val.setText(format.format(v));
                }
            }, onSeek);
            box.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));
            add(box);
            return bar;
        }
    }
}
