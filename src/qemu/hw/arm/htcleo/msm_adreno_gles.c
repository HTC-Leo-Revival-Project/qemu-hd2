/*
 * Adreno 200 host GPU backend: OpenGL ES 3.x on a surfaceless EGL context
 *
 * The same idea as msm_adreno_gl.c - the guest's pixel shaders are translated
 * to GLSL and run on the host GPU against GMEM - for a host that has no
 * desktop OpenGL: a phone.  An Adreno 8xx gives us GLES 3.x through EGL and
 * nothing else, so the shaders here are GLSL ES 3.00 and GMEM is a host
 * texture the shaders read and write themselves, with framebuffer fetch in
 * place of fragment shader interlock for the per-pixel read-modify-write.
 *
 * Everything is loaded with dlopen/dlsym out of libEGL and libGLESv3 (or
 * libGLESv2 on a desktop), so nothing has to be linked in and the same code
 * builds wherever the app does.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include <dlfcn.h>
#include <math.h>
#include "qemu/error-report.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/physmem.h"
#include "msm_adreno_gles.h"

/* ------------------------------------------------------- the host's GL ES */

typedef unsigned int    GLenum;
typedef unsigned char   GLboolean;
typedef unsigned int    GLbitfield;
typedef signed char     GLbyte;
typedef short           GLshort;
typedef int             GLint;
typedef unsigned char   GLubyte;
typedef unsigned short  GLushort;
typedef unsigned int    GLuint;
typedef int             GLsizei;
typedef float           GLfloat;
typedef float           GLclampf;
typedef char            GLchar;
typedef unsigned int    GLenum32;
typedef int             GLintptr;
typedef long            GLsizeiptr;

#define GL_FALSE_                       0
#define GL_NO_ERROR_                    0
#define GL_TRIANGLES_                   0x0004
#define GL_TRIANGLE_STRIP_              0x0005
#define GL_TRIANGLE_FAN_                0x0006
#define GL_POINTS_                      0x0000
#define GL_LINES_                       0x0001
#define GL_UNSIGNED_INT_                0x1405
#define GL_FLOAT_                       0x1406
#define GL_UNSIGNED_BYTE_               0x1401
#define GL_UNSIGNED_INT_                0x1405
#define GL_ELEMENT_ARRAY_BUFFER_        0x8893
#define GL_DYNAMIC_DRAW_                0x88E8
#define GL_VERSION_                     0x1F02
#define GL_RENDERER_                    0x1F01
#define GL_VENDOR_                      0x1F00
#define GL_EXTENSIONS_                  0x1F03
#define GL_SHADING_LANGUAGE_VERSION_    0x8B8C
#define GL_NUM_EXTENSIONS_              0x821D
#define GL_EXTENSIONS                   0x03F2
#define GL_FRAMEBUFFER_                 0x8D40
#define GL_RENDERBUFFER_                0x8D41
#define GL_COLOR_ATTACHMENT0_           0x8CE0
#define GL_RGBA32UI_                    0x8D68
#define GL_RGBA32F_                     0x8814
#define GL_R32UI_                       0x8236
#define GL_RGBA8_                       0x8058
#define GL_REPEAT_                       0x2901
#define GL_MIRRORED_REPEAT_              0x8370
#define GL_TEXTURE_WRAP_S_               0x2802
#define GL_TEXTURE_WRAP_T_               0x2803
#define GL_TEXTURE_MIN_LOD_              0x813A
#define GL_TEXTURE_MAX_LOD_              0x813D
#define GL_TEXTURE_LOD_BIAS_             0x8501
#define GL_RGBA_                        0x1908
#define GL_RGBA_INTEGER_                0x8D99
#define GL_RED_INTEGER_                  0x8D94
#define GL_UNPACK_ALIGNMENT_             0x0CF5
#define GL_PACK_ALIGNMENT_               0x0D05
#define GL_UNPACK_ALIGNMENT_             0x0CF5
#define GL_PACK_ALIGNMENT_               0x0D05
#define GL_TEXTURE_2D_                  0x0DE1
#define GL_TEXTURE_2D_ARRAY_            0x8C1A
#define GL_UNIFORM_BUFFER_              0x8A11
#define GL_SHADER_STORAGE_BUFFER_       0x90D2
#define GL_RGBA32F_                     0x8814
#define GL_RGBA8_                       0x8058
#define GL_REPEAT_                       0x2901
#define GL_MIRRORED_REPEAT_              0x8370
#define GL_TEXTURE_WRAP_S_               0x2802
#define GL_TEXTURE_WRAP_T_               0x2803
#define GL_TEXTURE_MIN_LOD_              0x813A
#define GL_TEXTURE_MAX_LOD_              0x813D
#define GL_TEXTURE_LOD_BIAS_             0x8501
#define GL_FLOAT_                       0x1406
#define GL_FRONT_                       0x0404
#define GL_BACK_                        0x0405
#define GL_FRONT_AND_BACK_              0x0408
#define GL_CW_                          0x0900
#define GL_CCW_                         0x0901
#define GL_TEXTURE_CUBE_MAP_            0x8513
#define GL_TEXTURE0_                    0x84C0
#define GL_TEXTURE_MAG_FILTER_          0x2800
#define GL_TEXTURE_MIN_FILTER_          0x2801
#define GL_TEXTURE_WRAP_S_              0x2802
#define GL_TEXTURE_WRAP_T_              0x2803
#define GL_TEXTURE_BASE_LEVEL_         0x813C
#define GL_TEXTURE_MAX_LEVEL_           0x813D
#define GL_NEAREST_                     0x2600
#define GL_NEAREST_MIPMAP_LINEAR_        0x2702
#define GL_LINEAR_MIPMAP_NEAREST_        0x2701
#define GL_NEAREST_MIPMAP_NEAREST_       0x2700
#define GL_LINEAR_                      0x2601
#define GL_LINEAR_MIPMAP_LINEAR_        0x2703
#define GL_CLAMP_TO_EDGE_               0x812F
#define GL_CLAMP_TO_BORDER_             0x812D
#define GL_TEXTURE_BORDER_COLOR_        0x1004
#define GL_ARRAY_BUFFER_                0x8892
#define GL_POINTS_                      0x0000
#define GL_TRIANGLES_                   0x0004
#define GL_TRIANGLE_STRIP_              0x0005
#define GL_TRIANGLE_FAN_                0x0006
#define GL_ELEMENT_ARRAY_BUFFER_        0x8893
#define GL_STATIC_DRAW_                 0x88E4
#define GL_DYNAMIC_DRAW_                0x88E8
#define GL_FRAGMENT_SHADER_             0x8B30
#define GL_VERTEX_SHADER_               0x8B31
#define GL_COMPILE_STATUS_              0x8B81
#define GL_LINK_STATUS_                 0x8B82
#define GL_INFO_LOG_LENGTH_             0x8B84
#define GL_DEBUG_OUTPUT_                0x92E0
#define GL_DEBUG_OUTPUT_SYNCHRONOUS_    0x8242
#define GL_COLOR_                       0x1800
#define GL_COLOR_BUFFER_BIT_            0x00004000
#define GL_DEPTH_BUFFER_BIT_            0x00000100
#define GL_STENCIL_BUFFER_BIT_          0x00000400
#define GL_SCISSOR_TEST_                0x0C11
#define GL_DEPTH_TEST_                  0x0B71
#define GL_CULL_FACE_                   0x0B44
#define GL_BLEND_                       0x0BE2
#define GL_POLYGON_OFFSET_FILL_         0x8037
#define GL_DITHER_                      0x0BD0
#define GL_STENCIL_TEST_                0x0B90
#define GL_MAX_TEXTURE_SIZE_             0x0D33
#define GL_MAX_RENDERBUFFER_SIZE_        0x84E8
#define GL_MAX_VERTEX_UNIFORM_VECTORS_   0x8DFB
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS_ 0x8DFD

/* GLES does not have these; the ROP runs in the shaders instead */
#define GL_NOSUCH_                      0

typedef const GLubyte *(*GlesGetStringFn)(GLenum);
typedef void (*GlesGetIntegervFn)(GLenum, GLint *);
typedef GLenum (*GlesGetErrorFn)(void);
typedef void (*GlesViewportFn)(GLint, GLint, GLsizei, GLsizei);
typedef void (*GlesScissorFn)(GLint, GLint, GLsizei, GLsizei);
typedef void (*GlesEnableFn)(GLenum);
typedef void (*GlesDisableFn)(GLenum);
typedef void (*GlesClearFn)(GLbitfield);
typedef void (*GlesClearColorFn)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*GlesClearBufferuivFn)(GLenum, GLint, const GLuint *);
typedef void (*GlesDrawArraysFn)(GLenum, GLint, GLsizei);
typedef void (*GlesReadPixelsFn)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                 void *);
typedef void (*GlesDrawElementsFn)(GLenum, GLsizei, GLenum, const void *);
typedef void (*GlesTexStorage2DFn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (*GlesTexStorage3DFn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei,
                                   GLsizei);
typedef void (*GlesTexSubImage3DFn)(GLenum, GLint, GLint, GLint, GLint, GLsizei,
                                    GLsizei, GLsizei, GLenum, GLenum,
                                    const void *);
typedef void (*GlesBindBufferBaseFn)(GLenum, GLuint, GLuint);
typedef void (*GlesBindBufferRangeFn)(GLenum, GLuint, GLuint, GLintptr,
                                      GLsizeiptr);
typedef GLuint (*GlesGetUniformBlockIndexFn)(GLuint, const GLchar *);
typedef void (*GlesUniformBlockBindingFn)(GLuint, GLuint, GLuint);
typedef void (*GlesFrontFaceFn)(GLenum);
typedef void (*GlesCullFaceFn)(GLenum);
typedef void (*GlesActiveTextureFn)(GLenum);
typedef void (*GlesGenBuffersFn)(GLsizei, GLuint *);
typedef void (*GlesBindBufferFn)(GLenum, GLuint);
typedef void (*GlesBufferDataFn)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*GlesDeleteBuffersFn)(GLsizei, const GLuint *);
typedef GLuint (*GlesGenVertexArraysFn)(GLsizei, GLuint *);
typedef void (*GlesBindVertexArrayFn)(GLuint);
typedef void (*GlesDeleteVertexArraysFn)(GLsizei, const GLuint *);
typedef void (*GlesVertexAttribPointerFn)(GLuint, GLint, GLenum, GLboolean,
                                          GLsizei, const void *);
typedef void (*GlesVertexAttribIPointerFn)(GLuint, GLint, GLenum, GLsizei,
                                            const void *);
typedef void (*GlesVertexAttribDivisorFn)(GLuint, GLuint);
typedef void (*GlesEnableVertexAttribArrayFn)(GLuint);
typedef void (*GlesDisableVertexAttribArrayFn)(GLuint);
typedef void (*GlesFinishFn)(void);
typedef void (*GlesFlushFn)(void);
typedef void *(*GlesMapBufferRangeFn)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef GLboolean (*GlesUnmapBufferFn)(GLenum);
typedef GLuint (*GlesCreateShaderFn)(GLenum);
typedef void (*GlesShaderSourceFn)(GLuint, GLsizei, const GLchar *const *,
                                   const GLint *);
typedef void (*GlesCompileShaderFn)(GLuint);
typedef void (*GlesGetShaderivFn)(GLuint, GLenum, GLint *);
typedef void (*GlesGetShaderInfoLogFn)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (*GlesDeleteShaderFn)(GLuint);
typedef GLuint (*GlesCreateProgramFn)(void);
typedef void (*GlesAttachShaderFn)(GLuint, GLuint);
typedef void (*GlesLinkProgramFn)(GLuint);
typedef void (*GlesGetProgramivFn)(GLuint, GLenum, GLint *);
typedef void (*GlesGetProgramInfoLogFn)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (*GlesUseProgramFn)(GLuint);
typedef void (*GlesDeleteProgramFn)(GLuint);
typedef GLint (*GlesGetUniformLocationFn)(GLuint, const GLchar *);
typedef void (*GlesUniform1iFn)(GLint, GLint);
typedef void (*GlesUniform1uiFn)(GLint, GLuint);
typedef void (*GlesUniform1fFn)(GLint, GLfloat);
typedef void (*GlesUniform2fFn)(GLint, GLfloat, GLfloat);
typedef void (*GlesUniform3fFn)(GLint, GLfloat, GLfloat, GLfloat);
typedef void (*GlesUniform4fFn)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*GlesUniformMatrix4fvFn)(GLint, GLsizei, GLboolean,
                                        const GLfloat *);
