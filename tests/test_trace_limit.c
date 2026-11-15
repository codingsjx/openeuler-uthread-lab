#include "uthread.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#ifdef NDEBUG
#error Runtime tests require assertions to be enabled
#endif
static void task(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) ut_yield();
}
int main(void) {
    assert(ut_init(UT_ROUND_ROBIN) == 0);
    assert(ut_spawn(task, NULL) >= 0 && ut_run() == 0);
    struct ut_stats stats;
    assert(ut_get_stats(&stats) == 0 && stats.dropped_events > 0);
    assert(ut_trace_write("build/trace-limit.csv") == -1 && errno == ENOSPC);
    ut_destroy();
    puts("PASS: trace exhaustion preserves task execution and fails export explicitly");
    return 0;
}
