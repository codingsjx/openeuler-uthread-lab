#ifndef UTHREAD_LAB_H
#define UTHREAD_LAB_H
#include <stddef.h>
#include <sys/types.h>
typedef void (*ut_fn)(void *);
enum ut_policy { UT_ROUND_ROBIN, UT_FIRST_READY };
/* One runtime and one kernel thread. Calls must originate on its owner thread. */
int ut_init(enum ut_policy policy);
int ut_spawn(ut_fn fn, void *arg);
int ut_run(void);
void ut_yield(void);
/* fd must already be O_NONBLOCK; timeout_ms >= 0, or -1 for no deadline. */
ssize_t ut_read(int fd, void *buf, size_t size, int timeout_ms);
void ut_mark(const char *label);
int ut_trace_write(const char *path);
void ut_destroy(void);
#endif
