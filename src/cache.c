/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 */

#define _POSIX_C_SOURCE 200809L
#include "cache.h"

#include <stdlib.h>
#include <string.h>

void file_id_from_stat(file_id_t *id, const struct stat *st)
{
    id->device = st->st_dev;
    id->inode = st->st_ino;
    id->size = st->st_size;
    id->mtime_sec = st->st_mtim.tv_sec;
    id->mtime_nsec = st->st_mtim.tv_nsec;
}

bool file_id_equal(const file_id_t *a, const file_id_t *b)
{
    return a->device == b->device && a->inode == b->inode && a->size == b->size &&
           a->mtime_sec == b->mtime_sec && a->mtime_nsec == b->mtime_nsec;
}

int cache_init(page_cache_t *cache, size_t capacity)
{
    memset(cache, 0, sizeof(*cache));
    if (capacity == 0 || capacity > CACHE_MAX_FRAMES)
    {
        return -1;
    }

    void *pool = NULL;
    if (posix_memalign(&pool, WS_PAGE_SIZE, capacity * WS_PAGE_SIZE) != 0)
    {
        return -1;
    }

    cache->frames = calloc(capacity, sizeof(cache->frames[0]));
    if (cache->frames == NULL)
    {
        free(pool);
        return -1;
    }

    cache->pool = pool;
    cache->capacity = capacity;
    return 0;
}

void cache_destroy(page_cache_t *cache)
{
    free(cache->pool);
    free(cache->frames);
    memset(cache, 0, sizeof(*cache));
}

const unsigned char *cache_frame_address(const page_cache_t *cache, size_t index)
{
    return cache->pool + index * WS_PAGE_SIZE;
}

static long find_frame(const page_cache_t *cache, const file_id_t *file, uint64_t page_index)
{
    for (size_t i = 0; i < cache->capacity; i++)
    {
        const cache_frame_t *frame = &cache->frames[i];
        if (frame->valid && frame->page_index == page_index && file_id_equal(&frame->file, file))
        {
            return (long)i;
        }
    }
    return -1;
}

bool cache_lookup(page_cache_t *cache, const file_id_t *file, uint64_t page_index, unsigned char *dst,
                  size_t *length_out)
{
    long index = find_frame(cache, file, page_index);
    if (index < 0)
    {
        cache->misses++;
        return false;
    }

    cache_frame_t *frame = &cache->frames[index];
    frame->last_used = ++cache->tick;
    memcpy(dst, cache_frame_address(cache, (size_t)index), frame->length);
    *length_out = frame->length;
    cache->hits++;
    return true;
}

void cache_insert(page_cache_t *cache, const file_id_t *file, uint64_t page_index, const unsigned char *src,
                  size_t length)
{
    if (length > WS_PAGE_SIZE)
    {
        return;
    }

    /* Two clients can miss on the same page at once; keep a single copy. */
    long index = find_frame(cache, file, page_index);

    if (index < 0)
    {
        for (size_t i = 0; i < cache->capacity; i++)
        {
            if (!cache->frames[i].valid)
            {
                index = (long)i;
                cache->used++;
                break;
            }
        }
    }

    if (index < 0)
    {
        size_t victim = 0;
        for (size_t i = 1; i < cache->capacity; i++)
        {
            if (cache->frames[i].last_used < cache->frames[victim].last_used)
            {
                victim = i;
            }
        }
        index = (long)victim;
        cache->evictions++;
    }

    cache_frame_t *frame = &cache->frames[index];
    memcpy(cache->pool + (size_t)index * WS_PAGE_SIZE, src, length);
    frame->valid = true;
    frame->file = *file;
    frame->page_index = page_index;
    frame->length = length;
    frame->last_used = ++cache->tick;
}

void cache_flush(page_cache_t *cache)
{
    for (size_t i = 0; i < cache->capacity; i++)
    {
        cache->frames[i].valid = false;
    }
    cache->used = 0;
    cache->epoch++;
}