typedef GLuint (*GlesGenTexturesFn)(GLsizei, GLuint *);
typedef void (*GlesBindTextureFn)(GLenum, GLuint);
typedef void (*GlesDeleteTexturesFn)(GLsizei, const GLuint *);
typedef void (*GlesTexImage2DFn)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                 GLenum, GLenum, const void *);
typedef void (*GlesTexParameteriFn)(GLenum, GLenum, GLint);
typedef void (*GlesTexParameterfvFn)(GLenum, GLenum, const GLfloat *);
typedef void (*GlesTexParameterfFn)(GLenum, GLenum, GLfloat);
typedef void (*GlesTexSubImage2DFn)(GLenum, GLint, GLint, GLint, GLsizei,
                                     GLsizei, GLenum, GLenum, const void *);
typedef void (*GlesGenerateMipmapFn)(GLenum);
typedef void (*GlesActiveTextureFn)(GLenum);
typedef void (*GlesCreateFramebufferFn)(GLsizei, GLuint *);
typedef void (*GlesBindFramebufferFn)(GLenum, GLuint);
typedef void (*GlesDeleteFramebuffersFn)(GLsizei, const GLuint *);
typedef void (*GlesFramebufferTexture2DFn)(GLenum, GLenum, GLenum, GLuint,
                                            GLint);
typedef GLenum (*GlesCheckFramebufferStatusFn)(GLenum);
typedef void (*GlesPixelStoreiFn)(GLenum, GLint);
typedef void (*GlesBlendFuncSeparateFn)(GLenum, GLenum, GLenum, GLenum);
typedef void (*GlesBlendEquationFn)(GLenum);
typedef void (*GlesDepthMaskFn)(GLboolean);
typedef void (*GlesStencilMaskSeparateFn)(GLenum, GLenum);
typedef void (*GlesStencilFuncSeparateFn)(GLenum, GLenum, GLenum, GLenum,
                                          GLint);
typedef void (*GlesStencilOpSeparateFn)(GLenum, GLenum, GLenum, GLenum,
                                        GLenum, GLenum);
typedef void (*GlesColorMaskFn)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (*GlesGenRenderbuffersFn)(GLsizei, GLuint *);
typedef void (*GlesBindRenderbufferFn)(GLenum, GLuint);
typedef void (*GlesRenderbufferStorageFn)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*GlesFramebufferRenderbufferFn)(GLenum, GLenum, GLenum, GLuint);
typedef void (*GlesDeleteRenderbuffersFn)(GLsizei, const GLuint *);

/* EGL */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef int EGLint;
typedef int EGLBoolean;
typedef unsigned int EGLenum;

#define EGL_NO_DISPLAY_            ((EGLDisplay)0)
#define FB_SIZE                    16384
#define EGL_NO_CONTEXT_           ((EGLContext)0)
#define EGL_NONE_                 0x3038
#define EGL_NO_SURFACE_           ((void *)0)
#define EGL_DEFAULT_DISPLAY_       ((void *)0)
#define EGL_FALSE_                0
#define EGL_TRUE_                 1
#define EGL_SUCCESS_              0x3000
#define EGL_OPENGL_ES_API_         0x30A0
#define EGL_CONTEXT_CLIENT_VERSION_ 0x3098
#define EGL_RENDERABLE_TYPE_       0x3040
#define EGL_OPENGL_ES3_BIT_        0x0040
#define EGL_SURFACE_TYPE_          0x3033
#define EGL_PBUFFER_BIT_           0x0001
#define EGL_WIDTH_                 0x3057
#define EGL_HEIGHT_                0x3056
#define EGL_PLATFORM_SURFACELESS_MESA_ 0x31DD
#define EGL_PLATFORM_DEVICE_EXT_   0x3130
#define EGL_PLATFORM_ANDROID_      0x31D8
#define EGL_PLATFORM_SURFACELESS_KHR_ 0x31DD

typedef EGLDisplay (*EglGetDisplayFn)(void *);
typedef EGLDisplay (*EglGetPlatformDisplayFn)(EGLenum, void *,
                                              const EGLint *);
typedef EGLBoolean (*EglInitializeFn)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*EglTerminateFn)(EGLDisplay);
typedef EGLBoolean (*EglBindAPIFn)(EGLenum);
typedef EGLContext (*EglCreateContextFn)(EGLDisplay, EGLConfig, EGLContext,
                                         const EGLint *);
typedef EGLBoolean (*EglDestroyContextFn)(EGLDisplay, EGLContext);
typedef EGLBoolean (*EglMakeCurrentFn)(EGLDisplay, void *, void *, EGLContext);
typedef EGLBoolean (*EglChooseConfigFn)(EGLDisplay, const EGLint *,
                                         EGLConfig *, EGLint, EGLint *);
typedef EGLint (*EglGetErrorFn)(void);
typedef void *EGLSurface;
typedef void (*EglDestroySurfaceFn)(EGLDisplay, EGLSurface);
typedef EGLSurface (*EglCreatePbufferSurfaceFn)(EGLDisplay, EGLConfig,
                                                  const EGLint *);

static struct {
    EglGetDisplayFn GetDisplay;
    EglGetPlatformDisplayFn GetPlatformDisplayEXT;
    EglInitializeFn Initialize;
    EglTerminateFn Terminate;
    EglBindAPIFn BindAPI;
    EglCreateContextFn CreateContext;
    EglDestroyContextFn DestroyContext;
    EglMakeCurrentFn MakeCurrent;
    EglChooseConfigFn ChooseConfig;
    EglGetErrorFn EglGetError;
    EglDestroySurfaceFn DestroySurface;
    EglCreatePbufferSurfaceFn CreatePbufferSurface;

    GlesGetStringFn GetString;
    GlesGetIntegervFn GetIntegerv;
    GlesGetErrorFn GetError;
    GlesViewportFn Viewport;
    GlesScissorFn Scissor;
    GlesEnableFn Enable;
    GlesDisableFn Disable;
    GlesClearFn Clear;
    GlesClearColorFn ClearColor;
    GlesClearBufferuivFn ClearBufferuiv;
    GlesDrawArraysFn DrawArrays;
    GlesReadPixelsFn ReadPixels;
    GlesDrawElementsFn DrawElements;
    GlesTexStorage2DFn TexStorage2D;
    GlesTexStorage3DFn TexStorage3D;
    GlesTexSubImage3DFn TexSubImage3D;
    GlesBindBufferBaseFn BindBufferBase;
    GlesBindBufferRangeFn BindBufferRange;
    GlesGetUniformBlockIndexFn GetUniformBlockIndex;
    GlesUniformBlockBindingFn UniformBlockBinding;
    GlesFrontFaceFn FrontFace;
    GlesCullFaceFn CullFace;
    GlesGenBuffersFn GenBuffers;
    GlesBindBufferFn BindBuffer;
    GlesBufferDataFn BufferData;
    GlesDeleteBuffersFn DeleteBuffers;
    GlesGenVertexArraysFn GenVertexArrays;
    GlesBindVertexArrayFn BindVertexArray;
    GlesDeleteVertexArraysFn DeleteVertexArrays;
    GlesVertexAttribPointerFn VertexAttribPointer;
    GlesVertexAttribIPointerFn VertexAttribIPointer;
    GlesVertexAttribDivisorFn VertexAttribDivisor;
    GlesEnableVertexAttribArrayFn EnableVertexAttribArray;
    GlesDisableVertexAttribArrayFn DisableVertexAttribArray;
    GlesFinishFn Finish;
    GlesFlushFn Flush;
    GlesMapBufferRangeFn MapBufferRange;
    GlesUnmapBufferFn UnmapBuffer;
    GlesCreateShaderFn CreateShader;
    GlesShaderSourceFn ShaderSource;
    GlesCompileShaderFn CompileShader;
    GlesGetShaderivFn GetShaderiv;
    GlesGetShaderInfoLogFn GetShaderInfoLog;
    GlesDeleteShaderFn DeleteShader;
    GlesCreateProgramFn CreateProgram;
    GlesAttachShaderFn AttachShader;
    GlesLinkProgramFn LinkProgram;
    GlesGetProgramivFn GetProgramiv;
    GlesGetProgramInfoLogFn GetProgramInfoLog;
    GlesUseProgramFn UseProgram;
    GlesDeleteProgramFn DeleteProgram;
    GlesGetUniformLocationFn GetUniformLocation;
    GlesUniform1iFn Uniform1i;
    GlesUniform1uiFn Uniform1ui;
    GlesUniform1fFn Uniform1f;
    GlesUniform2fFn Uniform2f;
    GlesUniform3fFn Uniform3f;
    GlesUniform4fFn Uniform4f;
    GlesUniformMatrix4fvFn UniformMatrix4fv;
    GlesGenTexturesFn GenTextures;
    GlesBindTextureFn BindTexture;
    GlesDeleteTexturesFn DeleteTextures;
    GlesTexImage2DFn TexImage2D;
    GlesTexParameteriFn TexParameteri;
    GlesTexParameterfvFn TexParameterfv;
    GlesTexParameterfFn TexParameterf;
    GlesTexSubImage2DFn TexSubImage2D;
    GlesGenerateMipmapFn GenerateMipmap;
    GlesActiveTextureFn ActiveTexture;
    GlesCreateFramebufferFn CreateFramebuffer;
    GlesBindFramebufferFn BindFramebuffer;
    GlesDeleteFramebuffersFn DeleteFramebuffers;
    GlesFramebufferTexture2DFn FramebufferTexture2D;
    GlesCheckFramebufferStatusFn CheckFramebufferStatus;
    GlesPixelStoreiFn PixelStorei;
    GlesBlendFuncSeparateFn BlendFuncSeparate;
    GlesBlendEquationFn BlendEquation;
    GlesDepthMaskFn DepthMask;
    GlesStencilMaskSeparateFn StencilMaskSeparate;
    GlesStencilFuncSeparateFn StencilFuncSeparate;
    GlesStencilOpSeparateFn StencilOpSeparate;
    GlesColorMaskFn ColorMask;
    GlesGenRenderbuffersFn GenRenderbuffers;
    GlesBindRenderbufferFn BindRenderbuffer;
    GlesRenderbufferStorageFn RenderbufferStorage;
    GlesFramebufferRenderbufferFn FramebufferRenderbuffer;
    GlesDeleteRenderbuffersFn DeleteRenderbuffers;

    /* the context, once one exists */
    void *dpy;
    void *ctx;
    void *surf;          /* the pbuffer, if we made one */
} gl;

