/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 *
 * Extends the Lab 07 poll(2) server with an explicit per-connection state
 * machine, page-by-page streaming of file bodies through the page cache,
 * file I/O delegated to a background worker, persistent and pipelined
 * requests, and bounded per-iteration work so no client can starve others.
 */

#define _POSIX_C_SOURCE 200809L
#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_BACKLOG 128
#define ACCEPT_BATCH 32
#define PAGES_PER_DRIVE 16
#define TRANSACTIONS_PER_DRIVE 16
#define CLIENT_TIMEOUT_MS 30000
#define ACCEPT_RETRY_MS 100
#define POLL_TIMEOUT_MS 1000

int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        return -1;
    }

    return 0;
}

int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags == -1)
    {
        return -1;
    }

    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
    {
        return -1;
    }

    return 0;
}

static void reset_response(client_conn_t *c)
{
    if (c->file_fd != -1)
    {
        close(c->file_fd);
        c->file_fd = -1;
    }
    c->status = 0;
    c->send_body = false;
    c->page_miss_pending = false;
    c->hdr_len = 0;
    c->hdr_sent = 0;
    c->body_total = 0;
    c->body_loaded = 0;
    c->body_sent = 0;
    c->stage_len = 0;
    c->stage_sent = 0;
}

/*
 * If a worker job is still in flight for this connection, the job keeps
 * ownership of any file descriptor it holds; the completion handler sees the
 * connection is gone and closes it there.
 */
static void close_client(server_ctx_t *srv, client_conn_t *c)
{
    reset_response(c);
    if (c->fd != -1)
    {
        close(c->fd);
        c->fd = -1;
        srv->active_clients--;
    }
    c->drive_pending = false;
    c->io_in_flight = false;
}

int server_init(server_ctx_t *srv, uint16_t port, const char *doc_root, size_t cache_frames, size_t io_jobs)
{
    memset(srv, 0, sizeof(*srv));
    srv->port = port;
    srv->doc_root = doc_root;
    srv->listen_fd = -1;
    srv->root_fd = -1;

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        srv->clients[i].fd = -1;
        srv->clients[i].file_fd = -1;
    }

    srv->root_fd = open(doc_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (srv->root_fd < 0)
    {
        fprintf(stderr, "webshell: cannot open document root '%s': %s\n", doc_root, strerror(errno));
        return -1;
    }

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0)
    {
        perror("webshell: socket");
        server_close(srv);
        return -1;
    }

    int opt = 1;
    if (setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0 ||
        set_nonblocking(srv->listen_fd) < 0 || set_cloexec(srv->listen_fd) < 0)
    {
        perror("webshell: configure listening socket");
        server_close(srv);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "webshell: bind port %u: %s\n", port, strerror(errno));
        server_close(srv);
        return -1;
    }

    if (listen(srv->listen_fd, LISTEN_BACKLOG) < 0)
    {
        perror("webshell: listen");
        server_close(srv);
        return -1;
    }

    if (cache_init(&srv->cache, cache_frames) < 0)
    {
        fprintf(stderr, "webshell: cannot allocate %zu cache frames\n", cache_frames);
        server_close(srv);
        return -1;
    }
    srv->cache_ready = true;

    srv->worker_ready = true;
    if (io_worker_start(&srv->worker, srv->root_fd, io_jobs) < 0)
    {
        perror("webshell: start I/O worker");
        server_close(srv);
        return -1;
    }

    srv->start_ms = monotonic_ms();
    return 0;
}

