#ifndef UTHREAD_LAB_H
#define UTHREAD_LAB_H
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
typedef void (*ut_fn)(void *);
struct ut_task_handle { int id; unsigned long long generation; };
enum ut_task_state { UT_TASK_READY, UT_TASK_RUNNING, UT_TASK_WAIT_IO, UT_TASK_DONE };
struct ut_task_info { enum ut_task_state state; int fd; short events; int cancelled; };
enum ut_policy { UT_ROUND_ROBIN, UT_FIRST_READY };
enum ut_trace_mode { UT_TRACE_FULL, UT_TRACE_OFF, UT_TRACE_RECENT };
struct ut_stats {
    unsigned long long dispatches, poll_calls, yield_shortcuts;
    size_t recorded_events, dropped_events, trace_allocated_bytes;
};
/* One runtime and one kernel thread. Calls must originate on its owner thread. */
int ut_init(enum ut_policy policy);
/* Full tracing remains the default. OFF allocates no trace buffer.
 * RECENT keeps at most 256 events (also bounded by UT_TRACE_LIMIT); its CSV
 * starts with an overwritten marker if earlier events were replaced. */
int ut_init_ex(enum ut_policy policy, enum ut_trace_mode trace_mode);
/* May also be called by an executing task. At most 32 live tasks total;
 * completed task slots are reusable. Not safe in a signal handler. */
int ut_spawn(ut_fn fn, void *arg);
/* Optional generation-checked handle prevents cancellation of a reused slot. */
int ut_spawn_task(ut_fn fn, void *arg, struct ut_task_handle *handle);
/* Cooperative request: pending/future runtime I/O returns ECANCELED.
 * The task must return normally and release its resources. Not signal-safe. */
int ut_cancel(struct ut_task_handle handle);
int ut_cancelled(void);
int ut_get_task_info(struct ut_task_handle handle, struct ut_task_info *info);
int ut_run(void);
void ut_yield(void);
/* fd must already be O_NONBLOCK; timeout_ms >= 0, or -1 for no deadline. */
ssize_t ut_read(int fd, void *buf, size_t size, int timeout_ms);
/* POSIX partial-write semantics: returns bytes written, not necessarily size.
 * Caller owns SIGPIPE policy (e.g. SIG_IGN in a standalone socket service).
 * One deadline per call; repeated partial writes need a caller-level deadline. */
ssize_t ut_write(int fd, const void *buf, size_t size, int timeout_ms);
/* Listening fd must be O_NONBLOCK. Returned fd is O_NONBLOCK and FD_CLOEXEC. */
int ut_accept(int fd, struct sockaddr *address, socklen_t *length, int timeout_ms);
void ut_mark(const char *label);
int ut_trace_write(const char *path);
int ut_get_stats(struct ut_stats *stats);
void ut_destroy(void);
#endif
