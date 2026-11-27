/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 *
 * Connection ownership and the poll(2) event loop. The event loop thread is
 * the only code that reads or writes client sockets or client_conn_t state.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "http.h"

#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_CLIENTS 64
#define WS_PAGE_SIZE 4096
#define HDR_BUF_SIZE 512

/*
 * Per-connection state machine:
 *
 *   READING_REQ -> SENDING_HEADERS -> [WAIT_PAGE <-> SENDING_BODY] -> READING_REQ
 *
 * A connection that is not persistent is closed instead of returning to
 * READING_REQ. HEAD requests and empty bodies skip the body states.
 */
typedef enum
{
    CONN_STATE_READING_REQ = 0,
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

    char rx_buf[REQ_BUF_SIZE];
    size_t rx_len;
    http_request_t request;

    int status;
    bool keep_alive;
    bool send_body;
    char hdr_buf[HDR_BUF_SIZE];
    size_t hdr_len;
    size_t hdr_sent;

    int file_fd;
    uint64_t body_total;
    uint64_t body_loaded;
    uint64_t body_sent;
    unsigned char stage[WS_PAGE_SIZE];
    size_t stage_len;
    size_t stage_sent;
} client_conn_t;

typedef struct
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

    uint64_t requests_served;
    uint64_t bytes_sent;
    uint64_t send_would_block;
} server_ctx_t;

int64_t monotonic_ms(void);
int set_nonblocking(int fd);
int set_cloexec(int fd);
int server_init(server_ctx_t *srv, uint16_t port, const char *doc_root);
int server_run_event_loop(server_ctx_t *srv, volatile sig_atomic_t *stop_flag);
void server_close(server_ctx_t *srv);

#endif
