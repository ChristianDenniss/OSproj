/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 */

#define _POSIX_C_SOURCE 200809L
#include "http_server.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define DEFAULT_DOC_ROOT "./www"
#define DEFAULT_LOG_PATH "access.log"

/* Large enough that it belongs in static storage rather than on the stack. */
static server_ctx_t server;

/*
 * Signal handlers only record the event and write one byte to a self-pipe
 * that the event loop polls; every real action happens in the loop. Both
 * operations are async-signal-safe.
 */
static volatile sig_atomic_t stop_signals = 0;
static int signal_pipe[2] = {-1, -1};

static void on_signal(int signo)
{
    int saved_errno = errno;
    if (signo == SIGINT || signo == SIGTERM)
    {
        stop_signals++;
    }
    const char byte = 1;
    ssize_t ignored = write(signal_pipe[1], &byte, 1);
    (void)ignored;
    errno = saved_errno;
}

static int make_signal_pipe(void)
{
    if (pipe(signal_pipe) < 0)
    {
        return -1;
    }
    for (int i = 0; i < 2; i++)
    {
        if (set_nonblocking(signal_pipe[i]) < 0 || set_cloexec(signal_pipe[i]) < 0)
        {
            return -1;
        }
    }
    return 0;
}

static void close_signal_pipe(void)
{
    for (int i = 0; i < 2; i++)
    {
        if (signal_pipe[i] != -1)
        {
            close(signal_pipe[i]);
            signal_pipe[i] = -1;
        }
    }
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    /* A client that disconnects mid-send must not terminate the server. */
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0)
    {
        return -1;
    }

    /* No SA_RESTART, so a blocked poll() returns EINTR promptly. */
    sa.sa_handler = on_signal;
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
    {
        return -1;
    }

    sa.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0)
    {
        return -1;
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-p port] [-r document_root] [-l access_log] [-c cache_frames] [-j io_jobs] "
            "[-q log_ring_slots]\n",
            prog);
}

static int parse_count(const char *text, unsigned long min, unsigned long max, size_t *out)
{
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value < min || value > max)
    {
        return -1;
    }
    *out = (size_t)value;
    return 0;
}

int main(int argc, char *argv[])
{
    server_config_t config = {
        .port = DEFAULT_PORT,
        .doc_root = DEFAULT_DOC_ROOT,
        .log_path = DEFAULT_LOG_PATH,
        .cache_frames = CACHE_DEFAULT_FRAMES,
        .io_jobs = IO_JOB_CAPACITY,
        .log_capacity = LOG_RING_CAPACITY,
    };
    size_t value = 0;

    int opt;
    while ((opt = getopt(argc, argv, "p:r:l:c:j:q:h")) != -1)
    {
        switch (opt)
        {
        case 'p':
            if (parse_count(optarg, 1024, 65535, &value) < 0)
            {
                fprintf(stderr, "webshell: port must be an unprivileged port (1024-65535)\n");
                return 1;
            }
            config.port = (uint16_t)value;
            break;
        case 'r':
            config.doc_root = optarg;
            break;
        case 'l':
            config.log_path = optarg;
            break;
        case 'c':
            if (parse_count(optarg, 1, CACHE_MAX_FRAMES, &config.cache_frames) < 0)
            {
                fprintf(stderr, "webshell: cache frames must be between 1 and %d\n", CACHE_MAX_FRAMES);
                return 1;
            }
            break;
        case 'j':
            if (parse_count(optarg, 1, IO_JOB_CAPACITY, &config.io_jobs) < 0)
            {
                fprintf(stderr, "webshell: I/O jobs must be between 1 and %d\n", IO_JOB_CAPACITY);
                return 1;
            }
            break;
        case 'q':
            if (parse_count(optarg, 1, LOG_RING_CAPACITY, &config.log_capacity) < 0)
            {
                fprintf(stderr, "webshell: log ring slots must be between 1 and %d\n", LOG_RING_CAPACITY);
                return 1;
            }
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    tzset();

    if (make_signal_pipe() < 0 || install_signal_handlers() < 0)
    {
        perror("webshell: signal setup");
        close_signal_pipe();
        return 1;
    }

    if (server_init(&server, &config) < 0)
    {
        server_close(&server);
        close_signal_pipe();
        return 1;
    }

    shell_printf("[webshell] listening on http://0.0.0.0:%u (root %s, log %s, cache %zu x %d B frames)\n",
                 config.port, config.doc_root, config.log_path, config.cache_frames, WS_PAGE_SIZE);
    shell_printf("[webshell] admin console ready; type 'help' for commands\n");

    int rc = server_run_event_loop(&server, signal_pipe[0], &stop_signals);

    uint64_t hits = server.cache.hits;
    uint64_t misses = server.cache.misses;
    uint64_t evictions = server.cache.evictions;
    server_close(&server);
    close_signal_pipe();

    shell_printf("[webshell] Stopped. requests=%llu bytes=%llu cache_hits=%llu cache_misses=%llu "
                 "cache_evictions=%llu io_deferred=%llu log_deferred=%llu log_written=%llu log_lost=%llu "
                 "send_would_block=%llu\n",
                 (unsigned long long)server.requests_served, (unsigned long long)server.bytes_sent,
                 (unsigned long long)hits, (unsigned long long)misses, (unsigned long long)evictions,
                 (unsigned long long)server.io_deferred, (unsigned long long)server.log_deferred,
                 (unsigned long long)server.log.written, (unsigned long long)server.log.lost,
                 (unsigned long long)server.send_would_block);
    return rc == 0 ? 0 : 1;
}
