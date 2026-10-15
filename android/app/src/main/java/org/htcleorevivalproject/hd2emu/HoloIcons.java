package org.htcleorevivalproject.hd2emu;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;

/**
 * The few glyphs the app needs - the action bar's, the phone's hardware keys
 * and the big ones on the empty screen - drawn with a Path on a 24x24 grid.
 * minSdk 15 has no vector drawables, and a set of PNGs per density for a
 * dozen glyphs is more weight than this class.
 *
 * Holo icons are flat, white and a little see-through on a dark theme; the
 * HD2's call and end keys keep their green and red.
 */
public final class HoloIcons {
    public static final int PLAY = 0;
    public static final int STOP = 1;
    public static final int CAMERA = 2;
    public static final int CALL = 3;
    public static final int END = 4;
    public static final int HOME = 5;
    public static final int WINDOWS = 6;
    public static final int BACK = 7;
    public static final int POWER = 8;
    public static final int VOL_UP = 9;
    public static final int VOL_DOWN = 10;
    public static final int WARNING = 11;
    public static final int CHIP = 12;
    public static final int CHECK = 13;
    public static final int CROSS = 14;
    public static final int USB = 15;

    /** The Holo dark theme's icon colour: white at 80%. */
    public static final int WHITE = 0xccffffff;

    private HoloIcons() {
    }

    public static Drawable get(Context c, int glyph, int sizeDp) {
        return get(c, glyph, sizeDp, WHITE);
    }

    public static Drawable get(Context c, int glyph, int sizeDp, int color) {
        float density = c.getResources().getDisplayMetrics().density;
        return new Glyph(glyph, color, Math.round(sizeDp * density), density);
    }

    /* --------------------------------------------------------------- the paths */

    private static final String HANDSET =
            "M6.62,10.79c1.44,2.83 3.76,5.14 6.59,6.59l2.2,-2.2c0.27,-0.27 0.67,-0.36 1.02,-0.24"
            + " 1.12,0.37 2.33,0.57 3.57,0.57 0.55,0 1,0.45 1,1V20c0,0.55 -0.45,1 -1,1"
            + " -9.39,0 -17,-7.61 -17,-17 0,-0.55 0.45,-1 1,-1h3.5c0.55,0 1,0.45 1,1"
            + " 0,1.25 0.2,2.45 0.57,3.57 0.11,0.35 0.03,0.74 -0.25,1.02l-2.2,2.2z";

    private static final String HANG_UP =
            "M12,9c-1.6,0 -3.15,0.25 -4.6,0.72v3.1c0,0.39 -0.23,0.74 -0.56,0.9"
            + " -0.98,0.49 -1.87,1.12 -2.66,1.85 -0.18,0.18 -0.43,0.28 -0.7,0.28"
            + " -0.28,0 -0.53,-0.11 -0.71,-0.29L0.29,13.08c-0.18,-0.17 -0.29,-0.42 -0.29,-0.7"
            + " 0,-0.28 0.11,-0.53 0.29,-0.71C3.34,8.78 7.46,7 12,7s8.66,1.78 11.71,4.67"
            + "c0.18,0.18 0.29,0.43 0.29,0.71 0,0.28 -0.11,0.53 -0.29,0.71l-2.48,2.48"
            + "c-0.18,0.18 -0.43,0.29 -0.71,0.29 -0.27,0 -0.52,-0.11 -0.7,-0.28"
            + " -0.79,-0.74 -1.69,-1.36 -2.67,-1.85 -0.33,-0.16 -0.56,-0.5 -0.56,-0.9v-3.1"
            + "C15.15,9.25 13.6,9 12,9z";

    private static final String SPEAKER = "M3,9v6h4l5,5V4L7,9H3z";

    /** One glyph, at one size, in one colour. */
    static final class Glyph extends Drawable {
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        private final Path lines = new Path();
        private final Matrix m = new Matrix();
        private final int glyph;
        private final int size;
        private final float density;
        private float unit = 1f;

        Glyph(int glyph, int color, int size, float density) {
            this.glyph = glyph;
            this.size = size;
            this.density = density;
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(color);
            stroke.setStyle(Paint.Style.STROKE);
            stroke.setColor(color);
            stroke.setStrokeCap(Paint.Cap.SQUARE);
            stroke.setStrokeJoin(Paint.Join.MITER);
        }

        @Override
        public int getIntrinsicWidth() {
            return size;
        }

        @Override
        public int getIntrinsicHeight() {
            return size;
        }

        @Override
        protected void onBoundsChange(Rect b) {
            int s = Math.min(b.width(), b.height());
            unit = s / 24f;
            /* scaled with the glyph, but a big one keeps a fine line */
            stroke.setStrokeWidth(Math.max(1f, Math.min(1.8f * unit, 3f * density)));
            path.reset();
            lines.reset();
            build(glyph, path, lines);
            m.setScale(unit, unit);
            m.postTranslate(b.left + (b.width() - s) / 2f, b.top + (b.height() - s) / 2f);
            path.transform(m);
            lines.transform(m);
        }

