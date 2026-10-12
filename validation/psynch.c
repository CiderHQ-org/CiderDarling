#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static int status;
static uint32_t update;
static int fake_rpc(uint32_t* value) { *value = update; return status; }
#define LINUX_EINTR 4
#define __simple_printf(...) ((void)0)
#define __simple_abort abort
#define CIDER_TRACE_BEGIN() 0
#define CIDER_TRACE_END(category, start, value) ((void)(start))
#include "psynch-source.h"
int main(void) {
    for (int i = 0; i < 9; i++) {
        status = 22; assert(call_psynch(i) == -22);
        status = 60; assert(call_psynch(i) == -60);
        status = 0; update = 0x80000003U; assert(call_psynch(i) == 0x80000003L);
        update = 0; assert(!call_psynch(i));
    }
    status = -4; assert(sys_psynch_cvwait(NULL,0,0,NULL,0,0,0,0) == -EINTR);
    assert(sys_psynch_mutexwait(NULL,0,0,0,0) == -EINTR);
    puts("PASS nine actual psynch RPC wrappers distinguish Darwin errors from unsigned sequence updates");
}
