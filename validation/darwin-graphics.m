/* Real AppKit/CGL fixture. Only its own offscreen content and events are used. */
#import <AppKit/NSApplication.h>
#import <AppKit/NSEvent.h>
#import <AppKit/NSWindow.h>
#import <AppKit/NSView.h>
#import <AppKit/NSOpenGLContext.h>
#import <AppKit/NSOpenGLPixelFormat.h>
#import <Foundation/NSAutoreleasePool.h>
#import <Foundation/NSDate.h>
#import <Foundation/NSRunLoop.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static void* post_event(void* argument) {
    NSAutoreleasePool* pool = [NSAutoreleasePool new];
    usleep(20000);
    NSEvent* event = [NSEvent otherEventWithType:NSApplicationDefined location:NSMakePoint(0,0)
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:(short)(intptr_t)argument data1:0 data2:0];
    [NSApp postEvent:event atStart:NO];
    [pool drain]; return NULL;
}
int main(void) {
    alarm(30); NSAutoreleasePool* pool = [NSAutoreleasePool new];
    [NSApplication sharedApplication];
    NSString* modes[] = {NSDefaultRunLoopMode, NSEventTrackingRunLoopMode, NSModalPanelRunLoopMode};
    for (int i = 0; i < 3; i++) {
        pthread_t thread; assert(!pthread_create(&thread, NULL, post_event, (void*)(intptr_t)(300+i)));
        NSEvent* event = [NSApp nextEventMatchingMask:NSApplicationDefinedMask
            untilDate:[NSDate dateWithTimeIntervalSinceNow:2] inMode:modes[i] dequeue:YES];
        assert(event && [event subtype] == 300+i); assert(!pthread_join(thread, NULL));
    }
    CGLPixelFormatAttribute attrs[] = {kCGLPFAOpenGLProfile,kCGLOGLPVersion_3_2_Core,
        kCGLPFAColorSize,24,kCGLPFAAlphaSize,8,kCGLPFADepthSize,24,kCGLPFAStencilSize,8,kCGLPFASamples,4,0};
    CGLPixelFormatObj format; GLint screens;
    assert(!CGLChoosePixelFormat(attrs,&format,&screens));
    for (int i = 0; i < 32; i++) {
        CGLContextObj context; assert(!CGLCreateContext(format,NULL,&context));
        assert(!CGLSetCurrentContext(context));
        if (!i) printf("CGL renderer=%s version=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        unsigned char pixel[4]; glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        assert(glGetError() == GL_NO_ERROR && pixel[0] == 255 && !pixel[1] && !pixel[2]);
        assert(!CGLClearDrawable(context)); assert(!CGLSetCurrentContext(NULL)); CGLReleaseContext(context);
    }
    CGLReleasePixelFormat(format);
    NSOpenGLPixelFormat* cocoaFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    assert(cocoaFormat);
    for (int i = 0; i < 4; i++) {
        NSRect rect = NSMakeRect(16,16,96,96);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:rect styleMask:0 backing:NSBackingStoreBuffered defer:NO];
        assert(window); [window setReleasedWhenClosed:NO];
        NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0,0,96,96)]; [window setContentView:view];
        /* Mapping creates the native drawable; ordering behind avoids activation. */
        [window orderBack:nil];
        NSOpenGLContext* context = [[NSOpenGLContext alloc] initWithFormat:cocoaFormat shareContext:nil];
        assert([context CGLContextObj]);
        GLint interval = 0, read_interval = -1;
        assert(!CGLSetParameter([context CGLContextObj], kCGLCPSwapInterval, &interval));
        assert(!CGLGetParameter([context CGLContextObj], kCGLCPSwapInterval, &read_interval) && !read_interval);
        [context setView:view];
        for (int resize = 0; resize < 5; resize++) {
            [window setContentSize:NSMakeSize(96+resize*8,96+resize*8)]; [context update]; [context makeCurrentContext];
            CGLContextObj raw = [context CGLContextObj]; assert(CGLGetCurrentContext() == raw);
            glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
            unsigned char pixel[4]; glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(!glGetError() && pixel[0] == 255 && !pixel[1] && !pixel[2]);
            assert(!CGLFlushDrawable(raw));
        }
        [context clearDrawable]; [NSOpenGLContext clearCurrentContext];
        [context release]; [view release]; [window close]; [window release];
    }
    [cocoaFormat release];
    puts("PASS real AppKit background events in three modes and 32 CGL framebuffer/lifecycle cycles");
    puts("PASS background native-window resize, presentation and destruction cycles");
    [pool drain]; return 0;
}
