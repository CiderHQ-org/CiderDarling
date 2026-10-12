/* Compile against Darling and run only in a dedicated test prefix, no shim. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <math.h>
#include <unistd.h>
extern double __exp10(double);
extern float __exp10f(float);
extern int __ulock_wait(uint32_t, void *, uint64_t, uint32_t);
extern int __ulock_wait2(uint32_t, void *, uint64_t, uint64_t, uint64_t);
extern int __ulock_wake(uint32_t, void *, uint64_t);
static int failed;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int ready;
static void* condition_worker(void* unused) {
    (void)unused; pthread_mutex_lock(&mutex);
    while (!ready) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex); return NULL;
}
struct parallel_state { int active, maximum; };
static void parallel_work(void* argument, size_t index) {
    (void)index; struct parallel_state* state = argument;
    int active = __atomic_add_fetch(&state->active, 1, __ATOMIC_SEQ_CST);
    int prior = __atomic_load_n(&state->maximum, __ATOMIC_RELAXED);
    while (active > prior && !__atomic_compare_exchange_n(&state->maximum,&prior,active,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    uint64_t finish = mach_absolute_time() + 10000000;
    while (mach_absolute_time() < finish) __asm__ volatile("pause");
    __atomic_sub_fetch(&state->active,1,__ATOMIC_SEQ_CST);
}
static void require(int valid, const char *description) {
    printf("%s %s\n", valid ? "PASS" : "FAIL", description);
    if (!valid) failed++;
}
int main(void) {
    alarm(30);
    /* Volatile input keeps Clang from folding away the library calls. A
     * broken builtin wrapper otherwise spins forever on the first call. */
    volatile double decimal_exponent = 2.0;
    volatile float single_exponent = -1.0f;
    require(fabs(__exp10(decimal_exponent) - 100.0) < 1e-12,
            "double exp10 returns instead of recursively calling itself");
    require(fabsf(__exp10f(single_exponent) - 0.1f) < 1e-6f,
            "float exp10 returns and rounds its power result");
    int active = 0, family = 0; size_t integer_size = sizeof(int);
    require(!sysctlbyname("hw.activecpu", &active, &integer_size, NULL, 0) && active > 0 && integer_size == sizeof(int), "libdispatch active CPU count");
    integer_size = sizeof(int);
    require(!sysctlbyname("machdep.cpu.family", &family, &integer_size, NULL, 0) && family > 0 && integer_size == sizeof(int), "CPU family integer ABI");
    uint32_t word = 0;
    errno = 1234;
    int result = __ulock_wait(0x01000001, &word, 0, 1000);
    printf("ulock timeout result=%d errno=%d\n",result,errno);
    require(result == -ETIMEDOUT && errno == 1234, "exported ulock timeout preserves errno");
    errno = 1234;
    result = __ulock_wait2(0x01000001, &word, 0, 1000, 0);
    require(result == -ETIMEDOUT && errno == 1234, "exported nanosecond wait2 timeout");
    errno = 1234;
    result = __ulock_wake(0x010000ff, &word, 0);
    require(result == -EINVAL && errno == 1234, "exported wake error ABI");
    pthread_attr_t attr; size_t stack_size = 0;
    require(!pthread_attr_init(&attr) && !pthread_attr_getstacksize(&attr, &stack_size), "default thread attributes");
    require(stack_size == 8UL * 1024 * 1024, "default pthread stack is 8 MiB (default build policy)");
    pthread_attr_destroy(&attr);
    pthread_mutex_lock(&mutex);
    struct timeval now; gettimeofday(&now,NULL);
    struct timespec deadline = {now.tv_sec,now.tv_usec * 1000 + 30000000};
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_nsec -= 1000000000; deadline.tv_sec++; }
    require(pthread_cond_timedwait(&condition,&mutex,&deadline) == ETIMEDOUT, "condition-variable timeout error ABI");
    pthread_mutex_unlock(&mutex);
    pthread_t worker; require(!pthread_create(&worker,NULL,condition_worker,NULL), "condition waiter creation");
    usleep(20000); pthread_mutex_lock(&mutex); ready = 1; pthread_cond_signal(&condition); pthread_mutex_unlock(&mutex);
    require(!pthread_join(worker,NULL), "condition-variable wakeup and join");
    struct parallel_state parallel = {0};
    dispatch_apply_f((size_t)active * 8,dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0),&parallel,parallel_work);
    require(!parallel.active && parallel.maximum >= 1, "libdispatch parallel work completes");
    printf("Available CPUs=%d observed parallel workers=%d\n",active,parallel.maximum);
    int pair[2]; char data[8];
    if (!socketpair(AF_UNIX, SOCK_DGRAM, 0, pair)) {
        errno = 0;
        result = (int)recvfrom(pair[0], data, sizeof data, MSG_DONTWAIT, NULL, NULL);
        require(result == -1 && errno == EAGAIN, "nonblocking empty receive");
        require(write(pair[1], "hello", 5) == 5, "datagram write");
        result = (int)recvfrom(pair[0], data, sizeof data, MSG_DONTWAIT | MSG_PEEK, NULL, NULL);
        require(result == 5 && !memcmp(data, "hello", 5), "translated PEEK receive");
        close(pair[0]); close(pair[1]);
    } else require(0, "socketpair creation");
    int queue = kqueue(), pipes[2];
    if (queue >= 0 && !pipe(pipes)) {
        struct kevent change, event; struct timespec zero = {0, 0};
        EV_SET(&change, pipes[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        require(kevent(queue, &change, 1, NULL, 0, NULL) == 0, "register read watch");
        close(pipes[0]);
        require(kevent(queue, NULL, 0, &event, 1, &zero) == 0, "close removes stale read watch");
        close(pipes[1]); close(queue);
    } else require(0, "kqueue/pipe creation");
    fflush(stdout);
    return failed ? 1 : 0;
}
