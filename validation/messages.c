#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <darling/emulation/conversion/network/recvmsg.h>
#include <darling/emulation/conversion/network/duct.h>
#include <darling/emulation/xnu_syscall/bsd/impl/network/sendmsg.h>
#include <darling/emulation/xnu_syscall/bsd/impl/network/recvmsg.h>
int errno_linux_to_bsd(int value) { return value == -EAGAIN ? -35 : value; }
long sys_pthread_canceled(int action) { (void)action; return -1; }
int sockaddr_fixup_from_bsd(void* out, const void* input, int size) {
    memcpy(out, input, size); ((unsigned char*)out)[0] = ((const unsigned char*)input)[1]; ((unsigned char*)out)[1] = 0; return size;
}
int sockaddr_fixup_from_linux(void* out, const void* input, int size) {
    memcpy(out, input, size); ((unsigned char*)out)[1] = ((const unsigned char*)input)[0]; ((unsigned char*)out)[0] = size; return size;
}
static int fd_count(void) {
    DIR* directory = opendir("/proc/self/fd"); assert(directory); int count = 0;
    while (readdir(directory)) count++; closedir(directory); return count;
}
static void rights(unsigned char* buffer, int fd) {
    struct bsd_cmsghdr header = {BSD_CMSG_LEN(sizeof(fd)), 65535, 1};
    memcpy(buffer, &header, sizeof(header)); memcpy(buffer + BSD_CMSG_LEN(0), &fd, sizeof(fd));
}
int main(void) {
    alarm(10); int pair[2], pipes[2]; assert(!socketpair(AF_UNIX, SOCK_DGRAM, 0, pair)); assert(!pipe(pipes));
    unsigned char control[32] = {0}; rights(control, pipes[0]); rights(control+16, pipes[1]);
    char byte = 'X', received = 0; struct iovec send_vector = {&byte, 1}, receive_vector = {&received, 1};
    struct bsd_msghdr send_header = {.msg_iov=&send_vector,.msg_iovlen=1,.msg_control=control,.msg_controllen=32};
    unsigned char destination[64]; memset(destination, 0xa5, sizeof(destination));
    struct bsd_msghdr receive_header = {.msg_iov=&receive_vector,.msg_iovlen=1,.msg_control=destination,.msg_controllen=sizeof(destination)};
    assert(sys_sendmsg_nocancel(pair[0], &send_header, 0) == 1);
    assert(sys_recvmsg_nocancel(pair[1], &receive_header, BSD_MSG_DONTWAIT) == 1 && received == 'X');
    assert(!receive_header.msg_flags && receive_header.msg_controllen == 20);
    struct bsd_cmsghdr header; memcpy(&header, destination, sizeof(header));
    assert(header.cmsg_len == 20 && header.cmsg_level == 65535 && header.cmsg_type == 1);
    int fds[2]; memcpy(fds, destination+12, sizeof(fds));
    assert(fcntl(fds[0], F_GETFD) >= 0 && fcntl(fds[1], F_GETFD) >= 0); close(fds[0]); close(fds[1]);
    assert(destination[20] == 0xa5);
    int before = fd_count();
    for (int i = 0; i < 100; i++) {
        assert(sys_sendmsg_nocancel(pair[0], &send_header, 0) == 1);
        memset(destination, 0xa5, sizeof(destination)); receive_header.msg_controllen = (i % 2) ? 3 : 16;
        assert(sys_recvmsg_nocancel(pair[1], &receive_header, 0) == 1);
        assert(receive_header.msg_flags & BSD_MSG_CTRUNC);
        if (!(i % 2)) { memcpy(fds, destination+12, sizeof(int)); assert(!close(fds[0])); assert(destination[16] == 0xa5); }
        else assert(!receive_header.msg_controllen && destination[3] == 0xa5);
    }
    assert(fd_count() == before);
    uint32_t bad_length = 33; memcpy(control, &bad_length, 4);
    assert(sys_sendmsg_nocancel(pair[0], &send_header, 0) == -EINVAL);
    rights(control, pipes[0]); bad_length = 13; memcpy(control, &bad_length, 4);
    assert(sys_sendmsg_nocancel(pair[0], &send_header, 0) == -EINVAL);
    receive_header.msg_controllen = 0;
    assert(sys_recvmsg_nocancel(pair[1], &receive_header, BSD_MSG_DONTWAIT) == -35);
    send_header.msg_iovlen = -1; assert(sys_sendmsg_nocancel(pair[0], &send_header, 0) == -EINVAL);
    close(pair[0]); close(pair[1]); close(pipes[0]); close(pipes[1]);
    puts("PASS actual multi-message send/recv, SCM_RIGHTS, truncation bounds, malformed input and descriptor leak checks");
}