static void accept_clients(server_ctx_t *srv, int64_t now)
{
    for (int n = 0; n < ACCEPT_BATCH && srv->active_clients < MAX_CLIENTS; n++)
    {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(srv->listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0)
        {
            if (errno == EINTR || errno == ECONNABORTED)
            {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM)
            {
                /* Out of descriptors: stop polling the listener briefly instead of spinning. */
                srv->accept_resume_ms = now + ACCEPT_RETRY_MS;
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                perror("webshell: accept");
            }
            return;
        }

        if (set_nonblocking(client_fd) < 0 || set_cloexec(client_fd) < 0)
        {
            close(client_fd);
            continue;
        }

        /*
         * Headers and body pages go out as separate sends. Without
         * TCP_NODELAY, Nagle's algorithm holds the second small segment until
         * the client's delayed ACK (about 40 ms) on every keep-alive request.
         */
        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        client_conn_t *c = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (srv->clients[i].fd == -1)
            {
                c = &srv->clients[i];
                break;
            }
        }

        memset(c, 0, sizeof(*c));
        c->fd = client_fd;
        c->file_fd = -1;
        c->generation = ++srv->next_generation;
        c->state = CONN_STATE_READING_REQ;
        c->last_active_ms = now;
        if (inet_ntop(AF_INET, &peer.sin_addr, c->peer_ip, sizeof(c->peer_ip)) == NULL)
        {
            strcpy(c->peer_ip, "-");
        }
        srv->active_clients++;
    }
}

/* Sends buf[*sent..len). Returns 1 when finished, 0 on EAGAIN, -1 on error. */
static int send_pending(server_ctx_t *srv, client_conn_t *c, const void *buf, size_t len, size_t *sent,
                        int64_t now)
{
    while (*sent < len)
    {
        ssize_t n = send(c->fd, (const char *)buf + *sent, len - *sent, MSG_NOSIGNAL);
        if (n > 0)
        {
            *sent += (size_t)n;
            srv->bytes_sent += (uint64_t)n;
            c->last_active_ms = now;
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            srv->send_would_block++;
            return 0;
        }
        return -1;
    }
    return 1;
}

static void start_error_response(client_conn_t *c, int status, bool keep_alive)
{
    reset_response(c);
    c->status = status;
    c->keep_alive = keep_alive;
    c->send_body = c->request.method != HTTP_HEAD;

    int body_len = snprintf((char *)c->stage, sizeof(c->stage), "%d %s\n", status, http_reason_phrase(status));
    c->stage_len = (size_t)body_len;
    c->body_total = (uint64_t)body_len;
    c->body_loaded = (uint64_t)body_len;

    int hdr_len = http_format_header(c->hdr_buf, sizeof(c->hdr_buf), status, "text/plain; charset=utf-8",
                                     c->body_total, keep_alive);
    c->hdr_len = hdr_len > 0 ? (size_t)hdr_len : 0;
    c->state = CONN_STATE_SENDING_HEADERS;
}

static void start_response(client_conn_t *c)
{
    http_request_t *req = &c->request;
    if (req->method == HTTP_UNSUPPORTED)
    {
        start_error_response(c, 405, false);
        return;
    }

    int status = http_decode_path(req->target, c->path, sizeof(c->path));
    if (status != 0)
    {
        start_error_response(c, status, req->keep_alive);
        return;
    }

    reset_response(c);
    c->keep_alive = req->keep_alive;
    c->state = CONN_STATE_WAIT_FILE;
}

/* Takes ownership of fd, which the worker opened for this request. */
static void begin_file_response(client_conn_t *c, int fd, const struct stat *st)
{
    reset_response(c);
    c->file_fd = fd;
    file_id_from_stat(&c->file, st);
    c->status = 200;
    c->send_body = c->request.method == HTTP_GET;
    c->body_total = (uint64_t)st->st_size;

    int hdr_len = http_format_header(c->hdr_buf, sizeof(c->hdr_buf), 200, http_mime_type(c->path),
                                     c->body_total, c->keep_alive);
    if (hdr_len < 0)
    {
        start_error_response(c, 500, false);
        return;
    }
    c->hdr_len = (size_t)hdr_len;
    c->state = CONN_STATE_SENDING_HEADERS;
}

static bool submit_open(server_ctx_t *srv, client_conn_t *c)
{
    io_job_t *job = io_job_acquire(&srv->worker);
    if (job == NULL)
    {
        srv->io_deferred++;
        return false;
    }
    job->type = IO_JOB_OPEN;
    job->client_slot = (int)(c - srv->clients);
    job->generation = c->generation;
    memcpy(job->path, c->path, sizeof(job->path));
    c->io_in_flight = true;
    io_worker_submit(&srv->worker, job);
    return true;
}