/* the GL ES calls, by the names the shaders and the code use */
#define glGetString(n)            gl.GetString(n)
#define glGetIntegerv(n, p)       gl.GetIntegerv(n, p)
#define glGetError                gl.GetError
#define glViewport                gl.Viewport
#define glScissor                 gl.Scissor
#define glEnable                  gl.Enable
#define glDisable                 gl.Disable
#define glClear                   gl.Clear
#define glClearColor              gl.ClearColor
#define glClearBufferuiv          gl.ClearBufferuiv
#define glDrawArrays              gl.DrawArrays
#define glReadPixels              gl.ReadPixels
#define glDrawElements            gl.DrawElements
#define glTexStorage2D            gl.TexStorage2D
#define glTexStorage3D            gl.TexStorage3D
#define glTexSubImage3D           gl.TexSubImage3D
#define glBindBufferBase          gl.BindBufferBase
#define glBindBufferRange         gl.BindBufferRange
#define glGetUniformBlockIndex   gl.GetUniformBlockIndex
#define glUniformBlockBinding     gl.UniformBlockBinding
#define glFrontFace               gl.FrontFace
#define glCullFace                gl.CullFace
#define glGenBuffers              gl.GenBuffers
#define glBindBuffer              gl.BindBuffer
#define glBufferData              gl.BufferData
#define glDeleteBuffers           gl.DeleteBuffers
#define glGenVertexArrays         gl.GenVertexArrays
#define glBindVertexArray         gl.BindVertexArray
#define glDeleteVertexArrays      gl.DeleteVertexArrays
#define glVertexAttribPointer     gl.VertexAttribPointer
#define glVertexAttribIPointer    gl.VertexAttribIPointer
#define glVertexAttribDivisor     gl.VertexAttribDivisor
#define glEnableVertexAttribArray gl.EnableVertexAttribArray
#define glDisableVertexAttribArray gl.DisableVertexAttribArray
#define glFinish                  gl.Finish
#define glFlush                   gl.Flush
#define glMapBufferRange          gl.MapBufferRange
#define glUnmapBuffer             gl.UnmapBuffer
#define glCreateShader            gl.CreateShader
#define glShaderSource            gl.ShaderSource
#define glCompileShader           gl.CompileShader
#define glGetShaderiv             gl.GetShaderiv
#define glGetShaderInfoLog        gl.GetShaderInfoLog
#define glDeleteShader            gl.DeleteShader
#define glCreateProgram           gl.CreateProgram
#define glAttachShader            gl.AttachShader
#define glLinkProgram             gl.LinkProgram
#define glGetProgramiv            gl.GetProgramiv
#define glGetProgramInfoLog       gl.GetProgramInfoLog
#define glUseProgram              gl.UseProgram
#define glDeleteProgram           gl.DeleteProgram
#define glGetUniformLocation      gl.GetUniformLocation
#define glUniform1i               gl.Uniform1i
#define glUniform1ui              gl.Uniform1ui
#define glUniform1f               gl.Uniform1f
#define glUniform2f               gl.Uniform2f
#define glUniform3f               gl.Uniform3f
#define glUniform4f               gl.Uniform4f
#define glUniformMatrix4fv        gl.UniformMatrix4fv
#define glGenTextures             gl.GenTextures
#define glBindTexture             gl.BindTexture
#define glDeleteTextures          gl.DeleteTextures
#define glTexImage2D              gl.TexImage2D
#define glTexParameteri           gl.TexParameteri
#define glTexParameterfv          gl.TexParameterfv
#define glTexParameterf           gl.TexParameterf
#define glTexSubImage2D           gl.TexSubImage2D
#define glGenerateMipmap          gl.GenerateMipmap
#define gl_                        /* keep the macro block tidy */
#define glActiveTexture           gl.ActiveTexture
#define glGenFramebuffers          gl.CreateFramebuffer
#define glBindFramebuffer         gl.BindFramebuffer
#define glDeleteFramebuffers      gl.DeleteFramebuffers
#define glFramebufferTexture2D    gl.FramebufferTexture2D
#define glCheckFramebufferStatus  gl.CheckFramebufferStatus
#define glPixelStorei             gl.PixelStorei
#define glBlendFuncSeparate       gl.BlendFuncSeparate
#define glBlendEquation           gl.BlendEquation
#define glDepthMask               gl.DepthMask
#define glStencilMaskSeparate     gl.StencilMaskSeparate
#define glStencilFuncSeparate     gl.StencilFuncSeparate
#define glStencilOpSeparate       gl.StencilOpSeparate
#define glColorMask               gl.ColorMask
#define glGenRenderbuffers        gl.GenRenderbuffers
#define glBindRenderbuffer        gl.BindRenderbuffer
#define glRenderbufferStorage     gl.RenderbufferStorage
#define glFramebufferRenderbuffer gl.FramebufferRenderbuffer
#define glDeleteRenderbuffers     gl.DeleteRenderbuffers

/* GL_FRAMEBUFFER is both an enum and a target in desktop GL; here they differ */
#define GL_FRAMEBUFFER_TARGET_    0x8D40
#undef  GL_FRAMEBUFFER
#define GL_FRAMEBUFFER            GL_FRAMEBUFFER_TARGET_

static void *lib;
static bool g_ctx_up;          /* a context is current on this thread */
static bool gles_render_ready;   /* gles_init() said yes */
static bool g_asked;            /* the guest has drawn once */
static bool gatomics_gles;    /* the host can do GMEM atomics */
static bool g_smoke_atomics;  /* what the smoke test found on this host */
static bool gles_tried;
static QemuThread gles_owner;

/* dlopen every entry point; true when the host can give us GL ES 3 */
static bool gles_load(void)
{
    static const char *egl_libs[] = { "libEGL.so", "libEGL.so.1", NULL };
    static const char *gl_libs[] = { "libGLESv3.so", "libGLESv3.so.2",
                                     "libGLESv2.so", "libGLESv2.so.2", NULL };
    void *egl = NULL, *g = NULL;

    for (int i = 0; egl_libs[i] && !egl; i++) {
        egl = dlopen(egl_libs[i], RTLD_LAZY | RTLD_LOCAL);
    }
    for (int i = 0; gl_libs[i] && !g; i++) {
        g = dlopen(gl_libs[i], RTLD_LAZY | RTLD_LOCAL);
    }
    if (!egl || !g) {
        error_report("msm-adreno: no libEGL%s on this host",
                     egl ? "" : " or libGLES3");
        return false;
    }
    lib = egl;

#define LOAD(field, name) \
    do { \
        *(void **)(&gl.field) = dlsym(egl, name); \
    } while (0)
    LOAD(GetDisplay, "eglGetDisplay");
    LOAD(GetPlatformDisplayEXT, "eglGetPlatformDisplayEXT");
    LOAD(Initialize, "eglInitialize");
    LOAD(Terminate, "eglTerminate");
    LOAD(BindAPI, "eglBindAPI");
    LOAD(CreateContext, "eglCreateContext");
    LOAD(DestroyContext, "eglDestroyContext");
    LOAD(MakeCurrent, "eglMakeCurrent");
    LOAD(ChooseConfig, "eglChooseConfig");
    LOAD(EglGetError, "eglGetError");
    LOAD(DestroySurface, "eglDestroySurface");
    LOAD(CreatePbufferSurface, "eglCreatePbufferSurface");
#undef LOAD

#define LOAD(field, name) \
    do { \
        *(void **)(&gl.field) = dlsym(g, name); \
        if (!gl.field) { \
            error_report("msm-adreno: %s has no %s", gl_libs[0], name); \
            return false; \
        } \
    } while (0)
    LOAD(GetString, "glGetString");
    LOAD(GetIntegerv, "glGetIntegerv");
    LOAD(GetError, "glGetError");
    LOAD(Viewport, "glViewport");
    LOAD(Scissor, "glScissor");
    LOAD(Enable, "glEnable");
    LOAD(Disable, "glDisable");
    LOAD(Clear, "glClear");
    LOAD(ClearColor, "glClearColor");
    LOAD(ClearBufferuiv, "glClearBufferuiv");
    LOAD(DrawArrays, "glDrawArrays");
    LOAD(ReadPixels, "glReadPixels");
    LOAD(DrawElements, "glDrawElements");
    LOAD(TexStorage2D, "glTexStorage2D");
    LOAD(TexStorage3D, "glTexStorage3D");
    LOAD(TexSubImage3D, "glTexSubImage3D");
    LOAD(BindBufferBase, "glBindBufferBase");
    LOAD(BindBufferRange, "glBindBufferRange");
    LOAD(GetUniformBlockIndex, "glGetUniformBlockIndex");
    LOAD(UniformBlockBinding, "glUniformBlockBinding");
    LOAD(FrontFace, "glFrontFace");
    LOAD(CullFace, "glCullFace");
    LOAD(GenBuffers, "glGenBuffers");
    LOAD(BindBuffer, "glBindBuffer");
    LOAD(BufferData, "glBufferData");
    LOAD(DeleteBuffers, "glDeleteBuffers");
    LOAD(GenVertexArrays, "glGenVertexArrays");
    LOAD(BindVertexArray, "glBindVertexArray");
    LOAD(DeleteVertexArrays, "glDeleteVertexArrays");
    LOAD(VertexAttribPointer, "glVertexAttribPointer");
    LOAD(VertexAttribIPointer, "glVertexAttribIPointer");
    LOAD(VertexAttribDivisor, "glVertexAttribDivisor");
    LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(DisableVertexAttribArray, "glDisableVertexAttribArray");
    LOAD(Finish, "glFinish");
    LOAD(Flush, "glFlush");
    LOAD(CreateShader, "glCreateShader");
    LOAD(ShaderSource, "glShaderSource");
    LOAD(CompileShader, "glCompileShader");
    LOAD(GetShaderiv, "glGetShaderiv");
    LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
    LOAD(DeleteShader, "glDeleteShader");
    LOAD(CreateProgram, "glCreateProgram");
    LOAD(AttachShader, "glAttachShader");
    LOAD(LinkProgram, "glLinkProgram");
    LOAD(GetProgramiv, "glGetProgramiv");
    LOAD(GetProgramInfoLog, "glGetProgramInfoLog");
    LOAD(UseProgram, "glUseProgram");
    LOAD(DeleteProgram, "glDeleteProgram");
    LOAD(GetUniformLocation, "glGetUniformLocation");
    LOAD(Uniform1i, "glUniform1i");
    LOAD(Uniform1f, "glUniform1f");
    LOAD(Uniform2f, "glUniform2f");
    LOAD(Uniform3f, "glUniform3f");
    LOAD(Uniform4f, "glUniform4f");
    LOAD(UniformMatrix4fv, "glUniformMatrix4fv");
    LOAD(GenTextures, "glGenTextures");
    LOAD(BindTexture, "glBindTexture");
    LOAD(DeleteTextures, "glDeleteTextures");
    LOAD(TexImage2D, "glTexImage2D");
    LOAD(TexParameteri, "glTexParameteri");
    LOAD(TexParameterfv, "glTexParameterfv");
    LOAD(TexParameterf, "glTexParameterf");
    LOAD(TexSubImage2D, "glTexSubImage2D");
    LOAD(GenerateMipmap, "glGenerateMipmap");
    LOAD(ActiveTexture, "glActiveTexture");
    LOAD(CreateFramebuffer, "glGenFramebuffers");
    LOAD(BindFramebuffer, "glBindFramebuffer");
    LOAD(DeleteFramebuffers, "glDeleteFramebuffers");
    LOAD(FramebufferTexture2D, "glFramebufferTexture2D");
    LOAD(PixelStorei, "glPixelStorei");
    LOAD(BlendFuncSeparate, "glBlendFuncSeparate");
    LOAD(BlendEquation, "glBlendEquation");
    LOAD(DepthMask, "glDepthMask");
    LOAD(StencilMaskSeparate, "glStencilMaskSeparate");
    LOAD(StencilFuncSeparate, "glStencilFuncSeparate");
    LOAD(StencilOpSeparate, "glStencilOpSeparate");
    LOAD(ColorMask, "glColorMask");
    LOAD(GenRenderbuffers, "glGenRenderbuffers");
    LOAD(BindRenderbuffer, "glBindRenderbuffer");
    LOAD(RenderbufferStorage, "glRenderbufferStorage");
    LOAD(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    LOAD(DeleteRenderbuffers, "glDeleteRenderbuffers");
#undef LOAD

    /* these two are optional: only the renderer uses them */
    *(void **)(&gl.MapBufferRange) = dlsym(g, "glMapBufferRange");
    *(void **)(&gl.UnmapBuffer) = dlsym(g, "glUnmapBuffer");
    *(void **)(&gl.CheckFramebufferStatus) = dlsym(g, "glCheckFramebufferStatus");
    *(void **)(&gl.Uniform1ui) = dlsym(g, "glUniform1ui");
    return true;
}

/*
 * The extension list, as one string.  GL ES 3.0 prefers
 * glGetString(GL_EXTENSIONS + n), but a driver that only knows the older,
 * concatenated form crashes on it, so ask for the list and walk it.
 */
/*
 * The "#version" line to open a shader with, taken from what the driver says
 * it speaks: a phone's Adreno is at 3.20, Mesa at 3.10, and there is no 3.30.
 */
static const char *gles_version_line(void)
{
    const char *v = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION_);
    int major = 3, minor = 0;

    if (v) {
        sscanf(v, "OpenGL ES GLSL ES %d.%d", &major, &minor);
    }
    /* GLSL ES numbers these 3.00, 3.10, 3.20: minor is 0, 10 or 20 */
    if (major != 3 || minor < 10) {
        return "#version 300 es\n";
    }
    return minor >= 20 ? "#version 320 es\n" : "#version 310 es\n";
}

