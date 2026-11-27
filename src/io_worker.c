/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 */

#define _POSIX_C_SOURCE 200809L
#include "io_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static void run_open_job(io_worker_t *w, io_job_t *job)
{
    job->status = http_open_file(w->root_fd, job->path, &job->fd, &job->st);
}

static void run_read_job(io_job_t *job)
{
    off_t offset = (off_t)(job->page_index * WS_PAGE_SIZE);
    size_t got = 0;

    job->failed = false;
    while (got < job->length)
    {
        ssize_t n = pread(job->fd, job->data + got, job->length - got, offset + (off_t)got);
        if (n > 0)
        {
            got += (size_t)n;
        }
        else if (n < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            /* Read error, or the file shrank below the length already promised. */
            job->failed = true;
            break;
        }
    }
    job->length = got;
}

static void notify_event_loop(io_worker_t *w)
{
    const char byte = 1;
    for (;;)
    {
        ssize_t n = write(w->notify_write_fd, &byte, 1);
        /* EAGAIN means the pipe already holds unread wakeups, which is enough. */
        if (n >= 0 || errno != EINTR)
        {
            return;
        }
    }
}

static void *worker_main(void *arg)
{
    io_worker_t *w = arg;

    for (;;)
    {
        pthread_mutex_lock(&w->lock);
        while (w->submit_count == 0 && !w->stopping)
        {
            pthread_cond_wait(&w->work_ready, &w->lock);
        }
        if (w->submit_count == 0)
        {
            pthread_mutex_unlock(&w->lock);
            break;
        }
        int index = w->submit_ring[w->submit_head];
        w->submit_head = (w->submit_head + 1) % IO_JOB_CAPACITY;
        w->submit_count--;
        pthread_mutex_unlock(&w->lock);

        /* The blocking work happens with no lock held. */
        io_job_t *job = &w->jobs[index];
        if (job->type == IO_JOB_OPEN)
        {
            run_open_job(w, job);
        }
        else
        {
            run_read_job(job);
        }

        pthread_mutex_lock(&w->lock);
        w->done_ring[(w->done_head + w->done_count) % IO_JOB_CAPACITY] = index;
        w->done_count++;
        pthread_mutex_unlock(&w->lock);

        notify_event_loop(w);
    }

    return NULL;
}

static int make_notify_pipe(io_worker_t *w)
{
    int fds[2];
    if (pipe(fds) < 0)
    {
        return -1;
    }
    w->notify_read_fd = fds[0];
    w->notify_write_fd = fds[1];

    for (int i = 0; i < 2; i++)
    {
        int fl = fcntl(fds[i], F_GETFL, 0);
        int fd_flags = fcntl(fds[i], F_GETFD, 0);
        if (fl < 0 || fd_flags < 0 || fcntl(fds[i], F_SETFL, fl | O_NONBLOCK) < 0 ||
            fcntl(fds[i], F_SETFD, fd_flags | FD_CLOEXEC) < 0)
        {
            return -1;
        }
    }
    return 0;
}

int io_worker_start(io_worker_t *w, int root_fd, size_t job_limit)
{
    memset(w, 0, sizeof(*w));
    w->root_fd = root_fd;
    w->notify_read_fd = -1;
    w->notify_write_fd = -1;

    if (job_limit == 0 || job_limit > IO_JOB_CAPACITY)
    {
        job_limit = IO_JOB_CAPACITY;
    }
    w->job_limit = job_limit;
    for (size_t i = 0; i < IO_JOB_CAPACITY; i++)
    {
        w->jobs[i].fd = -1;
    }
    for (size_t i = 0; i < job_limit; i++)
    {
        w->free_stack[i] = (int)(job_limit - 1 - i);
    }
    w->free_count = job_limit;

    if (make_notify_pipe(w) < 0)
    {
        return -1;
    }
    if (pthread_mutex_init(&w->lock, NULL) != 0)
    {
        return -1;
    }
    w->lock_ready = true;
    if (pthread_cond_init(&w->work_ready, NULL) != 0)
    {
        return -1;
    }
    w->cond_ready = true;

    /* Block process signals in the worker so they are delivered to the event loop. */
    sigset_t block;
    sigset_t previous;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGCHLD);
    sigaddset(&block, SIGHUP);
    sigaddset(&block, SIGQUIT);
    pthread_sigmask(SIG_BLOCK, &block, &previous);
    int rc = pthread_create(&w->thread, NULL, worker_main, w);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (rc != 0)
    {
        return -1;
    }
    w->thread_started = true;
    return 0;
}

void io_worker_stop(io_worker_t *w)
{
    if (!w->thread_started)
    {
        return;
    }
    pthread_mutex_lock(&w->lock);
    w->stopping = true;
    pthread_cond_broadcast(&w->work_ready);
    pthread_mutex_unlock(&w->lock);

    pthread_join(w->thread, NULL);
    w->thread_started = false;
}

void io_worker_destroy(io_worker_t *w)
{
    if (w->cond_ready)
    {
        pthread_cond_destroy(&w->work_ready);
        w->cond_ready = false;
    }
    if (w->lock_ready)
    {
        pthread_mutex_destroy(&w->lock);
        w->lock_ready = false;
    }
    if (w->notify_read_fd != -1)
    {
        close(w->notify_read_fd);
        w->notify_read_fd = -1;
    }
    if (w->notify_write_fd != -1)
    {
        close(w->notify_write_fd);
        w->notify_write_fd = -1;
    }
}

io_job_t *io_job_acquire(io_worker_t *w)
{
    if (w->free_count == 0)
    {
        return NULL;
    }
    io_job_t *job = &w->jobs[w->free_stack[--w->free_count]];
    memset(job, 0, offsetof(io_job_t, data));
    job->fd = -1;
    return job;
}

void io_job_release(io_worker_t *w, io_job_t *job)
{
    w->free_stack[w->free_count++] = (int)(job - w->jobs);
}

size_t io_jobs_in_use(const io_worker_t *w)
{
    return w->job_limit - w->free_count;
}

void io_worker_submit(io_worker_t *w, io_job_t *job)
{
    pthread_mutex_lock(&w->lock);
    w->submit_ring[(w->submit_head + w->submit_count) % IO_JOB_CAPACITY] = (int)(job - w->jobs);
    w->submit_count++;
    pthread_cond_signal(&w->work_ready);
    pthread_mutex_unlock(&w->lock);
}

size_t io_worker_take_completions(io_worker_t *w, io_job_t *out[IO_JOB_CAPACITY])
{
    /*
     * Drain wakeups first. A completion pushed after this point writes a new
     * wakeup byte, so it is either taken below or seen on the next poll.
     */
    char sink[64];
    while (read(w->notify_read_fd, sink, sizeof(sink)) > 0)
    {
    }

    size_t taken = 0;
    pthread_mutex_lock(&w->lock);
    while (w->done_count > 0)
    {
        out[taken++] = &w->jobs[w->done_ring[w->done_head]];
        w->done_head = (w->done_head + 1) % IO_JOB_CAPACITY;
        w->done_count--;
    }
    pthread_mutex_unlock(&w->lock);
    return taken;
}
