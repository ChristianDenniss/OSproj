/*
 * CS 3404 Lab 07
 * Student: Christian Dennis
 * Student ID: 3729509
 */

#define _POSIX_C_SOURCE 200809L
#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        return -1;
    }

    return 0;
}

int server_init(server_ctx_t *srv, uint16_t port, const char *doc_root)
{
    memset(srv, 0, sizeof(*srv));
    srv->port = port;
    srv->doc_root = doc_root;

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        srv->clients[i].fd = -1;
    }

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0)
    {
        return -1;
    }

    int opt = 1;
    if (setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0 ||
        set_nonblocking(srv->listen_fd) < 0)
    {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return -1;
    }

    if (listen(srv->listen_fd, 128) < 0)
    {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return -1;
    }

    return 0;
}

int sanitize_url_path(const char *url_path, const char *doc_root, char *out_safe_path, size_t max_out)
{
    if (url_path == NULL || url_path[0] != '/')
    {
        return -1;
    }

    if (strstr(url_path, "..") != NULL)
    {
        return -1;
    }

    const char *target = strcmp(url_path, "/") == 0 ? "/index.html" : url_path;
    int written = snprintf(out_safe_path, max_out, "%s%s", doc_root, target);
    if (written < 0 || (size_t)written >= max_out)
    {
        return -1;
    }

    return 0;
}

int parse_http_request(client_conn_t *c)
{
    if (c->rx_len >= sizeof(c->rx_buf))
    {
        return -1;
    }

    c->rx_buf[c->rx_len] = '\0';
    char *hdr_end = strstr(c->rx_buf, "\r\n\r\n");
    if (!hdr_end)
    {
        c->request.parse_complete = false;
        return 0;
    }

    char line[512];
    char *first_line_end = strstr(c->rx_buf, "\r\n");
    if (!first_line_end)
    {
        return -1;
    }

    size_t line_len = (size_t)(first_line_end - c->rx_buf);
    if (line_len >= sizeof(line))
    {
        return -1;
    }

    memcpy(line, c->rx_buf, line_len);
    line[line_len] = '\0';

    char method_str[16];
    char uri_str[MAX_PATH_LEN];
    char ver_str[16];
    if (sscanf(line, "%15s %511s %15s", method_str, uri_str, ver_str) != 3)
    {
        return -1;
    }

    if (strcmp(method_str, "GET") == 0)
    {
        c->request.method = HTTP_GET;
    }
    else if (strcmp(method_str, "HEAD") == 0)
    {
        c->request.method = HTTP_HEAD;
    }
    else
    {
        c->request.method = HTTP_UNSUPPORTED;
    }

    strcpy(c->request.path, uri_str);

    c->request.keep_alive = true;
    if (strcasestr(c->rx_buf, "Connection: close") != NULL)
    {
        c->request.keep_alive = false;
    }

    c->request.parse_complete = true;
    return 1;
}

static void prepare_response(client_conn_t *c, const char *doc_root)
{
    c->tx_sent = 0;

    if (c->request.method == HTTP_UNSUPPORTED)
    {
        const char *err405 = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 18\r\nConnection: close\r\n\r\nMethod Not Allowed";
        c->tx_len = (size_t)snprintf(c->tx_buf, sizeof(c->tx_buf), "%s", err405);
        c->request.keep_alive = false;
        c->state = CONN_STATE_SENDING_RESP;
        return;
    }

    char safe_path[MAX_PATH_LEN];
    if (sanitize_url_path(c->request.path, doc_root, safe_path, sizeof(safe_path)) < 0)
    {
        const char *err403 = "HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\nConnection: close\r\n\r\nForbidden";
        c->tx_len = (size_t)snprintf(c->tx_buf, sizeof(c->tx_buf), "%s", err403);
        c->request.keep_alive = false;
        c->state = CONN_STATE_SENDING_RESP;
        return;
    }

    FILE *f = fopen(safe_path, "rb");
    if (!f)
    {
        const char *err404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
        c->tx_len = (size_t)snprintf(c->tx_buf, sizeof(c->tx_buf), "%s", err404);
        c->request.keep_alive = false;
        c->state = CONN_STATE_SENDING_RESP;
        return;
    }

    long fsize = -1;
    if (fseek(f, 0, SEEK_END) == 0)
    {
        fsize = ftell(f);
    }

    if (fsize >= 0 && fseek(f, 0, SEEK_SET) != 0)
    {
        fsize = -1;
    }

    const char *conn_hdr = c->request.keep_alive ? "keep-alive" : "close";
    int hdr_len = -1;
    if (fsize >= 0)
    {
        hdr_len = snprintf(c->tx_buf, sizeof(c->tx_buf),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %ld\r\nConnection: %s\r\n\r\n",
            fsize, conn_hdr);
    }

    if (hdr_len < 0 || (size_t)hdr_len >= sizeof(c->tx_buf) ||
        (c->request.method == HTTP_GET && (unsigned long)fsize > sizeof(c->tx_buf) - (size_t)hdr_len))
    {
        fclose(f);
        const char *err500 = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 14\r\nConnection: close\r\n\r\nFile too large";
        c->tx_len = (size_t)snprintf(c->tx_buf, sizeof(c->tx_buf), "%s", err500);
        c->request.keep_alive = false;
        c->state = CONN_STATE_SENDING_RESP;
        return;
    }

    size_t read_bytes = 0;
    if (c->request.method == HTTP_GET)
    {
        read_bytes = fread(c->tx_buf + hdr_len, 1, (size_t)fsize, f);
    }
    fclose(f);

    if (c->request.method == HTTP_GET && read_bytes != (size_t)fsize)
    {
        const char *err500 = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 14\r\nConnection: close\r\n\r\nFile too large";
        c->tx_len = (size_t)snprintf(c->tx_buf, sizeof(c->tx_buf), "%s", err500);
        c->request.keep_alive = false;
        c->state = CONN_STATE_SENDING_RESP;
        return;
    }

    c->tx_len = (size_t)hdr_len + read_bytes;
    c->state = CONN_STATE_SENDING_RESP;
}