static const char *extension_list(void)
{
    return (const char *)glGetString(GL_EXTENSIONS_);
}

/* the n-th name, for a driver that has no list to walk */
static const char *extension_n(GLint n)
{
    if (extension_list() && *extension_list()) {
        return NULL;
    }
    return (const char *)glGetString(GL_EXTENSIONS_ + n);
}

/*
 * Is an extension in the driver's list?  GL ES 3.0 wants
 * glGetString(GL_EXTENSIONS + n), one name at a time, but Qualcomm's driver
 * crashes on that and answers the older concatenated form instead - so take
 * whichever this host gives.  Matched on a name boundary, so
 * "GL_EXT_shader_framebuffer_fetch" does not match
 * "GL_QCOM_shader_framebuffer_fetch_rate".
 */
static bool has_extension(const char *name)
{
    size_t n = strlen(name);

    for (const char *p = extension_list(); p && (p = strstr(p, name)) != NULL;
         p += n) {
        if (p != extension_list() && p[-1] != ' ') {
            continue;
        }
        if (p[n] == '\0' || p[n] == ' ') {
            return true;
        }
    }
    for (GLint i = 0; i < 4096; i++) {
        const char *e = extension_n(i);

        if (!e) {
            return false;
        }
        if (!strcmp(e, name)) {
            return true;
        }
    }
    return false;
}

/*
 * Bring up a context and say what we got.  Everything the renderer needs to
 * decide how to draw is reported here, so a host that cannot do the work
 * fails with an explanation instead of a blank screen.
 */
static bool gles_init(const char *what)
{
    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_CLIENT_VERSION_, 3, EGL_NONE_
    };
    EGLDisplay dpy = EGL_NO_DISPLAY_;
    EGLint major = 0, minor = 0;
    const char *renderer, *version, *glsl;

    if (!gles_load()) {
        return false;
    }
    if (gl.GetPlatformDisplayEXT) {
        dpy = gl.GetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA_,
                                        EGL_DEFAULT_DISPLAY_, NULL);
    }
    if (dpy == EGL_NO_DISPLAY_) {
        dpy = gl.GetDisplay(EGL_DEFAULT_DISPLAY_);
    }
    if (dpy == EGL_NO_DISPLAY_ ||
        !gl.Initialize(dpy, &major, &minor) ||
        !gl.BindAPI(EGL_OPENGL_ES_API_)) {
        error_report("msm-adreno: no EGL display for the %s backend "
                     "(eglInitialize: 0x%x)", what, gl.EglGetError());
        return false;
    }
    /*
     * A pbuffer to draw into, if the host wants one: surfaceless contexts are
     * the general case, but a driver whose shader compiler needs a drawable
     * is happier with one (ADRENO_GLES_PBUFFER=1 asks for it).
     */
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_,
        EGL_RENDERABLE_TYPE_, EGL_OPENGL_ES3_BIT_,
        EGL_NONE_
    };
    static const EGLint pb_attr[] = {
        EGL_WIDTH_, FB_SIZE, EGL_HEIGHT_, FB_SIZE, EGL_NONE_
    };
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    bool want_pbuffer = getenv("ADRENO_GLES_PBUFFER") != NULL;

    if (want_pbuffer && gl.ChooseConfig &&
        gl.ChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) && ncfg > 0) {
        gl.surf = gl.CreatePbufferSurface(dpy, cfg, pb_attr);
        if (gl.surf == EGL_NO_SURFACE_) {
            gl.surf = NULL;
        }
    }
    gl.ctx = gl.CreateContext(dpy, want_pbuffer ? cfg : EGL_NO_DISPLAY_,
                              EGL_NO_CONTEXT_, ctx_attr);
    if (gl.ctx == EGL_NO_CONTEXT_) {
        error_report("msm-adreno: no OpenGL ES 3 context for the %s backend "
                     "(eglCreateContext: 0x%x)", what, gl.EglGetError());
        return false;
    }
    if (!gl.MakeCurrent(dpy, gl.surf ? gl.surf : EGL_NO_SURFACE_,
                        gl.surf ? gl.surf : EGL_NO_SURFACE_, gl.ctx)) {
        error_report("msm-adreno: eglMakeCurrent failed for the %s backend "
                     "(0x%x)", what, gl.EglGetError());
        gl.DestroyContext(dpy, gl.ctx);
        gl.ctx = EGL_NO_CONTEXT_;
        return false;
    }
    /* what the renderer will ask of the host, said once and briefly */
    {
        static const char *const want[] = {
            "GL_EXT_shader_framebuffer_fetch",     /* read the framebuffer */
            "GL_ARM_shader_framebuffer_fetch",
            "GL_OES_shader_image_atomic",         /* GMEM read-modify-write */
            "GL_EXT_texture_buffer",
            "GL_EXT_clip_control",
            "GL_EXT_depth_clamp",
            "GL_EXT_multisampled_render_to_texture",
            NULL
        };

        for (int i = 0; want[i]; i++) {
            info_report("msm-adreno:   %s: %s",
                        has_extension(want[i]) ? "yes" : "no ", want[i] + 3);
        }
        if (!extension_list() || !*extension_list()) {
            GLint count = 0;

            glGetIntegerv(GL_NUM_EXTENSIONS_, &count);
            info_report("msm-adreno:   %d extensions by index, first: %s",
                        count, extension_n(0) ? extension_n(0) : "(none)");
        }
    }
    renderer = (const char *)glGetString(GL_RENDERER_);
    version = (const char *)glGetString(GL_VERSION_);
    glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION_);
    info_report("msm-adreno: %s backend on %s (EGL %d.%d, %s, %s)", what,
                renderer ? renderer : "?", major, minor,
                version ? version : "?", glsl ? glsl : "?");
    g_ctx_up = true;
    return true;
}

/* let go of the context; the renderer makes a new one when it needs it */
static void gles_teardown(void)
{
    gl.MakeCurrent(gl.dpy, EGL_NO_SURFACE_, EGL_NO_SURFACE_, EGL_NO_CONTEXT_);
    gl.DestroyContext(gl.dpy, gl.ctx);
    gl.ctx = EGL_NO_CONTEXT_;
    if (gl.surf) {
        gl.DestroySurface(gl.dpy, gl.surf);
        gl.surf = NULL;
    }
    g_ctx_up = false;
}

/*
 * Say what the host GPU can do, at device init: on a phone this is the only
 * answer for minutes, because nothing draws through the Adreno 200 until the
 * guest's own driver gets to it.
 */
/*
 * GMEM as an OpenGL ES image: the 256 KiB the Adreno bins into, one unsigned
 * int per texel of an R32UI image, so a byte address is a texel index.  The
 * same four functions the desktop backend gets from its SSBO, and the same
 * split whole words versus atomics - the neighbours sharing a word are not
 * ordered against this fragment either way, which is what framebuffer fetch
 * on a tiler is for.
 */
#define GMEM_TEXELS_W  1024
#define GMEM_TEXELS_H  64
#define GMEM_TEXELS    (GMEM_TEXELS_W * GMEM_TEXELS_H)     /* 65536, 256 KiB */

static const char glsl_gm_gles[] =
    "layout(binding = 0, r32ui) uniform highp uimage2D u_gmem;\n"
    "ivec2 gm_texel(uint a) {\n"
    "    uint i = a >> 2u;\n"
    "    return ivec2(int(i) & 1023, int(i >> 10));\n"
    "}\n"
    "uint gm_get(uint a, uint n) {\n"
    "    uint w = imageLoad(u_gmem, gm_texel(a)).r;\n"
    "    return n == 4u ? w : (w >> ((a & 3u) * 8u)) & ((1u << (n * 8u)) - 1u);\n"
    "}\n";

static const char glsl_gm_put_atomic[] =
    "void gm_put(uint a, uint n, uint v) {\n"
    "    ivec2 t = gm_texel(a);\n"
    "    if (n == 4u) {\n"
    "        imageStore(u_gmem, t, uvec4(v, 0u, 0u, 1u));\n"
    "        return;\n"
    "    }\n"
    "    uint sh = (a & 3u) * 8u;\n"
    "    uint m = ((1u << (n * 8u)) - 1u) << sh;\n"
    "    imageAtomicAnd(u_gmem, t, ~m);\n"
    "    imageAtomicOr(u_gmem, t, (v << sh) & m);\n"
    "}\n";

/* a host without image atomics: only to compile against */
static const char glsl_gm_put_plain[] =
    "void gm_put(uint a, uint n, uint v) {\n"
    "    ivec2 t = gm_texel(a);\n"
    "    if (n == 4u) {\n"
    "        imageStore(u_gmem, t, uvec4(v, 0u, 0u, 1u));\n"
    "        return;\n"
    "    }\n"
    "    uint sh = (a & 3u) * 8u;\n"
    "    uint m = ((1u << (n * 8u)) - 1u) << sh;\n"
    "    uint old = imageLoad(u_gmem, t).r;\n"
    "    imageStore(u_gmem, t, uvec4((old & ~m) | ((v << sh) & m), 0u, 0u, 1u));\n"
    "}\n";

static const char glsl_gm_px[] =
    "uvec4 px_get(uint a, uint bpp) {\n"
    "    if (bpp == 3u) {\n"
    "        return uvec4(gm_get(a, 1u) | (gm_get(a + 1u, 1u) << 8) |\n"
    "                     (gm_get(a + 2u, 1u) << 16), 0u, 0u, 0u);\n"
    "    }\n"
    "    if (bpp <= 4u) {\n"
    "        return uvec4(gm_get(a, bpp), 0u, 0u, 0u);\n"
    "    }\n"
    "    uint i = a >> 2;\n"
    "    if (bpp == 8u) {\n"
    "        return uvec4(gm_get(i * 4u, 4u), gm_get(i * 4u + 4u, 4u), 0u, 0u);\n"
    "    }\n"
    "    return uvec4(gm_get(i * 4u, 4u), gm_get(i * 4u + 4u, 4u),\n"
    "                 gm_get(i * 4u + 8u, 4u), gm_get(i * 4u + 12u, 4u));\n"
    "}\n"
    "void px_put(uint a, uint bpp, uvec4 w) {\n"
    "    if (bpp == 3u) {\n"
    "        gm_put(a, 1u, w.x & 0xffu);\n"
    "        gm_put(a + 1u, 1u, (w.x >> 8) & 0xffu);\n"
    "        gm_put(a + 2u, 1u, (w.x >> 16) & 0xffu);\n"
    "    } else if (bpp <= 4u) {\n"
    "        gm_put(a, bpp, w.x);\n"
    "    } else {\n"
    "        gm_put(a, 4u, w.x);\n"
    "        gm_put(a + 4u, 4u, w.y);\n"
    "        if (bpp == 16u) {\n"
    "            gm_put(a + 8u, 4u, w.z);\n"
    "            gm_put(a + 12u, 4u, w.w);\n"
    "        }\n"
    "    }\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* GLSL ES: the guest's shaders                                        */
/* ------------------------------------------------------------------ */

/*
 * GMEM through an image, with the atomic read-modify-write where the host
 * has it (the renderer needs it) and without where it does not (so a host
 * can still be asked whether the render backend compiles at all).
 */
static void gles_gm_source(GString *o, bool atomics)
{
    g_string_append(o, glsl_gm_gles);
    g_string_append(o, atomics ? glsl_gm_put_atomic : glsl_gm_put_plain);
    g_string_append(o, glsl_gm_px);
}

