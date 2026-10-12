/* Host adapter: compile the actual kqueue map/registry with Linux libc. */
#define _KQUEUE_PRIVATE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#define VISIBLE
#define slowpath(x) (x)
#define dbg_printf(...) ((void)0)
#define dbg_perror(...) ((void)0)
static inline void *atomic_ptr_cas(void **word, void *old, void *value) {
    __atomic_compare_exchange_n(word, &old, value, false,
                               __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}
