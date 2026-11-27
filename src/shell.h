/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 *
 * Administrative shell on standard input, multiplexed in the main poll(2)
 * array. Commands:
 *
 *   status        uptime, active clients, requests served, bytes sent
 *   cache stats   hit/miss ratio and frame utilization
 *   cache flush   invalidate every cached page
 *   exec <cmd>    run <cmd> with /bin/sh -c in a child process, asynchronously
 *   shutdown      stop accepting, drain responses, release everything, exit
 *   help          list commands
 *
 * stdin is the trusted local operator console; nothing received over HTTP
 * ever reaches exec.
 */

#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SHELL_LINE_MAX 512
#define MAX_CHILDREN 8
#define CHILD_TERM_GRACE_MS 2000

typedef struct
{
    pid_t pid;
    int64_t started_ms;
    char command[SHELL_LINE_MAX];
} child_proc_t;

typedef struct
{
    int input_fd;
    bool input_open;
    char line[SHELL_LINE_MAX];
    size_t line_len;
    bool discarding;

    child_proc_t children[MAX_CHILDREN];
    size_t child_count;
} admin_shell_t;

struct server_ctx;

void shell_init(admin_shell_t *sh, int input_fd);

/* Prints to stdout and flushes, so output appears promptly even through a pipe. */
void shell_printf(const char *fmt, ...);

/* Called when stdin is readable: reads once, runs every complete line. */
void shell_handle_input(struct server_ctx *srv);

/* Reaps every exited child without blocking. */
void shell_reap_children(struct server_ctx *srv);

/* Shutdown policy: SIGTERM each child's process group, then SIGKILL after a grace period. */
void shell_terminate_children(struct server_ctx *srv);

#endif
