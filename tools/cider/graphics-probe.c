/* Host-only rendering smoke test; does not launch Darling or read app data. */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
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

static const char* device_string(EGLDeviceEXT device, EGLint token) {
    PFNEGLQUERYDEVICESTRINGEXTPROC query = (void*)eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!query) return NULL;
    const char* extensions = query(device, EGL_EXTENSIONS);
    if (!extensions) return NULL;
    if (token == EGL_DRM_RENDER_NODE_FILE_EXT && !strstr(extensions, "EGL_EXT_device_drm_render_node")) return NULL;
    if (token == EGL_DRM_DEVICE_FILE_EXT && !strstr(extensions, "EGL_EXT_device_drm")) return NULL;
    return query(device, token);
}
static int adapter_path(const char* node, char output[PATH_MAX]) {
    if (!node) return 0;
    char resolved[PATH_MAX], path[PATH_MAX];
    if (!realpath(node, resolved)) return 0;
    const char* base = strrchr(resolved, '/');
    if (!base) return 0;
    if (snprintf(path, sizeof(path), "/sys/class/drm/%s/device", base+1) >= (int)sizeof(path)) return 0;
    return realpath(path, output) != NULL;
}
static int device_matches(EGLDeviceEXT device, const char* requested) {
    char expected[PATH_MAX], actual[PATH_MAX];
    if (!adapter_path(requested, expected)) return 0;
    const char* render = device_string(device, EGL_DRM_RENDER_NODE_FILE_EXT);
    const char* primary = device_string(device, EGL_DRM_DEVICE_FILE_EXT);
    return (adapter_path(render, actual) && !strcmp(actual, expected)) ||
           (adapter_path(primary, actual) && !strcmp(actual, expected));
}
static uint64_t now_ns(void) {
    struct timespec now; if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}
