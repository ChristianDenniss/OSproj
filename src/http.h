/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 *
 * HTTP/1.1 protocol helpers: request parsing, request-target decoding,
 * secure file resolution beneath the document root, MIME lookup and
 * response header formatting. Nothing in this module touches sockets.
 */

#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#define REQ_BUF_SIZE 8192
#define MAX_PATH_LEN 512
#define MAX_METHOD_LEN 16

typedef enum
{
    HTTP_GET = 0,
    HTTP_HEAD,
    HTTP_UNSUPPORTED
} http_method_t;

typedef enum
{
    PARSE_INCOMPLETE = 0,
    PARSE_COMPLETE,
    PARSE_FAILED
} parse_result_t;

typedef struct
{
    http_method_t method;
    char method_name[MAX_METHOD_LEN];
    char target[MAX_PATH_LEN];
    int minor_version;
    bool keep_alive;
    size_t header_len;
    int error_status;
} http_request_t;

/*
 * Parses one request head from buf[0..len). On PARSE_COMPLETE,
 * req->header_len is the number of bytes the request occupied, so any bytes
 * after it belong to the next pipelined request. On PARSE_FAILED,
 * req->error_status holds the HTTP status to send before closing.
 * capacity is the size of the caller's receive buffer, used to tell an
 * incomplete request apart from one that can never fit.
 */
parse_result_t http_parse_request(const char *buf, size_t len, size_t capacity, http_request_t *req);

/*
 * Converts a request target such as "/a%20b/?x=1" into a decoded,
 * validated path such as "/a b/index.html". Returns 0 on success or the
 * HTTP status code describing why the target was rejected.
 */
int http_decode_path(const char *target, char *out, size_t out_size);

/*
 * Opens a decoded path relative to root_fd one component at a time, refusing
 * symbolic links and anything that is not a regular file. Returns 0 and a
 * new descriptor owned by the caller, or an HTTP status code.
 */
int http_open_file(int root_fd, const char *path, int *out_fd, struct stat *out_st);

const char *http_mime_type(const char *path);
const char *http_reason_phrase(int status);

/* Returns the header length written to buf, or -1 if it did not fit. */
int http_format_header(char *buf, size_t size, int status, const char *content_type,
                       uint64_t content_length, bool keep_alive);

#endif
