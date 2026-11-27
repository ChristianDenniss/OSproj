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
    fprintf(stderr, "usage: %s [-p port] [-r document_root]\n", prog);
}

int main(int argc, char *argv[])
{
    uint16_t port = DEFAULT_PORT;
    const char *doc_root = DEFAULT_DOC_ROOT;

    int opt;
    while ((opt = getopt(argc, argv, "p:r:h")) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            char *end = NULL;
            unsigned long value = strtoul(optarg, &end, 10);
            if (end == optarg || *end != '\0' || value < 1024 || value > 65535)
            {
                fprintf(stderr, "webshell: port must be an unprivileged port (1024-65535)\n");
                return 1;
            }
            port = (uint16_t)value;
            break;
        }
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

    if (server_init(&server, port, doc_root) < 0)
    {
        return 1;
    }

    printf("[webshell] Listening on http://0.0.0.0:%u (document root: %s)\n", port, doc_root);
    fflush(stdout);

    int rc = server_run_event_loop(&server, &stop_requested);
    server_close(&server);

    printf("[webshell] Stopped. Requests served: %llu, bytes sent: %llu\n",
           (unsigned long long)server.requests_served, (unsigned long long)server.bytes_sent);
    return rc == 0 ? 0 : 1;
}