static bool submit_read(server_ctx_t *srv, client_conn_t *c, uint64_t page_index, size_t length)
{
    io_job_t *job = io_job_acquire(&srv->worker);
    if (job == NULL)
    {
        srv->io_deferred++;
        return false;
    }
    job->type = IO_JOB_READ;
    job->client_slot = (int)(c - srv->clients);
    job->generation = c->generation;
    job->cache_epoch = srv->cache.epoch;
    job->file = c->file;
    job->page_index = page_index;
    job->length = length;

    /* The job owns the descriptor until its completion hands it back. */
    job->fd = c->file_fd;
    c->file_fd = -1;
    c->io_in_flight = true;
    io_worker_submit(&srv->worker, job);
    return true;
}

/* Returns true if the connection stays open for another request. */
static bool finish_transaction(server_ctx_t *srv, client_conn_t *c)
{
    srv->requests_served++;
    if (!c->keep_alive)
    {
        close_client(srv, c);
        return false;
    }

    size_t consumed = c->request.header_len;
    memmove(c->rx_buf, c->rx_buf + consumed, c->rx_len - consumed);
    c->rx_len -= consumed;
    reset_response(c);
    c->state = CONN_STATE_READING_REQ;
    return true;
}

/*
 * Advances one connection as far as it can go without blocking. Called when
 * its socket is readable or writable, when a worker job for it completes,
 * and again later if it still has queued work after using up its quota.
 */
static void client_drive(server_ctx_t *srv, client_conn_t *c, int64_t now)
{
    int pages = 0;
    int transactions = 0;

    for (;;)
    {
        switch (c->state)
        {
        case CONN_STATE_READING_REQ:
        {
            if (c->rx_len == 0)
            {
                return;
            }
            parse_result_t result = http_parse_request(c->rx_buf, c->rx_len, sizeof(c->rx_buf), &c->request);
            if (result == PARSE_INCOMPLETE)
            {
                return;
            }
            if (result == PARSE_FAILED)
            {
                start_error_response(c, c->request.error_status, false);
            }
            else
            {
                start_response(c);
            }
            break;
        }

        case CONN_STATE_WAIT_FILE:
        {
            if (!c->io_in_flight)
            {
                submit_open(srv, c);
            }
            return;
        }

        case CONN_STATE_SENDING_HEADERS:
        {
            int r = send_pending(srv, c, c->hdr_buf, c->hdr_len, &c->hdr_sent, now);
            if (r < 0)
            {
                close_client(srv, c);
                return;
            }
            if (r == 0)
            {
                return;
            }
            if (!c->send_body || c->body_total == 0)
            {
                if (!finish_transaction(srv, c))
                {
                    return;
                }
                if (++transactions >= TRANSACTIONS_PER_DRIVE)
                {
                    c->drive_pending = c->rx_len > 0;
                    return;
                }
                break;
            }
            c->state = c->stage_len > 0 ? CONN_STATE_SENDING_BODY : CONN_STATE_WAIT_PAGE;
            break;
        }

        case CONN_STATE_WAIT_PAGE:
        {
            if (c->io_in_flight)
            {
                return;
            }
            uint64_t page_index = c->body_loaded / WS_PAGE_SIZE;
            uint64_t remaining = c->body_total - c->body_loaded;
            size_t want = remaining < WS_PAGE_SIZE ? (size_t)remaining : WS_PAGE_SIZE;

            size_t got = 0;
            if (!c->page_miss_pending && cache_lookup(&srv->cache, &c->file, page_index, c->stage, &got))
            {
                if (got != want)
                {
                    close_client(srv, c);
                    return;
                }
                c->stage_len = got;
                c->stage_sent = 0;
                c->body_loaded += got;
                c->state = CONN_STATE_SENDING_BODY;
                if (++pages >= PAGES_PER_DRIVE)
                {
                    return;
                }
                break;
            }

            /* Remember the miss so a deferred retry is not counted twice. */
            c->page_miss_pending = !submit_read(srv, c, page_index, want);
            return;
        }

        case CONN_STATE_SENDING_BODY:
        {
            int r = send_pending(srv, c, c->stage, c->stage_len, &c->stage_sent, now);
            if (r < 0)
            {
                close_client(srv, c);
                return;
            }
            if (r == 0)
            {
                return;
            }
            c->body_sent += c->stage_len;
            c->stage_len = 0;
            c->stage_sent = 0;
            if (c->body_sent < c->body_total)
            {
                c->state = CONN_STATE_WAIT_PAGE;
                break;
            }
            if (!finish_transaction(srv, c))
            {
                return;
            }
            if (++transactions >= TRANSACTIONS_PER_DRIVE)
            {
                c->drive_pending = c->rx_len > 0;
                return;
            }
            break;
        }
        }
    }
}

