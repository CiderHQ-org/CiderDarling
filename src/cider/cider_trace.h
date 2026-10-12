#ifndef CIDER_TRACE_H
#define CIDER_TRACE_H
/* Optional numeric diagnostics. No app text, URLs or credentials are recorded. */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#ifndef CIDER_TRACE_THREAD
#include <pthread.h>
#define CIDER_TRACE_THREAD() ((uintptr_t)pthread_self())
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int cider_trace_ready;
static int cider_trace_fd = -1;
static uint64_t cider_trace_records;
static mach_timebase_info_data_t cider_trace_timebase;
static void cider_trace_initialize(void) {
    int saved = errno;
    const char* path = getenv("CIDER_TRACE_FILE");
    if (path && *path && !mach_timebase_info(&cider_trace_timebase) && cider_trace_timebase.denom) {
        int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat info;
        if (fd >= 0 && !fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_uid == getuid() && !(info.st_mode & 077) && info.st_nlink == 1)
            cider_trace_fd = fd;
        else if (fd >= 0) close(fd);
    }
    errno = saved;
}
static inline uint64_t cider_trace_begin(void) {
    if (__atomic_load_n(&cider_trace_ready, __ATOMIC_ACQUIRE) != 2) {
        int idle = 0;
        if (__atomic_compare_exchange_n(&cider_trace_ready, &idle, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            cider_trace_initialize(); __atomic_store_n(&cider_trace_ready, 2, __ATOMIC_RELEASE);
        } else while (__atomic_load_n(&cider_trace_ready, __ATOMIC_ACQUIRE) != 2) __asm__ volatile("pause");
    }
    return cider_trace_fd < 0 ? 0 : mach_absolute_time();
}
static inline void cider_trace_end(const char* category, uint64_t start, uint64_t value) {
    if (!start || __atomic_fetch_add(&cider_trace_records, 1, __ATOMIC_RELAXED) >= 65536) return;
    uint64_t finish = mach_absolute_time();
    uint64_t ns = (uint64_t)((__uint128_t)(finish - start) * cider_trace_timebase.numer / cider_trace_timebase.denom);
    uint64_t timestamp = (uint64_t)((__uint128_t)finish * cider_trace_timebase.numer / cider_trace_timebase.denom);
    int saved = errno;
    char line[256];
    int length = snprintf(line, sizeof(line), "{\"category\":\"%s\",\"pid\":%d,\"thread\":%llu,\"timestamp_ns\":%llu,\"duration_ns\":%llu,\"value\":%llu}\n",
        category, getpid(), (unsigned long long)CIDER_TRACE_THREAD(),
        (unsigned long long)timestamp, (unsigned long long)ns, (unsigned long long)value);
    struct stat info;
    if (length > 0 && length < (int)sizeof(line) && !fstat(cider_trace_fd, &info)
        && info.st_size <= 32 * 1024 * 1024 - length) write(cider_trace_fd, line, length);
    errno = saved;
}
#define CIDER_TRACE_BEGIN() cider_trace_begin()
#define CIDER_TRACE_END(category, start, value) cider_trace_end(category, start, value)
#endif
