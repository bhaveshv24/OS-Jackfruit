/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define MAX_CONTAINERS 64

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[PATH_MAX];
    char stop_reason[64];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    int pipe_read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_arg_t;

static supervisor_ctx_t *g_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n", argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) { pthread_cond_destroy(&buffer->not_empty); pthread_mutex_destroy(&buffer->mutex); return rc; }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down && buffer->count == LOG_BUFFER_CAPACITY) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

void *logging_thread(void *arg)
{
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
    log_item_t item;

    while (1) {
        if (bounded_buffer_pop(buffer, &item) != 0)
            break;

        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t written = 0;
            size_t remaining = item.length;
            while (remaining > 0) {
                ssize_t n = write(fd, item.data + written, remaining);
                if (n <= 0) break;
                written += n;
                remaining -= n;
            }
            close(fd);
        }
    }

    /* drain remaining items after shutdown */
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count > 0) {
        item = buffer->items[buffer->head];
        buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
        buffer->count--;
        pthread_mutex_unlock(&buffer->mutex);

        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
        pthread_mutex_lock(&buffer->mutex);
    }
    pthread_mutex_unlock(&buffer->mutex);

    return NULL;
}

void *producer_thread(void *arg)
{
    producer_arg_t *parg = (producer_arg_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(parg->pipe_read_fd, buf, sizeof(buf))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, parg->container_id, CONTAINER_ID_LEN - 1);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        bounded_buffer_push(parg->buffer, &item);
    }

    close(parg->pipe_read_fd);
    free(parg);
    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* redirect stdout and stderr to log pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* set hostname to container id */
    sethostname(cfg->id, strlen(cfg->id));

    /* chroot into container rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    /* mount /proc */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* non-fatal, continue anyway */
    }

    /* apply nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* exec the command */
    char *argv_exec[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", argv_exec);

    /* if execv fails, try as direct binary */
    char *argv2[] = { cfg->command, NULL };
    execv(cfg->command, argv2);

    perror("execv");
    return 1;
}

int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid,
                          unsigned long soft_limit_bytes, unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    c->exit_signal = 0;
                    if (c->stop_requested) {
                        c->state = CONTAINER_STOPPED;
                        strncpy(c->stop_reason, "stopped", sizeof(c->stop_reason) - 1);
                    } else {
                        c->state = CONTAINER_EXITED;
                        strncpy(c->stop_reason, "exited", sizeof(c->stop_reason) - 1);
                    }
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    c->exit_code = 128 + c->exit_signal;
                    if (c->exit_signal == SIGKILL && !c->stop_requested) {
                        c->state = CONTAINER_KILLED;
                        strncpy(c->stop_reason, "hard_limit_killed", sizeof(c->stop_reason) - 1);
                    } else {
                        c->state = CONTAINER_STOPPED;
                        strncpy(c->stop_reason, "stopped", sizeof(c->stop_reason) - 1);
                    }
                }
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static void sigchld_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        reap_children(g_ctx);
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

static int do_start_container(supervisor_ctx_t *ctx, const control_request_t *req,
                               control_response_t *resp)
{
    /* check for duplicate id */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "Container '%s' already exists", req->container_id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* create log dir */
    mkdir(LOG_DIR, 0755);

    /* create pipe for container stdout/stderr */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "pipe: %s", strerror(errno));
        return -1;
    }

    /* set up child config */
    child_config_t *cfg = calloc(1, sizeof(child_config_t));
    if (!cfg) {
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "out of memory");
        return -1;
    }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* allocate stack for clone */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg); close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "out of memory");
        return -1;
    }

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);

    /* parent closes write end */
    close(pipefd[1]);
    free(stack);

    if (pid < 0) {
        free(cfg);
        close(pipefd[0]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "clone: %s", strerror(errno));
        return -1;
    }

    /* start producer thread to read from pipe */
    producer_arg_t *parg = calloc(1, sizeof(producer_arg_t));
    if (parg) {
        parg->pipe_read_fd = pipefd[0];
        strncpy(parg->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        parg->buffer = &ctx->log_buffer;
        pthread_t ptid;
        pthread_create(&ptid, NULL, producer_thread, parg);
        pthread_detach(ptid);
    } else {
        close(pipefd[0]);
    }

    /* register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);
    }

    /* add metadata record */
    container_record_t *rec = calloc(1, sizeof(container_record_t));
    if (!rec) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "out of memory");
        return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->exit_code = 0;
    rec->exit_signal = 0;
    rec->stop_requested = 0;
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    free(cfg);

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "Container '%s' started with PID %d", req->container_id, pid);
    return 0;
}

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    char buf[4096];
    int pos = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%-16s %-8s %-12s %-10s %-8s %-8s %s\n",
                    "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)", "EXIT", "REASON");

    while (c && pos < (int)sizeof(buf) - 128) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%-16s %-8d %-12s %-10lu %-8lu %-8d %s\n",
                        c->id,
                        c->host_pid,
                        state_to_string(c->state),
                        c->soft_limit_bytes >> 20,
                        c->hard_limit_bytes >> 20,
                        c->exit_code,
                        c->stop_reason[0] ? c->stop_reason : "-");
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    write(client_fd, buf, pos);
}

