#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
extern void *_platform_memmove(void *, const void *, size_t);
extern void *_platform_memchr(const void *, int, size_t);
extern int _platform_memcmp(const void *, const void *, size_t);
extern int _platform_strcmp(const char *, const char *);
extern size_t _platform_strlen(const char *);
extern void _platform_memset_pattern4(void *, const void *, size_t);
extern void _platform_memset_pattern16(void *, const void *, size_t);
static int sign(int value) { return (value > 0) - (value < 0); }
int main(void) {
    unsigned char actual[4096], expected[4096], pattern[16];
    for (int i = 0; i < 16; i++) pattern[i] = (unsigned char)(3 + 7 * i);
    for (size_t length = 0; length <= 1024; length++) {
        for (size_t offset = 0; offset < 32; offset++) {
            for (size_t i = 0; i < sizeof actual; i++) actual[i] = expected[i] = (unsigned char)(i * 31);
            _platform_memmove(actual + 100 + offset, actual + 100, length);
            memmove(expected + 100 + offset, expected + 100, length);
            assert(!memcmp(actual, expected, sizeof actual));
            _platform_memmove(actual + 100, actual + 100 + offset, length);
            memmove(expected + 100, expected + 100 + offset, length);
            assert(!memcmp(actual, expected, sizeof actual));
        }
        _platform_memset_pattern16(actual + 3, pattern, length);
        for (size_t i = 0; i < length; i++) assert(actual[3 + i] == pattern[i % 16]);
        _platform_memset_pattern4(actual + 1, pattern, length);
        for (size_t i = 0; i < length; i++) assert(actual[1 + i] == pattern[i % 4]);
        assert(_platform_memchr(actual, 255, length) == memchr(actual, 255, length));
        assert(sign(_platform_memcmp(actual, expected, length)) == sign(memcmp(actual, expected, length)));
    }
    long page = sysconf(_SC_PAGESIZE);
    char *guarded = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(guarded != MAP_FAILED && !mprotect(guarded + page, (size_t)page, PROT_NONE));
    for (size_t length = 0; length <= 256; length++) {
        char *text = guarded + page - length - 1;
        memset(text, 'x', length); text[length] = 0;
        assert(_platform_strlen(text) == length);
        assert(_platform_strcmp(text, text) == 0);
        assert(sign(_platform_strcmp(text, "abc")) == sign(strcmp(text, "abc")));
    }
    munmap(guarded, (size_t)page * 2);
    puts("PASS optimized portable strings: overlap, alignment, pattern tails, unsigned comparisons, page-boundary reads");
}
