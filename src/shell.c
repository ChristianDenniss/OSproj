/*
 * CS 3404 Capstone: webshell
 * Student: Christian Dennis
 * Student ID: 3729509
 */

#define _POSIX_C_SOURCE 200809L
#include "shell.h"
#include "http_server.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

void shell_init(admin_shell_t *sh, int input_fd)
{
    memset(sh, 0, sizeof(*sh));
    sh->input_fd = input_fd;
    sh->input_open = true;
}

void shell_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fflush(stdout);
}

static void print_help(void)
{
    shell_printf("[admin] commands: status | cache stats | cache flush | exec <cmd> | shutdown | help\n");
}

static void print_status(server_ctx_t *srv)
{
    int64_t uptime_ms = monotonic_ms() - srv->start_ms;
    long long seconds = (long long)(uptime_ms / 1000);
    shell_printf("[status] uptime=%lld.%03llds (%02lld:%02lld:%02lld) active_clients=%zu requests_served=%llu "
                 "bytes_sent=%llu\n",
                 seconds, (long long)(uptime_ms % 1000), seconds / 3600, (seconds / 60) % 60, seconds % 60,
                 srv->active_clients, (unsigned long long)srv->requests_served,
                 (unsigned long long)srv->bytes_sent);
    shell_printf("[status] state=%s io_jobs_in_use=%zu/%zu log_pending=%zu log_written=%llu children=%zu "
                 "send_would_block=%llu\n",
                 srv->draining ? "draining" : "running", io_jobs_in_use(&srv->worker), srv->worker.job_limit,
                 srv->log.count, (unsigned long long)srv->log.written, srv->shell.child_count,
                 (unsigned long long)srv->send_would_block);
}

static void print_cache_stats(server_ctx_t *srv)
{
    const page_cache_t *cache = &srv->cache;
    uint64_t lookups = cache->hits + cache->misses;
    char ratio[32] = "n/a";
    if (lookups > 0)
    {
        snprintf(ratio, sizeof(ratio), "%.1f%%", 100.0 * (double)cache->hits / (double)lookups);
    }
    shell_printf("[cache] hits=%llu misses=%llu hit_ratio=%s frames_used=%zu/%zu (%.1f%%) evictions=%llu "
                 "epoch=%u frame_size=%d\n",
                 (unsigned long long)cache->hits, (unsigned long long)cache->misses, ratio, cache->used,
                 cache->capacity, 100.0 * (double)cache->used / (double)cache->capacity,
                 (unsigned long long)cache->evictions, cache->epoch, WS_PAGE_SIZE);
}

static void flush_cache(server_ctx_t *srv)
{
    size_t used = srv->cache.used;
    cache_flush(&srv->cache);
    shell_printf("[cache] flushed %zu frame(s); epoch is now %u\n", used, srv->cache.epoch);
}

static void spawn_child(server_ctx_t *srv, const char *command)
{
    admin_shell_t *sh = &srv->shell;
    if (sh->child_count >= MAX_CHILDREN)
    {
        shell_printf("[exec] refused: %d diagnostic processes already running\n", MAX_CHILDREN);
        return;
    }

    child_proc_t *slot = NULL;
    for (size_t i = 0; i < MAX_CHILDREN; i++)
    {
        if (sh->children[i].pid == 0)
        {
            slot = &sh->children[i];
            break;
        }
    }
    snprintf(slot->command, sizeof(slot->command), "%s", command);

    /*
     * Everything the child needs is prepared before fork(). This process has
     * a worker thread, so between fork() and exec the child may only make
     * async-signal-safe calls: no malloc, no stdio, no mutexes.
     */
    static char shell_name[] = "sh";
    static char command_flag[] = "-c";
    char *argv[] = {shell_name, command_flag, slot->command, NULL};

    struct sigaction default_action;
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigset_t empty_mask;
    sigemptyset(&empty_mask);

    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull < 0)
    {
        shell_printf("[exec] cannot open /dev/null: %s\n", strerror(errno));
        return;
    }

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
    {
        shell_printf("[exec] fork failed: %s\n", strerror(errno));
        close(devnull);
        return;
    }

    if (pid == 0)
    {
        /* Own process group, so shutdown can signal the command and its descendants. */
        setpgid(0, 0);
        /* SIG_IGN survives exec; restore SIGPIPE so pipelines behave normally. */
        sigaction(SIGPIPE, &default_action, NULL);
        sigprocmask(SIG_SETMASK, &empty_mask, NULL);
        /* The child must never read the admin console. */
        dup2(devnull, STDIN_FILENO);
        execve("/bin/sh", argv, environ);
        _exit(127);
    }

    /* Set the group from the parent too, so it is in place whichever side runs first. */
    setpgid(pid, pid);
    close(devnull);

    slot->pid = pid;
    slot->started_ms = monotonic_ms();
    sh->child_count++;
    shell_printf("[exec] started pid %ld: %s\n", (long)pid, slot->command);
}