        /** {@code solid} is filled, {@code outline} is stroked. */
        private static void build(int glyph, Path solid, Path outline) {
            switch (glyph) {
                case PLAY:
                    solid.moveTo(8, 5);
                    solid.lineTo(19, 12);
                    solid.lineTo(8, 19);
                    solid.close();
                    break;
                case STOP:
                    solid.addRect(6.5f, 6.5f, 17.5f, 17.5f, Path.Direction.CW);
                    break;
                case CAMERA:
                    outline.addRect(3, 7.5f, 21, 19.5f, Path.Direction.CW);
                    outline.moveTo(8.5f, 7.5f);
                    outline.lineTo(9.8f, 4.8f);
                    outline.lineTo(14.2f, 4.8f);
                    outline.lineTo(15.5f, 7.5f);
                    outline.addCircle(12, 13.5f, 3.4f, Path.Direction.CW);
                    break;
                case CALL:
                    Svg.parse(HANDSET, solid);
                    break;
                case END:
                    Svg.parse(HANG_UP, solid);
                    break;
                case HOME:
                    outline.moveTo(3.5f, 11.5f);
                    outline.lineTo(12, 4);
                    outline.lineTo(20.5f, 11.5f);
                    outline.moveTo(6, 10);
                    outline.lineTo(6, 20);
                    outline.lineTo(18, 20);
                    outline.lineTo(18, 10);
                    outline.moveTo(10.5f, 20);
                    outline.lineTo(10.5f, 15);
                    outline.lineTo(13.5f, 15);
                    outline.lineTo(13.5f, 20);
                    break;
                case WINDOWS:
                    /* the WinMo 6.5 flag: four panes, a little taller on the right */
                    solid.moveTo(3.5f, 5.6f);
                    solid.lineTo(10.6f, 4.6f);
                    solid.lineTo(10.6f, 11.3f);
                    solid.lineTo(3.5f, 11.3f);
                    solid.close();
                    solid.moveTo(12, 4.4f);
                    solid.lineTo(20.5f, 3.2f);
                    solid.lineTo(20.5f, 11.3f);
                    solid.lineTo(12, 11.3f);
                    solid.close();
                    solid.moveTo(3.5f, 12.7f);
                    solid.lineTo(10.6f, 12.7f);
                    solid.lineTo(10.6f, 19.4f);
                    solid.lineTo(3.5f, 18.4f);
                    solid.close();
                    solid.moveTo(12, 12.7f);
                    solid.lineTo(20.5f, 12.7f);
                    solid.lineTo(20.5f, 20.8f);
                    solid.lineTo(12, 19.6f);
                    solid.close();
                    break;
                case BACK:
                    /* the ICS back key: an arrow whose tail turns back on itself */
                    outline.moveTo(4.5f, 9.5f);
                    outline.lineTo(14.5f, 9.5f);
                    outline.cubicTo(18, 9.5f, 20, 11.6f, 20, 14.2f);
                    outline.cubicTo(20, 16.9f, 18, 19, 14.5f, 19);
                    outline.lineTo(9, 19);
                    outline.moveTo(8.5f, 5.5f);
                    outline.lineTo(4.5f, 9.5f);
                    outline.lineTo(8.5f, 13.5f);
                    break;
                case POWER:
                    outline.addArc(new RectF(5, 5.5f, 19, 19.5f), -60, 300);
                    outline.moveTo(12, 3);
                    outline.lineTo(12, 11.5f);
                    break;
                case VOL_UP:
                    Svg.parse(SPEAKER, solid);
                    outline.moveTo(15, 12);
                    outline.lineTo(21, 12);
                    outline.moveTo(18, 9);
                    outline.lineTo(18, 15);
                    break;
                case VOL_DOWN:
                    Svg.parse(SPEAKER, solid);
                    outline.moveTo(15, 12);
                    outline.lineTo(21, 12);
                    break;
                case WARNING:
                    outline.moveTo(12, 3.5f);
                    outline.lineTo(21.5f, 20);
                    outline.lineTo(2.5f, 20);
                    outline.close();
                    outline.moveTo(12, 9.5f);
                    outline.lineTo(12, 14);
                    solid.addCircle(12, 17, 1.2f, Path.Direction.CW);
                    break;
                case CHIP:
                    outline.addRect(6.5f, 6.5f, 17.5f, 17.5f, Path.Direction.CW);
                    solid.addRect(9.5f, 9.5f, 14.5f, 14.5f, Path.Direction.CW);
                    for (int i = 0; i < 3; i++) {
                        float p = 9.5f + i * 2.5f;
                        outline.moveTo(p, 3.5f);
                        outline.lineTo(p, 6.5f);
                        outline.moveTo(p, 17.5f);
                        outline.lineTo(p, 20.5f);
                        outline.moveTo(3.5f, p);
                        outline.lineTo(6.5f, p);
                        outline.moveTo(17.5f, p);
                        outline.lineTo(20.5f, p);
                    }
                    break;
                case CHECK:
                    outline.moveTo(4.5f, 12.5f);
                    outline.lineTo(9.5f, 17.5f);
                    outline.lineTo(19.5f, 6.5f);
                    break;
                case CROSS:
                    outline.moveTo(6, 6);
                    outline.lineTo(18, 18);
                    outline.moveTo(18, 6);
                    outline.lineTo(6, 18);
                    break;
                case USB:
                    /* the trident: a stem, a circle at the foot, a square and a dot on the arms */
                    outline.moveTo(12, 3.5f);
                    outline.lineTo(12, 18);
                    outline.moveTo(9.5f, 6);
                    outline.lineTo(12, 3.5f);
                    outline.lineTo(14.5f, 6);
                    outline.moveTo(12, 14);
                    outline.lineTo(7, 11);
                    outline.lineTo(7, 8.5f);
                    outline.moveTo(12, 12);
                    outline.lineTo(17, 9.5f);
                    outline.lineTo(17, 7.5f);
                    solid.addCircle(12, 19.2f, 1.9f, Path.Direction.CW);
                    solid.addCircle(7, 7.6f, 1.4f, Path.Direction.CW);
                    solid.addRect(15.8f, 5.3f, 18.2f, 7.7f, Path.Direction.CW);
                    break;
                default:
                    break;
            }
        }

