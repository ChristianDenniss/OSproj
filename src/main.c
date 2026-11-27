/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 */

#define _POSIX_C_SOURCE 200809L
#include "http_server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define DEFAULT_DOC_ROOT "./www"

/* Large enough that it belongs in static storage rather than on the stack. */
static server_ctx_t server;
static volatile sig_atomic_t stop_requested = 0;

static void on_stop_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
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

    /* No SA_RESTART, so a blocked poll() returns EINTR and sees the flag. */
    sa.sa_handler = on_stop_signal;
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
    {
        return -1;
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-p port] [-r document_root] [-c cache_frames] [-j io_jobs]\n", prog);
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
    uint16_t port = DEFAULT_PORT;
    const char *doc_root = DEFAULT_DOC_ROOT;
    size_t cache_frames = CACHE_DEFAULT_FRAMES;
    size_t io_jobs = IO_JOB_CAPACITY;
    size_t value = 0;

    int opt;
    while ((opt = getopt(argc, argv, "p:r:c:j:h")) != -1)
    {
        switch (opt)
        {
        case 'c':
            if (parse_count(optarg, 1, CACHE_MAX_FRAMES, &cache_frames) < 0)
            {
                fprintf(stderr, "webshell: cache frames must be between 1 and %d\n", CACHE_MAX_FRAMES);
                return 1;
            }
            break;
        case 'j':
            if (parse_count(optarg, 1, IO_JOB_CAPACITY, &io_jobs) < 0)
            {
                fprintf(stderr, "webshell: I/O jobs must be between 1 and %d\n", IO_JOB_CAPACITY);
                return 1;
            }
            break;
        case 'p':
            if (parse_count(optarg, 1024, 65535, &value) < 0)
            {
                fprintf(stderr, "webshell: port must be an unprivileged port (1024-65535)\n");
                return 1;
            }
            port = (uint16_t)value;
            break;
        case 'r':
            doc_root = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (install_signal_handlers() < 0)
    {
        perror("webshell: sigaction");
        return 1;
    }

    if (server_init(&server, port, doc_root, cache_frames, io_jobs) < 0)
    {
        return 1;
    }

    printf("[webshell] Listening on http://0.0.0.0:%u (document root: %s, cache: %zu x %d-byte frames)\n",
           port, doc_root, cache_frames, WS_PAGE_SIZE);
    fflush(stdout);

    int rc = server_run_event_loop(&server, &stop_requested);

    uint64_t hits = server.cache.hits;
    uint64_t misses = server.cache.misses;
    uint64_t evictions = server.cache.evictions;
    server_close(&server);

    printf("[webshell] Stopped. requests=%llu bytes=%llu cache_hits=%llu cache_misses=%llu "
           "cache_evictions=%llu io_deferred=%llu send_would_block=%llu\n",
           (unsigned long long)server.requests_served, (unsigned long long)server.bytes_sent,
           (unsigned long long)hits, (unsigned long long)misses, (unsigned long long)evictions,
           (unsigned long long)server.io_deferred, (unsigned long long)server.send_would_block);
    return rc == 0 ? 0 : 1;
}
