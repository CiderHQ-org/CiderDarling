#include <cider_trace.h>
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
static CGLContextObj g_contexts;
struct _CGLWindow { EGLSurface surface; EGLDisplay display; struct _CGLWindow* next; };
static struct _CGLWindow* g_windows;

struct _CGLContextObj {
    GLuint retain_count;
    pthread_mutex_t lock;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLSurface fallback_surface;
    EGLDisplay egl_display;
    EGLConfig egl_config;
    int owns_surface;
    struct _CGLContextObj* next;
    // EGL has no function for getting the current swap interval,
    // so we need to save the last set value. The default is 1.
    int swap_interval;
};

struct _CGLPixelFormatObj {
    GLuint retain_count;
    CGLPixelFormatAttribute *attributes;
    EGLDisplay egl_display;
    EGLConfig egl_config;
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

static CGLError choose_config(EGLDisplay target, const CGLPixelFormatAttribute* attrs, EGLConfig* selected) {
    EGLint color = 24, alpha = 0, depth = 0, stencil = 0, samples = 0, buffers = 0;
    int offscreen = 0;
    for (int i = 0; attrs && attrs[i]; i++) {
        CGLPixelFormatAttribute attr = attrs[i];
        EGLint value = attribute_has_argument(attr) ? attrs[++i] : 1;
        if (attribute_has_argument(attr) && value < 0) return kCGLBadValue;
        switch (attr) {
        case kCGLPFAColorSize: color = value; break;
        case kCGLPFAAlphaSize: alpha = value; break;
        case kCGLPFADepthSize: depth = value; break;
        case kCGLPFAStencilSize: stencil = value; break;
        case kCGLPFASamples: samples = value; break;
        case kCGLPFASampleBuffers: buffers = value; break;
        case kCGLPFAOffScreen: case kCGLPFAPBuffer: offscreen = 1; break;
        case kCGLPFAStereo: case kCGLPFAColorFloat: case kCGLPFAAccumSize:
        case kCGLPFASupersample: case kCGLPFARemotePBuffer:
            if (value) return kCGLBadAttribute; break;
        default: break;
        }
    }
    if (color > 48 || alpha > 16 || depth > 64 || stencil > 16 || buffers > 1 || samples > 64) return kCGLBadValue;
    if (samples && !buffers) buffers = 1;
    EGLint red = color == 16 || color == 15 ? 5 : (color + 2) / 3;
    EGLint green = color == 16 ? 6 : red, blue = red;
    if (color == 32) red = green = blue = 8;
    EGLint request[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | (offscreen ? 0 : EGL_WINDOW_BIT),
        EGL_RED_SIZE, red, EGL_GREEN_SIZE, green, EGL_BLUE_SIZE, blue,
        EGL_ALPHA_SIZE, alpha, EGL_DEPTH_SIZE, depth, EGL_STENCIL_SIZE, stencil,
        EGL_SAMPLE_BUFFERS, buffers, EGL_SAMPLES, samples, EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(target, request, selected, 1, &count) || !count) return kCGLBadPixelFormat;
    return kCGLNoError;
}
CGLError CGLRegisterNativeDisplay(void *native_display) {
    EGLDisplay candidate = eglGetDisplay(native_display);
    if (candidate == EGL_NO_DISPLAY) return kCGLBadConnection;
    if (!eglInitialize(candidate, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API)) return kCGLBadConnection;
    EGLConfig selected;
    CGLError error = choose_config(candidate, NULL, &selected);
    if (error) return error;
    pthread_mutex_lock(&g_displaysMutex);
    display = candidate; config = selected; num_config = 1;
    pthread_mutex_unlock(&g_displaysMutex);
    return kCGLNoError;
}

static struct _CGLDisplay* getCGLDisplay(CGSConnectionID cid) {
    pthread_mutex_lock(&g_displaysMutex);
    struct _CGLDisplay* rv = (void*)CFDictionaryGetValue(g_displays, (const void*)(unsigned long)cid);
    if (!rv) {
        EGLDisplay candidate = eglGetDisplay(_CGSNativeDisplay(cid));
        if (candidate == EGL_NO_DISPLAY || !eglInitialize(candidate, NULL, NULL)) goto out;
        rv = calloc(1, sizeof(*rv));
        if (!rv) goto out;
        rv->display = candidate;
        if (choose_config(candidate, NULL, &rv->config)) { free(rv); rv = NULL; goto out; }
        rv->num_config = 1;
        CFDictionaryAddValue(g_displays, (const void*)(unsigned long)cid, rv);
    }
out:
    pthread_mutex_unlock(&g_displaysMutex);
    return rv;
}
CGLError CGLSetSurface(CGLContextObj context, CGSConnectionID cid, CGSWindowID wid, CGSSurfaceID sid) {
    if (!context) return kCGLBadContext;
    struct _CGLDisplay* target = getCGLDisplay(cid);
    if (!target) return kCGLBadConnection;
    if (target->display != context->egl_display) return kCGLBadMatch;
    EGLNativeWindowType window = (EGLNativeWindowType)(sid ? _CGSNativeWindowForSurfaceID(cid,wid,sid) : _CGSNativeWindowForID(cid,wid));
    if (!window) return kCGLBadWindow;
    EGLSurface surface = eglCreateWindowSurface(context->egl_display, context->egl_config, window, NULL);
    if (surface == EGL_NO_SURFACE) return kCGLBadDrawable;
    EGLSurface previous = __atomic_load_n(&context->egl_surface, __ATOMIC_ACQUIRE); int owned = context->owns_surface;
    __atomic_store_n(&context->egl_surface, surface, __ATOMIC_RELEASE); context->owns_surface = 1;
    if (CGLGetCurrentContext() == context && CGLSetCurrentContext(context)) {
        __atomic_store_n(&context->egl_surface, previous, __ATOMIC_RELEASE); context->owns_surface = owned;
        eglDestroySurface(context->egl_display, surface); return kCGLBadContext;
    }
    if (owned && previous) eglDestroySurface(context->egl_display, previous);
    return kCGLNoError;
}
static CGLWindowRef create_window(EGLDisplay target, EGLConfig format, void* native_window) {
    if (!native_window || target == EGL_NO_DISPLAY) return NULL;
    EGLSurface surface = eglCreateWindowSurface(target, format, (EGLNativeWindowType)native_window, NULL);
    if (surface == EGL_NO_SURFACE) {
        EGLint error = eglGetError(), identifier = 0, visual = 0;
        eglGetConfigAttrib(target, format, EGL_CONFIG_ID, &identifier);
        eglGetConfigAttrib(target, format, EGL_NATIVE_VISUAL_ID, &visual);
        fprintf(stderr, "CiderDarling EGL window creation failed: error=0x%x config=%d visual=0x%x window=%llu\n",
            error, identifier, visual, (unsigned long long)(uintptr_t)native_window);
        return NULL;
    }
    struct _CGLWindow* item = calloc(1, sizeof(*item));
    if (!item) { eglDestroySurface(target, surface); return NULL; }
    item->surface = surface; item->display = target;
    pthread_mutex_lock(&g_displaysMutex); item->next = g_windows; g_windows = item; pthread_mutex_unlock(&g_displaysMutex);
    return (CGLWindowRef)surface;
}
CGLWindowRef CGLGetWindow(void *native_window) { return create_window(display, config, native_window); }
CGLWindowRef CGLGetWindowForContext(CGLContextObj context, void *native_window) {
    return context ? create_window(context->egl_display, context->egl_config, native_window) : NULL;
}
unsigned long CGLGetPixelFormatNativeVisualID(CGLPixelFormatObj format) {
    EGLint visual = 0;
    if (!format || !eglGetConfigAttrib(format->egl_display, format->egl_config, EGL_NATIVE_VISUAL_ID, &visual)) return 0;
    return visual > 0 ? (unsigned long)visual : 0;
}
unsigned long CGLGetNativeVisualID(CGLContextObj context) {
    EGLint visual = 0;
    if (!context || !eglGetConfigAttrib(context->egl_display, context->egl_config, EGL_NATIVE_VISUAL_ID, &visual)) return 0;
    return visual > 0 ? (unsigned long)visual : 0;
}
CGL_EXPORT void CGLDestroyWindow(CGLWindowRef window) {
    if (!window) return;
    CGLContextObj current = CGLGetCurrentContext();
    if (current && __atomic_load_n(&current->egl_surface, __ATOMIC_ACQUIRE) == (EGLSurface)window && CGLClearDrawable(current)) return;
    pthread_mutex_lock(&g_displaysMutex);
    struct _CGLWindow** position = &g_windows;
    while (*position && (*position)->surface != (EGLSurface)window) position = &(*position)->next;
    struct _CGLWindow* item = *position;
    if (item) {
        *position = item->next;
        for (CGLContextObj context = g_contexts; context; context = context->next)
            if (__atomic_load_n(&context->egl_surface, __ATOMIC_ACQUIRE) == (EGLSurface)window) {
                __atomic_store_n(&context->egl_surface, EGL_NO_SURFACE, __ATOMIC_RELEASE);
            }
    }
    pthread_mutex_unlock(&g_displaysMutex);
    if (item) { eglDestroySurface(item->display, item->surface); free(item); }
}
CGL_EXPORT CGLError CGLContextMakeCurrentAndAttachToWindow(CGLContextObj context, CGLWindowRef window) {
    if (!context) return kCGLBadContext;
    pthread_mutex_lock(&g_displaysMutex);
    struct _CGLWindow* item = g_windows;
    while (item && item->surface != (EGLSurface)window) item = item->next;
    int valid = item && item->display == context->egl_display;
    pthread_mutex_unlock(&g_displaysMutex);
    if (!valid) return kCGLBadDrawable;
    EGLSurface previous = __atomic_load_n(&context->egl_surface, __ATOMIC_ACQUIRE); int owned = context->owns_surface;
    __atomic_store_n(&context->egl_surface, (EGLSurface)window, __ATOMIC_RELEASE); context->owns_surface = 0;
    CGLError error = CGLSetCurrentContext(context);
    if (error) { __atomic_store_n(&context->egl_surface, previous, __ATOMIC_RELEASE); context->owns_surface = owned; }
    else if (owned && previous) eglDestroySurface(context->egl_display, previous);
    return error;
}

static pthread_key_t current_context_key;

static int current_key_error;
static void release_thread_context(void* value) {
    CGLContextObj context = value;
    if (context) {
        eglMakeCurrent(context->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        CGLReleaseContext(context);
    }
}
static void make_key() { current_key_error = pthread_key_create(&current_context_key, release_thread_context); }

static pthread_key_t get_current_context_key() {
    static pthread_once_t key_once = PTHREAD_ONCE_INIT;
    pthread_once(&key_once, make_key);
    return current_context_key;
}

CGLContextObj CGLGetCurrentContext(void) {
    pthread_key_t key = get_current_context_key();
    return current_key_error ? NULL : pthread_getspecific(key);
}

CGLError CGLSetCurrentContext(CGLContextObj context) {
    pthread_key_t key = get_current_context_key();
    if (current_key_error) return kCGLBadAlloc;
    CGLContextObj previous = pthread_getspecific(key);
    EGLDisplay target = context ? context->egl_display : (previous ? previous->egl_display : display);
    if (target == EGL_NO_DISPLAY || !eglBindAPI(EGL_OPENGL_API)) return kCGLBadContext;
    EGLSurface surface = context ? __atomic_load_n(&context->egl_surface, __ATOMIC_ACQUIRE) : EGL_NO_SURFACE;
    if (context && surface == EGL_NO_SURFACE) surface = context->fallback_surface;
    uint64_t started = CIDER_TRACE_BEGIN();
    EGLBoolean accepted = eglMakeCurrent(target, surface, surface, context ? context->egl_context : EGL_NO_CONTEXT);
    CIDER_TRACE_END(accepted ? "cgl_bind" : "cgl_bind_error", started, (uintptr_t)context);
    if (!accepted) return kCGLBadContext;
    if (context && __atomic_load_n(&context->egl_surface, __ATOMIC_ACQUIRE) != EGL_NO_SURFACE &&
        !eglSwapInterval(target, __atomic_load_n(&context->swap_interval, __ATOMIC_ACQUIRE))) {
        EGLSurface old = previous ? __atomic_load_n(&previous->egl_surface, __ATOMIC_ACQUIRE) : EGL_NO_SURFACE;
        if (previous && old == EGL_NO_SURFACE) old = previous->fallback_surface;
        eglMakeCurrent(previous ? previous->egl_display : target, old, old, previous ? previous->egl_context : EGL_NO_CONTEXT);
        return kCGLBadValue;
    }
    if (pthread_setspecific(key, context)) {
        EGLSurface old = previous ? __atomic_load_n(&previous->egl_surface, __ATOMIC_ACQUIRE) : EGL_NO_SURFACE;
        if (previous && old == EGL_NO_SURFACE) old = previous->fallback_surface;
        eglMakeCurrent(previous ? previous->egl_display : target, old, old, previous ? previous->egl_context : EGL_NO_CONTEXT);
        return kCGLBadAlloc;
    }
    if (context != previous) { CGLRetainContext(context); CGLReleaseContext(previous); }
    return kCGLNoError;
}

CGLError CGLSetFullScreen(CGLContextObj ctx) {
    return ctx ? kCGLBadFullScreen : kCGLBadContext;
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

    EGLDisplay target = display;
    if (target == EGL_NO_DISPLAY) { free(format); return kCGLBadDisplay; }
    CGLError error = choose_config(target, attrs, &format->egl_config);
    if (error) { free(format); return error; }
    format->egl_display = target;
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
    if (!ctx) return kCGLBadContext;
    EGLSurface previous = __atomic_load_n(&ctx->egl_surface, __ATOMIC_ACQUIRE);
    if (CGLGetCurrentContext() == ctx && !eglMakeCurrent(ctx->egl_display, ctx->fallback_surface, ctx->fallback_surface, ctx->egl_context)) return kCGLBadDrawable;
    __atomic_store_n(&ctx->egl_surface, EGL_NO_SURFACE, __ATOMIC_RELEASE);
    if (ctx->owns_surface && previous) eglDestroySurface(ctx->egl_display, previous);
    ctx->owns_surface = 0;
    return kCGLNoError;
}

CGLError CGLDescribePixelFormat(
    CGLPixelFormatObj format,
    GLint sreen_num,
    CGLPixelFormatAttribute attr,
    GLint *value
) {
    if (!format) return kCGLBadPixelFormat;
    if (!value) return kCGLBadAddress;
    if (sreen_num != 0) return kCGLBadValue;
    EGLint actual, token = 0;
    switch (attr) {
    case kCGLPFAAlphaSize: token = EGL_ALPHA_SIZE; break;
    case kCGLPFADepthSize: token = EGL_DEPTH_SIZE; break;
    case kCGLPFAStencilSize: token = EGL_STENCIL_SIZE; break;
    case kCGLPFASamples: token = EGL_SAMPLES; break;
    case kCGLPFASampleBuffers: token = EGL_SAMPLE_BUFFERS; break;
    default: break;
    }
    if (token) {
        if (!eglGetConfigAttrib(format->egl_display, format->egl_config, token, &actual)) return kCGLBadPixelFormat;
        *value = actual; return kCGLNoError;
    }
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

    __atomic_add_fetch(&format->retain_count, 1, __ATOMIC_RELAXED);
    return format;
}

void CGLReleasePixelFormat(CGLPixelFormatObj format) {
    if (format == NULL) {
        return;
    }

    if (__atomic_sub_fetch(&format->retain_count, 1, __ATOMIC_ACQ_REL) == 0) {
        free(format->attributes);
        free(format);
    }
}

CGLError CGLDestroyPixelFormat(CGLPixelFormatObj pixelFormat) {
    CGLReleasePixelFormat(pixelFormat);
    return kCGLNoError;
}

GLuint CGLGetPixelFormatRetainCount(CGLPixelFormatObj pixelFormat) {
    return __atomic_load_n(&pixelFormat->retain_count, __ATOMIC_RELAXED);
}

CGLError CGLCreateContext(CGLPixelFormatObj pixelFormat, CGLContextObj share, CGLContextObj *resultp) {
    if (!resultp)
        return kCGLBadAddress;
    *resultp = NULL;
    if (!pixelFormat)
        return kCGLBadPixelFormat;
    if (!eglBindAPI(EGL_OPENGL_API))
        return kCGLBadContext;

    EGLDisplay target = pixelFormat->egl_display;
    EGLConfig selected = pixelFormat->egl_config;
    if (share && share->egl_display != target) return kCGLBadMatch;
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
        egl_context = eglCreateContext(target, selected, egl_share, core);
        if (egl_context == EGL_NO_CONTEXT && profile == kCGLOGLPVersion_3_2_Core) {
            core[1] = 3;
            core[3] = 2;
            egl_context = eglCreateContext(target, selected, egl_share, core);
        }
    } else if (profile == 0 || profile == kCGLOGLPVersion_Legacy) {
        egl_context = eglCreateContext(target, selected, egl_share, NULL);
    } else {
        return kCGLBadValue;
    }

    if (egl_context == EGL_NO_CONTEXT) {
        return kCGLBadContext;
    }

    CGLContextObj context = malloc(sizeof(struct _CGLContextObj));
    if (!context) {
        eglDestroyContext(target, egl_context);
        return kCGLBadAlloc;
    }

    context->retain_count = 1;
    if (pthread_mutex_init(&(context->lock), NULL) != 0) {
        free(context);
        eglDestroyContext(target, egl_context);
        return kCGLBadAlloc;
    }
    context->egl_context = egl_context;
    context->egl_surface = EGL_NO_SURFACE;
    context->egl_display = target; context->egl_config = selected; context->owns_surface = 0;
    EGLint size[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    context->fallback_surface = eglCreatePbufferSurface(target, selected, size);
    if (context->fallback_surface == EGL_NO_SURFACE) {
        pthread_mutex_destroy(&context->lock); eglDestroyContext(target, egl_context); free(context); return kCGLBadOffScreen;
    }
    pthread_mutex_lock(&g_displaysMutex); context->next = g_contexts; g_contexts = context; pthread_mutex_unlock(&g_displaysMutex);
    context->swap_interval = 1;

    *resultp = context;

    return kCGLNoError;
}

CGLContextObj CGLRetainContext(CGLContextObj context) {
    if (context == NULL) {
        return NULL;
    }

    __atomic_add_fetch(&context->retain_count, 1, __ATOMIC_RELAXED);
    return context;
}

void CGLReleaseContext(CGLContextObj context) {
    if (context == NULL) {
        return;
    }

    if (__atomic_sub_fetch(&context->retain_count, 1, __ATOMIC_ACQ_REL) != 0) return;
    pthread_mutex_lock(&g_displaysMutex);
    CGLContextObj* position = &g_contexts;
    while (*position && *position != context) position = &(*position)->next;
    if (*position) *position = context->next;
    pthread_mutex_unlock(&g_displaysMutex);
    if (context->owns_surface && context->egl_surface) eglDestroySurface(context->egl_display, context->egl_surface);
    eglDestroySurface(context->egl_display, context->fallback_surface);
    pthread_mutex_destroy(&context->lock);
    eglDestroyContext(context->egl_display, context->egl_context);

    free(context);
}

GLuint CGLGetContextRetainCount(CGLContextObj context) {
    if (context == NULL) {
        return 0;
    }

    return __atomic_load_n(&context->retain_count, __ATOMIC_RELAXED);
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
    if (!context) return kCGLBadContext;
    EGLSurface surface = __atomic_load_n(&context->egl_surface, __ATOMIC_ACQUIRE);
    if (surface == EGL_NO_SURFACE) return kCGLBadDrawable;
    uint64_t started = CIDER_TRACE_BEGIN();
    EGLBoolean accepted = eglSwapBuffers(context->egl_display, surface);
    CIDER_TRACE_END(accepted ? "cgl_present" : "cgl_present_error", started, (uintptr_t)context);
    return accepted ? kCGLNoError : kCGLBadDrawable;
}

CGLError CGLSetParameter(CGLContextObj context, CGLContextParameter parameter, const GLint *value) {
    if (!context) return kCGLBadContext;
    if (!value)
        return kCGLBadAddress;

    if (parameter == kCGLCPSwapInterval)
    {
        GLint v = *value;
        if (v < 0) return kCGLBadValue; /* Adaptive negative intervals need a separate extension path. */
        if (CGLGetCurrentContext() == context && __atomic_load_n(&context->egl_surface, __ATOMIC_ACQUIRE) != EGL_NO_SURFACE &&
            !eglSwapInterval(context->egl_display, v)) return kCGLBadValue;
        __atomic_store_n(&context->swap_interval, v, __ATOMIC_RELEASE);
        return kCGLNoError;
    }
    fprintf(stderr, "CGLSetParameter unimplemented for parameter %d\n", parameter);
    return kCGLNoError;
}

CGLError CGLGetParameter(CGLContextObj context, CGLContextParameter parameter, GLint *value) {
    if (!context) return kCGLBadContext;
    if (!value)
        return kCGLBadAddress;

    if (parameter == kCGLCPSwapInterval)
    {
        *value = __atomic_load_n(&context->swap_interval, __ATOMIC_ACQUIRE);
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
