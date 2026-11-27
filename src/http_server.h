/*
 * CS 3404 Lab 07
 * Student: Christian Dennis
 * Student ID: 3729509
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_CLIENTS 64
#define REQ_BUF_SIZE 8192
#define MAX_PATH_LEN 512

typedef enum
{
    HTTP_GET = 0,
    HTTP_HEAD,
    HTTP_UNSUPPORTED
} http_method_t;

typedef enum
{
    CONN_STATE_READING_REQ = 0,
    CONN_STATE_SENDING_RESP,
    CONN_STATE_CLOSING
} conn_state_t;

typedef struct
{
    http_method_t method;
    char path[MAX_PATH_LEN];
    bool keep_alive;
    bool parse_complete;
} http_request_t;

typedef struct
{
    int fd;
    conn_state_t state;
    char rx_buf[REQ_BUF_SIZE];
    size_t rx_len;
    char tx_buf[REQ_BUF_SIZE];
    size_t tx_len;
    size_t tx_sent;
    http_request_t request;
} client_conn_t;

typedef struct
{
    int listen_fd;
    uint16_t port;
    const char *doc_root;
    client_conn_t clients[MAX_CLIENTS];
} server_ctx_t;

int set_nonblocking(int fd);
int server_init(server_ctx_t *srv, uint16_t port, const char *doc_root);
int parse_http_request(client_conn_t *c);
int sanitize_url_path(const char *url_path, const char *doc_root, char *out_safe_path, size_t max_out);
void server_run_event_loop(server_ctx_t *srv);
void server_close(server_ctx_t *srv);

#endif
