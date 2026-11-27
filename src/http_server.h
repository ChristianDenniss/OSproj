/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 *
 * Connection ownership and the poll(2) event loop. The event loop thread is
 * the only code that reads or writes client sockets, client_conn_t state,
 * the page cache or the server counters.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "cache.h"
#include "http.h"
#include "io_worker.h"
#include "logger.h"
#include "shell.h"

#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_CLIENTS 64
#define HDR_BUF_SIZE 512
#define SHUTDOWN_DRAIN_MS 5000

/*
 * Per-connection state machine:
 *
 *   READING_REQ -> WAIT_FILE -> SENDING_HEADERS -> [WAIT_PAGE <-> SENDING_BODY] -> READING_REQ
 *
 * WAIT_FILE and WAIT_PAGE wait on the I/O worker (a cache hit skips the
 * wait). A connection that is not persistent is closed instead of returning
 * to READING_REQ. Errors and HEAD requests skip the states they do not need.
 */
typedef enum
{
    CONN_STATE_READING_REQ = 0,
    CONN_STATE_WAIT_FILE,
    CONN_STATE_SENDING_HEADERS,
    CONN_STATE_WAIT_PAGE,
    CONN_STATE_SENDING_BODY
} conn_state_t;

typedef struct
{
    int fd;
    uint32_t generation;
    conn_state_t state;
    char peer_ip[INET_ADDRSTRLEN];
    int64_t last_active_ms;
    bool drive_pending;
    bool io_in_flight;
    bool page_miss_pending;
    bool log_reserved;
    bool waiting_for_log;

    char rx_buf[REQ_BUF_SIZE];
    size_t rx_len;
    http_request_t request;
    char path[DECODED_PATH_LEN];

    int status;
    bool keep_alive;
    bool send_body;
    char hdr_buf[HDR_BUF_SIZE];
    size_t hdr_len;
    size_t hdr_sent;

    int file_fd;
    file_id_t file;
    uint64_t body_total;
    uint64_t body_loaded;
    uint64_t body_sent;
    unsigned char stage[WS_PAGE_SIZE];
    size_t stage_len;
    size_t stage_sent;
} client_conn_t;

typedef struct
{
    uint16_t port;
    const char *doc_root;
    const char *log_path;
    size_t cache_frames;
    size_t io_jobs;
    size_t log_capacity;
} server_config_t;

/*
 * Server lifecycle: RUNNING -> DRAINING -> exit. Draining closes the
 * listener and idle connections, lets in-progress responses finish with
 * Connection: close, and ends when no clients remain or the deadline passes.
 */
typedef struct server_ctx
{
    int listen_fd;
    int root_fd;
    uint16_t port;
    const char *doc_root;
    client_conn_t clients[MAX_CLIENTS];
    size_t active_clients;
    uint32_t next_generation;
    int64_t start_ms;
    int64_t accept_resume_ms;

    bool draining;
    int64_t drain_deadline_ms;

    page_cache_t cache;
    bool cache_ready;
    io_worker_t worker;
    bool worker_ready;
    access_log_t log;
    bool log_ready;
    admin_shell_t shell;

    uint64_t requests_served;
    uint64_t bytes_sent;
    uint64_t send_would_block;
    uint64_t io_deferred;
    uint64_t log_deferred;
} server_ctx_t;

int64_t monotonic_ms(void);
int set_nonblocking(int fd);
int set_cloexec(int fd);
int server_init(server_ctx_t *srv, const server_config_t *config);

/*
 * Runs until shutdown completes. signal_fd is the read end of the signal
 * self-pipe; stop_signals counts SIGINT/SIGTERM deliveries (a second one
 * during draining skips the remaining drain time).
 */
int server_run_event_loop(server_ctx_t *srv, int signal_fd, volatile sig_atomic_t *stop_signals);
void server_begin_shutdown(server_ctx_t *srv, const char *reason);
void server_close(server_ctx_t *srv);

#endif
