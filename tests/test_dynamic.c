#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef NDEBUG
#error Tests require assertions
#endif

static int completed;
static void child(void *arg) {
    (void)arg;
    errno = ERANGE;
    ut_mark("dynamic_child");
    ut_yield();
    assert(errno == ERANGE);
    completed++;
}
static void parent(void *arg) {
    (void)arg;
    for (int batch = 0; batch < 10; batch++) {
        int target = completed + 31;
        for (int i = 0; i < 31; i++) assert(ut_spawn(child, NULL) >= 0);
        assert(ut_spawn(child, NULL) == -1 && errno == EAGAIN);
        errno = EDOM;
        while (completed < target) {
            ut_yield();
            assert(errno == EDOM);
        }
    }
}
int main(void) {
    alarm(20);
    assert(ut_init_ex(UT_ROUND_ROBIN, UT_TRACE_RECENT) == 0);
    struct ut_stats before, after;
    assert(ut_get_stats(&before) == 0);
    assert(ut_spawn(parent, NULL) >= 0 && ut_run() == 0 && completed == 310);
    assert(ut_get_stats(&after) == 0);
    assert(after.trace_allocated_bytes == before.trace_allocated_bytes);
    assert(after.recorded_events > 0 && after.recorded_events <= 256);
    assert(after.dropped_events == 0);
    assert(ut_trace_write("build/trace-recent.csv") == 0);
    FILE *trace = fopen("build/trace-recent.csv", "r");
    assert(trace);
    char line[256], kind[32];
    assert(fgets(line, sizeof line, trace));
    assert(fgets(line, sizeof line, trace));
    size_t overwritten = 0;
    assert(sscanf(line, "0,-1,overwritten,%zu", &overwritten) == 1 && overwritten > 0);
    size_t count = 0;
    long long previous = -1, timestamp;
    int tid;
    while (fgets(line, sizeof line, trace)) {
        assert(sscanf(line, "%lld,%d,%31[^,]", &timestamp, &tid, kind) == 3);
        assert(timestamp >= previous && strcmp(kind, "dropped"));
        previous = timestamp;
        count++;
    }
    assert(count == after.recorded_events && !strcmp(kind, "done"));
    assert(fclose(trace) == 0);
    ut_destroy();
    puts("PASS: dynamic spawn, live-task cap, slot reuse, errno isolation, bounded recent trace");
    return 0;
}
