package com.hd2bench;

import android.app.Activity;
import android.graphics.Color;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.opengl.Matrix;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.nio.ShortBuffer;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * OpenGL ES 2.0 tests on a full-screen surface.  Each test draws a growing
 * batch of work per frame and waits for it (glFinish), so the rate is the
 * GPU's, not the display's; the last one measures frames per second with
 * the display's sync, as apps see it.
 */
public class GlBenchActivity extends Activity {
    private GLSurfaceView view;
    private TextView status;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN
                             | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON,
                             WindowManager.LayoutParams.FLAG_FULLSCREEN
                             | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout root = new FrameLayout(this);
        view = new GLSurfaceView(this);
        view.setEGLContextClientVersion(2);
        view.setRenderer(new Renderer());
        root.addView(view);
        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setBackgroundColor(0xb0101418);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        status.setPadding(pad, pad, pad, pad);
        status.setText("Starting OpenGL ES 2.0…");
        root.addView(status, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM));
        setContentView(root);
    }

    @Override
    protected void onPause() {
        super.onPause();
        view.onPause();
        /* a test interrupted half way would mean nothing */
        finish();
    }

    @Override
    protected void onResume() {
        super.onResume();
        view.onResume();
    }

    private void show(final String s) {
        runOnUiThread(new Runnable() {
            public void run() {
                status.setText(s);
            }
        });
    }

    private static final String[] NAMES = {
        "Fill rate: opaque textured quads",
        "Fill rate: blended textured quads",
        "Fragment shader: 8 sin/cos per pixel",
        "Geometry: lit 18K-triangle mesh",
        "Texture upload: 512x512 RGBA",
        "Scene at display refresh (vsync)",
    };
    private static final String[] UNITS = {
        "Mpixels/s", "Mpixels/s", "Mpixels/s", "Mtriangles/s", "MB/s", "fps",
    };
    private static final double TEST_SECS = 2.0;

    private static final String TEX_VS =
        "attribute vec2 a_pos;\n"
        + "uniform vec4 u_xf;\n"   /* scale xy, offset zw */
        + "varying vec2 v_uv;\n"
        + "void main() {\n"
        + "    gl_Position = vec4(a_pos * u_xf.xy + u_xf.zw, 0.0, 1.0);\n"
        + "    v_uv = a_pos + 1.0;\n"
        + "}\n";
    private static final String TEX_FS =
        "precision mediump float;\n"
        + "uniform sampler2D u_tex;\n"
        + "uniform float u_alpha;\n"
        + "varying vec2 v_uv;\n"
        + "void main() {\n"
        + "    gl_FragColor = texture2D(u_tex, v_uv) * vec4(1.0, 1.0, 1.0, "
        + "u_alpha);\n"
        + "}\n";
    private static final String ALU_FS =
        "precision mediump float;\n"
        + "varying vec2 v_uv;\n"
        + "void main() {\n"
        + "    vec2 p = v_uv * 3.0;\n"
        + "    float a = 0.1;\n"
        + "    a += sin(p.x * 1.3 + a) * cos(p.y * 0.7 - a);\n"
        + "    a += sin(p.y * 1.1 - a) * cos(p.x * 0.9 + a);\n"
        + "    a += sin(p.x * 0.5 + a) * cos(p.y * 1.7 - a);\n"
        + "    a += sin(p.y * 2.1 - a) * cos(p.x * 0.3 + a);\n"
        + "    gl_FragColor = vec4(0.5 + 0.5 * a, fract(a * 3.0), 0.6, 1.0);\n"
        + "}\n";
    private static final String MESH_VS =
        "attribute vec3 a_pos;\n"
        + "uniform mat4 u_mvp;\n"
        + "uniform mat4 u_rot;\n"
        + "varying float v_l;\n"
        + "void main() {\n"
        + "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
        + "    vec3 n = normalize((u_rot * vec4(a_pos, 0.0)).xyz);\n"
        + "    v_l = 0.2 + 0.8 * max(dot(n, vec3(0.48, 0.6, 0.64)), 0.0);\n"
        + "}\n";
    private static final String MESH_FS =
        "precision mediump float;\n"
        + "varying float v_l;\n"
        + "void main() {\n"
        + "    gl_FragColor = vec4(v_l * 0.95, v_l * 0.62, v_l * 0.25, 1.0);\n"
        + "}\n";

    private final class Renderer implements GLSurfaceView.Renderer {
        private int w, h;
        private int texProg, aluProg, meshProg;
        private int tex, upTex;
        private int quadVbo, meshVbo, meshIbo, meshIdx;
        private ByteBuffer upData;
        private int test, frame, batch = 1;
        private double work, time;
        private long sceneStart;
        private int sceneFrames;
        private boolean done;
        private final float[] mvp = new float[16], rot = new float[16];

        private int shader(int type, String src) {
            int s = GLES20.glCreateShader(type);

            GLES20.glShaderSource(s, src);
            GLES20.glCompileShader(s);
            return s;
        }

        private int program(String vs, String fs) {
            int p = GLES20.glCreateProgram();

            GLES20.glAttachShader(p, shader(GLES20.GL_VERTEX_SHADER, vs));
            GLES20.glAttachShader(p, shader(GLES20.GL_FRAGMENT_SHADER, fs));
            GLES20.glBindAttribLocation(p, 0, "a_pos");
            GLES20.glLinkProgram(p);
            return p;
        }

        private int buffer(int target, java.nio.Buffer data, int bytes) {
            int[] b = new int[1];

            GLES20.glGenBuffers(1, b, 0);
            GLES20.glBindBuffer(target, b[0]);
            GLES20.glBufferData(target, bytes, data, GLES20.GL_STATIC_DRAW);
            return b[0];
        }

        @Override
        public void onSurfaceCreated(GL10 unused, EGLConfig config) {
            Results.clear(Results.GPU);
            Results.putText(Results.GPU, "Renderer",
                            GLES20.glGetString(GLES20.GL_RENDERER), "", true);
            Results.putText(Results.GPU, "Version",
                            GLES20.glGetString(GLES20.GL_VERSION), "", true);
            texProg = program(TEX_VS, TEX_FS);
            aluProg = program(TEX_VS, ALU_FS);
            meshProg = program(MESH_VS, MESH_FS);

            /* a 256x256 mipmapped texture */
            ByteBuffer img = ByteBuffer.allocateDirect(256 * 256 * 4);
            for (int y = 0; y < 256; y++) {
                for (int x = 0; x < 256; x++) {
                    boolean on = ((x >> 5) ^ (y >> 5)) == 0
                                 || (((x >> 5) ^ (y >> 5)) & 1) == 0;
                    img.put((byte) (on ? 230 : x));
                    img.put((byte) (on ? 160 : y));
                    img.put((byte) (on ? 60 : 200));
                    img.put((byte) 160);
                }
            }
            img.position(0);
            int[] t = new int[2];
            GLES20.glGenTextures(2, t, 0);
            tex = t[0];
            upTex = t[1];
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex);
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, 256,
                                256, 0, GLES20.GL_RGBA,
                                GLES20.GL_UNSIGNED_BYTE, img);
            GLES20.glGenerateMipmap(GLES20.GL_TEXTURE_2D);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,
                                   GLES20.GL_TEXTURE_MIN_FILTER,
                                   GLES20.GL_LINEAR_MIPMAP_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,
                                   GLES20.GL_TEXTURE_MAG_FILTER,
                                   GLES20.GL_LINEAR);
            upData = ByteBuffer.allocateDirect(512 * 512 * 4);
            for (int i = 0; i < 512 * 512 * 4; i++) {
                upData.put((byte) (i * 31 + (i >> 11)));
            }
            upData.position(0);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, upTex);
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, 512,
                                512, 0, GLES20.GL_RGBA,
                                GLES20.GL_UNSIGNED_BYTE, upData);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,
                                   GLES20.GL_TEXTURE_MIN_FILTER,
                                   GLES20.GL_LINEAR);

            float[] quad = { -1, -1, 1, -1, -1, 1, 1, 1 };
            FloatBuffer q = ByteBuffer.allocateDirect(quad.length * 4)
                    .order(ByteOrder.nativeOrder()).asFloatBuffer();
            q.put(quad).position(0);
            quadVbo = buffer(GLES20.GL_ARRAY_BUFFER, q, quad.length * 4);

            /* a sphere of 96 x 96 quads */
            int g = 96, nv = (g + 1) * (g + 1);
            FloatBuffer v = ByteBuffer.allocateDirect(nv * 12)
                    .order(ByteOrder.nativeOrder()).asFloatBuffer();
            for (int j = 0; j <= g; j++) {
                for (int i = 0; i <= g; i++) {
                    double th = Math.PI * j / g, ph = 2 * Math.PI * i / g;
                    double r = 1 + 0.05 * Math.sin(8 * ph) * Math.sin(6 * th);

                    v.put((float) (r * Math.sin(th) * Math.cos(ph)));
                    v.put((float) (r * Math.cos(th)));
                    v.put((float) (r * Math.sin(th) * Math.sin(ph)));
                }
            }
            v.position(0);
            meshIdx = g * g * 6;
            ShortBuffer ix = ByteBuffer.allocateDirect(meshIdx * 2)
                    .order(ByteOrder.nativeOrder()).asShortBuffer();
            for (int j = 0; j < g; j++) {
                for (int i = 0; i < g; i++) {
                    int a = j * (g + 1) + i, bb = a + 1, c = a + g + 1;
                    int d = c + 1;

                    ix.put((short) a).put((short) bb).put((short) c);
                    ix.put((short) bb).put((short) d).put((short) c);
                }
            }
            ix.position(0);
            meshVbo = buffer(GLES20.GL_ARRAY_BUFFER, v, nv * 12);
            meshIbo = buffer(GLES20.GL_ELEMENT_ARRAY_BUFFER, ix, meshIdx * 2);
            show("Test 1 of " + NAMES.length + ": " + NAMES[0]);
        }

        @Override
        public void onSurfaceChanged(GL10 unused, int width, int height) {
            w = width;
            h = height;
            GLES20.glViewport(0, 0, w, h);
            Results.putText(Results.GPU, "Surface", w + " x " + h, "", true);
        }

        private void quad(int prog, float sx, float sy, float ox, float oy) {
            GLES20.glUseProgram(prog);
            GLES20.glUniform4f(GLES20.glGetUniformLocation(prog, "u_xf"),
                               sx, sy, ox, oy);
            GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, quadVbo);
            GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 8, 0);
            GLES20.glEnableVertexAttribArray(0);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        }

        private void textured(float alpha, float sx, float sy, float ox,
                              float oy, int t) {
            GLES20.glUseProgram(texProg);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, t);
            GLES20.glUniform1f(GLES20.glGetUniformLocation(texProg, "u_alpha"),
                               alpha);
            quad(texProg, sx, sy, ox, oy);
        }

        private void mesh(float scale, float angle) {
            float aspect = (float) w / h;

            Matrix.setRotateM(rot, 0, angle, 0.3f, 1, 0.2f);
            Matrix.setIdentityM(mvp, 0);
            Matrix.scaleM(mvp, 0, scale / aspect, scale, scale * 0.5f);
            Matrix.multiplyMM(mvp, 0, mvp.clone(), 0, rot, 0);
            GLES20.glUseProgram(meshProg);
            GLES20.glUniformMatrix4fv(
                    GLES20.glGetUniformLocation(meshProg, "u_mvp"), 1, false,
                    mvp, 0);
            GLES20.glUniformMatrix4fv(
                    GLES20.glGetUniformLocation(meshProg, "u_rot"), 1, false,
                    rot, 0);
            GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, meshVbo);
            GLES20.glVertexAttribPointer(0, 3, GLES20.GL_FLOAT, false, 12, 0);
            GLES20.glEnableVertexAttribArray(0);
            GLES20.glBindBuffer(GLES20.GL_ELEMENT_ARRAY_BUFFER, meshIbo);
            GLES20.glDrawElements(GLES20.GL_TRIANGLES, meshIdx,
                                  GLES20.GL_UNSIGNED_SHORT, 0);
        }

        /* one unit of a test's work; returns its size in the test's unit */
        private double unit(int k) {
            switch (test) {
            case 0:
                textured(1, 1, 1, 0, 0, tex);
                return w * h / 1e6;
            case 1:
                textured(0.5f, 1, 1, 0, 0, tex);
                return w * h / 1e6;
            case 2:
                quad(aluProg, 1, 1, 0, 0);
                return w * h / 1e6;
            case 3:
                mesh(0.25f, frame * 3 + k * 7);
                return meshIdx / 3 / 1e6;
            default:
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, upTex);
                upData.position(0);
                GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, 512,
                                       512, GLES20.GL_RGBA,
                                       GLES20.GL_UNSIGNED_BYTE, upData);
                /* use it, so that the upload really happens */
                textured(1, 4f / w, 4f / h, -0.9f, -0.9f, upTex);
                return 512 * 512 * 4 / 1e6;
            }
        }

        private void next(double value) {
            Results.put(Results.GPU, NAMES[test], value, UNITS[test], true);
            test++;
            frame = 0;
            batch = 1;
            work = 0;
            time = 0;
            if (test < NAMES.length) {
                show("Test " + (test + 1) + " of " + NAMES.length + ": "
                     + NAMES[test]);
            }
        }

        @Override
        public void onDrawFrame(GL10 unused) {
            if (test >= NAMES.length) {
                if (!done) {
                    done = true;
                    runOnUiThread(new Runnable() {
                        public void run() {
                            setResult(RESULT_OK);
                            finish();
                        }
                    });
                }
                return;
            }
            GLES20.glClearColor(0.06f, 0.08f, 0.1f, 1);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT
                           | GLES20.GL_DEPTH_BUFFER_BIT);
            boolean blend = test == 1;
            if (blend) {
                GLES20.glEnable(GLES20.GL_BLEND);
                GLES20.glBlendFunc(GLES20.GL_SRC_ALPHA,
                                   GLES20.GL_ONE_MINUS_SRC_ALPHA);
            } else {
                GLES20.glDisable(GLES20.GL_BLEND);
            }
            if (test == 3 || test == 5) {
                GLES20.glEnable(GLES20.GL_DEPTH_TEST);
                GLES20.glEnable(GLES20.GL_CULL_FACE);
            } else {
                GLES20.glDisable(GLES20.GL_DEPTH_TEST);
                GLES20.glDisable(GLES20.GL_CULL_FACE);
            }

            if (test == 5) {
                /* paced by the display, like an app */
                long now = System.nanoTime();

                GLES20.glDisable(GLES20.GL_DEPTH_TEST);
                textured(1, 1, 1, 0, 0, tex);
                GLES20.glEnable(GLES20.GL_DEPTH_TEST);
                mesh(0.7f, frame * 2);
                frame++;
                if (frame == 10) {
                    sceneStart = now;
                    sceneFrames = 0;
                } else if (frame > 10) {
                    sceneFrames++;
                    if (now - sceneStart > 3e9) {
                        next(sceneFrames / ((now - sceneStart) / 1e9));
                    }
                }
                return;
            }

            long t0 = System.nanoTime();
            double wk = 0;
            for (int k = 0; k < batch; k++) {
                wk += unit(k);
            }
            GLES20.glFinish();
            double dt = (System.nanoTime() - t0) / 1e9;

            /* the first frames compile shaders and upload: not counted */
            if (++frame > 3) {
                work += wk;
                time += dt;
            }
            if (dt < 0.02 && batch < 4096) {
                batch *= 2;
            }
            if (time >= TEST_SECS) {
                next(work / time);
            }
        }
    }
}