void server_run_event_loop(server_ctx_t *srv)
{
    struct pollfd fds[MAX_CLIENTS + 1];

    while (1)
    {
        fds[0].fd = srv->listen_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        int nfds = 1;

        int client_idx_map[MAX_CLIENTS + 1];
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (srv->clients[i].fd != -1)
            {
                fds[nfds].fd = srv->clients[i].fd;
                fds[nfds].events = srv->clients[i].state == CONN_STATE_READING_REQ ? POLLIN : POLLOUT;
                fds[nfds].revents = 0;
                client_idx_map[nfds] = i;
                nfds++;
            }
        }

        int ready = poll(fds, (nfds_t)nfds, 1000);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        if (fds[0].revents & POLLIN)
        {
            while (1)
            {
                int client_fd = accept(srv->listen_fd, NULL, NULL);
                if (client_fd < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        break;
                    }
                    break;
                }

                if (set_nonblocking(client_fd) < 0)
                {
                    close(client_fd);
                    continue;
                }

                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++)
                {
                    if (srv->clients[i].fd == -1)
                    {
                        slot = i;
                        break;
                    }
                }

                if (slot != -1)
                {
                    memset(&srv->clients[slot], 0, sizeof(client_conn_t));
                    srv->clients[slot].fd = client_fd;
                    srv->clients[slot].state = CONN_STATE_READING_REQ;
                }
                else
                {
                    close(client_fd);
                }
            }
        }

        for (int j = 1; j < nfds; j++)
        {
            int ci = client_idx_map[j];
            client_conn_t *c = &srv->clients[ci];

            if (c->fd == -1)
            {
                continue;
            }

            if (fds[j].revents & (POLLERR | POLLNVAL))
            {
                close(c->fd);
                c->fd = -1;
                continue;
            }

            if (fds[j].revents & POLLIN && c->state == CONN_STATE_READING_REQ)
            {
                // Leave one byte for the null terminator used by the parser.
                if (c->rx_len >= sizeof(c->rx_buf) - 1)
                {
                    close(c->fd);
                    c->fd = -1;
                    continue;
                }

                ssize_t n = recv(c->fd, c->rx_buf + c->rx_len, sizeof(c->rx_buf) - c->rx_len - 1, 0);
                if (n > 0)
                {
                    c->rx_len += (size_t)n;
                    int status = parse_http_request(c);
                    if (status == 1)
                    {
                        prepare_response(c, srv->doc_root);
                    }
                    else if (status < 0)
                    {
                        close(c->fd);
                        c->fd = -1;
                    }
                }
                else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
                {
                    close(c->fd);
                    c->fd = -1;
                }
            }
            else if (fds[j].revents & POLLOUT && c->state == CONN_STATE_SENDING_RESP)
            {
                ssize_t n = send(c->fd, c->tx_buf + c->tx_sent, c->tx_len - c->tx_sent, 0);
                if (n > 0)
                {
                    c->tx_sent += (size_t)n;
                    if (c->tx_sent >= c->tx_len)
                    {
                        if (c->request.keep_alive)
                        {
                            c->state = CONN_STATE_READING_REQ;
                            c->rx_len = 0;
                            c->tx_len = 0;
                            c->tx_sent = 0;
                            memset(&c->request, 0, sizeof(c->request));
                        }
                        else
                        {
                            close(c->fd);
                            c->fd = -1;
                        }
                    }
                }
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    close(c->fd);
                    c->fd = -1;
                }
            }
            else if (fds[j].revents & POLLHUP)
            {
                close(c->fd);
                c->fd = -1;
            }
        }
    }
}

void server_close(server_ctx_t *srv)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (srv->clients[i].fd != -1)
        {
            close(srv->clients[i].fd);
            srv->clients[i].fd = -1;
        }
    }

    if (srv->listen_fd != -1)
    {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
}
