#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../src/startup/mldr/elfcalls/threads.h"
#include "../src/startup/mldr/elfcalls/dthreads.h"
static int mappings, unmaps;
static size_t last_mapping;
static int fail_guard;
static void *test_mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    mappings++; last_mapping = size;
    return mmap(address, size, protection, flags, fd, offset);
}
static int test_munmap(void *address, size_t size) { unmaps++; return munmap(address, size); }
static int test_mprotect(void *address, size_t size, int protection) {
    if (fail_guard) { errno = EACCES; return -1; }
    return mprotect(address, size, protection);
}
static int fail_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *arg) {
    (void)thread; (void)start; (void)arg;
    size_t size;
    assert(!pthread_attr_getstacksize(attributes, &size) && size >= PTHREAD_STACK_MIN);
    return EAGAIN;
}
static void *darling_thread_entry(void *arg) { (void)arg; abort(); }
#define mmap test_mmap
#define munmap test_munmap
#define mprotect test_mprotect
#define pthread_create fail_create
#include "threads-source.h"
#undef mmap
#undef munmap
#undef mprotect
#undef pthread_create
int main(void) {
    void *top;
    assert(!dthread_structure_allocate(SIZE_MAX, 4096, &top));
    assert(mappings == 0);
    fail_guard = 1;
    assert(!dthread_structure_allocate(512 * 1024, 4096, &top));
    assert(mappings == 1 && unmaps == 1);
    fail_guard = 0;
    dthread_t thread = dthread_structure_allocate(1024 * 1024, 4096, &top);
    assert(thread && thread->stackaddr == top);
    assert((uintptr_t)thread->stackaddr - (uintptr_t)thread->stackbottom == 1024 * 1024);
    assert(thread->tsd[DTHREAD_TSD_SLOT_PTHREAD_SELF] == thread);
    assert(!munmap(thread->freeaddr, thread->freesize));
    int before = mappings;
    assert((intptr_t)__darling_thread_create(512 * 1024, 0, NULL, 0, 0, 0, 0, NULL, NULL) == -35);
    assert(mappings == before + 1 && last_mapping >= 8 * 1024 * 1024);
    assert(unmaps == 2); /* guard failure and failed thread's owned stack */
    struct _dthread supplied = {0};
    before = mappings;
    assert((intptr_t)__darling_thread_create(512 * 1024, 0, NULL, 1, 0, 123, 0, NULL, &supplied) == -35);
    assert(mappings == before && unmaps == 2); /* caller storage is untouched */
    puts("PASS stack overflow/guard failures, TSD layout, thread-creation error cleanup, and caller stack ownership");
}
