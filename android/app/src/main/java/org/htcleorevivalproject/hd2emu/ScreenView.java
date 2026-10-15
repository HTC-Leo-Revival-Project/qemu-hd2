package org.htcleorevivalproject.hd2emu;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.util.AttributeSet;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import java.nio.ByteBuffer;

/**
 * The emulated phone's screen.  QEMU's VNC server hands over whole frames as
 * BGRX, which is byte for byte what an ARGB_8888 Bitmap holds, so a frame
 * costs one copy into the bitmap and one draw.
 *
 * Touches are mapped back through the letterboxing into the 0..0x7fff axes
 * the machine's touchscreen expects, the same numbers the web UI sends.
 */
public class ScreenView extends View {
    private static final String TAG = "HtcleoScreen";

    public interface Input {
        void onTouch(float nx, float ny, String action);

        void onKey(int keyCode, boolean down);
    }

    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Rect src = new Rect();
    private final RectF dst = new RectF();
    private final Handler main = new Handler(Looper.getMainLooper());

    private Input input;
    private Bitmap bitmap;
    private byte[] frame;
    private volatile int fw, fh;
    private volatile boolean haveFrame;
    private volatile boolean renderPending;
    private int activePointer = -1;
    private int frames;

    public ScreenView(Context context) {
        super(context);
        init();
    }

    public ScreenView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    public void setInput(Input input) {
        this.input = input;
    }

    /* ------------------------------------------------------------ the frames */

    /** Called from the VNC thread; hands the frame over without copying it. */
    public void offerFrame(byte[] fb, int w, int h) {
        if (fb.length < w * h * 4) return;
        this.frame = fb;
        this.fw = w;
        this.fh = h;
        this.haveFrame = true;
        scheduleRender();
    }

    private void scheduleRender() {
        if (renderPending) return;
        renderPending = true;
        main.post(new Runnable() {
            @Override
            public void run() {
                renderPending = false;
                invalidate();
            }
        });
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        byte[] fb = frame;
        int w = fw, h = fh;
        if (fb == null || !haveFrame || w <= 0 || h <= 0) return;
        if (bitmap == null || bitmap.getWidth() != w || bitmap.getHeight() != h) {
            if (bitmap != null) bitmap.recycle();
            bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        }
        bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(fb, 0, w * h * 4));
        fit(w, h);
        src.set(0, 0, w, h);
        canvas.drawColor(Color.BLACK);
        canvas.drawBitmap(bitmap, src, dst, paint);
        frames++;
    }

    /** Frames drawn since the last call: called once a second, it is the frame rate. */
    public int takeFrames() {
        int n = frames;
        frames = 0;
        return n;
    }

    /** The screen is 480x800; show all of it, centred, never stretched. */
    private void fit(int w, int h) {
        float vw = getWidth(), vh = getHeight();
        if (vw <= 0 || vh <= 0) {
            dst.set(0, 0, w, h);
            return;
        }
        float scale = Math.min(vw / w, vh / h);
        float dw = w * scale, dh = h * scale;
        float left = (vw - dw) / 2f, top = (vh - dh) / 2f;
        dst.set(left, top, left + dw, top + dh);
    }

    /** Where a point on the glass lands on the phone's screen, 0..1. */
    private boolean normalise(float px, float py, float[] out) {
        if (dst.width() <= 0 || dst.height() <= 0) fit(fw, fh);
        if (dst.width() <= 0) return false;
        out[0] = (px - dst.left) / dst.width();
        out[1] = (py - dst.top) / dst.height();
        return out[0] >= -0.05f && out[0] <= 1.05f && out[1] >= -0.05f && out[1] <= 1.05f;
    }

    /* ------------------------------------------------------------ the touches */

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (input == null) return false;
        float[] n = new float[2];
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int i = e.getActionIndex();
            if (activePointer < 0 && normalise(e.getX(i), e.getY(i), n)) {
                activePointer = e.getPointerId(i);
                input.onTouch(n[0], n[1], "down");
                return true;
            }
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < e.getPointerCount(); i++) {
                if (e.getPointerId(i) == activePointer) {
                    if (normalise(e.getX(i), e.getY(i), n)) input.onTouch(n[0], n[1], "move");
                    return true;
                }
            }
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            int i = e.getActionIndex();
            if (e.getPointerId(i) == activePointer) {
                activePointer = -1;
                if (normalise(e.getX(i), e.getY(i), n)) input.onTouch(n[0], n[1], "up");
                return true;
            }
            return true;
        }
        if (action == MotionEvent.ACTION_CANCEL) {
            activePointer = -1;
            input.onTouch(0, 0, "up");
            return true;
        }
        return super.onTouchEvent(e);
    }

    /**
     * The phone's own buttons, on this phone: the hardware keys are the real
     * keys, so the volume rocker, Back, Home, Menu and the call keys go
     * straight into the emulated key matrix.
     */
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        String hard = hardKey(keyCode);
        if (hard != null && input != null) {
            input.onKey(keyCode, true);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        String hard = hardKey(keyCode);
        if (hard != null && input != null) {
            input.onKey(keyCode, false);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    public static String hardKey(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_VOLUME_UP: return "volup";
            case KeyEvent.KEYCODE_VOLUME_DOWN: return "voldown";
            case KeyEvent.KEYCODE_BACK: return "back";
            case KeyEvent.KEYCODE_HOME: return "home";
            case KeyEvent.KEYCODE_MENU: return "menu";
            case KeyEvent.KEYCODE_CALL: return "call";
            case KeyEvent.KEYCODE_ENDCALL: return "end";
            default: return null;
        }
    }

    /* -------------------------------------------------------------- teardown */

    public void release() {
        frame = null;
        haveFrame = false;
        if (bitmap != null) {
            bitmap.recycle();
            bitmap = null;
        }
    }

    /** The last complete frame, for the screenshot button. */
    public synchronized Bitmap lastBitmap() {
        byte[] fb = frame;
        if (fb == null || fw <= 0 || fh <= 0) return null;
        if (bitmap != null && bitmap.getWidth() == fw && bitmap.getHeight() == fh) return bitmap;
        Bitmap b = Bitmap.createBitmap(fw, fh, Bitmap.Config.ARGB_8888);
        b.copyPixelsFromBuffer(ByteBuffer.wrap(fb, 0, fw * fh * 4));
        return b;
    }
}
