#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <GL/gl.h>
#include <EGL/egl.h>
#include "../src/frameworks/OpenGL/include/OpenGL/CGLTypes.h"
static int fail_switch;
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
int main(void) {
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
    struct _CGLContextObj first = {.egl_context = (void *)1}, second = {.egl_context = (void *)2};
    assert(CGLSetCurrentContext(&first) == kCGLNoError);
    assert(CGLGetCurrentContext() == &first && host_context == (void *)1);
    fail_switch = 1;
    assert(CGLSetCurrentContext(&second) == kCGLBadContext);
    assert(CGLGetCurrentContext() == &first && host_context == (void *)1);
    assert(CGLSetCurrentContext(NULL) == kCGLBadContext);
    assert(CGLGetCurrentContext() == &first);
    fail_switch = 0;
    assert(CGLSetCurrentContext(NULL) == kCGLNoError && CGLGetCurrentContext() == NULL);
    CGLPixelFormatAttribute core[] = {kCGLPFAOpenGLProfile, kCGLOGLPVersion_3_2_Core, 0};
    assert(CGLChoosePixelFormat(core, &format, &screens) == kCGLNoError);
    CGLContextObj created;
    assert(CGLCreateContext(format, NULL, &created) == kCGLNoError);
    assert(create_calls == 1 && last_major == 4 && last_minor == 1);
    pthread_mutex_destroy(&created->lock); free(created);
    fail_core41 = 1;
    assert(CGLCreateContext(format, NULL, &created) == kCGLNoError);
    assert(create_calls == 3 && last_major == 3 && last_minor == 2);
    pthread_mutex_destroy(&created->lock); free(created);
    CGLReleasePixelFormat(format);
    core[1] = kCGLOGLPVersion_4_1_Core;
    assert(CGLChoosePixelFormat(core, &format, &screens) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &created) == kCGLBadContext && !created);
    assert(create_calls == 4); /* explicit 4.1 request cannot fall back to 3.2 */
    CGLReleasePixelFormat(format);
    puts("PASS CGL pixel format terminators/valued attributes and failed host context switches");
}
