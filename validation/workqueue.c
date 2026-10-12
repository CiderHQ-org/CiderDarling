#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern void *wqueue_entry_point;
extern void run_reuse(void *, int, void *, void *, int, int);
extern void reuse_entry(void);
uintptr_t saved_rsp, saved_rbp, saved_rbx, saved_r12, saved_r13, saved_r14, saved_r15;
uintptr_t observed_rsp, observed_rbp;
unsigned reuse_count;
int main(void) {
    unsigned char *stack = malloc(8 * 1024 * 1024);
    assert(stack);
    void *top = stack + 8 * 1024 * 1024;
    wqueue_entry_point = reuse_entry;
    run_reuse(top, 123, (void *)456, (void *)789, 0, 321);
    assert(reuse_count == 10000);
    assert(observed_rsp == ((uintptr_t)top & ~(uintptr_t)15));
    assert(observed_rbp == 0);
    free(stack);
    puts("PASS 10000 worker reuses preserve arguments and restart at the same aligned stack top");
}