/*
 * The precision every type needs before it is used in GL ES.  The guest's
 * shaders are the same GLSL as on the desktop, so the only changes are the
 * version line, these, GMEM's access (glsl_gm_gles) and the interlock.
 */
static const char glsl_precision[] =
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "precision highp samplerCube;\n"
    "precision highp usampler2D;\n"
    "precision highp uimage2D;\n";

/* the interpolators between the two stages, the way the desktop one has them */
static void interp_decls_gles(GString *o, int ninterp, const char *dir)
{
    g_string_append_printf(o, "layout(location = 0) %s vec4 v_zw;\n",
                           dir);
    g_string_append_printf(o, "layout(location = 1) flat %s float v_face;\n",
                           dir);
    for (int i = 0; i < ninterp; i++) {
        g_string_append_printf(o, "layout(location = %d) %s vec4 v_i%d;\n",
                               i + 2, dir, i);
    }
}

/*
 * The vertex shader for a draw whose vertices the CPU shaded: the positions
 * arrive already in window coordinates, so this only passes them on.
 */
static GString *vs_source_cpu_gles(int ninterp)
{
    GString *o = g_string_new(NULL);

    g_string_append(o, gles_version_line());
    g_string_append(o, glsl_precision);
    g_string_append(o, glsl_common);
    g_string_append(o, adreno_glsl_helpers);
    g_string_append_printf(o,
        "layout(std430, binding = 1) readonly buffer Verts { vec4 vd[]; };\n"
        "void main() {\n"
        "    int b = gl_VertexID * %d;\n"
        "    vec4 p = vd[b], m = vd[b + 1];\n"
        "    gl_Position = vec4((p.xy + u_misc.w) * (1.0 / %d.0) - 1.0, "
        "0.0, 1.0);\n"
        "    v_zw = vec4(p.z, p.w, m.x, m.y);\n"
        "    v_face = m.z;\n",
        ADRENO_GL_VTX_VEC4(ninterp), FB_SIZE / 2);
    for (int i = 0; i < ninterp; i++) {
        g_string_append_printf(o, "    v_i%d = vd[b + %d];\n", i, 2 + i);
    }
    g_string_append(o, "}\n");
    return o;
}

/*
 * The guest's vertex shader, then what msm_adreno_draw.c does to its
 * position: the clip planes as clip distances, the viewport transform and
 * the window offset, to the host's window coordinates.  As on the desktop,
 * except that the clip distances need GL_EXT_clip_cull_distance, which the
 * renderer asks for before it takes a draw.
 */
static GString *vs_source_gles(const GlProgKey *k, const AdrenoProgram *vs,
                               AdrenoGlslInfo *info)
{
    GString *o = g_string_new(gles_version_line());
    bool points = k->geom == GEOM_POINTS;

    /* the clip planes the guest's VS clips with, as clip distances */
    if (has_extension("GL_EXT_clip_cull_distance")) {
        g_string_append(o, "#extension GL_EXT_clip_cull_distance : enable\n");
    }

    g_string_append(o, glsl_precision);
    g_string_append(o, glsl_common);
    g_string_append(o, glsl_vfetch);
    g_string_append(o, "#define CONST(i) u_c[((i) + u_cb.y) & 511]\n");
    adreno_glsl_translate(vs, false, k->vs_cube_mask, ADRENO_GL_MAX_UNITS, o,
                          info);
    interp_decls_gles(o, k->ninterp, "out");
    g_string_append_printf(o,
        "void main() {\n"
        "    for (int i = 0; i < 64; i++) {\n"
        "        r[i] = vec4(0.0);\n"
        "    }\n"
        "    for (int i = 0; i < 16; i++) {\n"
        "        o_interp[i] = vec4(0.0);\n"
        "    }\n"
        "    r[0].x = float(gl_VertexID);\n"
        "    sq_main();\n"
        "    vec4 p = o_pos;\n"
        "    uint vte = u_vte.x, clip = u_vte.y;\n"
        "    bool cl = %s && (clip & 0x10000u) == 0u;\n"
        "    gl_ClipDistance[0] = cl ? p.w : 1.0;\n"
        "    gl_ClipDistance[1] = cl ? p.w + p.x : 1.0;\n"
        "    gl_ClipDistance[2] = cl ? p.w - p.x : 1.0;\n"
        "    gl_ClipDistance[3] = cl ? p.w + p.y : 1.0;\n"
        "    gl_ClipDistance[4] = cl ? p.w - p.y : 1.0;\n"
        "    gl_ClipDistance[5] = cl ? p.w - p.z : 1.0;\n"
        "    gl_ClipDistance[6] = !cl ? 1.0 : (clip & 0x80000u) != 0u ? p.z "
        ": p.z + p.w;\n"
        "    float sx = (vte & 1u) != 0u ? u_vp0.x : 1.0;\n"
        "    float ox = ((vte & 2u) != 0u ? u_vp0.y : 0.0) + u_vp2.x + "
        "u_vp2.z;\n"
        "    float sy = (vte & 4u) != 0u ? u_vp0.z : 1.0;\n"
        "    float oy = ((vte & 8u) != 0u ? u_vp0.w : 0.0) + u_vp2.y + "
        "u_vp2.z;\n"
        "    float sz = (vte & 16u) != 0u ? u_vp1.x : 1.0;\n"
        "    float oz = (vte & 32u) != 0u ? u_vp1.y : 0.0;\n"
        "    const float k = 1.0 / %d.0;\n"
        "    if ((vte & 0x700u) == 0x400u) {\n"
        /* the usual: x/w, y/w, z/w; linear in clip space this way */
        "        gl_Position = vec4((sx * p.x + ox * p.w) * k - p.w,\n"
        "                           (sy * p.y + oy * p.w) * k - p.w,\n"
        "                           sz * p.z + oz * p.w, p.w);\n"
        "    } else {\n"
        "        float rhw = (vte & 0x400u) != 0u ? 1.0 / p.w : p.w;\n"
        "        bool xy = (vte & 0x100u) != 0u, zf = (vte & 0x200u) != 0u;\n"
        "        float x = xy ? p.x : p.x * rhw, y = xy ? p.y : p.y * rhw;\n"
        "        float z = zf ? p.z : p.z * rhw;\n"
        "        float wp = rhw != 0.0 ? 1.0 / rhw : 1.0;\n"
        "        gl_Position = vec4(((sx * x + ox) * k - 1.0) * wp,\n"
        "                           ((sy * y + oy) * k - 1.0) * wp,\n"
        "                           (sz * z + oz) * wp, wp);\n"
        "    }\n", points ? "false" : "true", FB_SIZE / 2);
    if (points) {
        /* PA_SU_POINT_SIZE holds half the size; the shader's is the size */
        g_string_append(o,
            "    float hs = u_psz.z != 0.0 ? o_psize * 0.5 : u_vp1.z;\n"
            "    if (u_psz.y > 0.0) {\n"
            "        hs = min(max(hs, u_psz.x), u_psz.y);\n"
            "    }\n"
            "    gl_PointSize = hs > 0.0 ? 2.0 * hs : 1.0;\n");
    }
    for (int i = 0; i < k->ninterp; i++) {
        g_string_append_printf(o, "    v_i%d = o_interp[%d];\n", i, i);
    }
    g_string_append(o, "}\n");
    return o;
}

/*
 * The pixel shader: the guest's own, translated, then the render backend -
 * the same depth/stencil and colour write as on the desktop, over the same
 * GMEM in an image.  What is missing is the interlock: on a tiler the
 * fragments of a tile run in primitive order, which is what framebuffer
 * fetch would guarantee.
 */
static GString *fs_source_gles(const AdrenoProgram *ps, uint32_t cube_mask,
                               int ninterp, uint32_t flat_mask, bool param_gen,
                               unsigned param_gen_reg, bool exports_depth,
                               AdrenoGlslInfo *info)
{
    GString *o = g_string_new(NULL);
    g_string_append(o, gles_version_line());
    g_string_append(o, glsl_precision);
    g_string_append(o, glsl_common);
    g_string_append(o, "#define CONST(i) u_c[((i) + u_cb.x) & 511]\n");
    adreno_glsl_translate(ps, true, cube_mask, 0, o, info);
    gles_gm_source(o, gatomics_gles);
    g_string_append(o, glsl_rop_mid);
    g_string_append(o, glsl_rop_tail);
    interp_decls_gles(o, ninterp, "in");
    g_string_append(o,
        "void main() {\n"
        "    for (int i = 0; i < 64; i++) {\n"
        "        r[i] = vec4(0.0);\n"
        "    }\n"
        "    float rhw = v_zw.y;\n"
        "    bool face = v_face != 0.0;\n"
        "    vec2 pc = v_zw.zw;\n"
        "    float zi = v_zw.x;\n");
    for (int i = 0; i < ninterp; i++) {
        if (flat_mask & (1u << i)) {
            g_string_append_printf(o, "    r[%d] = v_i%d;\n", i, i);
        } else {
            g_string_append_printf(o, "    r[%d] = rhw != 0.0 ? v_i%d / rhw "
                                       ": v_i%d;\n", i, i, i);
        }
    }
    if (param_gen) {
        g_string_append_printf(o,
            "    float fx = gl_FragCoord.x - u_misc.y;\n"
            "    r[%u] = vec4(face ? -fx : fx, gl_FragCoord.y - u_misc.z, "
            "pc);\n", param_gen_reg & 63);
    }
    g_string_append_printf(o,
        "    sq_main();\n"
        "    bool alive = !killed;\n"
        "    uint cc = u_rb.z;\n"
        "    if ((cc & 8u) != 0u && !cmpf(cc & 7u, o_color.w, u_misc.x)) {\n"
        "        alive = false;\n"
        "    }\n"
        "    int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);\n"
        "    float z = %s;\n"
        "    if (alive && depth_stencil(x, y, z, face)) {\n"
        "        write_color(x, y, o_color);\n"
        "    }\n"
        "}\n", exports_depth ? "o_depth" : "zi");
    return o;
}

/*
 * Does the render backend compile as GLSL ES?  The pieces that matter are
 * glsl_gm_gles() - GMEM as an image - and the shared depth/stencil and colour
 * write, which is where the ES dialect shows.  The atomics are swapped out
 * here so a host without them (Mesa) can still answer the question; the phone
 * compiles the real ones.
 */
static void gles_rop_smoke_test(void)
{
    GString *o = g_string_new(NULL);
    bool gatomics_gles = false;      /* any host can answer this one */
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER_);
    GLint ok = 0;
    char log[1024];

    g_string_append(o, glsl_precision);
    g_string_append(o, glsl_common);
    g_string_append(o, adreno_glsl_helpers);
    g_string_append_printf(o, "#define CONST(i) u_c[((i) + u_cb.x) & 511]\n");
    gles_gm_source(o, gatomics_gles);
    g_string_append(o, glsl_rop_mid);
    g_string_append(o, glsl_rop_tail);
    g_string_append(o,
        "void main() {\n"
        "    int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);\n"
        "    if (depth_stencil(x, y, 0.5, false)) {\n"
        "        write_color(x, y, vec4(1.0, 0.5, 0.25, 1.0));\n"
        "    }\n"
        "}\n");
    if (getenv("ADRENO_GLES_DEBUG")) {
        g_file_set_contents("adreno-gles-rop.frag", o->str, -1, NULL);
    }
    {
        const GLchar *parts[2];

        parts[0] = gles_version_line();
        parts[1] = o->str;
        glShaderSource(fs, 2, parts, NULL);
    }
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS_, &ok);
    if (ok) {
        info_report("msm-adreno:   smoke test: the render backend compiles "
                    "as GLSL ES");
    } else {
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        info_report("msm-adreno:   smoke test: the render backend does NOT "
                    "compile as GLSL ES: %s", log);
    }
    glDeleteShader(fs);
    g_string_free(o, true);
}

/* ------------------------------------------------------------------ */
/* The renderer                                                        */
/* ------------------------------------------------------------------ */

/*
 * What the host GPU holds.  The same job as AdrenoGl in msm_adreno_gl.c, with
 * the GL ES 3 equivalents: no direct state access (glGenTextures and friends
 * are gone), no sampler objects (the parameters ride on the texture), and no
 * buffer ranges, so what a draw reads is written into buffers of its own.
 */