static void handle_completion(server_ctx_t *srv, io_job_t *job, int64_t now)
{
    client_conn_t *c = &srv->clients[job->client_slot];
    bool live = c->fd != -1 && c->generation == job->generation && c->io_in_flight;

    if (job->type == IO_JOB_OPEN)
    {
        if (!live)
        {
            if (job->fd != -1)
            {
                close(job->fd);
            }
        }
        else
        {
            c->io_in_flight = false;
            if (job->status != 0)
            {
                start_error_response(c, job->status, c->keep_alive);
            }
            else
            {
                begin_file_response(c, job->fd, &job->st);
            }
        }
    }
    else
    {
        /* Pages read before a flush carry the old epoch and are not cached. */
        if (!job->failed && job->cache_epoch == srv->cache.epoch)
        {
            cache_insert(&srv->cache, &job->file, job->page_index, job->data, job->length);
        }

        if (!live)
        {
            close(job->fd);
        }
        else
        {
            c->io_in_flight = false;
            c->file_fd = job->fd;
            if (job->failed)
            {
                /* Headers are already out, so the only safe signal is closing. */
                close_client(srv, c);
                live = false;
            }
            else
            {
                memcpy(c->stage, job->data, job->length);
                c->stage_len = job->length;
                c->stage_sent = 0;
                c->body_loaded += job->length;
                c->state = CONN_STATE_SENDING_BODY;
            }
        }
    }

    io_job_release(&srv->worker, job);
    if (live)
    {
        client_drive(srv, c, now);
    }
}

static void process_completions(server_ctx_t *srv, int64_t now)
{
    io_job_t *done[IO_JOB_CAPACITY];
    size_t count = io_worker_take_completions(&srv->worker, done);
    for (size_t i = 0; i < count; i++)
    {
        handle_completion(srv, done[i], now);
    }
}

static void handle_readable(server_ctx_t *srv, client_conn_t *c, int64_t now)
{
    size_t space = sizeof(c->rx_buf) - c->rx_len;
    if (space == 0)
    {
        start_error_response(c, 431, false);
        client_drive(srv, c, now);
        return;
    }

    ssize_t n = recv(c->fd, c->rx_buf + c->rx_len, space, 0);
    if (n > 0)
    {
        c->rx_len += (size_t)n;
        c->last_active_ms = now;
        client_drive(srv, c, now);
    }
    else if (n == 0)
    {
        close_client(srv, c);
    }
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
    {
        close_client(srv, c);
    }
}

/* Only ask for POLLOUT when bytes are actually waiting to be sent. */
static short client_poll_events(const client_conn_t *c)
{
    switch (c->state)
    {
    case CONN_STATE_READING_REQ:
        return POLLIN;
    case CONN_STATE_SENDING_HEADERS:
    case CONN_STATE_SENDING_BODY:
        return POLLOUT;
    default:
        return 0;
    }
}

static bool client_waiting_for_job(const client_conn_t *c)
{
    return (c->state == CONN_STATE_WAIT_FILE || c->state == CONN_STATE_WAIT_PAGE) && !c->io_in_flight;
}

static bool client_needs_retry(const server_ctx_t *srv, const client_conn_t *c)
{
    if (c->drive_pending)
    {
        return true;
    }
        return client_waiting_for_job(c) && io_jobs_in_use(&srv->worker) < srv->worker.job_limit;
}

static void expire_idle_clients(server_ctx_t *srv, int64_t now)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        client_conn_t *c = &srv->clients[i];
        if (c->fd != -1 && !c->io_in_flight && now - c->last_active_ms >= CLIENT_TIMEOUT_MS)
        {
            close_client(srv, c);
        }
    }
}