static long rss_bytes(void) {
    FILE* file = fopen("/proc/self/statm", "r"); unsigned long pages, resident;
    if (!file) return -1;
    int scanned = fscanf(file, "%lu %lu", &pages, &resident); fclose(file);
    return scanned == 2 ? (long)resident * sysconf(_SC_PAGESIZE) : -1;
}
static int compare_ns(const void* first, const void* second) {
    uint64_t a = *(const uint64_t*)first, b = *(const uint64_t*)second;
    return (a > b) - (a < b);
}
static int number(const char* text, int maximum) {
    char* end; long value = strtol(text, &end, 10);
    return *text && !*end && value >= 0 && value <= maximum ? (int)value : -1;
}
int main(int argc, char** argv) {
    int frames = 120, cycles = 16, samples = 0, depth = 24, stencil = 8;
    const char* requested_device = NULL;
    for (int i = 1; i < argc; i++) {
        if (i+1 >= argc) return failure("Missing probe option value");
        const char* option = argv[i++]; const char* argument = argv[i];
        if (!strcmp(option, "--device")) requested_device = argument;
        else if (!strcmp(option, "--frames")) frames = number(argument, 10000);
        else if (!strcmp(option, "--cycles")) cycles = number(argument, 1000);
        else if (!strcmp(option, "--samples")) samples = number(argument, 64);
        else if (!strcmp(option, "--depth")) depth = number(argument, 64);
        else if (!strcmp(option, "--stencil")) stencil = number(argument, 16);
        else return failure("Unknown probe option");
    }
    if (frames < 1 || cycles < 0 || samples < 0 || depth < 0 || stencil < 0) return failure("Invalid probe option value");
    alarm(60);
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    const char *platform = "default";
    if (requested_device || display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
        // A terminal/CI run may have no display server. Device displays are
        // still useful for offscreen shader and resource-sharing checks.
        PFNEGLQUERYDEVICESEXTPROC query = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        PFNEGLGETPLATFORMDISPLAYEXTPROC get = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        EGLDeviceEXT devices[32]; EGLint count = 0;
        display = EGL_NO_DISPLAY;
        if (query && get && query(32, devices, &count)) {
            for (int i = 0; i < count && i < 32; i++) {
                if (requested_device && !device_matches(devices[i], requested_device)) continue;
                EGLDisplay candidate = get(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
                if (candidate != EGL_NO_DISPLAY && eglInitialize(candidate, NULL, NULL)) {
                    display = candidate; platform = "device"; break;
                }
            }
        }
        if (display == EGL_NO_DISPLAY) return failure("EGL display initialization failed");
    }
    EGLDeviceEXT selected_device = NULL;
    PFNEGLQUERYDISPLAYATTRIBEXTPROC display_query = (void*)eglGetProcAddress("eglQueryDisplayAttribEXT");
    EGLAttrib selected = 0;
    if (display_query && display_query(display, EGL_DEVICE_EXT, &selected)) selected_device = (EGLDeviceEXT)selected;
    if (requested_device && (!selected_device || !device_matches(selected_device, requested_device))) return failure("Cannot verify requested GPU identity");
    if (!eglBindAPI(EGL_OPENGL_API)) return failure("Desktop OpenGL API unavailable");
    EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, depth, EGL_STENCIL_SIZE, stencil,
        EGL_SAMPLE_BUFFERS, samples ? 1 : 0, EGL_SAMPLES, samples, EGL_NONE};
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
    uint64_t* timings = calloc((size_t)frames, sizeof(*timings));
    if (!timings) return failure("Timing allocation failed");
    for (int i = 0; i < frames; i++) {
        uint64_t start = now_ns();
        glDrawArrays(GL_TRIANGLES, 0, 3); glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        timings[i] = now_ns() - start;
        if (glGetError() != GL_NO_ERROR || pixel[0] != 255 || pixel[1] || pixel[2]) return failure("Repeated rendering failed");
    }
    qsort(timings, (size_t)frames, sizeof(*timings), compare_ns);
    long rss_before = rss_bytes();
    for (int i = 0; i < cycles; i++) {
        EGLSurface temporary_surface = eglCreatePbufferSurface(display, config, size);
        EGLContext temporary_context = eglCreateContext(display, config, context, core);
        if (temporary_surface == EGL_NO_SURFACE || temporary_context == EGL_NO_CONTEXT || !eglMakeCurrent(display, temporary_surface, temporary_surface, temporary_context)) return failure("Lifecycle context creation failed");
        if (!glIsTexture(texture)) return failure("Lifecycle resource sharing failed");
        if (!eglMakeCurrent(display, surface, surface, context)) return failure("Lifecycle context restoration failed");
        if (!eglDestroyContext(display, temporary_context) || !eglDestroySurface(display, temporary_surface)) return failure("Lifecycle resource cleanup failed");
    }
    long rss_after = rss_bytes();
    EGLint actual_depth = 0, actual_stencil = 0, actual_samples = 0;
    eglGetConfigAttrib(display, config, EGL_DEPTH_SIZE, &actual_depth);
    eglGetConfigAttrib(display, config, EGL_STENCIL_SIZE, &actual_stencil);
    eglGetConfigAttrib(display, config, EGL_SAMPLES, &actual_samples);
    GLint max_texture, max_samples;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture); glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    printf("{\"ok\":true,\"draw_readback\":true,\"shared_thread\":true,\"platform\":"); json_string(platform);
    printf(",\"vendor\":"); json_string((const char *)glGetString(GL_VENDOR));
    printf(",\"renderer\":"); json_string((const char *)glGetString(GL_RENDERER));
    printf(",\"version\":"); json_string((const char *)glGetString(GL_VERSION));
    printf(",\"drm_render_node\":"); json_string(selected_device ? device_string(selected_device, EGL_DRM_RENDER_NODE_FILE_EXT) : NULL);
    printf(",\"drm_device_node\":"); json_string(selected_device ? device_string(selected_device, EGL_DRM_DEVICE_FILE_EXT) : NULL);
    printf(",\"requested_device\":"); json_string(requested_device);
    printf(",\"device_matches_request\":%s", requested_device ? "true" : "null");
    printf(",\"max_texture_size\":%d,\"max_samples\":%d,\"depth_bits\":%d,\"stencil_bits\":%d,\"samples\":%d", max_texture, max_samples, actual_depth, actual_stencil, actual_samples);
    printf(",\"frames\":%d,\"lifecycle_cycles\":%d,\"rss_before_cycles\":%ld,\"rss_after_cycles\":%ld", frames, cycles, rss_before, rss_after);
    printf(",\"draw_readback_latency_ns\":{\"p50\":%llu,\"p95\":%llu,\"p99\":%llu}}\n", (unsigned long long)timings[(frames*50+99)/100-1], (unsigned long long)timings[(frames*95+99)/100-1], (unsigned long long)timings[(frames*99+99)/100-1]);
    free(timings);
    glDeleteTextures(1, &texture); glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program); glDeleteShader(vertex); glDeleteShader(fragment);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, shared_context); eglDestroyContext(display, context);
    eglDestroySurface(display, shared_surface); eglDestroySurface(display, surface); eglTerminate(display);
    return 0;
}
