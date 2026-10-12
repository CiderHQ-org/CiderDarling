#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#define LINUX_SYSCALL(...) ({int saved = errno; long result = syscall(__VA_ARGS__); if (result == -1) result = -(long)errno; errno = saved; result;})
#define WQ_FLAG_THREAD_PRIO_MASK 0x00000fff
#define WQ_FLAG_THREAD_OVERCOMMIT 0x00010000
#include "workqueue-deferral-source.h"

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int64_t waited_ms;
static void* waiter(void* limit) {
    int64_t started = now_ms();
    workq_lock();
    while (workq_running >= (intptr_t)limit) workq_wait_for_slot();
    workq_running++;
    workq_unlock();
    waited_ms = now_ms() - started;
    return NULL;
}

int main(void) {
    int flags;
    /* Interchangeable requests merge; the most urgent QoS is served first. */
    assert(workq_defer(0x2 | WQ_FLAG_THREAD_OVERCOMMIT, 2));
    assert(workq_defer(0x4, 1));
    assert(workq_defer(0x2 | WQ_FLAG_THREAD_OVERCOMMIT, 1));
    assert(workq_take_deferred(&flags) && flags == 0x4);
    for (int i = 0; i < 3; i++) assert(workq_take_deferred(&flags) && flags == (0x2 | WQ_FLAG_THREAD_OVERCOMMIT));
    assert(!workq_take_deferred(&flags));

    /* A full table refuses (the caller then waits for a slot instead). */
    for (int i = 0; i < WQ_DEFERRED_SLOTS; i++) assert(workq_defer(0x100 + i, 1));
    assert(!workq_defer(0x1000, 1));
    assert(workq_defer(0x100, 4));
    int served = 0;
    while (workq_take_deferred(&flags)) served++;
    assert(served == WQ_DEFERRED_SLOTS + 4);

    /* Returning a slot wakes a request waiting for one well before the
     * waiter's 100 ms re-check. */
    workq_running = 8;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, waiter, (void*)(intptr_t)8));
    for (;;) {
        workq_lock();
        int waiting = workq_capacity_waiters;
        workq_unlock();
        if (waiting) break;
        usleep(1000);
    }
    workq_lock();
    workq_release_slot();
    workq_unlock();
    assert(!pthread_join(thread, NULL));
    assert(workq_running == 8 && workq_capacity_waiters == 0);
    assert(waited_ms < 90);
    puts("PASS deferred worker requests merge, run by QoS, and released slots wake waiting requests");
}
