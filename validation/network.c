#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <darling/emulation/xnu_syscall/bsd/impl/network/connectx.h>
#include <darling/emulation/conversion/network/recvmsg.h>

extern long sys_recvfrom_nocancel(int, void *, unsigned long, int, void *, int *);
int errno_linux_to_bsd(int value) { return value == -EAGAIN ? -35 : value; }
long sys_pthread_canceled(int action) { (void)action; return -1; }
struct sockaddr_fixup;
int sockaddr_fixup_from_linux(struct sockaddr_fixup *out, const void *address, int length) {
    (void)out; (void)address; return length;
}
static int bind_calls, connect_calls, send_calls;
static long bind_result, connect_result, send_result;
long sys_bind(int fd, const void *address, int length) {
    (void)fd; (void)address; (void)length; bind_calls++; return bind_result;
}
long sys_connect_nocancel(int fd, const void *address, int length) {
    (void)fd; (void)address; (void)length; connect_calls++; return connect_result;
}
long sys_sendmsg_nocancel(int fd, const struct bsd_msghdr *message, int flags) {
    (void)fd; assert(!flags); send_calls++;
    assert(message->msg_iovlen == 2 && message->msg_iov[0].iov_len == 3);
    return send_result;
}
int main(void) {
    alarm(3);
    int pair[2]; char data[8];
    assert(!socketpair(AF_UNIX, SOCK_DGRAM, 0, pair));
    errno = 1234;
    assert(sys_recvfrom_nocancel(pair[0], data, sizeof data, 0x80, NULL, NULL) == -35);
    assert(errno == 1234); /* syscall layer does not change libc errno */
    long wrote = send(pair[1], "hello", 5, 0);
    if (wrote != 5) fprintf(stderr, "datagram send=%ld errno=%d fds=%d/%d\n", wrote, errno, pair[0], pair[1]);
    assert(wrote == 5);
    assert(sys_recvfrom_nocancel(pair[0], data, sizeof data, 0x82, NULL, NULL) == 5);
    assert(!memcmp(data, "hello", 5));
    assert(sys_recvfrom_nocancel(pair[0], data, sizeof data, 0x80, NULL, NULL) == 5);
    assert(sys_recvfrom_nocancel(pair[0], data, sizeof data, 0x80, NULL, NULL) == -35);
    close(pair[0]); close(pair[1]);

    struct cider_sa_endpoints endpoints = {.dstaddr = "address", .dstaddrlen = 16};
    struct iovec vectors[2] = {{"abc", 3}, {"def", 3}};
    size_t sent = 999; uint32_t connid = 999;
    connect_result = -36; /* Darwin EINPROGRESS */
    assert(sys_connectx(1, &endpoints, 0, 0, vectors, 2, &sent, &connid) == -36);
    assert(sent == 0 && connid == 999 && send_calls == 0);
    connect_result = 0; send_result = 4;
    assert(sys_connectx(1, &endpoints, 0, 0, vectors, 2, &sent, &connid) == 0);
    assert(sent == 4 && connid == 1 && send_calls == 1);
    int previous_connect = connect_calls;
    assert(sys_connectx(1, &endpoints, 0, 1, NULL, 0, &sent, NULL) < 0);
    assert(sys_connectx(1, &endpoints, 0, 0, NULL, 2, &sent, NULL) == -22);
    assert(connect_calls == previous_connect);
    endpoints.srcaddr = "source"; endpoints.srcaddrlen = 16; bind_result = -22;
    assert(sys_connectx(1, &endpoints, 0, 0, NULL, 0, &sent, NULL) == -22);
    assert(bind_calls == 1 && connect_calls == previous_connect);
    puts("PASS real nonblocking datagrams, PEEK flag translation, connectx async/error/partial-send contracts");
}
