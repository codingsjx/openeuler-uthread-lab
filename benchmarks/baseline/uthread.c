#define _XOPEN_SOURCE 700
#include "uthread.h"
#include <ucontext.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_TASKS 32
#define STACK_BYTES (128 * 1024)
#define MAX_EVENTS 131072
enum state { READY, RUNNING, WAIT_IO, DONE };
struct task {
    ucontext_t ctx;
    void *stack;
    ut_fn fn;
    void *arg;
    enum state state;
    int fd, wait_error;
    int64_t deadline;
};
struct event { int64_t us; int tid; char kind[20]; char detail[48]; };
static struct {
    ucontext_t scheduler;
    struct task tasks[MAX_TASKS];
    struct event *events;
    size_t event_count, dropped;
    int count, current, cursor, active, initialized;
    int64_t start;
    enum ut_policy policy;
} rt;

static int64_t now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void record(int tid, const char *kind, const char *detail) {
    if (rt.event_count == MAX_EVENTS) { rt.dropped++; return; }
    struct event *e = &rt.events[rt.event_count++];
    e->us = now_us() - rt.start;
    e->tid = tid;
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    snprintf(e->detail, sizeof e->detail, "%s", detail);
}
static void entry(int id) {
    struct task *t = &rt.tasks[id];
    t->fn(t->arg);
    t->state = DONE;
    record(id, "done", "function_return");
    /* Returning activates uc_link. The scheduler frees the inactive stack. */
}
int ut_init(enum ut_policy policy) {
    if (rt.initialized) { errno = EBUSY; return -1; }
    if (policy != UT_ROUND_ROBIN && policy != UT_FIRST_READY) {
        errno = EINVAL; return -1;
    }
    memset(&rt, 0, sizeof rt);
    rt.events = calloc(MAX_EVENTS, sizeof *rt.events);
    if (!rt.events) return -1;
    rt.current = -1;
    rt.cursor = -1;
    rt.policy = policy;
    rt.start = now_us();
    rt.initialized = 1;
    return 0;
}
int ut_spawn(ut_fn fn, void *arg) {
    if (!rt.initialized || rt.active || !fn) { errno = EINVAL; return -1; }
    if (rt.count == MAX_TASKS) { errno = EAGAIN; return -1; }
    int id = rt.count;
    struct task *t = &rt.tasks[id];
    t->stack = malloc(STACK_BYTES);
    if (!t->stack) return -1;
    if (getcontext(&t->ctx) != 0) {
        int saved = errno; free(t->stack); t->stack = NULL; errno = saved; return -1;
    }
    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = STACK_BYTES;
    t->ctx.uc_stack.ss_flags = 0;
    t->ctx.uc_link = &rt.scheduler;
    t->fn = fn; t->arg = arg; t->state = READY;
    makecontext(&t->ctx, (void (*)(void))entry, 1, id);
    rt.count++;
    record(id, "ready", "spawn");
    return id;
}
void ut_yield(void) {
    if (!rt.active || rt.current < 0) { errno = EINVAL; return; }
    struct task *t = &rt.tasks[rt.current];
    t->state = READY;
    record(rt.current, "ready", "yield");
    if (swapcontext(&t->ctx, &rt.scheduler) != 0) abort();
}
void ut_mark(const char *label) {
    if (rt.initialized) record(rt.current, "mark", label ? label : "");
}
static int choose(void) {
    for (int n = 0; n < rt.count; n++) {
        int id = rt.policy == UT_FIRST_READY ? n : (rt.cursor + 1 + n) % rt.count;
        if (rt.tasks[id].state == READY) return id;
    }
    return -1;
}
/* Poll at every scheduling boundary, including when other tasks are runnable. */
static int poll_waiters(int may_block) {
    struct pollfd fds[MAX_TASKS];
    int ids[MAX_TASKS], n = 0;
    int64_t earliest = -1, now = now_us();
    for (int i = 0; i < rt.count; i++) {
        struct task *t = &rt.tasks[i];
        if (t->state != WAIT_IO) continue;
        ids[n] = i;
        fds[n++] = (struct pollfd){ .fd = t->fd, .events = POLLIN };
        if (t->deadline >= 0 && (earliest < 0 || t->deadline < earliest)) earliest = t->deadline;
    }
    if (!n) return 0;
    int timeout = 0;
    if (may_block) {
        if (earliest < 0) timeout = -1;
        else {
            int64_t remaining_ms = (earliest - now + 999) / 1000;
            timeout = remaining_ms <= 0 ? 0 : remaining_ms > INT32_MAX ? INT32_MAX : (int)remaining_ms;
        }
    }
    int result = poll(fds, (nfds_t)n, timeout);
    if (result < 0) return errno == EINTR ? 0 : -1;
    now = now_us();
    for (int j = 0; j < n; j++) {
        struct task *t = &rt.tasks[ids[j]];
        if (fds[j].revents) {
            t->wait_error = (fds[j].revents & POLLNVAL) ? EBADF : 0;
            t->state = READY;
            record(ids[j], "ready", "io_event");
        } else if (t->deadline >= 0 && now >= t->deadline) {
            t->wait_error = ETIMEDOUT;
            t->state = READY;
            record(ids[j], "ready", "timeout");
        }
    }
    return 0;
}
int ut_run(void) {
    if (!rt.initialized || rt.active) { errno = EINVAL; return -1; }
    rt.active = 1;
    for (;;) {
        int alive = 0;
        for (int i = 0; i < rt.count; i++) alive += rt.tasks[i].state != DONE;
        if (!alive) break;
        if (poll_waiters(choose() < 0) != 0) { rt.active = 0; return -1; }
        int id = choose();
        if (id < 0) continue;
        rt.cursor = id; rt.current = id;
        rt.tasks[id].state = RUNNING;
        record(id, "running", "dispatch");
        if (swapcontext(&rt.scheduler, &rt.tasks[id].ctx) != 0) abort();
        rt.current = -1;
        if (rt.tasks[id].state == DONE) {
            free(rt.tasks[id].stack);
            rt.tasks[id].stack = NULL;
        }
    }
    rt.active = 0;
    return 0;
}
ssize_t ut_read(int fd, void *buf, size_t size, int timeout_ms) {
    if (!rt.active || rt.current < 0 || timeout_ms < -1) { errno = EINVAL; return -1; }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    if (!(flags & O_NONBLOCK)) { errno = EINVAL; return -1; }
    struct task *t = &rt.tasks[rt.current];
    int64_t deadline = timeout_ms < 0 ? -1 : now_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        ssize_t n = read(fd, buf, size);
        if (n >= 0) return n; /* includes EOF */
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (deadline >= 0 && now_us() >= deadline) { errno = ETIMEDOUT; return -1; }
        t->fd = fd; t->deadline = deadline; t->wait_error = 0;
        t->state = WAIT_IO;
        record(rt.current, "wait_io", "read_eagain");
        if (swapcontext(&t->ctx, &rt.scheduler) != 0) abort();
        if (t->wait_error) { errno = t->wait_error; return -1; }
    }
}
int ut_trace_write(const char *path) {
    if (!rt.initialized || rt.active || !path) { errno = EINVAL; return -1; }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "time_us,tid,event,detail\n");
    for (size_t i = 0; i < rt.event_count; i++) {
        struct event *e = &rt.events[i];
        /* Quotes escaped according to CSV rules; labels cannot inject columns. */
        fprintf(f, "%lld,%d,%s,\"", (long long)e->us, e->tid, e->kind);
        for (const char *p = e->detail; *p; p++) {
            if (*p == '"') fputc('"', f);
            fputc(*p, f);
        }
        fprintf(f, "\"\n");
    }
    if (rt.dropped) fprintf(f, "%lld,-1,dropped,%zu\n", (long long)(now_us()-rt.start), rt.dropped);
    int failed = ferror(f);
    if (fclose(f) != 0) return -1;
    if (failed) { errno = EIO; return -1; }
    if (rt.dropped) { errno = ENOSPC; return -1; }
    return 0;
}
void ut_destroy(void) {
    if (rt.active) return;
    for (int i = 0; i < rt.count; i++) free(rt.tasks[i].stack);
    free(rt.events);
    memset(&rt, 0, sizeof rt);
}
