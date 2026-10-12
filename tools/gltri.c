/*
 * gltri: draw a colour-interpolated triangle with OpenGL ES 2.0 on the
 * framebuffer through the ROM's EGL (the Adreno 200 driver).
 *
 * Build (NDK r23, API 16 sysroot; ICS needs non-PIE and SysV hashes):
 *   armv7a-linux-androideabi16-clang -O2 -fno-pie -no-pie \
 *     -Wl,--hash-style=sysv -o gltri gltri.c -lEGL -lGLESv2 -ldl
 * Run with SurfaceFlinger stopped, since this takes the framebuffer:
 *   adb push gltri /data/local/tmp && adb shell stop &&
 *   adb shell /data/local/tmp/gltri [seconds]
 * It prints the GL strings, reads back three pixels to check the
 * rendering and reports the average time per frame (clear, draw, swap).
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

static const char vs_src[] =
    "attribute vec4 pos;\n"
    "attribute vec4 col;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    gl_Position = pos;\n"
    "    v_col = col;\n"
    "}\n";

static const char fs_src[] =
    "precision mediump float;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    gl_FragColor = v_col;\n"
    "}\n";

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

int main(int argc, char **argv)
{
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
    };
    static const GLfloat verts[] = {
        /* x, y, r, g, b */
         0.0f,  0.6f, 1, 0, 0,
        -0.7f, -0.5f, 0, 1, 0,
         0.7f, -0.5f, 0, 0, 1,
    };
    int seconds = argc > 1 ? atoi(argv[1]) : 5;
    void *ui = dlopen("libui.so", RTLD_NOW);
    EGLNativeWindowType (*create)(void);
    EGLNativeWindowType win;
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLint n, w, h;
    EGLSurface surf;
    EGLContext ctx;
    GLuint prog;
    GLint pos, col;
    GLubyte px[3][4];

    create = ui ? (EGLNativeWindowType (*)(void))
                  dlsym(ui, "android_createDisplaySurface") : NULL;
    if (!create || !(win = create())) {
        fprintf(stderr, "no framebuffer window (is SurfaceFlinger "
                "stopped?)\n");
        return 1;
    }
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, NULL, NULL) ||
        !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "EGL init/config failed: 0x%x\n", eglGetError());
        return 1;
    }
    surf = eglCreateWindowSurface(dpy, cfg, win, NULL);
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "EGL surface/context failed: 0x%x\n", eglGetError());
        return 1;
    }
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    printf("EGL %s, %s\nGL_VENDOR %s\nGL_RENDERER %s\nGL_VERSION %s\n"
           "surface %dx%d\n",
           eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION),
           glGetString(GL_VENDOR), glGetString(GL_RENDERER),
           glGetString(GL_VERSION), w, h);

    prog = glCreateProgram();
    glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
    glLinkProgram(prog);
    glUseProgram(prog);
    pos = glGetAttribLocation(prog, "pos");
    col = glGetAttribLocation(prog, "col");
    glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                          verts);
    glVertexAttribPointer(col, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                          verts + 2);
    glEnableVertexAttribArray(pos);
    glEnableVertexAttribArray(col);
    glViewport(0, 0, w, h);

    struct timespec t0, t1;
    double busy = 0;

    for (int frame = 0; frame < seconds * 10; frame++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        glClearColor(0.1f, 0.1f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (frame == 0) {
            /* near the red top vertex, the centre, outside */
            glReadPixels(w / 2, h * 3 / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                         px[0]);
            glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                         px[1]);
            glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[2]);
            for (int i = 0; i < 3; i++) {
                printf("pixel %d: %3d %3d %3d %3d\n", i, px[i][0], px[i][1],
                       px[i][2], px[i][3]);
            }
            printf("glGetError 0x%x\n", glGetError());
            fflush(stdout);
        }
        eglSwapBuffers(dpy, surf);
        glFinish();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (frame) {
            busy += (t1.tv_sec - t0.tv_sec) * 1e3 +
                    (t1.tv_nsec - t0.tv_nsec) / 1e6;
        }
        usleep(100000);
    }
    if (seconds * 10 > 1) {
        printf("%.1f ms per frame\n", busy / (seconds * 10 - 1));
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(dpy);
    return 0;
}
