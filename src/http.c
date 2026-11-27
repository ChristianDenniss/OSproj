/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 */

#define _POSIX_C_SOURCE 200809L
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define MAX_SEGMENT_LEN 255

/* RFC 9110 token characters, used for methods and header names. */
static bool is_tchar(unsigned char ch)
{
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
    {
        return true;
    }
    return ch != '\0' && strchr("!#$%&'*+-.^_`|~", ch) != NULL;
}

static bool is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

static bool is_ows(char ch)
{
    return ch == ' ' || ch == '\t';
}

static const char *find_crlf(const char *p, const char *limit)
{
    for (; p + 1 < limit; p++)
    {
        if (p[0] == '\r' && p[1] == '\n')
        {
            return p;
        }
    }
    return NULL;
}

static bool header_name_is(const char *name, size_t len, const char *expected)
{
    return strlen(expected) == len && strncasecmp(name, expected, len) == 0;
}

static parse_result_t parse_fail(http_request_t *req, int status)
{
    req->error_status = status;
    req->keep_alive = false;
    return PARSE_FAILED;
}

/* Scans a comma-separated Connection header value for close / keep-alive. */
static void parse_connection_tokens(const char *value, size_t len, bool *saw_close, bool *saw_keep_alive)
{
    size_t i = 0;
    while (i < len)
    {
        while (i < len && (is_ows(value[i]) || value[i] == ','))
        {
            i++;
        }
        size_t start = i;
        while (i < len && value[i] != ',')
        {
            i++;
        }
        size_t end = i;
        while (end > start && is_ows(value[end - 1]))
        {
            end--;
        }

        size_t token_len = end - start;
        if (header_name_is(value + start, token_len, "close"))
        {
            *saw_close = true;
        }
        else if (header_name_is(value + start, token_len, "keep-alive"))
        {
            *saw_keep_alive = true;
        }
    }
}

static parse_result_t parse_request_line(const char *line, size_t line_len, http_request_t *req)
{
    size_t i = 0;
    while (i < line_len && is_tchar((unsigned char)line[i]))
    {
        i++;
    }
    if (i == 0 || i >= line_len || line[i] != ' ')
    {
        return parse_fail(req, 400);
    }
    if (i >= MAX_METHOD_LEN)
    {
        return parse_fail(req, 501);
    }
    memcpy(req->method_name, line, i);
    req->method_name[i] = '\0';

    if (strcmp(req->method_name, "GET") == 0)
    {
        req->method = HTTP_GET;
    }
    else if (strcmp(req->method_name, "HEAD") == 0)
    {
        req->method = HTTP_HEAD;
    }
    else
    {
        req->method = HTTP_UNSUPPORTED;
    }

    size_t target_start = i + 1;
    size_t target_end = target_start;
    while (target_end < line_len && line[target_end] != ' ')
    {
        target_end++;
    }
    size_t target_len = target_end - target_start;
    if (target_len == 0 || target_end >= line_len)
    {
        return parse_fail(req, 400);
    }
    if (target_len >= MAX_PATH_LEN)
    {
        return parse_fail(req, 414);
    }
    for (size_t k = target_start; k < target_end; k++)
    {
        unsigned char ch = (unsigned char)line[k];
        if (ch <= 0x20 || ch >= 0x7f)
        {
            return parse_fail(req, 400);
        }
    }
    memcpy(req->target, line + target_start, target_len);
    req->target[target_len] = '\0';
    if (req->target[0] != '/')
    {
        return parse_fail(req, 400);
    }

    const char *version = line + target_end + 1;
    size_t version_len = line_len - target_end - 1;
    if (version_len != 8 || memcmp(version, "HTTP/", 5) != 0 || !is_digit(version[5]) ||
        version[6] != '.' || !is_digit(version[7]))
    {
        return parse_fail(req, 400);
    }
    if (version[5] != '1')
    {
        return parse_fail(req, 505);
    }
    req->minor_version = version[7] - '0';
    return PARSE_COMPLETE;
}