static void run_command(server_ctx_t *srv, char *line)
{
    char *start = line;
    while (*start == ' ' || *start == '\t')
    {
        start++;
    }
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
    {
        *--end = '\0';
    }
    if (*start == '\0')
    {
        return;
    }

    char *word_end = start;
    while (*word_end != '\0' && *word_end != ' ' && *word_end != '\t')
    {
        word_end++;
    }
    size_t word_len = (size_t)(word_end - start);
    char *rest = word_end;
    while (*rest == ' ' || *rest == '\t')
    {
        rest++;
    }

    if (word_len == 6 && strncmp(start, "status", 6) == 0 && *rest == '\0')
    {
        print_status(srv);
    }
    else if (word_len == 5 && strncmp(start, "cache", 5) == 0 && strcmp(rest, "stats") == 0)
    {
        print_cache_stats(srv);
    }
    else if (word_len == 5 && strncmp(start, "cache", 5) == 0 && strcmp(rest, "flush") == 0)
    {
        flush_cache(srv);
    }
    else if (word_len == 4 && strncmp(start, "exec", 4) == 0)
    {
        if (*rest == '\0')
        {
            shell_printf("[exec] usage: exec <command>\n");
        }
        else if (srv->draining)
        {
            shell_printf("[exec] refused: server is shutting down\n");
        }
        else
        {
            spawn_child(srv, rest);
        }
    }
    else if (word_len == 8 && strncmp(start, "shutdown", 8) == 0 && *rest == '\0')
    {
        server_begin_shutdown(srv, "shutdown command");
    }
    else if (word_len == 4 && strncmp(start, "help", 4) == 0 && *rest == '\0')
    {
        print_help();
    }
    else
    {
        shell_printf("[admin] unknown command: %s (type 'help')\n", start);
    }
}

void shell_handle_input(server_ctx_t *srv)
{
    admin_shell_t *sh = &srv->shell;
    char buf[1024];

    /* poll() reported readable, so this single read returns without waiting. */
    ssize_t n = read(sh->input_fd, buf, sizeof(buf));
    if (n == 0)
    {
        sh->input_open = false;
        shell_printf("[admin] stdin closed; still serving (stop with SIGINT or SIGTERM)\n");
        return;
    }
    if (n < 0)
    {
        if (errno != EINTR && errno != EAGAIN)
        {
            sh->input_open = false;
            shell_printf("[admin] stdin read failed: %s\n", strerror(errno));
        }
        return;
    }

    for (ssize_t i = 0; i < n; i++)
    {
        char ch = buf[i];
        if (ch == '\n')
        {
            if (sh->discarding)
            {
                shell_printf("[admin] line too long (limit %d bytes); ignored\n", SHELL_LINE_MAX - 1);
                sh->discarding = false;
            }
            else
            {
                sh->line[sh->line_len] = '\0';
                run_command(srv, sh->line);
            }
            sh->line_len = 0;
        }
        else if (!sh->discarding)
        {
            if (sh->line_len < SHELL_LINE_MAX - 1)
            {
                sh->line[sh->line_len++] = ch;
            }
            else
            {
                sh->discarding = true;
            }
        }
    }
}

static void report_exit(child_proc_t *child, int status)
{
    double seconds = (double)(monotonic_ms() - child->started_ms) / 1000.0;
    if (WIFEXITED(status))
    {
        shell_printf("[exec] pid %ld exited with status %d after %.1fs: %s\n", (long)child->pid,
                     WEXITSTATUS(status), seconds, child->command);
    }
    else if (WIFSIGNALED(status))
    {
        shell_printf("[exec] pid %ld terminated by signal %d after %.1fs: %s\n", (long)child->pid,
                     WTERMSIG(status), seconds, child->command);
    }
}

static void forget_child(admin_shell_t *sh, pid_t pid, int status)
{
    for (size_t i = 0; i < MAX_CHILDREN; i++)
    {
        if (sh->children[i].pid == pid)
        {
            report_exit(&sh->children[i], status);
            sh->children[i].pid = 0;
            sh->child_count--;
            return;
        }
    }
}

void shell_reap_children(server_ctx_t *srv)
{
    admin_shell_t *sh = &srv->shell;
    while (sh->child_count > 0)
    {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0)
        {
            forget_child(sh, pid, status);
        }
        else if (pid < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            return;
        }
    }
}

static void signal_children(admin_shell_t *sh, int signo)
{
    for (size_t i = 0; i < MAX_CHILDREN; i++)
    {
        pid_t pid = sh->children[i].pid;
        if (pid != 0 && kill(-pid, signo) < 0)
        {
            kill(pid, signo);
        }
    }
}

void shell_terminate_children(server_ctx_t *srv)
{
    admin_shell_t *sh = &srv->shell;
    shell_reap_children(srv);
    if (sh->child_count == 0)
    {
        return;
    }

    shell_printf("[shutdown] terminating %zu diagnostic process(es)\n", sh->child_count);
    signal_children(sh, SIGTERM);

    int64_t deadline = monotonic_ms() + CHILD_TERM_GRACE_MS;
    struct timespec pause = {0, 20 * 1000 * 1000};
    while (sh->child_count > 0 && monotonic_ms() < deadline)
    {
        nanosleep(&pause, NULL);
        shell_reap_children(srv);
    }

    if (sh->child_count > 0)
    {
        signal_children(sh, SIGKILL);
        for (size_t i = 0; i < MAX_CHILDREN; i++)
        {
            pid_t pid = sh->children[i].pid;
            if (pid == 0)
            {
                continue;
            }
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }
            forget_child(sh, pid, status);
        }
    }
}
