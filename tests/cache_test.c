/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 *
 * Unit tests for the page cache: frame alignment, fixed capacity, LRU
 * eviction order, exact hit/miss accounting, short pages and flushing.
 * Built and run by `make test`.
 */

#define _POSIX_C_SOURCE 200809L
#include "cache.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                        \
    do                                                                                                     \
    {                                                                                                      \
        if (!(cond))                                                                                       \
        {                                                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                \
            failures++;                                                                                    \
        }                                                                                                  \
    } while (0)

static file_id_t make_file(ino_t inode)
{
    file_id_t id;
    memset(&id, 0, sizeof(id));
    id.device = 1;
    id.inode = inode;
    id.size = 3 * WS_PAGE_SIZE;
    id.mtime_sec = 1700000000;
    id.mtime_nsec = 0;
    return id;
}

static void fill_page(unsigned char *page, unsigned char seed)
{
    for (size_t i = 0; i < WS_PAGE_SIZE; i++)
    {
        page[i] = (unsigned char)(seed + i);
    }
}

static bool lookup(page_cache_t *cache, const file_id_t *file, uint64_t index)
{
    unsigned char out[WS_PAGE_SIZE];
    size_t len = 0;
    return cache_lookup(cache, file, index, out, &len);
}

static void test_alignment_and_capacity(void)
{
    page_cache_t cache;
    CHECK(cache_init(&cache, 0) == -1);
    CHECK(cache_init(&cache, 64) == 0);
    CHECK(cache.capacity == 64);
    CHECK(cache.used == 0);
    for (size_t i = 0; i < cache.capacity; i++)
    {
        uintptr_t addr = (uintptr_t)cache_frame_address(&cache, i);
        CHECK(addr % WS_PAGE_SIZE == 0);
    }

    unsigned char page[WS_PAGE_SIZE];
    fill_page(page, 7);
    file_id_t file = make_file(10);
    for (uint64_t i = 0; i < 200; i++)
    {
        cache_insert(&cache, &file, i, page, WS_PAGE_SIZE);
    }
    CHECK(cache.used == 64);
    CHECK(cache.evictions == 200 - 64);
    cache_destroy(&cache);
}

/* Capacity 3, access order A B C A D: B is least recently used, so D evicts B. */
static void test_lru_order(void)
{
    page_cache_t cache;
    CHECK(cache_init(&cache, 3) == 0);
    unsigned char page[WS_PAGE_SIZE];
    fill_page(page, 1);

    file_id_t a = make_file(1);
    file_id_t b = make_file(2);
    file_id_t c = make_file(3);
    file_id_t d = make_file(4);

    cache_insert(&cache, &a, 0, page, WS_PAGE_SIZE);
    cache_insert(&cache, &b, 0, page, WS_PAGE_SIZE);
    cache_insert(&cache, &c, 0, page, WS_PAGE_SIZE);
    CHECK(cache.used == 3);
    CHECK(lookup(&cache, &a, 0));
    cache_insert(&cache, &d, 0, page, WS_PAGE_SIZE);

    CHECK(cache.evictions == 1);
    CHECK(!lookup(&cache, &b, 0));
    CHECK(lookup(&cache, &a, 0));
    CHECK(lookup(&cache, &c, 0));
    CHECK(lookup(&cache, &d, 0));
    CHECK(cache.hits == 4);
    CHECK(cache.misses == 1);
    cache_destroy(&cache);
}

static void test_short_pages_and_contents(void)
{
    page_cache_t cache;
    CHECK(cache_init(&cache, 8) == 0);
    file_id_t file = make_file(5);
    const size_t lengths[] = {1, WS_PAGE_SIZE - 1, WS_PAGE_SIZE};

    for (size_t i = 0; i < 3; i++)
    {
        unsigned char page[WS_PAGE_SIZE];
        fill_page(page, (unsigned char)(i * 31));
        cache_insert(&cache, &file, i, page, lengths[i]);

        unsigned char out[WS_PAGE_SIZE];
        size_t len = 0;
        CHECK(cache_lookup(&cache, &file, i, out, &len));
        CHECK(len == lengths[i]);
        CHECK(memcmp(out, page, len) == 0);
    }

    /* Same inode with a newer modification time is a different version. */
    file_id_t modified = file;
    modified.mtime_nsec = 1;
    CHECK(!lookup(&cache, &modified, 0));
    file_id_t resized = file;
    resized.size = 1;
    CHECK(!lookup(&cache, &resized, 0));

    /* Inserting a page that is already cached refreshes it instead of duplicating. */
    unsigned char page[WS_PAGE_SIZE];
    fill_page(page, 99);
    size_t used_before = cache.used;
    cache_insert(&cache, &file, 0, page, WS_PAGE_SIZE);
    CHECK(cache.used == used_before);
    cache_destroy(&cache);
}

static void test_flush(void)
{
    page_cache_t cache;
    CHECK(cache_init(&cache, 4) == 0);
    unsigned char page[WS_PAGE_SIZE];
    fill_page(page, 3);
    file_id_t file = make_file(6);

    cache_insert(&cache, &file, 0, page, WS_PAGE_SIZE);
    cache_insert(&cache, &file, 1, page, WS_PAGE_SIZE);
    CHECK(lookup(&cache, &file, 0));
    uint32_t epoch = cache.epoch;

    cache_flush(&cache);
    CHECK(cache.used == 0);
    CHECK(cache.epoch == epoch + 1);
    CHECK(cache.hits == 1);
    CHECK(!lookup(&cache, &file, 0));
    CHECK(!lookup(&cache, &file, 1));
    CHECK(cache.misses == 2);

    cache_insert(&cache, &file, 0, page, WS_PAGE_SIZE);
    CHECK(cache.used == 1);
    CHECK(cache.evictions == 0);
    cache_destroy(&cache);
}

int main(void)
{
    test_alignment_and_capacity();
    test_lru_order();
    test_short_pages_and_contents();
    test_flush();

    if (failures != 0)
    {
        fprintf(stderr, "cache_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("cache_test: all checks passed\n");
    return 0;
}
