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
#ifndef UT_TRACE_LIMIT
#define UT_TRACE_LIMIT 131072
#endif
#if UT_TRACE_LIMIT < 1
#error UT_TRACE_LIMIT must be positive
#endif
#define INITIAL_EVENTS 256
enum state { READY, RUNNING, WAIT_IO, DONE };
enum event_kind { EV_READY, EV_RUNNING, EV_WAIT, EV_DONE, EV_MARK };
enum event_reason { RE_SPAWN, RE_YIELD, RE_DISPATCH, RE_IO, RE_TIMEOUT,
                    RE_RETURN, RE_EAGAIN, RE_CONTINUE };
static const char *const kind_names[] = {"ready", "running", "wait_io", "done", "mark"};
static const char *const reason_names[] = {"spawn", "yield", "dispatch", "io_event", "timeout",
                                          "function_return", "read_eagain", "same_task"};
struct task {
    ucontext_t ctx;
    void *stack;
    ut_fn fn;
    void *arg;
    enum state state;
    int fd, wait_error, saved_errno;
    int64_t deadline;
};
/* Built-in labels use enum values; only user marks need string copying. */
struct event { int64_t us; int tid; uint8_t kind, reason; char label[48]; };
static struct {
    ucontext_t scheduler;
    struct task tasks[MAX_TASKS];
    struct event *events;
    size_t event_count, event_capacity, dropped;
    int count, current, cursor, active, initialized, live_count, waiting_count;
    int trace_error;
    enum ut_trace_mode trace_mode;
    unsigned long long dispatches, poll_calls, yield_shortcuts;
    int64_t start;
    enum ut_policy policy;
} rt;

