#define CIDER_TRACE_BEGIN() 0
#define CIDER_TRACE_END(category, start, value) ((void)(start))
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#define LINUX_SYSCALL(...) ({ long result = syscall(__VA_ARGS__); result == -1 ? -(long)errno : result; })
struct kevent_qos_s { uint64_t value; };
#include "event-structure.h"
static int workq_parked_lock;
int sem_down(int*, int);
void sem_up(int*);
#include "semaphore-source.h"
static int tokens = 8, active, maximum, finished;
static void* worker(void* ignored) {
    (void)ignored;
    for (int i = 0; i < 2000; i++) {
        assert(sem_down(&tokens, -1));
        int count = __atomic_add_fetch(&active, 1, __ATOMIC_SEQ_CST);
        assert(count <= 8);
        int prior = __atomic_load_n(&maximum, __ATOMIC_RELAXED);
        while (count > prior && !__atomic_compare_exchange_n(&maximum, &prior, count, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        if (!(i % 100)) sched_yield();
        __atomic_sub_fetch(&active, 1, __ATOMIC_SEQ_CST); sem_up(&tokens);
    }
    __atomic_add_fetch(&finished, 1, __ATOMIC_RELAXED); return NULL;
}
static int wait_token;
static void ignore_signal(int signal) { (void)signal; }
static void* waiter(void* ignored) { (void)ignored; assert(sem_down(&wait_token, 2)); return NULL; }
int main(void) {
    alarm(30); pthread_t threads[64];
    for (int i = 0; i < 64; i++) assert(!pthread_create(threads + i, NULL, worker, NULL));
    for (int i = 0; i < 64; i++) assert(!pthread_join(threads[i], NULL));
    assert(tokens == 8 && finished == 64 && !active && maximum > 1);
    assert(!sem_down(&wait_token, 0) && !wait_token);
    int64_t before = workq_now_ns(); assert(!sem_down(&wait_token, 1));
    assert(workq_now_ns() - before >= 900000000LL && !wait_token);
    signal(SIGUSR1, ignore_signal); assert(!pthread_create(threads, NULL, waiter, NULL));
    usleep(20000); assert(!pthread_kill(threads[0], SIGUSR1)); usleep(20000);
    sem_up(&wait_token); assert(!pthread_join(threads[0], NULL) && !wait_token);
    struct kevent_qos_s buffers[3] = {{0}}, results[2] = {{11}, {22}};
    struct wq_kevent_data events[3] = {{.events=buffers,.capacity=1}, {.events=buffers+1,.capacity=1}, {.events=buffers+2,.capacity=1}};
    for (int i = 0; i < 3; i++) cider_workq_bind_event(events + i, 100 + i);
    assert(!workq_complete_event(102, results+1, 1) && buffers[2].value == 22 && events[2].sem == 1);
    assert(!workq_complete_event(100, results, 1) && buffers[0].value == 11 && events[0].sem == 1);
    assert(!workq_complete_event(101, results, 2) && events[1].nevents == -EINVAL && !buffers[1].value);
    assert(workq_complete_event(999, results, 1) == -EINVAL && !workq_events);
    puts("PASS 128000 contended semaphore operations, deadlines, signal retry, worker event ownership and bounds");
}