typedef struct GlesProg {
    GlProgKey key;
    GLuint prog;
    bool exports_depth;
    AdrenoGlslInfo info, vinfo;
} GlesProg;

typedef struct GlesTexKey {
    uint32_t fc[6];
} GlesTexKey;

typedef struct GlesTex {
    GlesTexKey key;
    GLuint tex;
    uint64_t hash;
    uint32_t lo, hi;
    AdrenoMemStamp stamp;
    AdrenoTexImage im;
    uint64_t used;
    bool fresh;
} GlesTex;

#define GlesTexCacheMax   768

typedef struct AdrenoGles {
    GLuint vao;
    GLuint consts, draw, verts, vdata, idx;
    GLuint gmem_tex, fbo;
    GLuint dummy2d, dummy_arr;
    GHashTable *progs, *texes;
    AdrenoTlb tlb;
    uint8_t *scratch;
    size_t scratch_size;
    uint64_t ndraw;
    bool cpu_newer;          /* the CPU changed s->gmem */
    bool gpu_newer;          /* the GPU has drawn since the last copy back */
    /* GMEM snapshots for resolves done later */
    uint8_t *snap[32];
    int nsnap, snap_used;
} AdrenoGles;

static AdrenoGles *g_gles;       /* one machine per process */
static bool g_gles_ready;

static guint gles_prog_hash(gconstpointer k)
{
    const GlProgKey *p = k;

    return (guint)(p->ps_hash * 31 + p->vs_hash + p->ninterp +
                   (p->flat_mask << 8) + (p->geom << 16));
}

static gboolean gles_prog_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(GlProgKey)) == 0;
}

static guint gles_tex_hash(gconstpointer k)
{
    const GlesTexKey *t = k;

    return (guint)(t->fc[0] * 31 + t->fc[2] * 7 + t->fc[4]);
}

static gboolean gles_tex_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(GlesTexKey)) == 0;
}

static void *gles_scratch(size_t n)
{
    AdrenoGles *gls = g_gles;

    if (gls->scratch_size < n) {
        g_free(gls->scratch);
        gls->scratch = g_malloc(n);
        gls->scratch_size = n;
    }
    return gls->scratch;
}

static GLuint gles_compile(GLenum type, const char *src)
{
    GLuint id = glCreateShader(type);
    const GLchar *parts[2];
    GLint ok = 0;

    parts[0] = gles_version_line();
    parts[1] = src;
    glShaderSource(id, 2, parts, NULL);
    glCompileShader(id);
    glGetShaderiv(id, GL_COMPILE_STATUS_, &ok);
    if (!ok) {
        char log[4096] = "";

        glGetShaderInfoLog(id, sizeof(log), NULL, log);
        error_report("msm-adreno: GLSL ES shader did not compile: %s", log);
        glDeleteShader(id);
        return 0;
    }
    return id;
}

/* the pixel shader the guest's program translates to, and the vertex shader */
static GlesProg *gles_program(MsmAdrenoState *s, const AdrenoGlDraw *g)
{
    AdrenoGles *gls = g_gles;
    GlProgKey key;
    GlesProg *p;
    g_autoptr(GString) vs = NULL;
    g_autoptr(GString) fs = NULL;
    GLuint v, f;
    GLint ok = 0;

    memset(&key, 0, sizeof(key));
    key.ps_hash = g->ps->hash;
    key.ninterp = g->ninterp;
    key.flat_mask = g->flat_mask & ((1u << g->ninterp) - 1);
    key.param_gen = g->param_gen;
    key.param_gen_reg = g->param_gen ? g->param_gen_reg : 0;
    key.cube_mask = adreno_gl_cube_mask(s, g->ps->tex_mask);
    key.persp = g->persp;
    if (g->vs) {
        key.geom = (g->prim == A2_PT_POINTLIST ||
                    g->prim == A2_PT_POINTLIST_PSIZE) ? GEOM_POINTS
                                                      : GEOM_TRIS;
        key.vs_hash = g->vs->hash;
        key.vs_cube_mask = adreno_gl_cube_mask(s, g->vs->tex_mask);
    } else {
        key.geom = GEOM_CPU;
    }

    p = g_hash_table_lookup(gls->progs, &key);
    if (p) {
        return p->prog ? p : NULL;
    }
    p = g_new0(GlesProg, 1);
    p->key = key;
    p->exports_depth = g->ps->exports_depth;
    g_hash_table_insert(gls->progs, &p->key, p);

    if (key.geom == GEOM_CPU) {
        vs = vs_source_cpu_gles(key.ninterp);
    } else {
        vs = vs_source_gles(&key, g->vs, &p->vinfo);
    }
    fs = fs_source_gles(g->ps, key.cube_mask, key.ninterp, key.flat_mask,
                        key.param_gen, key.param_gen_reg,
                        p->exports_depth, &p->info);
    if (getenv("ADRENO_GLES_DEBUG")) {
        g_file_set_contents("adreno-gles.vs.glsl", vs->str, -1, NULL);
        g_file_set_contents("adreno-gles.fs.glsl", fs->str, -1, NULL);
    }
    v = gles_compile(GL_VERTEX_SHADER_, vs->str);
    f = gles_compile(GL_FRAGMENT_SHADER_, fs->str);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return NULL;
    }
    p->prog = glCreateProgram();
    glAttachShader(p->prog, v);
    glAttachShader(p->prog, f);
    glLinkProgram(p->prog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p->prog, GL_LINK_STATUS_, &ok);
    if (!ok) {
        char log[4096] = "";

        glGetProgramInfoLog(p->prog, sizeof(log), NULL, log);
        error_report("msm-adreno: GLSL ES program did not link: %s", log);
        glDeleteProgram(p->prog);
        p->prog = 0;
        return NULL;
    }
    return p;
}

/*
 * Textures: the guest's own memory decoded into a host texture, kept while
 * the memory under it hashes the same, as in msm_adreno_gl.c.  GL ES has no
 * sampler objects, so the parameters of the fetch constant ride on the
 * texture itself.
 */
static GLenum gles_wrap(unsigned m)
{
    return m == 1 ? GL_MIRRORED_REPEAT_ : m == 3 ? GL_CLAMP_TO_BORDER_ :
           m == 2 ? GL_CLAMP_TO_EDGE_ : GL_REPEAT_;
}

static GlesTex *gles_texture(MsmAdrenoState *s, unsigned idx)
{
    AdrenoGles *gls = g_gles;
    const uint32_t *fc = &s->regs[A2_SQ_FETCH_0 + idx * 6];
    GlesTexKey key;
    GlesTex *t;
    AdrenoTexImage im;
    uint64_t h;
    uint32_t lo, hi;
    bool fresh = false;

    /* what the texels depend on: not clamping, filters, swizzle, bias */
    key.fc[0] = fc[0] & ~(0x1ffu << 10);
    key.fc[1] = fc[1];
    key.fc[2] = fc[2];
    key.fc[3] = fc[3] & 1;
    key.fc[4] = fc[4] & 0x3fc;
    key.fc[5] = fc[5] & ~3u;
    /* a resolve not done yet may have written what we are about to read */
    adreno_resolves_flush(s);
    t = g_hash_table_lookup(gls->texes, &key);
    if (t && t->tex && t->hash == adreno_tex_hash(s, &gls->tlb, idx, &lo, &hi)) {
        t->stamp.kick = s->kick_epoch;
        t->stamp.writes = s->gpu_writes;
        t->used = gls->ndraw;
        return t;
    }
    if (!t) {
        t = g_new0(GlesTex, 1);
        t->key = key;
        g_hash_table_insert(gls->texes, &t->key, t);
    }
    h = adreno_tex_hash(s, &gls->tlb, idx, &lo, &hi);
    if (t->tex && t->hash == h) {
        t->stamp.kick = s->kick_epoch;
        t->stamp.writes = s->gpu_writes;
        t->used = gls->ndraw;
        return t;
    }
    t->hash = h;
    t->lo = lo;
    t->hi = hi;
    t->stamp.kick = s->kick_epoch;
    t->stamp.writes = s->gpu_writes;
    t->used = gls->ndraw;
    if (!adreno_tex_image(s, idx, &im)) {
        if (t->tex) {
            glDeleteTextures(1, &t->tex);
            t->tex = 0;
        }
        return NULL;
    }
    im.levels = MIN(im.levels, 32 - clz32(MAX(im.w, im.h)));
    if (!t->tex || t->im.w != im.w || t->im.h != im.h ||
        t->im.levels != im.levels || t->im.layers != im.layers ||
        t->im.is_float != im.is_float) {
        GLenum fmt = im.is_float ? GL_RGBA32F_ : GL_RGBA8_;

        if (t->tex) {
            glDeleteTextures(1, &t->tex);
        }
        glGenTextures(1, &t->tex);
        glBindTexture(im.layers > 1 ? GL_TEXTURE_2D_ARRAY_ : GL_TEXTURE_2D_,
                      t->tex);
        if (im.layers > 1) {
            glTexStorage3D(GL_TEXTURE_2D_ARRAY_, im.levels, fmt, im.w, im.h,
                           im.layers);
        } else {
            glTexStorage2D(GL_TEXTURE_2D_, im.levels, fmt, im.w, im.h);
        }
        fresh = true;
    }
    t->im = im;
    glBindTexture(im.layers > 1 ? GL_TEXTURE_2D_ARRAY_ : GL_TEXTURE_2D_, t->tex);
    {
        size_t texel = im.is_float ? 16 : 4;
        void *buf = gles_scratch((size_t)im.w * im.h * texel);
        GLenum type = im.is_float ? GL_FLOAT_ : GL_UNSIGNED_BYTE_;
        unsigned mag = im.mag, min = im.min, mip = im.mip;

        for (int layer = 0; layer < im.layers; layer++) {
            for (int level = 0; level < im.levels; level++) {
                uint32_t w = MAX(im.w >> level, 1u);
                uint32_t hh = MAX(im.h >> level, 1u);

                adreno_tex_decode(s, &gls->tlb, idx, level, layer, buf);
                if (im.layers > 1) {
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY_, level, 0, 0, layer, w,
                                    hh, 1, GL_RGBA_, type, buf);
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D_, level, 0, 0, w, hh, GL_RGBA_,
                                    type, buf);
                }
            }
        }
        /* GL ES has no sampler objects: the filters live on the texture */
        glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_,
                        mag == 1 ? GL_LINEAR_ : GL_NEAREST_);
        if (mip == 2 || im.levels <= 1) {
            glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_,
                            min == 1 ? GL_LINEAR_ : GL_NEAREST_);
        } else {
            glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_,
                            min == 1 ? (mip == 1 ? GL_LINEAR_MIPMAP_LINEAR_
                                                 : GL_LINEAR_)
                                     : (mip == 1 ? GL_NEAREST_
                                                : GL_NEAREST_MIPMAP_LINEAR_));
        }
        glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, gles_wrap(im.clamp[0]));
        glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, gles_wrap(im.clamp[1]));
        glTexParameterf(GL_TEXTURE_2D_, GL_TEXTURE_MIN_LOD_, im.min_level);
        glTexParameterf(GL_TEXTURE_2D_, GL_TEXTURE_MAX_LOD_, im.max_level);
        glTexParameterf(GL_TEXTURE_2D_, GL_TEXTURE_LOD_BIAS_, im.lod_bias);
    }
    adreno_trace(s, "  gles: texture %u %ux%u x%d levels %d %s\n", idx, im.w,
                 im.h, im.layers, im.levels, fresh ? "new" : "changed");
    return t;
}

