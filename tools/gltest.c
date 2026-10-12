/*
 * gltest: OpenGL ES 2.0 feature checks on the framebuffer through the
 * ROM's EGL (the Adreno 200 driver).  Build and run like gltri.c:
 *   gltest mip | comp | pts | cube  [seconds]
 *   mip   mip levels of distinct colours: a 64x64 quad of a 256x256
 *         texture must sample level 2, and a receding floor shows bands
 *   comp  ETC1, ATC (RGB, interpolated alpha) and DXT1 blocks of known
 *         colours (whatever the driver lists in GL_EXTENSIONS)
 *   pts   40-pixel points coloured by gl_PointCoord, and lines
 *   cube  a spinning depth-tested cube (frame time is reported)
 *   pb565 the triangle of gltri on a 480x800 RGB565 pbuffer
 * Each scene prints read-back pixels and their expected values.
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define GL_ETC1_RGB8_OES                    0x8D64
#define GL_ATC_RGB_AMD                      0x8C92
#define GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD  0x87EE
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT     0x83F0

static int W, H;
static EGLDisplay dpy;
static EGLSurface surf;

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];

        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        exit(1);
    }
    return s;
}

static GLuint program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();

    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glBindAttribLocation(p, 1, "attr");
    glLinkProgram(p);
    glUseProgram(p);
    return p;
}

static void probe(const char *what, int x, int y, const char *expect)
{
    GLubyte px[4];

    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("%-22s (%3d,%3d): %3d %3d %3d %3d   expect %s\n", what, x, y,
           px[0], px[1], px[2], px[3], expect);
}

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static const char tex_vs[] =
    "attribute vec4 pos; attribute vec4 attr; uniform mat4 m;\n"
    "varying vec2 uv;\n"
    "void main() { gl_Position = m * pos; uv = attr.xy; }\n";
static const char tex_fs[] =
    "precision mediump float; uniform sampler2D t; varying vec2 uv;\n"
    "void main() { gl_FragColor = texture2D(t, uv); }\n";

static void identity(GLfloat *m)
{
    memset(m, 0, 16 * sizeof(GLfloat));
    m[0] = m[5] = m[10] = m[15] = 1;
}

/* a quad of w x h pixels centred at (cx, cy), uv 0..r */
static void quad_px(GLuint prog, float cx, float cy, float w, float h,
                    float r)
{
    GLfloat x0 = (cx - w / 2) / W * 2 - 1, x1 = (cx + w / 2) / W * 2 - 1;
    GLfloat y0 = (cy - h / 2) / H * 2 - 1, y1 = (cy + h / 2) / H * 2 - 1;
    GLfloat v[] = {
        x0, y0, 0, 1, 0, 0,  x1, y0, 0, 1, r, 0,
        x0, y1, 0, 1, 0, r,  x1, y1, 0, 1, r, r,
    };
    GLfloat m[16];

    identity(m);
    glUniformMatrix4fv(glGetUniformLocation(prog, "m"), 1, GL_FALSE, m);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 24, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 24, v + 4);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void scene_mip(void)
{
    static const GLubyte colors[9][4] = {
        { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 },
        { 255, 255, 0, 255 }, { 255, 0, 255, 255 }, { 0, 255, 255, 255 },
        { 255, 255, 255, 255 }, { 128, 128, 128, 255 }, { 0, 0, 0, 255 },
    };
    GLuint prog = program(tex_vs, tex_fs), tex;
    GLubyte *img = malloc(256 * 256 * 4);
    /* a floor from the bottom of the screen to the horizon */
    GLfloat floor[] = {
        -1, -1, 0, 1, 0, 0,     1, -1, 0, 1, 8, 0,
        -0.05f, 0.2f, 0, 0.05f, 0, 0.4f,  0.05f, 0.2f, 0, 0.05f, 0.4f, 0.4f,
    };
    GLfloat m[16];

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    for (int l = 0, s = 256; s >= 1; l++, s /= 2) {
        for (int i = 0; i < s * s; i++) {
            memcpy(img + 4 * i, colors[l], 4);
        }
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, s, s, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, img);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    /* the floor: uv grows towards the horizon; w shrinks */
    for (int i = 0; i < 4; i++) {
        floor[i * 6 + 0] *= floor[i * 6 + 3];
        floor[i * 6 + 1] *= floor[i * 6 + 3];
        floor[i * 6 + 5] = i < 2 ? 0 : 8;
        floor[i * 6 + 4] = (i & 1) ? 8 : 0;
    }
    identity(m);
    glUniformMatrix4fv(glGetUniformLocation(prog, "m"), 1, GL_FALSE, m);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 24, floor);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 24, floor + 4);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* exact levels: 256 texels over 256, 64 and 16 pixels */
    quad_px(prog, 80, H - 150, 256 / 2, 256 / 2, 0.5f);
    quad_px(prog, 240, H - 150, 64, 64, 1);
    quad_px(prog, 380, H - 150, 16, 16, 1);
    probe("mip floor near", W / 2, 20, "red/green (levels 0-1)");
    probe("mip floor far", W / 2, H / 2 + 60, "later levels");
    probe("mip 128px of 128tx", 80, H - 150, "255 0 0 (level 0)");
    probe("mip 64px of 256tx", 240, H - 150, "0 0 255 (level 2)");
    probe("mip 16px of 256tx", 380, H - 150, "255 0 255 (level 4)");
    free(img);
}

