#include "adapters/kqueue.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
#include "../src/external/libkqueue/src/common/map.c"
#include "../src/external/libkqueue/src/darling/listenregistry.c"

struct visitor { struct map *map; unsigned seen; };
static void visit(int index, void *value, void *opaque) {
    struct visitor *state = opaque;
    assert(value == (void *)(uintptr_t)(index + 1));
    state->seen++;
    if (index == 2)
        assert(map_insert(state->map, 100, (void *)101) == 0);
}
static void *register_many(void *opaque) {
    int base = (int)(intptr_t)opaque;
    for (int i = base; i < base + 1000; i++) {
        __darling_kqueue_register_listen(i);
        __darling_kqueue_register_listen(i);
        assert(__darling_kqueue_get_listen_status(i));
    }
    return NULL;
}
int main(void) {
    struct map *map = map_new(1024 * 1024 - 1);
    assert(map && map->scan_end == 0);
    assert(map_insert(map, -1, (void *)1) == -1);
    assert(map_insert(map, 1024 * 1024, (void *)1) == -1);
    assert(map_insert(map, 2, (void *)3) == 0);
    assert(map_insert(map, 2, (void *)999) == -1);
    assert(map_lookup(map, 2) == (void *)3);
    struct visitor visitor = {map, 0};
    map_foreach(map, visit, &visitor);
    assert(visitor.seen == 2 && map->scan_end == 101);
    assert(map_remove(map, 2, (void *)999) == -1);
    assert(map_remove(map, 2, (void *)3) == 0);
    assert(map_lookup(map, 2) == NULL);
    assert(map_replace(map, 700, NULL, (void *)701) == 0);
    assert(map->scan_end == 701);
    assert(map_delete(map, 700) == (void *)701);
    int last = 1024 * 1024 - 1;
    assert(map_insert(map, last, (void *)(uintptr_t)(last + 1)) == 0);
    visitor.seen = 0;
    map_foreach(map, visit, &visitor);
    assert(visitor.seen == 2); /* descriptor 100 and inclusive final descriptor */
    assert(map_delete(map, last) == (void *)(uintptr_t)(last + 1));
    munmap(map->data, (map->len + 1) * sizeof(void *));
    free(map);
    assert(!map_new(SIZE_MAX));

    pthread_t threads[4];
    for (int i = 0; i < 4; i++)
        assert(!pthread_create(&threads[i], NULL, register_many, (void *)(intptr_t)(1000 * i)));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(registry_vector_size == 4000);
    for (int i = 0; i < 4000; i += 2) __darling_kqueue_unregister_listen(i);
    for (int i = 0; i < 4000; i++)
        assert(__darling_kqueue_get_listen_status(i) == (bool)(i % 2));
    for (int i = 1; i < 4000; i += 2) __darling_kqueue_unregister_listen(i);
    __darling_kqueue_unregister_listen(-1);
    assert(registry_vector_size == 0);
    free(registry_vector);
    puts("PASS sparse map, inclusive boundary, callbacks, CAS removal, and concurrent listening registry");
}
