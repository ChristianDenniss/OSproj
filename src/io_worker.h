/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 *
 * Background filesystem worker. Opening and reading regular files and
 * appending to access.log can block even when sockets are nonblocking, so
 * those operations run on one worker thread instead of the event loop.
 *
 * Every job lives in a fixed pool and is always in exactly one place:
 *
 *   free stack (event loop) -> submit queue -> worker -> done queue -> free stack
 *
 * Both queues hold job indices and have the same capacity as the pool, so a
 * push can never overflow and the worker never waits for the event loop.
 * The event loop never waits for the worker either: if no job is free, the
 * connection simply stays in its waiting state and is retried later. One
 * extra job is reserved for log batches so file traffic cannot starve them.
 *
 * Ownership: the worker never touches sockets, client state or the cache.
 * A READ job takes ownership of the file descriptor while it is queued, and
 * the completion hands it back, so a connection that closes mid-read can
 * never cause a descriptor to be closed while the worker is using it. A LOG
 * job only reads ring records that the event loop has marked in flight.
 */

#ifndef IO_WORKER_H
#define IO_WORKER_H

#include "cache.h"
#include "http.h"
#include "logger.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#define IO_JOB_CAPACITY 32
#define IO_QUEUE_CAPACITY (IO_JOB_CAPACITY + 1)

typedef enum
{
    IO_JOB_OPEN = 0,
    IO_JOB_READ,
    IO_JOB_LOG
} io_job_type_t;

typedef struct
{
    io_job_type_t type;
    int client_slot;
    uint32_t generation;
    uint32_t cache_epoch;

    char path[DECODED_PATH_LEN];
    int fd;
    int status;
    struct stat st;

    file_id_t file;
    uint64_t page_index;
    size_t length;
    bool failed;

    int log_fd;
    const log_record_t *log_ring;
    size_t log_limit;
    size_t log_start;
    size_t log_count;
    int log_error;

    unsigned char data[WS_PAGE_SIZE];
} io_job_t;

typedef struct
{
    pthread_t thread;
    bool thread_started;
    bool lock_ready;
    bool cond_ready;
    pthread_mutex_t lock;
    pthread_cond_t work_ready;
    bool stopping;

    int root_fd;
    int notify_read_fd;
    int notify_write_fd;

    io_job_t jobs[IO_QUEUE_CAPACITY];
    size_t job_limit;
    bool log_job_busy;

    /* Event loop only. */
    int free_stack[IO_JOB_CAPACITY];
    size_t free_count;

    /* Protected by lock. */
    int submit_ring[IO_QUEUE_CAPACITY];
    size_t submit_head;
    size_t submit_count;
    int done_ring[IO_QUEUE_CAPACITY];
    size_t done_head;
    size_t done_count;
} io_worker_t;

/* job_limit caps how many file jobs are used (0 means IO_JOB_CAPACITY). */
int io_worker_start(io_worker_t *w, int root_fd, size_t job_limit);

/* Waits for already submitted jobs to finish, then joins the thread. */
void io_worker_stop(io_worker_t *w);

/* Releases the pipe and synchronization objects. Call after io_worker_stop. */
void io_worker_destroy(io_worker_t *w);

io_job_t *io_job_acquire(io_worker_t *w);
void io_job_release(io_worker_t *w, io_job_t *job);
size_t io_jobs_in_use(const io_worker_t *w);

/* The dedicated log job, or NULL if a log batch is already in flight. */
io_job_t *io_log_job_acquire(io_worker_t *w);

void io_worker_submit(io_worker_t *w, io_job_t *job);

/*
 * Drains the notification pipe, then removes every finished job. Taking all
 * of them matters: the wakeups were already drained, so a job left behind
 * would not trigger another poll event.
 */
size_t io_worker_take_completions(io_worker_t *w, io_job_t *out[IO_QUEUE_CAPACITY]);

#endif