static void scene_comp(void)
{
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    GLuint prog = program(tex_vs, tex_fs), tex[4];
    /* one 4x4 block each */
    /* ETC1 individual mode, both halves base (0,15,0)*17, table 0, idx 0 */
    static const GLubyte etc1[8] = { 0x00, 0xff, 0x00, 0x00,
                                     0, 0, 0, 0 };
    /* ATC: colour0 RGB555 blue (mode 0), colour1 565 blue, indices 0 */
    static const GLubyte atc[8] = { 0x1f, 0x00, 0x1f, 0x00, 0, 0, 0, 0 };
    /* ATC interpolated alpha: alpha 128 (index 0), colour0 555 yellow */
    static const GLubyte atci[16] = { 128, 128, 0, 0, 0, 0, 0, 0,
                                      0xe0, 0x7f, 0xe0, 0xff, 0, 0, 0, 0 };
    /* DXT1: colour0 565 red > colour1 0, indices 0 */
    static const GLubyte dxt1[8] = { 0x00, 0xf8, 0x00, 0x00, 0, 0, 0, 0 };

    printf("GL_EXTENSIONS %s\n", ext);
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glGenTextures(4, tex);
    for (int i = 0; i < 4; i++) {
        static const GLenum fmt[4] = {
            GL_ETC1_RGB8_OES, GL_ATC_RGB_AMD,
            GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD,
            GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
        };
        static const char *const name[4] = {
            "ETC1", "ATC RGB", "ATC interp alpha", "DXT1",
        };
        static const char *const expect[4] = {
            "0 255 0 (green)", "0 0 255 (blue)", "~140 140 25 (yellow, a .5)",
            "255 0 0 (red)",
        };
        const GLubyte *data[4] = { etc1, atc, atci, dxt1 };
        GLsizei size[4] = { 8, 8, 16, 8 };
        float cx = 60 + i * 120;

        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt[i], 4, 4, 0, size[i],
                               data[i]);
        if (glGetError()) {
            printf("%-22s not supported\n", name[i]);
            continue;
        }
        quad_px(prog, cx, H / 2, 100, 100, 1);
        probe(name[i], cx, H / 2, expect[i]);
    }
}

static void scene_pts(void)
{
    static const char vs[] =
        "attribute vec4 pos; void main() {\n"
        "  gl_Position = pos; gl_PointSize = 40.0; }\n";
    static const char fs[] =
        "precision mediump float; void main() {\n"
        "  gl_FragColor = vec4(gl_PointCoord, 0.0, 1.0); }\n";
    static const char lvs[] =
        "attribute vec4 pos; void main() { gl_Position = pos; }\n";
    static const char lfs[] =
        "precision mediump float; void main() {\n"
        "  gl_FragColor = vec4(1.0); }\n";
    GLfloat pts[] = { -0.5f, 0.5f, 0.5f, 0.5f, 0, 0 };
    GLfloat lines[] = { -0.9f, -0.5f, 0.9f, -0.5f, 0, -0.9f, 0, -0.1f };

    glClearColor(0.1f, 0.1f, 0.3f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    program(vs, fs);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pts);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_POINTS, 0, 3);
    program(lvs, lfs);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, lines);
    glDrawArrays(GL_LINES, 0, 4);
    probe("point centre", W / 2, H / 2, "~128 128 0");
    probe("point left/upper", W / 2 - 15, H / 2 + 15, "~32 32 0 (pc .1 .1)");
    probe("point right", W / 2 + 15, H / 2, "~224 128 0");
    probe("outside point", W / 2 + 30, H / 2, "26 26 76 (clear)");
    probe("horizontal line", W / 2 + 100, H / 4, "255 255 255");
    probe("vertical line", W / 2, H / 4 + 60, "255 255 255");
}

