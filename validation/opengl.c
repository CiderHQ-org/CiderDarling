#define CIDER_TRACE_BEGIN() 0
#define CIDER_TRACE_END(category, start, value) ((void)(start))
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <GL/gl.h>
#include <EGL/egl.h>
#include "../src/frameworks/OpenGL/include/OpenGL/CGLTypes.h"
typedef void* CGLWindowRef;
CGLError CGLSetCurrentContext(CGLContextObj);
CGLContextObj CGLGetCurrentContext(void);
CGLContextObj CGLRetainContext(CGLContextObj);
void CGLReleaseContext(CGLContextObj);
CGLError CGLClearDrawable(CGLContextObj);
static int fail_switch, fail_format, surface_count, interval_calls, applied_interval;
EGLBoolean eglSwapInterval(EGLDisplay display, EGLint interval) { (void)display; interval_calls++; applied_interval = interval; return EGL_TRUE; }
static EGLint requested[64];
EGLBoolean eglChooseConfig(EGLDisplay display, const EGLint* attributes, EGLConfig* config, EGLint size, EGLint* count) {
    (void)display; assert(size == 1); int i;
    for (i = 0; attributes[i] != EGL_NONE; i++) requested[i] = attributes[i]; requested[i] = EGL_NONE;
    *config = (void*)4; *count = !fail_format; return EGL_TRUE;
}
EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config, EGLint attribute, EGLint* value) {
    (void)display; (void)config; *value = 0;
    for (int i = 0; requested[i] != EGL_NONE; i += 2) if (requested[i] == attribute) *value = requested[i+1];
    return EGL_TRUE;
}
EGLSurface eglCreatePbufferSurface(EGLDisplay display, EGLConfig config, const EGLint* attributes) {
    (void)display; (void)config; assert(attributes[0] == EGL_WIDTH); surface_count++; return (void*)(uintptr_t)(100 + surface_count);
}
EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config, EGLNativeWindowType window, const EGLint* attributes) {
    (void)display; (void)config; (void)attributes; assert(window); surface_count++; return (void*)(uintptr_t)(100 + surface_count);
}
EGLBoolean eglDestroySurface(EGLDisplay display, EGLSurface surface) {
    (void)display; assert(surface); surface_count--; return EGL_TRUE;
}
EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) { (void)display; assert(surface); return !fail_switch; }
EGLint eglGetError(void) { return EGL_BAD_MATCH; }
static EGLContext host_context;
static int fail_core41, create_calls, destroyed;
static int last_major, last_minor;
EGLBoolean eglBindAPI(EGLenum api) { assert(api == EGL_OPENGL_API); return EGL_TRUE; }
EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share, const EGLint *attributes) {
    (void)display; (void)config; (void)share; create_calls++;
    last_major = attributes ? attributes[1] : 0;
    last_minor = attributes ? attributes[3] : 0;
    if (attributes && last_major == 4 && fail_core41) return EGL_NO_CONTEXT;
    return (void *)3;
}
EGLBoolean eglDestroyContext(EGLDisplay display, EGLContext context) {
    (void)display; (void)context; destroyed++; return EGL_TRUE;
}
EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context) {
    (void)display; (void)draw; (void)read;
    if (fail_switch) return EGL_FALSE;
    host_context = context;
    return EGL_TRUE;
}
#include "opengl-source.h"
static void* thread_bind(void* context) { assert(!CGLSetCurrentContext(context)); return NULL; }
int main(void) {
    display = (void*)1; config = (void*)4;
    CGLPixelFormatAttribute attributes[] = {kCGLPFAColorSize, 24, kCGLPFASamples, 4, 99, 0x4100, 0};
    CGLPixelFormatObj format; GLint screens, value;
    assert(CGLChoosePixelFormat(attributes, &format, &screens) == kCGLNoError && screens == 1);
    assert(CGLDescribePixelFormat(format, 0, kCGLPFASamples, &value) == kCGLNoError && value == 4);
    assert(CGLDescribePixelFormat(format, 0, 99, &value) == kCGLNoError && value == 0x4100);
    assert(CGLDescribePixelFormat(format, 0, kCGLPFAStencilSize, &value) == kCGLNoError && value == 0);
    CGLReleasePixelFormat(format);
    assert(CGLChoosePixelFormat(NULL, &format, &screens) == kCGLNoError);
    assert(CGLDescribePixelFormat(format, 0, kCGLPFAColorSize, &value) == kCGLNoError && value == 0);
    CGLReleasePixelFormat(format);
    struct _CGLContextObj first = {.retain_count=1,.egl_display=(void*)1,.egl_context = (void *)1}, second = {.retain_count=1,.egl_display=(void*)1,.egl_context = (void *)2};
    assert(CGLSetCurrentContext(&first) == kCGLNoError);
    assert(CGLGetCurrentContext() == &first && host_context == (void *)1);
    fail_switch = 1;
    assert(CGLSetCurrentContext(&second) == kCGLBadContext);
    assert(CGLGetCurrentContext() == &first && host_context == (void *)1);
    assert(CGLSetCurrentContext(NULL) == kCGLBadContext);
    assert(CGLGetCurrentContext() == &first);
    fail_switch = 0;
    int before_interval = interval_calls; GLint interval = 0;
    assert(!CGLSetParameter(&second, kCGLCPSwapInterval, &interval) && interval_calls == before_interval);
    second.egl_surface = (void*)9;
    assert(!CGLSetCurrentContext(&second) && applied_interval == 0 && interval_calls == before_interval + 1);
    assert(CGLSetCurrentContext(NULL) == kCGLNoError && CGLGetCurrentContext() == NULL);
    CGLPixelFormatAttribute core[] = {kCGLPFAOpenGLProfile, kCGLOGLPVersion_3_2_Core, 0};
    assert(CGLChoosePixelFormat(core, &format, &screens) == kCGLNoError);
    CGLContextObj created;
    assert(CGLCreateContext(format, NULL, &created) == kCGLNoError);
    assert(create_calls == 1 && last_major == 4 && last_minor == 1);
    CGLReleaseContext(created);
    fail_core41 = 1;
    assert(CGLCreateContext(format, NULL, &created) == kCGLNoError);
    assert(create_calls == 3 && last_major == 3 && last_minor == 2);
    CGLReleaseContext(created);
    CGLReleasePixelFormat(format);
    core[1] = kCGLOGLPVersion_4_1_Core;
    assert(CGLChoosePixelFormat(core, &format, &screens) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &created) == kCGLBadContext && !created);
    assert(create_calls == 4); /* explicit 4.1 request cannot fall back to 3.2 */
    CGLReleasePixelFormat(format);
    fail_core41 = 0;
    CGLPixelFormatAttribute precise[] = {kCGLPFAColorSize,24,kCGLPFAAlphaSize,8,kCGLPFADepthSize,24,kCGLPFAStencilSize,8,kCGLPFASamples,4,0};
    assert(!CGLChoosePixelFormat(precise, &format, &screens));
    assert(!CGLDescribePixelFormat(format, 0, kCGLPFADepthSize, &value) && value == 24);
    assert(!CGLDescribePixelFormat(format, 0, kCGLPFAStencilSize, &value) && value == 8);
    assert(!CGLDescribePixelFormat(format, 0, kCGLPFASampleBuffers, &value) && value == 1);
    fail_format = 1; CGLPixelFormatObj unavailable;
    assert(CGLChoosePixelFormat(precise, &unavailable, &screens) == kCGLBadPixelFormat && !unavailable); fail_format = 0;
    for (int i = 0; i < 1000; i++) {
        assert(!CGLCreateContext(format, NULL, &created));
        CGLWindowRef window = CGLGetWindowForContext(created, (void*)1); assert(window);
        assert(!CGLContextMakeCurrentAndAttachToWindow(created, window));
        assert(!CGLFlushDrawable(created)); fail_switch = 1;
        assert(CGLFlushDrawable(created) == kCGLBadDrawable);
        assert(CGLClearDrawable(created) == kCGLBadDrawable && created->egl_surface == (EGLSurface)window); fail_switch = 0;
        assert(!CGLClearDrawable(created) && created->egl_surface == EGL_NO_SURFACE);
        CGLDestroyWindow(window); assert(!CGLSetCurrentContext(NULL));
        pthread_t worker; assert(!pthread_create(&worker, NULL, thread_bind, created)); assert(!pthread_join(worker, NULL));
        assert(created->retain_count == 1); CGLReleaseContext(created);
        assert(!surface_count && !g_windows && !g_contexts);
    }
    CGLReleasePixelFormat(format);
    puts("PASS framebuffer negotiation, failed switches, presentation errors, thread lifetime and 1000 drawable cleanup cycles");
}
