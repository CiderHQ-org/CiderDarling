#include <stdlib.h>
#include <stdio.h>  // for fprintf(stderr, "unimplemented")

#include <pthread.h>

#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLInternal.h>
#include <CoreFoundation/CFDictionary.h>
#include <pthread.h>

// Try to get the right (generic) type definitions.
// In particular, we really want EGLNativeDisplayType to be void *,
// not int as it is if __APPLE__ is defined.
#undef APPLE
#undef __APPLE__

#define __unix__
#define EGL_NO_X11

#include <EGL/egl.h>

#define APPLE
#define __APPLE__

static EGLDisplay display;

static EGLConfig config;
static int num_config;

static EGLint const attribute_list[] = {
    EGL_RED_SIZE, 1,
    EGL_GREEN_SIZE, 1,
    EGL_BLUE_SIZE, 1,
    EGL_NONE
};

struct _CGLDisplay
{
    EGLDisplay display;
    EGLConfig config;
    int num_config;
};

static CFMutableDictionaryRef g_displays;
static pthread_mutex_t g_displaysMutex = PTHREAD_MUTEX_INITIALIZER;

struct _CGLContextObj {
    GLuint retain_count;
    pthread_mutex_t lock;
    EGLContext egl_context;
    EGLSurface egl_surface;
    // EGL has no function for getting the current swap interval,
    // so we need to save the last set value. The default is 1.
    int swap_interval;
};

struct _CGLPixelFormatObj {
    GLuint retain_count;
    CGLPixelFormatAttribute *attributes;
};

static inline int attribute_has_argument(CGLPixelFormatAttribute attr) {
    switch (attr) {
    case kCGLPFAAuxBuffers:
    case kCGLPFAColorSize:
    case kCGLPFAAlphaSize:
    case kCGLPFADepthSize:
    case kCGLPFAStencilSize:
    case kCGLPFAAccumSize:
    case kCGLPFASampleBuffers:
    case kCGLPFASamples:
    case kCGLPFAOpenGLProfile:
    case kCGLPFAVirtualScreenCount:
    case kCGLPFARendererID:
    case kCGLPFADisplayMask:
        return 1;
    default:
        return 0;
   }
}

__attribute__((constructor))
static void _CGLInitialize(void)
{
    g_displays = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
}

static int attributes_count(const CGLPixelFormatAttribute *attrs) {
    int result;
    for (result = 0; attrs[result] != 0; result++) {
        if (attribute_has_argument(attrs[result])) {
            result++;
        }
    }
    return result;
}

CGLError CGLRegisterNativeDisplay(void *native_display) {

    display = eglGetDisplay(native_display);

    if (display == EGL_NO_DISPLAY) {
        return kCGLBadConnection;
    }

    eglInitialize(display, NULL, NULL);
    eglChooseConfig(display, attribute_list, &config, 1, &num_config);

    eglBindAPI(EGL_OPENGL_API);

    return kCGLNoError;
}

static struct _CGLDisplay* getCGLDisplay(CGSConnectionID cid)
{
    struct _CGLDisplay* rv;

    pthread_mutex_lock(&g_displaysMutex);
    rv = (struct _CGLDisplay*) CFDictionaryGetValue(g_displays, (const void*)(unsigned long) cid);
    pthread_mutex_unlock(&g_displaysMutex);

    if (!rv)
    {
        EGLDisplay disp = eglGetDisplay(_CGSNativeDisplay(cid));
        if (disp == EGL_NO_DISPLAY)
            return NULL;

        rv = (struct _CGLDisplay*) malloc(sizeof(*rv));
        rv->display = disp;

        eglInitialize(rv->display, NULL, NULL);
        eglChooseConfig(rv->display, attribute_list, &rv->config, 1, &rv->num_config);

        eglBindAPI(EGL_OPENGL_API);

        pthread_mutex_lock(&g_displaysMutex);
        CFDictionaryAddValue(g_displays, (const void*)(unsigned long) cid, rv);
        pthread_mutex_unlock(&g_displaysMutex);
    }

    return rv;
}

CGLError CGLSetSurface(CGLContextObj gl, CGSConnectionID cid, CGSWindowID wid, CGSSurfaceID sid)
{
    struct _CGLDisplay* disp = getCGLDisplay(cid);
    if (!disp)
        return kCGLBadConnection;

    EGLNativeWindowType window;
    if (sid)
        window = (EGLNativeWindowType) _CGSNativeWindowForSurfaceID(cid, wid, sid);
    else
        window = (EGLNativeWindowType) _CGSNativeWindowForID(cid, wid);

    if (!window)
        return kCGLBadWindow;

    gl->egl_surface = eglCreateWindowSurface(disp->display, disp->config, window, NULL);
    if (gl->egl_surface == EGL_NO_SURFACE)
        return kCGLBadState;
    return kCGLNoError;
}