parse_result_t http_parse_request(const char *buf, size_t len, size_t capacity, http_request_t *req)
{
    memset(req, 0, sizeof(*req));
    req->method = HTTP_UNSUPPORTED;
    strcpy(req->method_name, "-");
    strcpy(req->target, "-");

    /* Servers should ignore empty lines that precede a request line. */
    size_t start = 0;
    while (len - start >= 2 && buf[start] == '\r' && buf[start + 1] == '\n')
    {
        start += 2;
    }

    const char *head = buf + start;
    const char *limit = buf + len;
    const char *head_end = NULL;
    for (const char *p = head; p + 3 < limit; p++)
    {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
        {
            head_end = p + 4;
            break;
        }
    }

    if (head_end == NULL)
    {
        if (len >= capacity)
        {
            bool has_line_end = memchr(head, '\n', (size_t)(limit - head)) != NULL;
            return parse_fail(req, has_line_end ? 431 : 414);
        }
        return PARSE_INCOMPLETE;
    }

    req->header_len = (size_t)(head_end - buf);

    const char *line_end = find_crlf(head, head_end);
    parse_result_t result = parse_request_line(head, (size_t)(line_end - head), req);
    if (result != PARSE_COMPLETE)
    {
        return result;
    }

    int host_count = 0;
    int content_length_count = 0;
    bool has_body = false;
    bool has_transfer_encoding = false;
    bool saw_close = false;
    bool saw_keep_alive = false;

    /* The header block ends with the empty line at head_end - 2. */
    const char *fields_end = head_end - 2;
    const char *p = line_end + 2;
    while (p < fields_end)
    {
        const char *eol = find_crlf(p, head_end);
        if (is_ows(*p))
        {
            return parse_fail(req, 400);
        }

        const char *colon = p;
        while (colon < eol && is_tchar((unsigned char)*colon))
        {
            colon++;
        }
        if (colon == p || colon >= eol || *colon != ':')
        {
            return parse_fail(req, 400);
        }
        size_t name_len = (size_t)(colon - p);

        const char *value = colon + 1;
        const char *value_end = eol;
        while (value < value_end && is_ows(*value))
        {
            value++;
        }
        while (value_end > value && is_ows(value_end[-1]))
        {
            value_end--;
        }
        size_t value_len = (size_t)(value_end - value);
        for (size_t k = 0; k < value_len; k++)
        {
            unsigned char ch = (unsigned char)value[k];
            if ((ch < 0x20 && ch != '\t') || ch == 0x7f)
            {
                return parse_fail(req, 400);
            }
        }

        if (header_name_is(p, name_len, "Host"))
        {
            host_count++;
        }
        else if (header_name_is(p, name_len, "Connection"))
        {
            parse_connection_tokens(value, value_len, &saw_close, &saw_keep_alive);
        }
        else if (header_name_is(p, name_len, "Content-Length"))
        {
            content_length_count++;
            if (value_len == 0)
            {
                return parse_fail(req, 400);
            }
            for (size_t k = 0; k < value_len; k++)
            {
                if (!is_digit(value[k]))
                {
                    return parse_fail(req, 400);
                }
                if (value[k] != '0')
                {
                    has_body = true;
                }
            }
        }
        else if (header_name_is(p, name_len, "Transfer-Encoding"))
        {
            has_transfer_encoding = true;
        }

        p = eol + 2;
    }

    if (host_count > 1 || (req->minor_version >= 1 && host_count == 0) || content_length_count > 1)
    {
        return parse_fail(req, 400);
    }

    if (req->minor_version >= 1)
    {
        req->keep_alive = !saw_close;
    }
    else
    {
        req->keep_alive = saw_keep_alive && !saw_close;
    }

    /*
     * Request bodies are not supported. Unsupported methods still get a 405,
     * and every one of these responses closes the connection so unread body
     * bytes are never mistaken for the next request.
     */
    if (req->method == HTTP_UNSUPPORTED)
    {
        req->keep_alive = false;
        return PARSE_COMPLETE;
    }
    if (has_transfer_encoding)
    {
        return parse_fail(req, 501);
    }
    if (has_body)
    {
        return parse_fail(req, 413);
    }
    return PARSE_COMPLETE;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f')
    {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F')
    {
        return ch - 'A' + 10;
    }
    return -1;
}

int http_decode_path(const char *target, char *out, size_t out_size)
{
    size_t used = 0;
    for (const char *p = target; *p != '\0' && *p != '?'; p++)
    {
        unsigned char ch = (unsigned char)*p;
        if (ch == '#')
        {
            return 400;
        }
        if (ch == '%')
        {
            int high = hex_value(p[1]);
            if (high < 0)
            {
                return 400;
            }
            int low = hex_value(p[2]);
            if (low < 0)
            {
                return 400;
            }
            ch = (unsigned char)(high * 16 + low);
            p += 2;

            /* An encoded slash would make the segment boundaries ambiguous. */
            if (ch == '/')
            {
                return 400;
            }
        }
        if (ch < 0x20 || ch == 0x7f || ch == '\\')
        {
            return 400;
        }
        if (used + 1 >= out_size)
        {
            return 414;
        }
        out[used++] = (char)ch;
    }
    out[used] = '\0';

    if (used == 0 || out[0] != '/')
    {
        return 400;
    }

    /* Dot segments are rejected after decoding, so %2e%2e is caught too. */
    const char *segment = out;
    while (*segment != '\0')
    {
        while (*segment == '/')
        {
            segment++;
        }
        const char *end = segment;
        while (*end != '\0' && *end != '/')
        {
            end++;
        }
        size_t seg_len = (size_t)(end - segment);
        if ((seg_len == 1 && segment[0] == '.') || (seg_len == 2 && segment[0] == '.' && segment[1] == '.'))
        {
            return 403;
        }
        segment = end;
    }

    if (out[used - 1] == '/')
    {
        static const char index_name[] = "index.html";
        if (used + sizeof(index_name) > out_size)
        {
            return 414;
        }
        memcpy(out + used, index_name, sizeof(index_name));
    }
    return 0;
}

static int status_from_errno(int err)
{
    switch (err)
    {
    case ENOENT:
    case ENOTDIR:
        return 404;
    case EACCES:
    case EPERM:
    case ELOOP:
        return 403;
    case ENAMETOOLONG:
        return 414;
    case EMFILE:
    case ENFILE:
        return 503;
    default:
        return 500;
    }
}

int http_open_file(int root_fd, const char *path, int *out_fd, struct stat *out_st)
{
    char segment[MAX_SEGMENT_LEN + 1];
    int dir_fd = root_fd;
    const char *p = path;

    *out_fd = -1;
    for (;;)
    {
        while (*p == '/')
        {
            p++;
        }
        const char *end = strchr(p, '/');
        size_t seg_len = end != NULL ? (size_t)(end - p) : strlen(p);
        if (seg_len == 0 || seg_len > MAX_SEGMENT_LEN)
        {
            if (dir_fd != root_fd)
            {
                close(dir_fd);
            }
            return seg_len == 0 ? 404 : 414;
        }
        memcpy(segment, p, seg_len);
        segment[seg_len] = '\0';

        if (end != NULL)
        {
            /* Intermediate component: must be a real directory, not a link. */
            int next_fd = openat(dir_fd, segment, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            int saved_errno = errno;
            if (dir_fd != root_fd)
            {
                close(dir_fd);
            }
            if (next_fd < 0)
            {
                return status_from_errno(saved_errno);
            }
            dir_fd = next_fd;
            p = end;
            continue;
        }

        /* O_NONBLOCK keeps a FIFO planted in the root from stalling open(). */
        int fd = openat(dir_fd, segment, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        int saved_errno = errno;
        if (dir_fd != root_fd)
        {
            close(dir_fd);
        }
        if (fd < 0)
        {
            return status_from_errno(saved_errno);
        }
        if (fstat(fd, out_st) < 0)
        {
            saved_errno = errno;
            close(fd);
            return status_from_errno(saved_errno);
        }
        if (!S_ISREG(out_st->st_mode))
        {
            close(fd);
            return 403;
        }
        *out_fd = fd;
        return 0;
    }
}

const char *http_mime_type(const char *path)
{
    static const struct
    {
        const char *extension;
        const char *type;
    } types[] = {
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"},
        {"json", "application/json"},
        {"png", "image/png"},
        {"txt", "text/plain; charset=utf-8"},
    };

    const char *slash = strrchr(path, '/');
    const char *base = slash != NULL ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    if (dot != NULL && dot != base)
    {
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        {
            if (strcasecmp(dot + 1, types[i].extension) == 0)
            {
                return types[i].type;
            }
        }
    }
    return "application/octet-stream";
}

const char *http_reason_phrase(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 431:
        return "Request Header Fields Too Large";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Internal Server Error";
    }
}

int http_format_header(char *buf, size_t size, int status, const char *content_type,
                       uint64_t content_length, bool keep_alive)
{
    char date[64] = "Thu, 01 Jan 1970 00:00:00 GMT";
    time_t now = time(NULL);
    struct tm tm_utc;
    if (gmtime_r(&now, &tm_utc) != NULL)
    {
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm_utc);
    }

    int written = snprintf(buf, size,
                           "HTTP/1.1 %d %s\r\n"
                           "Date: %s\r\n"
                           "Server: webshell\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %llu\r\n"
                           "%s"
                           "Connection: %s\r\n"
                           "\r\n",
                           status, http_reason_phrase(status), date, content_type,
                           (unsigned long long)content_length,
                           status == 405 ? "Allow: GET, HEAD\r\n" : "",
                           keep_alive ? "keep-alive" : "close");
    if (written < 0 || (size_t)written >= size)
    {
        return -1;
    }
    return written;
}