/* the fetch constants' textures onto texture units, and what they are */
static void gles_bind_textures(MsmAdrenoState *s, const AdrenoGlslInfo *info,
                               uint32_t cubes, int base, DrawU *du)
{
    AdrenoGles *gls = g_gles;

    for (int i = 0; i < info->ntex; i++) {
        const AdrenoGlslTexUnit *tu = &info->tex[i];
        bool cube = cubes & (1u << tu->const_idx);
        GlesTex *t = gles_texture(s, tu->const_idx);
        int u = base + i;
        TexU *x = &du->tex[u];

        glActiveTexture(GL_TEXTURE0_ + u);
        if (!t || (t->im.layers > 1) != cube) {
            /* no such texture: (0, 0, 0, 1), as the software sampler */
            glBindTexture(cube ? GL_TEXTURE_2D_ARRAY_ : GL_TEXTURE_2D_,
                          cube ? gls->dummy_arr : gls->dummy2d);
            x->size[0] = x->size[1] = x->size[2] = x->size[3] = 1.0f;
            x->scale[0] = 1.0f;
            for (int k = 0; k < 4; k++) {
                x->swz[k] = k;
            }
            continue;
        }
        glBindTexture(cube ? GL_TEXTURE_2D_ARRAY_ : GL_TEXTURE_2D_, t->tex);
        x->size[0] = t->im.w;
        x->size[1] = t->im.h;
        x->size[2] = 1.0f / t->im.w;
        x->size[3] = 1.0f / t->im.h;
        x->scale[0] = ldexpf(1.0f, t->im.exp_adj);
        x->scale[1] = t->im.lod_bias;
        for (int k = 0; k < 4; k++) {
            x->swz[k] = t->im.swz[k];
        }
    }
}

/* ------------------------------------------------------------------ */
/* GMEM                                                                */
/* ------------------------------------------------------------------ */

/*
 * GMEM is a 256 KiB image of 65536 unsigned ints, laid out so a byte address
 * is a texel index.  The CPU's copy is s->gmem; the two are copied across
 * when one of them changed, the way the desktop backend moves its buffer.
 */
static void gles_gmem_upload(MsmAdrenoState *s)
{
    AdrenoGles *gls = g_gles;

    if (gls->cpu_newer) {
        glBindTexture(GL_TEXTURE_2D_, gls->gmem_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT_, 4);
        glTexSubImage2D(GL_TEXTURE_2D_, 0, 0, 0, GMEM_TEXELS_W, GMEM_TEXELS_H,
                        GL_RED_INTEGER_, GL_UNSIGNED_INT_, s->gmem);
        gls->cpu_newer = false;
    }
}

void adreno_gles_gmem_to_cpu(MsmAdrenoState *s)
{
    AdrenoGles *gls = g_gles;

    if (!gls || !gls->gpu_newer) {
        return;
    }
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER_, gls->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT_, 4);
    glReadPixels(0, 0, GMEM_TEXELS_W, GMEM_TEXELS_H, GL_RED_INTEGER_,
                 GL_UNSIGNED_INT_, s->gmem);
    gls->gpu_newer = false;
    gls->cpu_newer = true;
}

void adreno_gles_gmem_from_cpu(MsmAdrenoState *s)
{
    AdrenoGles *gls = g_gles;

    if (gls) {
        gls->cpu_newer = true;
    }
}

/*
 * A copy of GMEM for a resolve that happens later: a read-back now, a pointer
 * the resolve can walk when it comes round.
 */
bool adreno_gles_gmem_snapshot(MsmAdrenoState *s, int *slot)
{
    AdrenoGles *gls = g_gles;

    if (!gls || gls->snap_used >= ARRAY_SIZE(gls->snap)) {
        return false;
    }
    adreno_gles_gmem_to_cpu(s);
    gls->snap[gls->snap_used] = g_malloc(ADRENO_GMEM_SIZE);
    memcpy(gls->snap[gls->snap_used], s->gmem, ADRENO_GMEM_SIZE);
    *slot = gls->snap_used++;
    return true;
}

void adreno_gles_snapshots_wait(MsmAdrenoState *s)
{
}

uint8_t *adreno_gles_snapshot_ptr(MsmAdrenoState *s, int slot)
{
    AdrenoGles *gls = g_gles;

    return gls && slot >= 0 && slot < gls->snap_used ? gls->snap[slot] : NULL;
}

void adreno_gles_snapshots_release(MsmAdrenoState *s)
{
    AdrenoGles *gls = g_gles;

    for (int i = 0; i < gls->snap_used; i++) {
        g_free(gls->snap[i]);
        gls->snap[i] = NULL;
    }
    gls->snap_used = 0;
}

/*
 * The guest's vertex buffers, copied into vd[] for the vertex shader to fetch
 * from, and the draw's indices: GL ES 3.1 has no buffer ranges, so vd[] and
 * the indices each get a buffer of their own.
 */
static void gles_upload_vertices(MsmAdrenoState *s, const GlesProg *p,
                                const AdrenoGlDraw *g, DrawU *du)
{
    AdrenoGles *gls = g_gles;
    const AdrenoGlslInfo *vi = &p->vinfo;
    size_t total = 16, base, at;
    int64_t lo[ADRENO_GL_MAX_VBUFS], hi[ADRENO_GL_MAX_VBUFS];
    uint8_t *buf;

    for (int i = 0; i < vi->nvtx; i++) {
        uint32_t stride = vi->vtx_stride[i];

        lo[i] = (int64_t)g->min_idx * stride + vi->vtx_lo[i];
        hi[i] = (int64_t)g->max_idx * stride + vi->vtx_hi[i];
        hi[i] = MIN(hi[i], lo[i] + (16 << 20));
        total += ROUND_UP(hi[i] - lo[i], 16);
    }
    buf = g_malloc(total);
    base = 0;
    at = 0;
    for (int i = 0; i < vi->nvtx; i++) {
        unsigned c = vi->vtx[i];
        uint32_t fc0 = s->regs[A2_SQ_FETCH_0 + c * 2];
        uint32_t fc1 = s->regs[A2_SQ_FETCH_0 + c * 2 + 1];
        uint32_t len = hi[i] - lo[i];

        adreno_tlb_read(s, &gls->tlb, (fc0 & ~3u) + (uint32_t)lo[i],
                        buf + at, len);
        du->vb[i][0] = (base + at) / 4;
        du->vb[i][1] = lo[i];
        du->vb[i][2] = fc1 & 3;
        du->vb[i][3] = len;
        at += ROUND_UP(len, 16);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER_, gls->vdata);
    glBufferData(GL_SHADER_STORAGE_BUFFER_, total, buf, GL_DYNAMIC_DRAW_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER_, 2, gls->vdata);
    g_free(buf);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, gls->idx);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER_, g->nidx * 4, g->idx,
                 GL_DYNAMIC_DRAW_);
}

/* ------------------------------------------------------------------ */
/* Draws                                                               */
/* ------------------------------------------------------------------ */

void adreno_gles_draw(MsmAdrenoState *s, const AdrenoGlDraw *g)
{
    AdrenoGles *gls = g_gles;
    const uint32_t *r = s->regs;
    const AdrenoColorFmt *cf;
    GlesProg *p;
    DrawU du;
    uint32_t cc = r[A2_RB_COLORCONTROL], bc = r[A2_RB_BLEND_CONTROL];
    size_t vbytes;
    GLint loc;

    if (!g->nverts || g->clip_x0 >= g->clip_x1 || g->clip_y0 >= g->clip_y1) {
        return;
    }
    gls->ndraw++;
    adreno_tlb_flush(&gls->tlb);
    p = gles_program(s, g);
    if (!p) {
        return;
    }
    gles_gmem_upload(s);

    /* what the draw reads */
    memset(&du, 0, sizeof(du));
    gles_bind_textures(s, &p->info, p->key.cube_mask, 0, &du);
    memcpy(du.bools, &r[A2_SQ_CF_BOOLEANS], sizeof(du.bools));
    memcpy(du.loops, &r[A2_SQ_CF_LOOP], sizeof(du.loops));
    du.cb[0] = r[A2_SQ_PS_CONST] & 0x1ff;
    du.cb[1] = r[A2_SQ_VS_CONST] & 0x1ff;
    cf = adreno_color_fmt(g->color_fmt);
    du.csurf[0] = g->color_off;
    du.csurf[1] = g->pitch;
    du.csurf[2] = cf->bpp;
    du.csurf[3] = g->sx | g->sy << 4;
    for (int k = 0; k < 4; k++) {
        du.cfmt[k] = cf->bits[k];
    }
    du.cinfo[0] = cf->kind;
    du.cinfo[1] = g->color_swap;
    du.cinfo[2] = g->color_write;
    du.cinfo[3] = r[A2_RB_COLOR_MASK] & 0xf;
    du.dsurf[0] = g->depth_off;
    du.dsurf[1] = g->depth_24 ? 4 : 2;
    du.rb[0] = r[A2_RB_DEPTHCONTROL];
    du.rb[1] = bc;
    du.rb[2] = cc;
    du.rb[3] = r[A2_RB_STENCILREFMASK];
    du.rb2[0] = r[A2_RB_STENCILREFMASK_BF];
    du.rb2[1] = !(cc & (1 << 5)) && bc != 0x00010001;
    du.blendc[0] = (r[A2_RB_BLEND_RED] & 0xff) / 255.0f;
    du.blendc[1] = (r[A2_RB_BLEND_GREEN] & 0xff) / 255.0f;
    du.blendc[2] = (r[A2_RB_BLEND_BLUE] & 0xff) / 255.0f;
    du.blendc[3] = (r[A2_RB_BLEND_ALPHA] & 0xff) / 255.0f;
    du.misc[0] = adreno_f(r[A2_RB_ALPHA_REF]);
    du.misc[1] = g->win_x;
    du.misc[2] = g->win_y;
    du.misc[3] = g->pix_ofs;

    /*
     * GL ES 3.1 has no buffer ranges, so each of the three blocks a draw
     * reads is written whole into a buffer of its own.
     */
    if (s->const_dirty) {
        glBindBuffer(GL_UNIFORM_BUFFER_, gls->consts);
        glBufferData(GL_UNIFORM_BUFFER_, 512 * 16, &r[A2_SQ_CONSTANT_0],
                     GL_DYNAMIC_DRAW_);
        s->const_dirty = false;
    }
    glBindBuffer(GL_UNIFORM_BUFFER_, gls->draw);
    glBufferData(GL_UNIFORM_BUFFER_, sizeof(du), &du, GL_DYNAMIC_DRAW_);
    if (p->key.geom == GEOM_CPU) {
        vbytes = (size_t)g->nverts * ADRENO_GL_VTX_VEC4(g->ninterp) * 16;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER_, gls->verts);
        glBufferData(GL_SHADER_STORAGE_BUFFER_, vbytes, g->verts,
                     GL_DYNAMIC_DRAW_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER_, 1, gls->verts);
    } else {
        gles_upload_vertices(s, p, g, &du);
    }

    glUseProgram(p->prog);
    {
        GLuint cb = glGetUniformBlockIndex(p->prog, "Consts");
        GLuint db = glGetUniformBlockIndex(p->prog, "Draw");

        if (cb != ~0u) {
            glUniformBlockBinding(p->prog, cb, 0);
            glBindBufferBase(GL_UNIFORM_BUFFER_, 0, gls->consts);
        }
        if (db != ~0u) {
            glUniformBlockBinding(p->prog, db, 1);
            glBindBufferBase(GL_UNIFORM_BUFFER_, 1, gls->draw);
        }
    }

    /* GMEM is both an image the shaders write and the attachment they read */
    glActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D_, gls->gmem_tex);
    loc = glGetUniformLocation(p->prog, "u_gmem");
    if (loc >= 0) {
        glUniform1i(loc, 0);
    }

    glScissor(g->clip_x0, g->clip_y0, g->clip_x1 - g->clip_x0,
              g->clip_y1 - g->clip_y0);
    /*
     * The render backend runs in the shaders, so the host's own depth,
     * stencil and blending stay out of the way - as in the desktop backend.
     */
    glDisable(GL_DEPTH_TEST_);
    glDisable(GL_BLEND_);
    {
        uint32_t sc = r[A2_PA_SU_SC_MODE_CNTL];

        if (p->key.geom == GEOM_TRIS && (sc & 3)) {
            glEnable(GL_CULL_FACE_);
            glCullFace((sc & 3) == 3 ? GL_FRONT_AND_BACK_ :
                       (sc & 1) ? GL_FRONT_ : GL_BACK_);
        } else {
            glDisable(GL_CULL_FACE_);
        }
        /* window y is the guest's (down), so the winding is the other way */
        glFrontFace((sc & (1 << 2)) ? GL_CCW_ : GL_CW_);
    }
    if (p->key.geom == GEOM_CPU) {
        glDrawArrays(GL_TRIANGLES_, 0, g->nverts);
    } else {
        GLenum mode = p->key.geom == GEOM_POINTS ? GL_POINTS_ :
                      g->prim == A2_PT_TRISTRIP ? GL_TRIANGLE_STRIP_ :
                      g->prim == A2_PT_TRIFAN ? GL_TRIANGLE_FAN_ :
                      GL_TRIANGLES_;

        glDrawElements(mode, g->nidx, GL_UNSIGNED_INT_, (void *)0);
    }
    gls->gpu_newer = true;
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

