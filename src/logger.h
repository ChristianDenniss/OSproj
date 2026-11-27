/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 *
 * Bounded access-log ring buffer. Records are formatted by the event loop
 * and written to access.log in batches by the I/O worker, so a slow disk
 * never blocks socket processing.
 *
 * Ring layout (indices modulo limit):
 *
 *   [head, head + in_flight)       batch currently being written by the worker
 *   [head + in_flight, head + count) records waiting for the next batch
 *
 * Admission control: a connection reserves a slot before its request is
 * processed and converts the reservation into a record when the transaction
 * ends (completed or aborted). The invariant reserved + count <= limit means
 * a record always has a slot, nothing unread is ever overwritten, and when
 * the ring is full new requests wait instead of logs being dropped.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOG_RING_CAPACITY 256
#define LOG_LINE_MAX 768
#define LOG_FLUSH_INTERVAL_MS 1000

typedef struct
{
    size_t length;
    char line[LOG_LINE_MAX];
} log_record_t;

typedef struct
{
    int fd;
    log_record_t records[LOG_RING_CAPACITY];
    size_t limit;
    size_t head;
    size_t count;
    size_t reserved;
    size_t in_flight;
    int64_t next_flush_ms;

    uint64_t written;
    uint64_t write_errors;
    uint64_t lost;
} access_log_t;

/* limit caps the ring size (0 means LOG_RING_CAPACITY). */
int logger_open(access_log_t *log, const char *path, size_t limit);
void logger_close(access_log_t *log);

bool logger_can_reserve(const access_log_t *log);
bool logger_reserve(access_log_t *log);

/* Converts one reservation into a record. status 0 is logged as "-". */
void logger_append(access_log_t *log, const char *client_ip, const char *method, const char *target, int status,
                   uint64_t body_bytes);

size_t logger_pending(const access_log_t *log);

/* Marks every waiting record as in flight and reports where the batch starts. */
void logger_begin_batch(access_log_t *log, size_t *start, size_t *count);
void logger_end_batch(access_log_t *log, bool ok);

/*
 * Writes count records starting at ring index start, staging them through
 * buf. Safe to call from the worker. Returns 0 or an errno value.
 */
int logger_write_records(int fd, const log_record_t *ring, size_t ring_limit, size_t start, size_t count,
                         char *buf, size_t buf_size);

#endif