CGLWindowRef CGLGetWindow(void *native_window) {

    EGLNativeWindowType window = (EGLNativeWindowType) native_window;
    EGLSurface surface = eglCreateWindowSurface(display, config, window, NULL);

    if (surface == EGL_NO_SURFACE) {
        return NULL;
    }

    return (CGLWindowRef) surface;
}

CGL_EXPORT void CGLDestroyWindow(CGLWindowRef window) {
    eglDestroySurface(display, (EGLSurface) window);
}

CGL_EXPORT CGLError CGLContextMakeCurrentAndAttachToWindow(CGLContextObj context, CGLWindowRef window) {
    if (!context)
        return kCGLBadContext;
    EGLSurface previous = context->egl_surface;
    context->egl_surface = (EGLSurface) window;
    CGLError error = CGLSetCurrentContext(context);
    if (error != kCGLNoError)
        context->egl_surface = previous;
    return error;
}

static pthread_key_t current_context_key;

static void make_key() {
    pthread_key_create(&current_context_key, NULL);
}

static pthread_key_t get_current_context_key() {
    static pthread_once_t key_once = PTHREAD_ONCE_INIT;
    pthread_once(&key_once, make_key);
    return current_context_key;
}

CGLContextObj CGLGetCurrentContext(void) {
    pthread_key_t key = get_current_context_key();
    return pthread_getspecific(key);
}

CGLError CGLSetCurrentContext(CGLContextObj context) {
    pthread_key_t key = get_current_context_key();
    // EGL's current API is thread-local and initially defaults to GLES.
    if (!eglBindAPI(EGL_OPENGL_API))
        return kCGLBadContext;

    if (context != NULL) {
        EGLSurface surface = context->egl_surface;
        if (!eglMakeCurrent(display, surface, surface, context->egl_context))
            return kCGLBadContext;
    } else {
        if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
            return kCGLBadContext;
    }

    // Publish only a context actually accepted by the host driver.
    // A failed switch must leave CGLGetCurrentContext consistent with EGL.
    if (pthread_setspecific(key, context) != 0)
        return kCGLBadAlloc;
    return kCGLNoError;
}

CGLError CGLSetFullScreen(CGLContextObj ctx) {
    printf("STUB: CGLSetFullScreen\n");

    return kCGLNoError;
}

CGLError CGLChoosePixelFormat(
    const CGLPixelFormatAttribute *attrs,
    CGLPixelFormatObj *result,
    GLint *number_of_screens
) {
    if (!result || !number_of_screens)
        return kCGLBadAddress;
    *result = NULL;
    *number_of_screens = 0;
    CGLPixelFormatAttribute empty[] = {0};
    if (!attrs) attrs = empty;
    CGLPixelFormatObj format = malloc(sizeof(struct _CGLPixelFormatObj));
    if (!format) return kCGLBadAlloc;
    int count = attributes_count(attrs);

    format->retain_count = 1;
    format->attributes = calloc((size_t)count + 1, sizeof(CGLPixelFormatAttribute));
    if (!format->attributes) {
        free(format);
        return kCGLBadAlloc;
    }
    for (int i = 0; i < count; i++) {
        format->attributes[i] = attrs[i];
    }

    *result = format;
    *number_of_screens = 1;

    return kCGLNoError;
}

CGLError CGLClearDrawable(CGLContextObj ctx)
{
    printf("STUB: CGLClearDrawable\n");

    return kCGLNoError;
}

CGLError CGLDescribePixelFormat(
    CGLPixelFormatObj format,
    GLint sreen_num,
    CGLPixelFormatAttribute attr,
    GLint *value
) {
    for (int i = 0; format->attributes[i] != 0; i++) {
        int has_arg = attribute_has_argument(format->attributes[i]);

        if (format->attributes[i] == attr) {
            if (has_arg) {
                *value = format->attributes[i + 1];
            } else {
                *value = 1;
            }
            return kCGLNoError;
        }

        if (has_arg) {
            i++;
        }
    }

    *value = 0;
    return kCGLNoError;
}

CGLPixelFormatObj CGLRetainPixelFormat(CGLPixelFormatObj format) {
    if (format == NULL) {
        return NULL;
    }

    format->retain_count++;
    return format;
}

void CGLReleasePixelFormat(CGLPixelFormatObj format) {
    if (format == NULL) {
        return;
    }

    format->retain_count--;

    if (format->retain_count == 0) {
        free(format->attributes);
        free(format);
    }
}

CGLError CGLDestroyPixelFormat(CGLPixelFormatObj pixelFormat) {
    CGLReleasePixelFormat(pixelFormat);
    return kCGLNoError;
}

GLuint CGLGetPixelFormatRetainCount(CGLPixelFormatObj pixelFormat) {
    return pixelFormat->retain_count;
}