static void handle_logs(supervisor_ctx_t *ctx, const char *id, int client_fd)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, id);
    char log_path[PATH_MAX];
    if (c)
        strncpy(log_path, c->log_path, sizeof(log_path) - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!c) {
        char msg[] = "Container not found\n";
        write(client_fd, msg, strlen(msg));
        return;
    }

    int fd = open(log_path, O_RDONLY);
    if (fd < 0) {
        char msg[] = "No log file yet\n";
        write(client_fd, msg, strlen(msg));
        return;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(client_fd, buf, n);
    close(fd);
}

static void handle_stop(supervisor_ctx_t *ctx, const char *id, int client_fd)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, id);
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        char msg[] = "Container not found\n";
        write(client_fd, msg, strlen(msg));
        return;
    }
    if (c->state != CONTAINER_RUNNING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        char msg[] = "Container is not running\n";
        write(client_fd, msg, strlen(msg));
        return;
    }
    c->stop_requested = 1;
    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&ctx->metadata_lock);

    kill(pid, SIGTERM);
    usleep(300000);

    pthread_mutex_lock(&ctx->metadata_lock);
    c = find_container(ctx, id);
    if (c && c->state == CONTAINER_RUNNING)
        kill(pid, SIGKILL);
    pthread_mutex_unlock(&ctx->metadata_lock);

    char msg[64];
    snprintf(msg, sizeof(msg), "Stop signal sent to '%s'\n", id);
    write(client_fd, msg, strlen(msg));
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    (void)rootfs;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { perror("bounded_buffer_init"); pthread_mutex_destroy(&ctx.metadata_lock); return 1; }

    mkdir(LOG_DIR, 0755);

    /* open kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: cannot open /dev/container_monitor: %s\n", strerror(errno));

    /* create UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) { perror("listen"); return 1; }

    /* install signal handlers */
    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT, &sa_term, NULL);

    /* start logger consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx.log_buffer);
    if (rc != 0) { perror("pthread_create"); return 1; }

    fprintf(stderr, "[supervisor] Started. Listening on %s\n", CONTROL_PATH);

    /* event loop */
    fd_set readfds;
    struct timeval tv;
    while (!ctx.should_stop) {
        FD_ZERO(&readfds);
        FD_SET(ctx.server_fd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(ctx.server_fd + 1, &readfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) continue;

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        control_request_t req;
        memset(&req, 0, sizeof(req));
        ssize_t nr = read(client_fd, &req, sizeof(req));
        if (nr != sizeof(req)) { close(client_fd); continue; }

        control_response_t resp;
        memset(&resp, 0, sizeof(resp));

        switch (req.kind) {
        case CMD_START: {
            do_start_container(&ctx, &req, &resp);
            write(client_fd, &resp, sizeof(resp));
            break;
        }
        case CMD_RUN: {
            do_start_container(&ctx, &req, &resp);
            write(client_fd, &resp, sizeof(resp));
            if (resp.status == 0) {
                /* find pid and wait */
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *c = find_container(&ctx, req.container_id);
                pid_t wait_pid = c ? c->host_pid : -1;
                pthread_mutex_unlock(&ctx.metadata_lock);

                if (wait_pid > 0) {
                    int wstatus;
                    waitpid(wait_pid, &wstatus, 0);
                    reap_children(&ctx);

                    pthread_mutex_lock(&ctx.metadata_lock);
                    c = find_container(&ctx, req.container_id);
                    int exit_code = c ? c->exit_code : -1;
                    pthread_mutex_unlock(&ctx.metadata_lock);

                    /* send exit code as extra message */
                    char exit_msg[64];
                    snprintf(exit_msg, sizeof(exit_msg), "EXIT:%d\n", exit_code);
                    write(client_fd, exit_msg, strlen(exit_msg));
                }
            }
            break;
        }
        case CMD_PS:
            handle_ps(&ctx, client_fd);
            break;
        case CMD_LOGS:
            handle_logs(&ctx, req.container_id, client_fd);
            break;
        case CMD_STOP:
            handle_stop(&ctx, req.container_id, client_fd);
            break;
        default:
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "Unknown command");
            write(client_fd, &resp, sizeof(resp));
        }

        close(client_fd);
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    sleep(1);

    /* force kill remaining */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING)
            kill(c->host_pid, SIGKILL);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* reap all children */
    int wstatus;
    while (waitpid(-1, &wstatus, WNOHANG) > 0);

    /* shutdown logger */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* free metadata */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] Clean shutdown complete.\n");
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != sizeof(*req)) {
        perror("write"); close(fd); return 1;
    }

    /* read response */
    if (req->kind == CMD_START || req->kind == CMD_RUN) {
        control_response_t resp;
        ssize_t n = read(fd, &resp, sizeof(resp));
        if (n == sizeof(resp)) {
            printf("%s\n", resp.message);
            if (req->kind == CMD_RUN && resp.status == 0) {
                /* read exit code message */
                char buf[128];
                n = read(fd, buf, sizeof(buf) - 1);
                if (n > 0) { buf[n] = '\0'; printf("%s", buf); }
            }
        }
    } else {
        /* PS, LOGS, STOP — read text response */
        char buf[8192];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
    }

    close(fd);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]); return 1; }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0)   return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0)    return cmd_ps();
    if (strcmp(argv[1], "logs") == 0)  return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0)  return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