int server_run_event_loop(server_ctx_t *srv, volatile sig_atomic_t *stop_flag)
{
    struct pollfd fds[MAX_CLIENTS + 2];
    int slot_of[MAX_CLIENTS + 2];
    uint32_t generation_of[MAX_CLIENTS + 2];

    while (!*stop_flag)
    {
        int64_t now = monotonic_ms();
        expire_idle_clients(srv, now);

        nfds_t nfds = 0;
        int listen_index = -1;
        bool retry_needed = false;

        fds[nfds].fd = srv->worker.notify_read_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        int notify_index = (int)nfds;
        nfds++;

        if (srv->active_clients < MAX_CLIENTS && now >= srv->accept_resume_ms)
        {
            fds[nfds].fd = srv->listen_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            listen_index = (int)nfds;
            nfds++;
        }

        int first_client = (int)nfds;
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            client_conn_t *c = &srv->clients[i];
            if (c->fd == -1)
            {
                continue;
            }
            fds[nfds].fd = c->fd;
            fds[nfds].events = client_poll_events(c);
            fds[nfds].revents = 0;
            slot_of[nfds] = i;
            generation_of[nfds] = c->generation;
            nfds++;
            retry_needed = retry_needed || client_needs_retry(srv, c);
        }

        /* Waiting for a free job needs no timeout: a completion wakes the loop. */
        int timeout = retry_needed ? 0 : POLL_TIMEOUT_MS;
        if (listen_index < 0 && srv->accept_resume_ms > now && srv->accept_resume_ms - now < timeout)
        {
            timeout = (int)(srv->accept_resume_ms - now);
        }

        int ready = poll(fds, nfds, timeout);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("webshell: poll");
            return -1;
        }

        now = monotonic_ms();
        if (fds[notify_index].revents & POLLIN)
        {
            process_completions(srv, now);
        }

        for (nfds_t j = (nfds_t)first_client; j < nfds; j++)
        {
            client_conn_t *c = &srv->clients[slot_of[j]];
            short revents = fds[j].revents;
            if (revents == 0 || c->fd == -1 || c->generation != generation_of[j])
            {
                continue;
            }

            if (revents & (POLLERR | POLLNVAL))
            {
                close_client(srv, c);
            }
            else if ((revents & POLLIN) && c->state == CONN_STATE_READING_REQ)
            {
                handle_readable(srv, c, now);
            }
            else if ((revents & POLLOUT) && c->state != CONN_STATE_READING_REQ)
            {
                client_drive(srv, c, now);
            }
            else if (revents & POLLHUP)
            {
                close_client(srv, c);
            }
        }

        /* Connections deferred for lack of a free worker job, or out of quota. */
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            client_conn_t *c = &srv->clients[i];
            if (c->fd != -1 && client_needs_retry(srv, c))
            {
                c->drive_pending = false;
                client_drive(srv, c, now);
            }
        }

        /* Accept last so new descriptors never collide with this round's poll entries. */
        if (listen_index >= 0 && (fds[listen_index].revents & POLLIN))
        {
            accept_clients(srv, now);
        }
    }

    return 0;
}

void server_close(server_ctx_t *srv)
{
    if (srv->worker_ready)
    {
        /* The worker finishes queued jobs; their descriptors are closed here. */
        io_worker_stop(&srv->worker);
        io_job_t *done[IO_JOB_CAPACITY];
        size_t count = io_worker_take_completions(&srv->worker, done);
        for (size_t i = 0; i < count; i++)
        {
            if (done[i]->fd != -1)
            {
                close(done[i]->fd);
            }
            io_job_release(&srv->worker, done[i]);
        }
        io_worker_destroy(&srv->worker);
        srv->worker_ready = false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        close_client(srv, &srv->clients[i]);
    }

    if (srv->cache_ready)
    {
        cache_destroy(&srv->cache);
        srv->cache_ready = false;
    }

    if (srv->listen_fd != -1)
    {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }

    if (srv->root_fd != -1)
    {
        close(srv->root_fd);
        srv->root_fd = -1;
    }
}
