#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
static unsigned signature, ecx, edx;
static uint64_t xcr0;
static int xget_calls;
static void test_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
    (void)subleaf; memset(out, 0, 16);
    if (!leaf) { out[0] = 7; memcpy(out + 1, "Genu", 4); memcpy(out + 3, "ineI", 4); memcpy(out + 2, "ntel", 4); }
    else if (leaf == 1) { out[0] = signature; out[2] = ecx; out[3] = edx; }
    else if (leaf == 0x80000000) out[0] = 0x80000004;
    else { assert(leaf >= 0x80000002 && leaf <= 0x80000004); memset(out, 'A' + leaf - 0x80000002, 16); }
}
static uint64_t test_xcr0(void) { xget_calls++; return xcr0; }
#define CIDER_CPUID test_cpuid
#define CIDER_XCR0 test_xcr0
#define __simple_strlen strlen
#define sysctl_handler(name) static long name(void *old, unsigned long *oldlen)
static int cider_physical_cpus(void);
#include "cpu-source.h"
#define __simple_sprintf sprintf
#define __simple_atoi(text, end) atoi(text)
#define LINUX_SYSCALL(...) syscall(__VA_ARGS__)
static struct {int avail_cpus, physical_cpu;} host = {8, 4};
static const void *unused_host;
static typeof(host)* gethostinfo(void) { (void)unused_host; return &host; }
static int read_string(const char *path, char *out, int size) {
    FILE *file = fopen(path, "r"); if (!file) return 0;
    int ok = fgets(out, size, file) != NULL; fclose(file); return ok;
}
#include "affinity-source.h"
static int integer(long (*query)(void*, unsigned long*)) {
    int value = -1; unsigned long size = sizeof(value);
    assert(!query(&value, &size) && size == sizeof(value)); return value;
}
int main(void) {
    signature = (8U << 20) | (2U << 16) | (15U << 8) | (1U << 4) | 2;
    assert(integer(handle_family) == 23 && integer(handle_model) == 33 && integer(handle_stepping) == 2);
    signature = (9U << 16) | (6U << 8) | (7U << 4) | 1;
    assert(integer(handle_family) == 6 && integer(handle_model) == 151);
    unsigned long size = 0; assert(!handle_max_basic(NULL, &size) && size == 4);
    unsigned char guard[8]; memset(guard, 0xa5, sizeof(guard)); size = 3;
    assert(handle_max_basic(guard, &size) == -ENOMEM && size == 4 && guard[0] == 0xa5);
    char text[512]; size = sizeof(text); assert(!handle_vendor(text, &size) && !strcmp(text, "GenuineIntel") && size == 13);
    size = sizeof(text); assert(!handle_brand_string(text, &size) && size == 49 && text[0] == 'A' && text[16] == 'B' && text[32] == 'C');
    edx = (1U << 26) | (1U << 31); ecx = (1U << 28) | (1U << 12) | (1U << 29);
    size = sizeof(text); assert(!handle_features(text, &size) && strstr(text, "SSE2") && !strstr(text, "AVX") && !xget_calls);
    ecx |= (1U << 27) | (1U << 26); xcr0 = 2; size = sizeof(text); assert(!handle_features(text, &size) && !strstr(text, "AVX"));
    xcr0 = 6; size = sizeof(text); assert(!handle_features(text, &size) && strstr(text, "AVX1.0") && strstr(text, "FMA"));
    unsigned long exact = size; size = 0; assert(!handle_features(NULL, &size) && size == exact);
    size = 1; text[0] = 'Z'; assert(handle_features(text, &size) == -ENOMEM && text[0] == 'Z');
    cpu_set_t original; assert(!sched_getaffinity(0, sizeof(original), &original));
    int available = CPU_COUNT(&original); assert(cider_available_cpus() > 0 && cider_available_cpus() <= available);
    assert(cider_physical_cpus() > 0 && cider_physical_cpus() <= available);
    cpu_set_t one; CPU_ZERO(&one); for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &original)) { CPU_SET(i, &one); break; }
    assert(!sched_setaffinity(0, sizeof(one), &one));
    assert(cider_available_cpus() == 1 && cider_physical_cpus() == 1);
    assert(!sched_setaffinity(0, sizeof(original), &original));
    puts("PASS Intel/AMD CPUID leaves, integer ABI, feature buffer bounds, OS AVX gating and real CPU affinity");
}