        @Override
        public void draw(Canvas canvas) {
            if (!path.isEmpty()) canvas.drawPath(path, fill);
            if (!lines.isEmpty()) canvas.drawPath(lines, stroke);
        }

        @Override
        public void setAlpha(int alpha) {
            fill.setAlpha(alpha);
            stroke.setAlpha(alpha);
            invalidateSelf();
        }

        @Override
        public void setColorFilter(ColorFilter cf) {
            fill.setColorFilter(cf);
            stroke.setColorFilter(cf);
            invalidateSelf();
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }

    /**
     * Just enough of SVG's path syntax for the glyphs above: M L H V C S Z, upper
     * and lower case, with repeated arguments.  No arcs.
     */
    static final class Svg {
        private final String s;
        private int i;

        private Svg(String s) {
            this.s = s;
        }

        static void parse(String d, Path out) {
            new Svg(d).run(out);
        }

        private void run(Path p) {
            float x = 0, y = 0, sx = 0, sy = 0, cx = 0, cy = 0;
            char cmd = 'M';
            while (true) {
                skip();
                if (i >= s.length()) break;
                char c = s.charAt(i);
                if (Character.isLetter(c)) {
                    cmd = c;
                    i++;
                } else if (cmd == 'M') {
                    cmd = 'L';                      /* extra pairs after M are lines */
                } else if (cmd == 'm') {
                    cmd = 'l';
                }
                boolean rel = Character.isLowerCase(cmd);
                float ox = rel ? x : 0, oy = rel ? y : 0;
                switch (Character.toUpperCase(cmd)) {
                    case 'M':
                        x = ox + num();
                        y = oy + num();
                        p.moveTo(x, y);
                        sx = cx = x;
                        sy = cy = y;
                        break;
                    case 'L':
                        x = ox + num();
                        y = oy + num();
                        p.lineTo(x, y);
                        cx = x;
                        cy = y;
                        break;
                    case 'H':
                        x = ox + num();
                        p.lineTo(x, y);
                        cx = x;
                        cy = y;
                        break;
                    case 'V':
                        y = oy + num();
                        p.lineTo(x, y);
                        cx = x;
                        cy = y;
                        break;
                    case 'C': {
                        float x1 = ox + num(), y1 = oy + num();
                        float x2 = ox + num(), y2 = oy + num();
                        x = ox + num();
                        y = oy + num();
                        p.cubicTo(x1, y1, x2, y2, x, y);
                        cx = x2;
                        cy = y2;
                        break;
                    }
                    case 'S': {
                        float x1 = 2 * x - cx, y1 = 2 * y - cy;
                        float x2 = ox + num(), y2 = oy + num();
                        x = ox + num();
                        y = oy + num();
                        p.cubicTo(x1, y1, x2, y2, x, y);
                        cx = x2;
                        cy = y2;
                        break;
                    }
                    case 'Z':
                        p.close();
                        x = cx = sx;
                        y = cy = sy;
                        break;
                    default:
                        return;                     /* not a path this class knows */
                }
            }
        }

        private void skip() {
            while (i < s.length() && (s.charAt(i) == ' ' || s.charAt(i) == ',')) i++;
        }

        private float num() {
            skip();
            int start = i;
            if (i < s.length() && (s.charAt(i) == '-' || s.charAt(i) == '+')) i++;
            boolean dot = false;
            while (i < s.length()) {
                char c = s.charAt(i);
                if (c >= '0' && c <= '9') {
                    i++;
                } else if (c == '.' && !dot) {
                    dot = true;
                    i++;
                } else {
                    break;
                }
            }
            return Float.parseFloat(s.substring(start, i));
        }
    }
}