static int64_t now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void record(int tid, enum event_kind kind, enum event_reason reason, const char *label) {
    if (rt.trace_mode == UT_TRACE_OFF) return;
    int saved_errno = errno;
    if (rt.trace_error) { rt.dropped++; return; }
    if (rt.event_count == rt.event_capacity) {
        if (rt.event_capacity == UT_TRACE_LIMIT) {
            rt.trace_error = ENOSPC; rt.dropped++; return;
        }
        size_t capacity = rt.event_capacity * 2;
        if (capacity > UT_TRACE_LIMIT) capacity = UT_TRACE_LIMIT;
        struct event *events = realloc(rt.events, capacity * sizeof *events);
        if (!events) {
            rt.trace_error = ENOMEM; rt.dropped++; errno = saved_errno; return;
        }
        rt.events = events; rt.event_capacity = capacity;
    }
    struct event *e = &rt.events[rt.event_count++];
    e->us = now_us() - rt.start;
    e->tid = tid;
    e->kind = (uint8_t)kind; e->reason = (uint8_t)reason;
    if (kind == EV_MARK) {
        size_t length = strnlen(label, sizeof e->label - 1);
        memcpy(e->label, label, length); e->label[length] = '\0';
    }
    errno = saved_errno;
}
static void entry(int id) {
    struct task *t = &rt.tasks[id];
    t->fn(t->arg);
    t->state = DONE;
    rt.live_count--;
    record(id, EV_DONE, RE_RETURN, NULL);
    /* Returning activates uc_link. The scheduler frees the inactive stack. */
}
int ut_init(enum ut_policy policy) {
    return ut_init_ex(policy, UT_TRACE_FULL);
}
int ut_init_ex(enum ut_policy policy, enum ut_trace_mode trace_mode) {
    if (rt.initialized) { errno = EBUSY; return -1; }
    if ((policy != UT_ROUND_ROBIN && policy != UT_FIRST_READY) ||
        (trace_mode != UT_TRACE_FULL && trace_mode != UT_TRACE_OFF)) {
        errno = EINVAL; return -1;
    }
    memset(&rt, 0, sizeof rt);
    rt.trace_mode = trace_mode;
    if (trace_mode == UT_TRACE_FULL) {
        rt.event_capacity = INITIAL_EVENTS < UT_TRACE_LIMIT ? INITIAL_EVENTS : UT_TRACE_LIMIT;
        rt.events = malloc(rt.event_capacity * sizeof *rt.events);
        if (!rt.events) return -1;
    }
    rt.current = -1;
    rt.cursor = -1;
    rt.policy = policy;
    rt.start = now_us();
    rt.initialized = 1;
    return 0;
}
int ut_spawn(ut_fn fn, void *arg) {
    if (!rt.initialized || rt.active || !fn) { errno = EINVAL; return -1; }
    int id = rt.count;
    for (int i = 0; i < rt.count; i++) {
        if (rt.tasks[i].state == DONE) { id = i; break; }
    }
    if (id == MAX_TASKS) { errno = EAGAIN; return -1; }
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
    t->fn = fn; t->arg = arg; t->state = READY; t->saved_errno = 0;
    t->fd = -1; t->wait_error = 0; t->deadline = -1;
    makecontext(&t->ctx, (void (*)(void))entry, 1, id);
    if (id == rt.count) rt.count++;
    rt.live_count++;
    record(id, EV_READY, RE_SPAWN, NULL);
    return id;
}
void ut_yield(void) {
    if (!rt.active || rt.current < 0) { errno = EINVAL; return; }
    struct task *t = &rt.tasks[rt.current];
    int saved_errno = errno;
    t->state = READY;
    record(rt.current, EV_READY, RE_YIELD, NULL);
    /* No other live task exists, so a scheduler round cannot change the task. */
    if (rt.live_count == 1) {
        rt.yield_shortcuts++;
        t->state = RUNNING;
        record(rt.current, EV_RUNNING, RE_CONTINUE, NULL);
        errno = saved_errno;
        return;
    }
    if (swapcontext(&t->ctx, &rt.scheduler) != 0) abort();
    errno = saved_errno;
}
void ut_mark(const char *label) {
    if (rt.initialized) record(rt.current, EV_MARK, RE_SPAWN, label ? label : "");
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
    if (!rt.waiting_count) return 0;
    struct pollfd fds[MAX_TASKS];
    int ids[MAX_TASKS], n = 0;
    int64_t earliest = -1, now = 0;
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
            int64_t remaining_ms = (earliest - now_us() + 999) / 1000;
            timeout = remaining_ms <= 0 ? 0 : remaining_ms > INT32_MAX ? INT32_MAX : (int)remaining_ms;
        }
    }
    rt.poll_calls++;
    int result = poll(fds, (nfds_t)n, timeout);
    if (result < 0) return errno == EINTR ? 0 : -1;
    if (earliest >= 0) now = now_us();
    for (int j = 0; j < n; j++) {
        struct task *t = &rt.tasks[ids[j]];
        if (fds[j].revents) {
            t->wait_error = (fds[j].revents & POLLNVAL) ? EBADF : 0;
            t->state = READY;
            rt.waiting_count--;
            record(ids[j], EV_READY, RE_IO, NULL);
        } else if (t->deadline >= 0 && now >= t->deadline) {
            t->wait_error = ETIMEDOUT;
            t->state = READY;
            rt.waiting_count--;
            record(ids[j], EV_READY, RE_TIMEOUT, NULL);
        }
    }
    return 0;
}
int ut_run(void) {
    if (!rt.initialized || rt.active) { errno = EINVAL; return -1; }
    rt.active = 1;
    while (rt.live_count) {
        int id = choose();
        if (rt.waiting_count) {
            if (poll_waiters(id < 0) != 0) { rt.active = 0; return -1; }
            id = choose();
        }
        if (id < 0) {
            if (rt.waiting_count) continue; /* e.g. poll interrupted by a signal */
            rt.active = 0; errno = EDEADLK; return -1;
        }
        rt.cursor = id; rt.current = id;
        rt.tasks[id].state = RUNNING;
        rt.dispatches++;
        record(id, EV_RUNNING, RE_DISPATCH, NULL);
        errno = rt.tasks[id].saved_errno;
        if (swapcontext(&rt.scheduler, &rt.tasks[id].ctx) != 0) abort();
        rt.tasks[id].saved_errno = errno;
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
        rt.waiting_count++;
        record(rt.current, EV_WAIT, RE_EAGAIN, NULL);
        if (swapcontext(&t->ctx, &rt.scheduler) != 0) abort();
        if (t->wait_error) { errno = t->wait_error; return -1; }
    }
}
int ut_trace_write(const char *path) {
    if (!rt.initialized || rt.active || !path) { errno = EINVAL; return -1; }
    if (rt.trace_mode == UT_TRACE_OFF) { errno = ENOTSUP; return -1; }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "time_us,tid,event,detail\n");
    for (size_t i = 0; i < rt.event_count; i++) {
        struct event *e = &rt.events[i];
        /* Quotes escaped according to CSV rules; labels cannot inject columns. */
        fprintf(f, "%lld,%d,%s,\"", (long long)e->us, e->tid, kind_names[e->kind]);
        const char *detail = e->kind == EV_MARK ? e->label : reason_names[e->reason];
        for (const char *p = detail; *p; p++) {
            if (*p == '"') fputc('"', f);
            fputc(*p, f);
        }
        fprintf(f, "\"\n");
    }
    if (rt.dropped) fprintf(f, "%lld,-1,dropped,%zu\n", (long long)(now_us()-rt.start), rt.dropped);
    int failed = ferror(f);
    if (fclose(f) != 0) return -1;
    if (failed) { errno = EIO; return -1; }
    if (rt.dropped) { errno = rt.trace_error; return -1; }
    return 0;
}
int ut_get_stats(struct ut_stats *stats) {
    if (!rt.initialized || !stats) { errno = EINVAL; return -1; }
    *stats = (struct ut_stats){rt.dispatches, rt.poll_calls, rt.yield_shortcuts,
                             rt.event_count, rt.dropped, rt.event_capacity * sizeof *rt.events};
    return 0;
}
void ut_destroy(void) {
    if (rt.active) return;
    for (int i = 0; i < rt.count; i++) free(rt.tasks[i].stack);
    free(rt.events);
    memset(&rt, 0, sizeof rt);
}
