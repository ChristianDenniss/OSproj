/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 *
 * Fixed-capacity page cache. The pool is one allocation aligned to 4096
 * bytes and split into frames of exactly one page each, so every frame
 * address is a multiple of 4096. Replacement is least recently used (LRU):
 * every lookup hit or insert stamps the frame with an increasing tick, and
 * when no frame is free the frame with the smallest tick is evicted.
 *
 * The cache is owned by the event loop thread and is not locked.
 */

#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define WS_PAGE_SIZE 4096
#define CACHE_DEFAULT_FRAMES 64
#define CACHE_MAX_FRAMES 65536

/*
 * Identifies one version of one file. If a file is replaced or modified,
 * its inode, size or modification time changes, so pages cached for the
 * old version are never returned for the new one.
 */
typedef struct
{
    dev_t device;
    ino_t inode;
    off_t size;
    time_t mtime_sec;
    long mtime_nsec;
} file_id_t;

typedef struct
{
    bool valid;
    file_id_t file;
    uint64_t page_index;
    size_t length;
    uint64_t last_used;
} cache_frame_t;

typedef struct
{
    unsigned char *pool;
    cache_frame_t *frames;
    size_t capacity;
    size_t used;
    uint64_t tick;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint32_t epoch;
} page_cache_t;

void file_id_from_stat(file_id_t *id, const struct stat *st);
bool file_id_equal(const file_id_t *a, const file_id_t *b);

int cache_init(page_cache_t *cache, size_t capacity);
void cache_destroy(page_cache_t *cache);

/* Address of frame i inside the aligned pool (exposed for tests and stats). */
const unsigned char *cache_frame_address(const page_cache_t *cache, size_t index);

/* Copies a cached page into dst on a hit. Counts exactly one hit or miss. */
bool cache_lookup(page_cache_t *cache, const file_id_t *file, uint64_t page_index, unsigned char *dst,
                  size_t *length_out);

/* Stores a page, reusing a free frame first and evicting the LRU frame if full. */
void cache_insert(page_cache_t *cache, const file_id_t *file, uint64_t page_index, const unsigned char *src,
                  size_t length);

/*
 * Invalidates every frame and advances the epoch. Reads that were started
 * before the flush carry the old epoch and must not repopulate the cache.
 * Hit, miss and eviction counters are lifetime totals and are kept.
 */
void cache_flush(page_cache_t *cache);

#endif