static void scene_cube(int frames)
{
    static const char vs[] =
        "attribute vec4 pos; attribute vec4 attr; uniform mat4 m;\n"
        "varying vec4 c;\n"
        "void main() { gl_Position = m * pos; c = attr; }\n";
    static const char fs[] =
        "precision mediump float; varying vec4 c;\n"
        "void main() { gl_FragColor = c; }\n";
    static const GLfloat face_col[6][3] = {
        { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
        { 1, 1, 0 }, { 1, 0, 1 }, { 0, 1, 1 },
    };
    GLfloat v[36 * 7];
    GLuint prog = program(vs, fs);
    GLint mloc = glGetUniformLocation(prog, "m");
    int n = 0;
    double t0, busy = 0;

    /* 6 faces x 2 triangles */
    for (int f = 0; f < 6; f++) {
        int axis = f % 3;
        float side = f < 3 ? 1 : -1;
        static const int tri[6][2] = {
            { -1, -1 }, { 1, -1 }, { -1, 1 }, { -1, 1 }, { 1, -1 }, { 1, 1 },
        };

        for (int i = 0; i < 6; i++) {
            float p[3];

            p[axis] = side;
            p[(axis + 1) % 3] = tri[i][0];
            p[(axis + 2) % 3] = tri[i][1];
            v[n * 7 + 0] = p[0] * 0.5f;
            v[n * 7 + 1] = p[1] * 0.5f;
            v[n * 7 + 2] = p[2] * 0.5f;
            v[n * 7 + 3] = 1;
            v[n * 7 + 4] = face_col[f][0];
            v[n * 7 + 5] = face_col[f][1];
            v[n * 7 + 6] = face_col[f][2];
            n++;
        }
    }
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 28, v);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 28, v + 4);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    for (int fr = 0; fr < frames; fr++) {
        float a = fr * 0.05f + 0.6f, b = fr * 0.03f + 0.4f;
        float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        float asp = (float)W / H, f = 1.0f / tanf(0.5f), zn = 1, zf = 10;
        /* rotate (y then x), push to z = -3, perspective */
        float r[9] = {
            ca, 0, -sa,
            sa * sb, cb, ca * sb,
            sa * cb, -sb, ca * cb,
        };
        GLfloat m[16];

        for (int c = 0; c < 3; c++) {
            /* column c of rotation, then projection */
            float x = r[0 * 3 + c], y = r[1 * 3 + c], z = r[2 * 3 + c];

            m[c * 4 + 0] = x * f / asp;
            m[c * 4 + 1] = y * f;
            m[c * 4 + 2] = z * (zf + zn) / (zn - zf);
            m[c * 4 + 3] = -z;
        }
        m[12] = 0;
        m[13] = 0;
        m[14] = -3 * (zf + zn) / (zn - zf) + 2 * zf * zn / (zn - zf);
        m[15] = 3;
        glUniformMatrix4fv(mloc, 1, GL_FALSE, m);
        t0 = now_ms();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        if (fr == 0) {
            probe("cube centre", W / 2, H / 2, "one face colour");
            probe("cube background", 5, 5, "26 26 26");
        }
        eglSwapBuffers(dpy, surf);
        glFinish();
        busy += now_ms() - t0;
    }
    printf("%.1f ms per frame\n", busy / frames);
}

int main(int argc, char **argv)
{
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 16, EGL_NONE
    };
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2,
                                       EGL_NONE };
    const char *scene = argc > 1 ? argv[1] : "mip";
    static const EGLint pb_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
        EGL_ALPHA_SIZE, 0, EGL_DEPTH_SIZE, 0, EGL_NONE
    };
    static const EGLint pb_size[] = { EGL_WIDTH, 480, EGL_HEIGHT, 800,
                                      EGL_NONE };
    bool pb = !strcmp(scene, "pb565");
    int seconds = argc > 2 ? atoi(argv[2]) : 5;
    void *ui = dlopen("libui.so", RTLD_NOW);
    EGLNativeWindowType (*create)(void) =
        ui ? (EGLNativeWindowType (*)(void))
             dlsym(ui, "android_createDisplaySurface") : NULL;
    EGLConfig cfg;
    EGLint n;
    EGLContext ctx;

    if (!create) {
        fprintf(stderr, "no libui\n");
        return 1;
    }
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy, NULL, NULL);
    eglChooseConfig(dpy, pb ? pb_attr : cfg_attr, &cfg, 1, &n);
    surf = pb ? eglCreatePbufferSurface(dpy, cfg, pb_size)
              : eglCreateWindowSurface(dpy, cfg, create(), NULL);
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "EGL failed: 0x%x\n", eglGetError());
        return 1;
    }
    eglQuerySurface(dpy, surf, EGL_WIDTH, &W);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &H);
    glViewport(0, 0, W, H);

    if (!strcmp(scene, "cube")) {
        scene_cube(seconds * 10);
    } else {
        if (!strcmp(scene, "comp")) {
            scene_comp();
        } else if (pb) {
            glClearColor(0.5f, 0.25f, 1.0f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            probe("pb565 clear", 100, 100, "128 64 255 (as 565)");
        } else if (!strcmp(scene, "pts")) {
            scene_pts();
        } else {
            scene_mip();
        }
        eglSwapBuffers(dpy, surf);
        sleep(seconds);
    }
    printf("glGetError 0x%x\n", glGetError());
    return 0;
}
