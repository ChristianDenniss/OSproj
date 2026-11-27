/*
 * CS 3404 Lab 07
 * Student: Christian Dennis
 * Student ID: 3729509
 */

#include "http_server.h"
#include <stdio.h>
#include <signal.h>

int main(void)
{
    server_ctx_t server;
    uint16_t port = 8080;
    const char *doc_root = "./www";

    // A disconnected client should not terminate the server during send().
    signal(SIGPIPE, SIG_IGN);

    if (server_init(&server, port, doc_root) < 0)
    {
        perror("Failed to bind non-blocking server socket");
        return 1;
    }

    printf("[CS3404 Server] Listening on http://0.0.0.0:%u (Doc Root: %s)\n", port, doc_root);
    server_run_event_loop(&server);
    server_close(&server);
    return 0;
}
