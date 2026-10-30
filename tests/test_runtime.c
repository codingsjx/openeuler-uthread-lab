#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef NDEBUG
#error Runtime tests require assertions to be enabled
#endif

static int descriptors[2], observed;
static int received;
static volatile sig_atomic_t alarms;
static int recycled_tasks;
static void many_yields(void *arg) {
    (void)arg;
    ut_mark("comma,\"quote\"\nnewline");
    for (int i = 0; i < 1000; i++) ut_yield();
}
static void recycled_task(void *arg) {
    (void)arg;
    recycled_tasks++;
    ut_yield();
}
static void errno_task(void *arg) {
    int expected = *(int *)arg;
    errno = expected;
    for (int i = 0; i < 10; i++) {
        ut_mark("errno_preserved");
        ut_yield();
        assert(errno == expected);
    }
}
static void alarm_handler(int number) { (void)number; alarms++; }
static void competing_reader(void *arg) {
    (void)arg;
    char c;
    assert(ut_read(descriptors[0], &c, 1, 1000) == 1);
    assert(c == 'A' || c == 'B');
    received |= c == 'A' ? 1 : 2;
}
static void gradual_writer(void *arg) {
    (void)arg;
    assert(write(descriptors[1], "A", 1) == 1);
    ut_yield();
    assert(write(descriptors[1], "B", 1) == 1);
}
static void eof_reader(void *arg) {
    (void)arg;
    char c;
    assert(ut_read(descriptors[0], &c, 1, 100) == 0);
    observed++;
}
static void close_writer(void *arg) {
    (void)arg;
    assert(close(descriptors[1]) == 0);
}
static void deadline_reader(void *arg) {
    (void)arg;
    char c;
    assert(ut_read(descriptors[0], &c, 1, 20) == -1);
    assert(errno == ETIMEDOUT);
    observed++;
}
static void bad_fd(void *arg) {
    (void)arg;
    char c;
    assert(ut_read(-1, &c, 1, 0) == -1 && errno == EBADF);
    assert(ut_read(descriptors[1], &c, 1, 0) == -1 && errno == EINVAL);
    observed++;
}
static void prepare(void) {
    assert(pipe(descriptors) == 0);
    assert(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0);
    assert(ut_init(UT_ROUND_ROBIN) == 0);
}
int main(void) {
    prepare();
    assert(ut_spawn(eof_reader, NULL) >= 0);
    assert(ut_spawn(close_writer, NULL) >= 0);
    assert(ut_run() == 0);
    assert(close(descriptors[0]) == 0);
    ut_destroy();
    prepare();
    assert(ut_spawn(deadline_reader, NULL) >= 0);
    assert(ut_spawn(bad_fd, NULL) >= 0);
    assert(ut_run() == 0);
    close(descriptors[0]); close(descriptors[1]);
    ut_destroy();
    assert(observed == 3);
    prepare();
    assert(ut_spawn(competing_reader, NULL) >= 0);
    assert(ut_spawn(competing_reader, NULL) >= 0);
    assert(ut_spawn(gradual_writer, NULL) >= 0);
    assert(ut_run() == 0 && received == 3);
    close(descriptors[0]); close(descriptors[1]);
    ut_destroy();
    struct sigaction action = {0}, previous;
    action.sa_handler = alarm_handler;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGALRM, &action, &previous) == 0);
    struct itimerval timer = {{0,1000},{0,1000}}, off = {{0,0},{0,0}};
    prepare();
    assert(ut_spawn(deadline_reader, NULL) >= 0);
    assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    assert(ut_run() == 0);
    assert(setitimer(ITIMER_REAL, &off, NULL) == 0);
    assert(alarms > 0 && observed == 4);
    assert(sigaction(SIGALRM, &previous, NULL) == 0);
    close(descriptors[0]); close(descriptors[1]);
    ut_destroy();
    assert(ut_init(UT_ROUND_ROBIN) == 0);
    assert(ut_spawn(many_yields, NULL) >= 0);
    assert(ut_run() == 0);
    struct ut_stats stats;
    assert(ut_get_stats(&stats) == 0);
    assert(stats.dispatches == 1 && stats.yield_shortcuts == 1000 && stats.poll_calls == 0);
    assert(stats.recorded_events > 256 && stats.dropped_events == 0);
    assert(ut_trace_write("build/trace-growth.csv") == 0);
    ut_destroy();
    assert(ut_init_ex(UT_ROUND_ROBIN, UT_TRACE_OFF) == 0);
    assert(ut_spawn(many_yields, NULL) >= 0);
    assert(ut_run() == 0 && ut_get_stats(&stats) == 0);
    assert(stats.trace_allocated_bytes == 0 && stats.recorded_events == 0);
    assert(ut_trace_write("build/trace-disabled.csv") == -1 && errno == ENOTSUP);
    ut_destroy();
    assert(ut_init(UT_ROUND_ROBIN) == 0);
    for (int batch = 0; batch < 100; batch++) {
        assert(ut_spawn(recycled_task, NULL) >= 0);
        assert(ut_spawn(recycled_task, NULL) >= 0);
        assert(ut_run() == 0);
    }
    assert(recycled_tasks == 200);
    ut_destroy();
    assert(ut_init(UT_ROUND_ROBIN) == 0);
    int first_errno = ERANGE, second_errno = ECHILD;
    assert(ut_spawn(errno_task, &first_errno) >= 0);
    assert(ut_spawn(errno_task, &second_errno) >= 0);
    assert(ut_run() == 0);
    ut_destroy();
    puts("PASS: EOF, deadline, invalid fd, blocking-fd rejection, competing readers, EINTR, reinitialization");
    puts("PASS: single-task yield, trace growth/CSV, trace-off, slot reuse, errno isolation");
    return 0;
}
