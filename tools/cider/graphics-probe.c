/* Host-only rendering smoke test; does not launch Darling or read app data. */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static EGLDisplay display;
static EGLConfig config;
static EGLContext context, shared_context;
static EGLSurface surface, shared_surface;
static GLuint texture;
static int shared_ok;
static void json_string(const char *text) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static int failure(const char *message) {
    printf("{\"ok\":false,\"error\":"); json_string(message);
    printf(",\"egl_error\":%d}\n", eglGetError());
    return 1;
}
static GLuint shader(GLenum stage, const char *source) {
    GLuint result = glCreateShader(stage); GLint ok = 0;
    glShaderSource(result, 1, &source, NULL); glCompileShader(result);
    glGetShaderiv(result, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(result, sizeof log, NULL, log); fprintf(stderr, "%s\n", log); glDeleteShader(result); return 0; }
    return result;
}
static void *share_test(void *unused) {
    (void)unused;
    if (eglBindAPI(EGL_OPENGL_API) && eglMakeCurrent(display, shared_surface, shared_surface, shared_context)) {
        shared_ok = glIsTexture(texture) && glGetError() == GL_NO_ERROR;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    return NULL;
}
int main(void) {
    alarm(15);
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    const char *platform = "default";
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
        // A terminal/CI run may have no display server. Device displays are
        // still useful for offscreen shader and resource-sharing checks.
        PFNEGLQUERYDEVICESEXTPROC query = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        PFNEGLGETPLATFORMDISPLAYEXTPROC get = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        EGLDeviceEXT devices[32]; EGLint count = 0;
        display = EGL_NO_DISPLAY;
        if (query && get && query(32, devices, &count)) {
            for (int i = 0; i < count && i < 32; i++) {
                EGLDisplay candidate = get(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
                if (candidate != EGL_NO_DISPLAY && eglInitialize(candidate, NULL, NULL)) {
                    display = candidate; platform = "device"; break;
                }
            }
        }
        if (display == EGL_NO_DISPLAY) return failure("EGL display initialization failed");
    }
    if (!eglBindAPI(EGL_OPENGL_API)) return failure("Desktop OpenGL API unavailable");
    EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint count;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || !count) return failure("No usable OpenGL framebuffer format");
    EGLint size[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    surface = eglCreatePbufferSurface(display, config, size);
    EGLint core[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, core);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) return failure("OpenGL 4.1 core context unavailable");
    const char *vertex_source = "#version 150\nconst vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3)); void main(){gl_Position=vec4(p[gl_VertexID],0,1);}";
    const char *fragment_source = "#version 150\nout vec4 color; void main(){color=vec4(1,0,0,1);}";
    GLuint vertex = shader(GL_VERTEX_SHADER, vertex_source), fragment = shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) return failure("Shader compilation failed");
    GLuint program = glCreateProgram(); GLint linked;
    glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) return failure("Shader linking failed");
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glViewport(0, 0, 64, 64); glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program); glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char pixel[4] = {0}; glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (glGetError() != GL_NO_ERROR || pixel[0] != 255 || pixel[1] || pixel[2]) return failure("Rendered pixel readback did not match");
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel); glFinish();
    shared_surface = eglCreatePbufferSurface(display, config, size);
    shared_context = eglCreateContext(display, config, context, core);
    if (shared_surface == EGL_NO_SURFACE || shared_context == EGL_NO_CONTEXT) return failure("Shared context creation failed");
    pthread_t thread;
    if (pthread_create(&thread, NULL, share_test, NULL) || pthread_join(thread, NULL) || !shared_ok) return failure("Cross-thread texture sharing failed");
    GLint max_texture, max_samples;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture); glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    printf("{\"ok\":true,\"draw_readback\":true,\"shared_thread\":true,\"platform\":"); json_string(platform);
    printf(",\"vendor\":"); json_string((const char *)glGetString(GL_VENDOR));
    printf(",\"renderer\":"); json_string((const char *)glGetString(GL_RENDERER));
    printf(",\"version\":"); json_string((const char *)glGetString(GL_VERSION));
    printf(",\"max_texture_size\":%d,\"max_samples\":%d}\n", max_texture, max_samples);
    glDeleteTextures(1, &texture); glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program); glDeleteShader(vertex); glDeleteShader(fragment);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, shared_context); eglDestroyContext(display, context);
    eglDestroySurface(display, shared_surface); eglDestroySurface(display, surface); eglTerminate(display);
    return 0;
}
