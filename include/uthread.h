#ifndef UTHREAD_LAB_H
#define UTHREAD_LAB_H
#include <stddef.h>
#include <sys/types.h>
typedef void (*ut_fn)(void *);
enum ut_policy { UT_ROUND_ROBIN, UT_FIRST_READY };
enum ut_trace_mode { UT_TRACE_FULL, UT_TRACE_OFF };
struct ut_stats {
    unsigned long long dispatches, poll_calls, yield_shortcuts;
    size_t recorded_events, dropped_events, trace_allocated_bytes;
};
/* One runtime and one kernel thread. Calls must originate on its owner thread. */
int ut_init(enum ut_policy policy);
/* Full tracing remains the default. OFF allocates no trace buffer. */
int ut_init_ex(enum ut_policy policy, enum ut_trace_mode trace_mode);
int ut_spawn(ut_fn fn, void *arg);
int ut_run(void);
void ut_yield(void);
/* fd must already be O_NONBLOCK; timeout_ms >= 0, or -1 for no deadline. */
ssize_t ut_read(int fd, void *buf, size_t size, int timeout_ms);
void ut_mark(const char *label);
int ut_trace_write(const char *path);
int ut_get_stats(struct ut_stats *stats);
void ut_destroy(void);
#endif
