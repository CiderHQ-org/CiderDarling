#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

extern long _test_wait(uint32_t, void *, uint64_t, uint32_t);
extern long _test_wait2(uint32_t, void *, uint64_t, uint64_t, uint64_t);
extern long _test_wake(uint32_t, void *, uint64_t);
extern long _test_getpid(void);
static const uint32_t no_errno = 0x01000000;
static uint32_t word;
static int cancellation_checks;
int errno_linux_to_bsd(int value) {
    switch (-value) {
    case EAGAIN: return -35;
    case ETIMEDOUT: return -60;
    default: return value;
    }
}
long sys_pthread_canceled(int action) { (void)action; cancellation_checks++; return -1; }
long _cerror_nocancel(int error) { errno = error; return -1; }
long fake_getpid(void) { return -22; }
static void *wake_thread(void *unused) {
    (void)unused;
    usleep(20000);
    __atomic_store_n(&word, 1, __ATOMIC_RELEASE);
    assert(_test_wake(no_errno | 1 | 0x100, &word, 0) == 0);
    return NULL;
}
int main(void) {
    alarm(5);
    errno = 1234;
    assert(_test_wait(no_errno | 1, &word, 0, 1000) == -60 && errno == 1234);
    assert(_test_wait(1, &word, 0, 1000) == -1 && errno == 60);
    errno = 1234;
    assert(_test_wait(no_errno | 255, &word, 0, 1) == -22 && errno == 1234);
    assert(_test_wake(no_errno | 255, &word, 0) == -22 && errno == 1234);
    assert(_test_wait(no_errno | 1, &word, 1, 0) == 1 && errno == 1234);
    assert(_test_wait2(no_errno | 1, &word, 0, 1, 0) == -60 && errno == 1234);
    assert(_test_wait2(no_errno | 1, &word, 1, UINT64_MAX, 0) == 1 && errno == 1234);
    assert(_test_wait2(no_errno | 1, &word, 0, 1, 1) == -22 && errno == 1234);
    assert(_test_wait2(no_errno | 1 | 0x20000, &word, 0, 1000, 0) == -60);
    assert(cancellation_checks == 1);
    /* Ordinary wrappers must retain cerror behavior. */
    errno = 1234;
    assert(_test_getpid() == -1 && errno == 22);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, wake_thread, NULL));
    errno = 1234;
    assert(_test_wait2(no_errno | 1, &word, 0, 1000000000ULL, 0) >= 0 && errno == 1234);
    assert(!pthread_join(thread, NULL));
    puts("PASS exported ulock ABI, errno preservation, nanosecond deadlines, cancellation flag, real futex wake");
}