CGLError CGLCreateContext(CGLPixelFormatObj pixelFormat, CGLContextObj share, CGLContextObj *resultp) {
    if (!resultp)
        return kCGLBadAddress;
    *resultp = NULL;
    if (!pixelFormat)
        return kCGLBadPixelFormat;
    if (!eglBindAPI(EGL_OPENGL_API))
        return kCGLBadContext;

    EGLContext egl_share = EGL_NO_CONTEXT;
    if (share != NULL) {
        egl_share = share->egl_context;
    }
    GLint profile = 0;
    CGLDescribePixelFormat(pixelFormat, 0, kCGLPFAOpenGLProfile, &profile);
    EGLContext egl_context = EGL_NO_CONTEXT;
    if (profile == kCGLOGLPVersion_3_2_Core || profile == kCGLOGLPVersion_4_1_Core) {
        // Try the profile commonly supplied by macOS first, then honor the
        // requested minimum without silently selecting legacy OpenGL.
        EGLint core[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        egl_context = eglCreateContext(display, config, egl_share, core);
        if (egl_context == EGL_NO_CONTEXT && profile == kCGLOGLPVersion_3_2_Core) {
            core[1] = 3;
            core[3] = 2;
            egl_context = eglCreateContext(display, config, egl_share, core);
        }
    } else if (profile == 0 || profile == kCGLOGLPVersion_Legacy) {
        egl_context = eglCreateContext(display, config, egl_share, NULL);
    } else {
        return kCGLBadValue;
    }

    if (egl_context == EGL_NO_CONTEXT) {
        return kCGLBadContext;
    }

    CGLContextObj context = malloc(sizeof(struct _CGLContextObj));
    if (!context) {
        eglDestroyContext(display, egl_context);
        return kCGLBadAlloc;
    }

    context->retain_count = 1;
    if (pthread_mutex_init(&(context->lock), NULL) != 0) {
        free(context);
        eglDestroyContext(display, egl_context);
        return kCGLBadAlloc;
    }
    context->egl_context = egl_context;
    context->egl_surface = NULL;
    context->swap_interval = 1;

    *resultp = context;

    return kCGLNoError;
}

CGLContextObj CGLRetainContext(CGLContextObj context) {
    if (context == NULL) {
        return NULL;
    }

    context->retain_count++;
    return context;
}

void CGLReleaseContext(CGLContextObj context) {
    if (context == NULL) {
        return;
    }

    context->retain_count--;

    if (context->retain_count != 0) {
        return;
    }

    if (CGLGetCurrentContext() == context) {
        CGLSetCurrentContext(NULL);
    }

    pthread_mutex_destroy(&(context->lock));

    eglDestroyContext(display, context->egl_context);

    free(context);
}

GLuint CGLGetContextRetainCount(CGLContextObj context) {
    if (context == NULL) {
        return 0;
    }

    return context->retain_count;
}

CGLError CGLDestroyContext(CGLContextObj context) {
    CGLReleaseContext(context);

    return kCGLNoError;
}

CGLError CGLLockContext(CGLContextObj context) {
    pthread_mutex_lock(&(context->lock));
    return kCGLNoError;
}

CGLError CGLUnlockContext(CGLContextObj context) {
    pthread_mutex_unlock(&(context->lock));
    return kCGLNoError;
}

CGLError CGLFlushDrawable(CGLContextObj context) {
    eglSwapBuffers(display,context->egl_surface);
    return kCGLNoError;
}

CGLError CGLSetParameter(CGLContextObj context, CGLContextParameter parameter, const GLint *value) {
    if (!value)
        return kCGLBadAddress;

    if (parameter == kCGLCPSwapInterval)
    {
        GLint v = *value;
        EGLBoolean success = eglSwapInterval(display, v);
        if (success)
            context->swap_interval = v;
        return success ? kCGLNoError : kCGLBadValue;
    }
    fprintf(stderr, "CGLSetParameter unimplemented for parameter %d\n", parameter);
    return kCGLNoError;
}

CGLError CGLGetParameter(CGLContextObj context, CGLContextParameter parameter, GLint *value) {
    if (!value)
        return kCGLBadAddress;

    if (parameter == kCGLCPSwapInterval)
    {
        *value = context->swap_interval;
        return kCGLNoError;
    }
    fprintf(stderr, "CGLGetParameter unimplemented for parameter %d\n", parameter);
    return kCGLNoError;
}

CGLError CGLDescribeRenderer(CGLRendererInfoObj rend, long rend_num, CGLRendererProperty prop, long *value) {
    return kCGLNoError;
}

CGLError CGLQueryRendererInfo(unsigned long display_mask, CGLRendererInfoObj *rend, long *nrend) {
    return kCGLNoError;
}

CGLError CGLDestroyRendererInfo(CGLRendererInfoObj rend) {
    return kCGLNoError;
}