/* what a texture with no memory behind it reads as: (0, 0, 0, 1) */
static GLuint gles_dummy(GLenum target, bool array)
{
    GLuint t;
    static const uint8_t black[4] = { 0, 0, 0, 255 };

    glGenTextures(1, &t);
    glBindTexture(target, t);
    if (array) {
        glTexStorage3D(target, 1, GL_RGBA8_, 1, 1, 1);
        glTexSubImage3D(target, 0, 0, 0, 0, 1, 1, 1, GL_RGBA_,
                        GL_UNSIGNED_BYTE_, black);
    } else {
        glTexStorage2D(target, 1, GL_RGBA8_, 1, 1);
        glTexSubImage2D(target, 0, 0, 0, 1, 1, GL_RGBA_,
                        GL_UNSIGNED_BYTE_, black);
    }
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER_, GL_NEAREST_);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER_, GL_NEAREST_);
    return t;
}

/*
 * Everything a draw needs, made once on the thread that draws.  GMEM is the
 * colour attachment of an empty framebuffer the size of the window space a
 * draw can reach, so the shaders can read what they wrote.
 */
static bool gles_render_init(MsmAdrenoState *s)
{
    AdrenoGles *gls = g_gles;
    GLuint dummy_arr = 0, dummy2d = 0;

    gls->progs = g_hash_table_new(gles_prog_hash, gles_prog_equal);
    gls->texes = g_hash_table_new(gles_tex_hash, gles_tex_equal);

    glGenVertexArrays(1, &gls->vao);
    glBindVertexArray(gls->vao);
    glGenBuffers(1, &gls->consts);
    glGenBuffers(1, &gls->draw);
    glGenBuffers(1, &gls->verts);
    glGenBuffers(1, &gls->vdata);
    glGenBuffers(1, &gls->idx);
    /* the vertices are read by index: one attribute of vec4s */
    glBindBuffer(GL_ARRAY_BUFFER_, gls->verts);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT_, GL_FALSE_, 0, (void *)0);

    glGenTextures(1, &gls->gmem_tex);
    glBindTexture(GL_TEXTURE_2D_, gls->gmem_tex);
    glTexStorage2D(GL_TEXTURE_2D_, 1, GL_R32UI_, GMEM_TEXELS_W, GMEM_TEXELS_H);
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_NEAREST_);
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_NEAREST_);
    glGenFramebuffers(1, &gls->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER_, gls->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_,
                           gls->gmem_tex, 0);
    glViewport(0, 0, FB_SIZE, FB_SIZE);
    glEnable(GL_SCISSOR_TEST_);
    glDisable(GL_DEPTH_TEST_);
    glDisable(GL_STENCIL_TEST_);
    glDisable(GL_CULL_FACE_);
    glDisable(GL_BLEND_);
    glDisable(GL_DITHER_);

    dummy2d = gles_dummy(GL_TEXTURE_2D_, false);
    dummy_arr = gles_dummy(GL_TEXTURE_2D_ARRAY_, true);
    gls->dummy2d = dummy2d;
    gls->dummy_arr = dummy_arr;

    gls->cpu_newer = true;
    s->const_dirty = true;
    g_gles_ready = true;
    info_report("msm-adreno: rendering on this host GPU with OpenGL ES");
    return true;
}

/*
 * Everything the renderer will ask of the host, asked once: a shader in GLSL
 * ES, a vertex array, a draw, an image store (how GMEM is written), an atomic
 * read-modify-write (how a pixel of a three-byte format is put back), and the
 * read-back.  Whatever this cannot do, the renderer must not use.
 */
static void gles_smoke_test(void)
{
    static const char vs[] =
        "layout(location = 0) in highp vec2 a_pos;\n"
        "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    /*
     * Writes GMEM the way the render backend will: through an image.  The
     * atomic read-modify-write is what a pixel of a three-byte format needs
     * (its neighbours share the word), so a driver without image atomics
     * cannot run the backend - the smoke test says so and stops there.
     */
    static const char fs_head[] =
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp usampler2D;\n"
        "precision highp uimage2D;\n"
        "layout(r32ui) uniform uimage2D u_gmem;\n"
        "out uvec4 o_col;\n";
    static const char fs_atomic[] =
        "void main() {\n"
        "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
        "    uint v = imageAtomicOr(u_gmem, p, 0x0a0bu);\n"
        "    imageStore(u_gmem, p, uvec4(v | 0x00c0u, 0u, 0u, 1u));\n"
        "    o_col = uvec4(v | 0xc0u, 0u, 0u, 1u);\n"
        "}\n";
    static const char fs_plain[] =
        "void main() {\n"
        "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
        "    imageStore(u_gmem, p, uvec4(0x000000c0u, 0u, 0u, 1u));\n"
        "    o_col = uvec4(0xc0u, 0u, 0u, 1u);\n"
        "}\n";
    static const float verts[] = { -1, -1, 3, -1, -1, 3 };
    GLuint tex = 0, fbo = 0, vao = 0, vbo = 0, vs_id = 0, fs_id = 0, prog = 0;
    GLint ok = 0;
    GLuint out[4] = { 0, 0, 0, 0 };   /* four red texels */
    bool atomic_gmem = false;
    char log[512];

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_, tex);
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_NEAREST_);
    glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_NEAREST_);
    glTexImage2D(GL_TEXTURE_2D_, 0, GL_R32UI_, 1024, 64, 0, GL_RED_INTEGER_,
                 GL_UNSIGNED_INT_, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER_, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_,
                           tex, 0);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER_) != 0x8CD5 /* COMPLETE */) {
        info_report("msm-adreno:   smoke test: no framebuffer (0x%x)",
                    gl.CheckFramebufferStatus(GL_FRAMEBUFFER_));
        goto out;
    }

    vs_id = glCreateShader(GL_VERTEX_SHADER_);
    {
        const GLchar *src[2];

        src[0] = gles_version_line();
        src[1] = vs;
        glShaderSource(vs_id, 2, src, NULL);
    }
    glCompileShader(vs_id);
    glGetShaderiv(vs_id, GL_COMPILE_STATUS_, &ok);
    if (!ok) {
        glGetShaderInfoLog(vs_id, sizeof(log), NULL, log);
        info_report("msm-adreno:   smoke test: vertex shader: %s", log);
        goto out;
    }
    fs_id = glCreateShader(GL_FRAGMENT_SHADER_);
    {
        const GLchar *src[3];

        src[0] = gles_version_line();
        src[1] = fs_head;
        src[2] = fs_atomic;
        glShaderSource(fs_id, 3, src, NULL);
    }
    glCompileShader(fs_id);
    glGetShaderiv(fs_id, GL_COMPILE_STATUS_, &ok);
    if (!ok) {
        const GLchar *src[3];

        /* no image atomics: the renderer cannot use this host, but say what
         * it can do */
        glGetShaderInfoLog(fs_id, sizeof(log), NULL, log);
        info_report("msm-adreno:   smoke test: no image atomics (%s)", log);
        src[0] = gles_version_line();
        src[1] = fs_head;
        src[2] = fs_plain;
        glShaderSource(fs_id, 3, src, NULL);
        glCompileShader(fs_id);
        glGetShaderiv(fs_id, GL_COMPILE_STATUS_, &ok);
        if (!ok) {
            glGetShaderInfoLog(fs_id, sizeof(log), NULL, log);
            info_report("msm-adreno:   smoke test: pixel shader: %s", log);
            goto out;
        }
        atomic_gmem = false;
    } else {
        atomic_gmem = true;
    }
    g_smoke_atomics = atomic_gmem;
    prog = glCreateProgram();
    glAttachShader(prog, vs_id);
    glAttachShader(prog, fs_id);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS_, &ok);
    if (!ok) {
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        info_report("msm-adreno:   smoke test: link: %s", log);
        goto out;
    }

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER_, vbo);
    glBufferData(GL_ARRAY_BUFFER_, sizeof(verts), verts, GL_STATIC_DRAW_);
    glVertexAttribPointer(0, 2, GL_FLOAT_, GL_FALSE_, 0, (void *)0);
    glEnableVertexAttribArray(0);

    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_gmem"), 0);
    glActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D_, tex);
    glViewport(0, 0, 1024, 64);
    glDisable(GL_SCISSOR_TEST_);
    glDisable(GL_BLEND_);
    {
        static const GLuint zero[4] = { 0, 0, 0, 0 };

        glClearBufferuiv(GL_COLOR_, 0, zero);
    }
    glDrawArrays(GL_TRIANGLES_, 0, 3);
    glFinish();
    glReadPixels(0, 0, 4, 1, GL_RED_INTEGER_, GL_UNSIGNED_INT_, out);
    info_report("msm-adreno:   smoke test: GMEM image store + atomic read "
                "gave %08x %08x %08x (want 000000c0 each)", out[0], out[1],
                out[2]);
    if (out[0] == 0xc0 && out[1] == 0xc0 && out[2] == 0xc0) {
        info_report("msm-adreno:   smoke test: OK, the host GPU runs the "
                    "translated shaders and writes GMEM%s",
                    atomic_gmem ? ", with atomics" : " (no atomics: the "
                    "renderer needs them for three-byte formats)");
    } else {
        info_report("msm-adreno:   smoke test: the draw did not land as "
                    "asked");
    }
out:
    glDeleteProgram(prog);
    glDeleteShader(fs_id);
    glDeleteShader(vs_id);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glBindFramebuffer(GL_FRAMEBUFFER_, 0);
}

void adreno_gles_probe(MsmAdrenoState *s)
{
    if (!s->backend || strcmp(s->backend, "gles")) {
        return;
    }
    if (!gles_init("GLES 3")) {
        return;
    }
    gles_smoke_test();
    gatomics_gles = g_smoke_atomics;
    gles_rop_smoke_test();
    /* the real context belongs to the thread that draws */
    gles_teardown();
}

bool adreno_gles_available(MsmAdrenoState *s)
{
    if (!s->backend || strcmp(s->backend, "gles")) {
        return false;
    }
    if (!g_asked) {
        g_asked = true;
        info_report("msm-adreno: the guest asked for the host GPU");
    }
    /*
     * GMEM's per-pixel read-modify-write has to stay in primitive order.
     * Desktop OpenGL does that with fragment shader interlock; here it is
     * framebuffer fetch, and a host with neither is a software renderer.
     */
    if (g_ctx_up &&
        !has_extension("GL_EXT_shader_framebuffer_fetch") &&
        !has_extension("GL_ARM_shader_framebuffer_fetch") &&
        !has_extension("GL_OES_shader_framebuffer_fetch")) {
        error_report("msm-adreno: this host GPU cannot read the framebuffer "
                     "in a fragment shader (no framebuffer fetch): the draws "
                     "would not stay in order");
        gles_teardown();
        return false;
    }
    if (!gles_tried) {
        gles_tried = true;
        if (gles_init("GLES 3")) {
            gles_render_ready = true;
            qemu_thread_get_self(&gles_owner);
        }
    }
    if (!gles_render_ready) {
        return false;
    }
    if (!g_gles_ready && !gles_render_init(s)) {
        return false;
    }
    /* the context lives on the thread that made it */
    return qemu_thread_is_self(&gles_owner);
}
