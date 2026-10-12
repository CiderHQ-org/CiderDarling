/* Compile against Darling and run only in a dedicated test prefix, no shim. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>
extern int __ulock_wait(uint32_t, void *, uint64_t, uint32_t);
extern int __ulock_wait2(uint32_t, void *, uint64_t, uint64_t, uint64_t);
extern int __ulock_wake(uint32_t, void *, uint64_t);
static int failed;
static void require(int valid, const char *description) {
    printf("%s %s\n", valid ? "PASS" : "FAIL", description);
    if (!valid) failed++;
}
int main(void) {
    alarm(5);
    uint32_t word = 0;
    errno = 1234;
    int result = __ulock_wait(0x01000001, &word, 0, 1000);
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
