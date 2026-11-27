/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 */

#define _POSIX_C_SOURCE 200809L
#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_TARGET_MAX 600
#define LOG_METHOD_MAX 64

int logger_open(access_log_t *log, const char *path, size_t limit)
{
    memset(log, 0, sizeof(*log));
    log->limit = (limit == 0 || limit > LOG_RING_CAPACITY) ? LOG_RING_CAPACITY : limit;
    log->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    return log->fd < 0 ? -1 : 0;
}

void logger_close(access_log_t *log)
{
    if (log->fd != -1)
    {
        close(log->fd);
        log->fd = -1;
    }
}

bool logger_can_reserve(const access_log_t *log)
{
    return log->reserved + log->count < log->limit;
}

bool logger_reserve(access_log_t *log)
{
    if (!logger_can_reserve(log))
    {
        return false;
    }
    log->reserved++;
    return true;
}

/*
 * Copies src into dst, replacing quotes, backslashes and non-printable bytes
 * with \xHH so a request cannot forge extra fields or lines in the log.
 */
static void escape_field(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789abcdef";
    size_t used = 0;

    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++)
    {
        bool plain = *p >= 0x20 && *p < 0x7f && *p != '"' && *p != '\\';
        size_t needed = plain ? 1 : 4;
        if (used + needed + 4 > dst_size)
        {
            memcpy(dst + used, "...", 3);
            used += 3;
            break;
        }
        if (plain)
        {
            dst[used++] = (char)*p;
        }
        else
        {
            dst[used++] = '\\';
            dst[used++] = 'x';
            dst[used++] = hex[*p >> 4];
            dst[used++] = hex[*p & 0x0f];
        }
    }
    dst[used] = '\0';
}

void logger_append(access_log_t *log, const char *client_ip, const char *method, const char *target, int status,
                   uint64_t body_bytes)
{
    if (log->reserved == 0 || log->count >= log->limit)
    {
        return;
    }
    log->reserved--;

    char timestamp[64] = "-";
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) != NULL)
    {
        strftime(timestamp, sizeof(timestamp), "%d/%b/%Y:%H:%M:%S %z", &local);
    }

    char safe_method[LOG_METHOD_MAX];
    char safe_target[LOG_TARGET_MAX];
    escape_field(method, safe_method, sizeof(safe_method));
    escape_field(target, safe_target, sizeof(safe_target));

    char status_text[16] = "-";
    if (status > 0)
    {
        snprintf(status_text, sizeof(status_text), "%d", status);
    }

    log_record_t *record = &log->records[(log->head + log->count) % log->limit];
    int written = snprintf(record->line, sizeof(record->line), "[%s] %s \"%s %s\" %s %llu\n", timestamp, client_ip,
                           safe_method, safe_target, status_text, (unsigned long long)body_bytes);
    record->length = (written > 0 && (size_t)written < sizeof(record->line)) ? (size_t)written : 0;
    log->count++;
}

size_t logger_pending(const access_log_t *log)
{
    return log->count - log->in_flight;
}

void logger_begin_batch(access_log_t *log, size_t *start, size_t *count)
{
    *start = log->head;
    *count = log->count;
    log->in_flight = log->count;
}

void logger_end_batch(access_log_t *log, bool ok)
{
    log->head = (log->head + log->in_flight) % log->limit;
    log->count -= log->in_flight;
    if (ok)
    {
        log->written += log->in_flight;
    }
    else
    {
        log->write_errors++;
        log->lost += log->in_flight;
    }
    log->in_flight = 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t done = 0;
    while (done < len)
    {
        ssize_t n = write(fd, buf + done, len - done);
        if (n > 0)
        {
            done += (size_t)n;
        }
        else if (n < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            return n < 0 ? errno : EIO;
        }
    }
    return 0;
}

int logger_write_records(int fd, const log_record_t *ring, size_t ring_limit, size_t start, size_t count,
                         char *buf, size_t buf_size)
{
    size_t buffered = 0;
    for (size_t i = 0; i < count; i++)
    {
        const log_record_t *record = &ring[(start + i) % ring_limit];
        if (buffered + record->length > buf_size)
        {
            int err = write_all(fd, buf, buffered);
            if (err != 0)
            {
                return err;
            }
            buffered = 0;
        }
        memcpy(buf + buffered, record->line, record->length);
        buffered += record->length;
    }
    return write_all(fd, buf, buffered);
}
